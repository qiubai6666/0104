#include "ougaflashservice.h"
#include "deviceoperationlease.h"
#include "ougaflashplanner.h"
#include "ougapackage.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUuid>
#include <QtConcurrent>
using namespace Ouga;
OugaFlashService::OugaFlashService(OugaCommandRunner *runner, QObject *parent)
    : QObject(parent), m_runner(runner) {
  Q_ASSERT(runner);
  m_wait.setSingleShot(true);
  connect(runner, &OugaCommandRunner::output, this,
          [this](const QString &text) {
            m_streamedOutput += text;
            writeLog(text);
          });
  connect(runner, &OugaCommandRunner::stalled, this, [this] {
    writeLog("命令长时间无响应。请勿拔线；等待人工处理。程序不会强制杀死正在写"
             "入的进程。");
  });
  connect(runner, &OugaCommandRunner::completed, this,
          [this](int c, bool n, const QString &out) {
            // A runner may stream output, return it only at completion, or
            // both. Persist the final unread suffix instead of logging every
            // command twice.
            writeLog(out.startsWith(m_streamedOutput)
                         ? out.mid(m_streamedOutput.size())
                         : out);
            m_streamedOutput.clear();
            auto cb = std::move(m_callback);
            m_callback = {};
            if (cb)
              cb(c, n, out);
          });
}
void OugaFlashService::configure(const QString &tool, const QString &dir) {
  if (m_busy)
    return;
  m_tool = tool;
  m_logBase = dir;
}
bool OugaFlashService::acquire() {
  if (m_busy)
    return false;
  QString error;
  if (!DeviceOperationLease::acquire(this, &error)) {
    emit finished(false, error);
    return false;
  }
  ++m_generation;
  m_busy = true;
  m_stop = false;
  m_paused = false;
  emit busyChanged(true);
  return true;
}
void OugaFlashService::finish(bool success, const QString &message) {
  ++m_generation;
  m_wait.stop();
  m_modeClock.invalidate();
  success = success && !m_stop;
  writeLog((success ? "完成：" : "已停止/失败：") + message);
  QString finalMessage = message;
  if (success && m_stop) {
    success = false;
    finalMessage = "日志保存失败或结束前收到停止请求；请保留本次日志排错。";
  }
  if (m_log.isOpen()) {
    // Final durability is part of success, not an after-the-fact warning.
    if (!m_log.flush()) {
      success = false;
      finalMessage = "无法持久保存最终命令日志；请保留本次日志排错。";
      emit log(finalMessage);
    }
    QSaveFile result(QDir(m_logDir).filePath("result.json"));
    QJsonObject record{{"success", success},
                       {"message", finalMessage},
                       {"completedWrites", m_completed},
                       {"totalWrites", m_plan.flashCount},
                       {"timestamp", QDateTime::currentDateTime().toString(
                                         Qt::ISODateWithMs)}};
    const QByteArray data = QJsonDocument(record).toJson();
    if (!result.open(QIODevice::WriteOnly) ||
        result.write(data) != data.size() || !result.commit()) {
      success = false;
      finalMessage = "无法持久保存最终结果；请保留本次日志排错。";
      emit log(finalMessage);
    }
    m_log.close();
    if (success)
      emit progress(m_plan.flashCount, m_plan.flashCount, "全部步骤成功");
  }
  m_busy = false;
  m_paused = false;
  m_callback = {};
  DeviceOperationLease::release(this);
  emit busyChanged(false);
  emit finished(success, finalMessage);
}
void OugaFlashService::writeLog(const QString &text) {
  if (text.isEmpty())
    return;
  QString line = QDateTime::currentDateTime().toString(Qt::ISODateWithMs) +
                 " " + text + '\n';
  if (m_log.isOpen()) {
    QByteArray bytes = line.toUtf8();
    if (m_log.write(bytes) != bytes.size() || !m_log.flush()) {
      m_stop = true;
      emit log("日志写入失败；当前命令结束后停止。");
    }
  }
  emit log(line);
}
void OugaFlashService::command(
    const QStringList &args, std::function<void(int, bool, const QString &)> cb,
    bool bound) {
  if (m_stop) {
    finish(false, "已请求停止；不会启动下一条命令，已写内容不会回滚");
    return;
  }
  QStringList a;
  if (bound) {
    if (m_serial.isEmpty()) {
      finish(false, "缺少绑定序列号");
      return;
    }
    a << "-s" << m_serial;
  }
  a += args;
  m_streamedOutput.clear();
  m_callback = std::move(cb);
  writeLog("命令：" + m_tool + " " + a.join(' '));
  if (m_stop) {
    finish(false, "日志保存失败，未启动下一条命令");
    return;
  }
  int timeout = m_timing.readTimeoutMs;
  if (m_modeClock.isValid())
    timeout = qMax(
        1, qMin(timeout, m_timing.modeTimeoutMs - int(m_modeClock.elapsed())));
  m_runner->run(m_tool, a, timeout);
}
void OugaFlashService::discover() {
  if (!acquire())
    return;
  m_serial.clear();
  command(
      {"devices"},
      [this](int c, bool n, const QString &out) {
        if (!commandSucceeded(c, n, out)) {
          finish(false, "读取设备列表失败");
          return;
        }
        QStringList serials;
        for (QString l : out.split('\n')) {
          QStringList fields = l.simplified().split(' ');
          if (fields.size() >= 2 && fields[1] == "fastboot" &&
              !serials.contains(fields[0]))
            serials << fields[0];
        }
        emit devicesFound(serials);
        finish(true, serials.isEmpty() ? "没有 Fastboot 设备"
                                       : "已读取设备列表，请明确选择设备");
      },
      false);
}
void OugaFlashService::queryDevice(
    std::function<void(int, bool, const QString &)> callback) {
  command({"getvar", "all"},
          [this, callback](int c, bool normal, const QString &all) {
            if (!commandSucceeded(c, normal, all)) {
              callback(c, normal, all);
              return;
            }
            Device d = parseDevice(m_serial, all);
            QStringList keys;
            if (!d.modeKnown)
              keys << "is-userspace";
            if (!d.unlockKnown)
              keys << "unlocked";
            if (d.slot.isEmpty())
              keys << "current-slot";
            if (d.product.isEmpty())
              keys << "product";
            queryMissing(keys, all, callback);
          });
}
void OugaFlashService::queryMissing(
    QStringList keys, QString text,
    std::function<void(int, bool, const QString &)> callback) {
  if (keys.isEmpty()) {
    callback(0, true, text);
    return;
  }
  const QString key = keys.takeFirst();
  command({"getvar", key}, [this, keys, text, callback](int code, bool normal,
                                                        const QString &out) {
    if (!commandSucceeded(code, normal, out)) {
      callback(code, normal, out);
      return;
    }
    queryMissing(keys, text + '\n' + out, callback);
  });
}
void OugaFlashService::probe(const QString &serial) {
  if (!acquire())
    return;
  m_serial = serial;
  queryDevice([this](int c, bool n, const QString &out) {
    if (!commandSucceeded(c, n, out)) {
      finish(false, "设备探测失败");
      return;
    }
    Device d = parseDevice(m_serial, out);
    if (!d.modeKnown) {
      finish(false, "无法确定 Fastboot/FastbootD 模式");
      return;
    }
    probeStable(d.userspace, [this](const Device &dev) {
      emit deviceReady(dev);
      finish(true, "设备快照已读取，尚未执行任何写入");
    });
  });
}
void OugaFlashService::probeStable(bool userspace,
                                   std::function<void(const Device &)> ready) {
  m_expectedMode = userspace;
  m_stable = 0;
  m_modeClock.start();
  m_stableClock.invalidate();
  pollStable(std::move(ready));
}
void OugaFlashService::pollStable(std::function<void(const Device &)> ready) {
  if (m_stop) {
    finish(false, "已在命令边界停止");
    return;
  }
  if (m_modeClock.elapsed() >= m_timing.modeTimeoutMs) {
    finish(false, "等待原设备模式超时（不接管其他设备）");
    return;
  }
  queryDevice([this, ready](int c, bool n, const QString &out) {
    Device d = parseDevice(m_serial, out);
    bool sameSerial = !d.variables.contains("serialno") ||
                      d.variables.value("serialno") == m_serial;
    if (commandSucceeded(c, n, out) && d.modeKnown &&
        d.userspace == m_expectedMode && sameSerial) {
      if (m_stable == 0 || !d.sameSnapshot(m_lastSnapshot) ||
          !m_stableClock.isValid() ||
          m_stableClock.elapsed() > m_timing.stableWindowMs) {
        m_stable = 0;
        m_lastSnapshot = d;
        m_stableClock.start();
      }
      if (++m_stable >= m_timing.stableSamples) {
        m_modeClock.invalidate();
        ready(d);
        return;
      }
    } else {
      m_stable = 0;
    }
    const quint64 generation = m_generation;
    QTimer::singleShot(m_timing.pollMs, this, [this, ready, generation] {
      if (m_busy && generation == m_generation)
        pollStable(ready);
    });
  });
}
void OugaFlashService::prepareMode(const QString &serial, bool userspace) {
  if (!acquire())
    return;
  m_serial = serial;
  command({"reboot", userspace ? "fastboot" : "bootloader"},
          [this, userspace](int c, bool n, const QString &out) {
            if (!commandSucceeded(c, n, out)) {
              finish(false, "准备模式切换失败");
              return;
            }
            probeStable(userspace, [this](const Device &d) {
              emit deviceReady(d);
              finish(true, "准备模式切换完成，请生成并确认计划");
            });
          });
}
bool OugaFlashService::savePlan(const Plan &p) {
  QFile f(QDir(m_logDir).filePath(
      "plan-" + QString::number(m_checkpointCount++) + ".json"));
  if (!f.open(QIODevice::WriteOnly | QIODevice::NewOnly))
    return false;
  QJsonObject obj;
  obj["serial"] = p.device.serial;
  obj["product"] = p.device.product;
  obj["summary"] = p.summary;
  obj["text"] = planText(p);
  QJsonArray steps;
  for (const Step &s : p.steps) {
    QJsonObject step;
    step["kind"] = int(s.kind);
    step["arguments"] = QJsonArray::fromStringList(s.arguments);
    step["image"] = s.image;
    step["target"] = s.target;
    step["sha256"] = QString::fromLatin1(s.sha256.toHex());
    step["title"] = s.title;
    steps << step;
  }
  obj["steps"] = steps;
  QByteArray data = QJsonDocument(obj).toJson();
  return f.write(data) == data.size() && f.flush();
}
void OugaFlashService::checkAsync(std::function<QString()> check,
                                  std::function<void()> ready) {
  const quint64 generation = m_generation;
  auto *watch = new QFutureWatcher<QString>(this);
  connect(watch, &QFutureWatcher<QString>::finished, this,
          [this, watch, ready, generation] {
            const QString error = watch->result();
            watch->deleteLater();
            if (!m_busy || generation != m_generation)
              return;
            if (!error.isEmpty() || m_stop) {
              finish(false, m_stop ? "镜像复检后停止" : error);
              return;
            }
            ready();
          });
  watch->setFuture(QtConcurrent::run(std::move(check)));
}
void OugaFlashService::verifyImages(std::function<void()> ready) {
  const auto images = m_plan.images;
  checkAsync(
      [images] {
        for (const Partition &image : images) {
          if (!image.selected)
            continue;
          QString error;
          if (OugaPackage::expandedSize(image.path, &error) !=
                  image.expandedBytes ||
              OugaPackage::digest(image.path, &error) != image.sha256)
            return QString("镜像自预览后已变更或不可读：") + image.path;
        }
        return QString();
      },
      std::move(ready));
}
void OugaFlashService::validateAndResume() {
  verifyImages([this] {
    probeStable(m_expected.userspace, [this](const Device &device) {
      QString error;
      if (!compatible(device, true, &error)) {
        finish(false, error);
        return;
      }
      next();
    });
  });
}
bool OugaFlashService::compatible(const Device &d, bool initial,
                                  QString *e) const {
  if (d.serial != m_serial ||
      (!d.variables.value("serialno").isEmpty() &&
       d.variables.value("serialno") != m_serial) ||
      d.product != m_expected.product || d.platform != m_expected.platform ||
      !d.unlockKnown || !d.unlocked || !d.modeKnown) {
    *e = "原设备身份、平台或解锁状态不一致/未知";
    return false;
  }
  if (d.slot != m_expected.slot) {
    *e = "活动槽发生非计划改变";
    return false;
  }
  if (initial &&
      (!d.sameLayout(m_expected) || d.userspace != m_expected.userspace)) {
    *e = "设备表自预览后改变，请重新生成计划";
    return false;
  }
  return true;
}
void OugaFlashService::execute(const Plan &plan) {
  if (!acquire())
    return;
  Plan rebuilt;
  QString error;
  if (!OugaFlashPlanner::build(plan.images, plan.device, plan.options, &rebuilt,
                               &error) ||
      planText(rebuilt) != planText(plan)) {
    finish(false, "执行计划无效或已变更：" + error);
    return;
  }
  if (plan.options.clearData &&
      plan.options.mode != FlashMode::RepairFastbootd &&
      plan.device.platform == Platform::Qualcomm) {
    const QString dependencyError = OugaProcessRunner::formatToolsError(m_tool);
    if (!dependencyError.isEmpty()) {
      finish(false, dependencyError);
      return;
    }
  }
  m_plan = plan;
  m_expected = plan.device;
  m_serial = plan.device.serial;
  m_index = 0;
  m_completed = 0;
  m_checkpointCount = 0;
  QString session = "ouga-" +
                    QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-") +
                    QUuid::createUuid().toString(QUuid::Id128);
  m_logDir = QDir(m_logBase).filePath(session);
  if (!QDir().mkpath(m_logDir)) {
    finish(false, "不能创建持久日志目录");
    return;
  }
  m_log.setFileName(QDir(m_logDir).filePath("execution.log"));
  if (!m_log.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
      !savePlan(plan)) {
    finish(false, "不能保存计划或命令日志，拒绝执行");
    return;
  }
  writeLog("日志目录：" + m_logDir);
  validateAndResume();
}
void OugaFlashService::requestStop() {
  if (!m_busy)
    return;
  m_stop = true;
  writeLog("停止请求已记录：等待当前写入结束，不再启动下一步；不会回滚。");
  if (m_paused) {
    finish(false, "用户取消检查点确认");
  } else if (m_wait.isActive()) {
    m_wait.stop();
    finish(false, "等待阶段停止");
  }
}
void OugaFlashService::confirmCheckpoint(bool proceed) {
  if (!m_busy || !m_paused)
    return;
  if (!proceed) {
    finish(false, "用户拒绝后续计划；Super 已写入，不会回滚");
    return;
  }
  m_paused = false;
  if (!savePlan(m_plan)) {
    finish(false, "无法持久保存检查点计划");
    return;
  }
  validateAndResume();
}
void OugaFlashService::next() {
  if (!m_busy || m_paused)
    return;
  if (m_stop) {
    finish(false, "已在分区边界停止；未执行后续擦数据/重启");
    return;
  }
  if (m_index >= m_plan.steps.size()) {
    finish(true, "全部计划步骤已成功；模拟验证不代表真机兼容性");
    return;
  }
  Step s = m_plan.steps[m_index];
  emit progress(m_completed, m_plan.flashCount, s.title);
  writeLog(s.title);
  if (m_stop) {
    finish(false, "已在分区边界停止");
    return;
  }
  if (s.kind == Step::Wait) {
    disconnect(&m_wait, nullptr, this, nullptr);
    connect(&m_wait, &QTimer::timeout, this, [this] {
      ++m_index;
      next();
    });
    m_wait.start(qMax(0, s.waitMs * m_timing.waitScale));
    return;
  }
  if (s.kind == Step::Checkpoint) {
    probeStable(s.userspace, [this](const Device &d) {
      QString e;
      if (!compatible(d, false, &e)) {
        finish(false, e);
        return;
      }
      Options opts = m_plan.options;
      opts.afterSuper = true;
      Plan tail;
      if (!OugaFlashPlanner::build(m_plan.images, d, opts, &tail, &e)) {
        finish(false, "Super 后重新探测阻止继续：" + e);
        return;
      }
      tail.flashCount += m_completed;
      m_plan = tail;
      m_expected = d;
      m_index = 0;
      m_paused = true;
      writeLog(planText(tail));
      emit checkpoint(tail);
    });
    return;
  }
  if (s.kind == Step::Reprobe) {
    probeStable(s.userspace, [this](const Device &d) {
      QString error;
      if (!compatible(d, true, &error)) {
        finish(false, "售后收尾分区表核对失败，请重新生成并确认计划：" + error);
        return;
      }
      const QString slot = afterSalesSlot(d, &error);
      if (slot.isEmpty() || slot != m_plan.options.targetSlot) {
        finish(false, "售后启动槽与确认计划不一致，拒绝切槽/清数据/重启：" + error);
        return;
      }
      m_expected = d;
      ++m_index;
      next();
    });
    return;
  }
  auto run = [this, s] {
    command(s.arguments, [this, s](int c, bool n, const QString &out) {
      bool ok = commandSucceeded(c, n, out);
      if (!ok && !(s.allowMissing && n && partitionMissing(out))) {
        finish(false, "步骤失败：" + s.title);
        return;
      }
      if (s.kind == Step::ModeSwitch) {
        probeStable(s.userspace, [this](const Device &d) {
          QString e;
          if (!compatible(d, false, &e)) {
            finish(false, e);
            return;
          }
          m_expected = d;
          ++m_index;
          next();
        });
        return;
      }
      if (!s.image.isEmpty())
        ++m_completed;
      if (s.arguments.value(0) == "set_active")
        m_expected.slot = s.arguments.value(1);
      if (s.arguments.value(0) == "create-logical-partition") {
        m_expected.sizes[s.target] = s.arguments.value(2).toULongLong();
        m_expected.partitions.insert(s.target);
        m_expected.logical.insert(s.target);
      }
      if (s.arguments.value(0) == "delete-logical-partition") {
        m_expected.sizes.remove(s.target);
        m_expected.partitions.remove(s.target);
        m_expected.logical.remove(s.target);
      }
      ++m_index;
      const quint64 generation = m_generation;
      QTimer::singleShot(0, this, [this, generation] {
        if (generation == m_generation)
          next();
      });
    });
  };
  if (s.image.isEmpty()) {
    run();
    return;
  }
  checkAsync(
      [s] {
        QString error;
        if (OugaPackage::digest(s.image, &error) != s.sha256)
          return QString("镜像在执行期间改变：") + s.image;
        return QString();
      },
      [this, s, run] {
        if (!m_expected.sizes.contains(s.target) ||
            quint64(s.bytes) > m_expected.sizes[s.target]) {
          finish(false, "实际目标容量变化：" + s.target);
          return;
        }
        run();
      });
}
