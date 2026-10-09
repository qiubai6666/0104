#include "ougaflashservice.h"
#include "deviceoperationlease.h"
#include "ougaflashplanner.h"
#include "ougapackage.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QStandardPaths>
#include <QSaveFile>
#include <QRegularExpression>
#include <QUuid>
#include <memory>
#include <optional>
using namespace Ouga;
namespace {
QStringList connectedSerials(const QString &output) {
  static const QRegularExpression pattern(
      "^\\s*(\\S+)\\s+fastboot\\s*$",
      QRegularExpression::CaseInsensitiveOption | QRegularExpression::MultilineOption);
  QStringList serials;
  auto matches = pattern.globalMatch(output);
  while (matches.hasNext()) {
    const QString serial = matches.next().captured(1);
    if (!serials.contains(serial, Qt::CaseInsensitive)) serials << serial;
  }
  return serials;
}
} // namespace
OugaFlashService::OugaFlashService(OugaCommandRunner *runner, QObject *parent)
    : QObject(parent), m_runner(runner) {
  Q_ASSERT(runner);
  m_wait.setSingleShot(true);
  connect(runner, &OugaCommandRunner::output, this,
          [this](const QString &text) {
            m_streamedOutput += text;
            if (!m_executing) return;
            updateTransferOutput(text);
          });
  connect(runner, &OugaCommandRunner::stalled, this, [this] {
    writeLog("设备Fastboot通信链路堵塞，请检查数据线或连接接口后重试.");
  });
  connect(runner, &OugaCommandRunner::nativeOutputLine, this,
          [this](const QString &line, bool standardError) {
    if (!m_executing) return;
    m_channelOutputRecorded = true;
    m_nativeOutput += QDateTime::currentDateTime().toString("[HH:mm:ss] ") +
        (standardError ? "[FASTBOOT ERROR] " : "[FASTBOOT OUTPUT] ") + line + '\n';
  });
  connect(runner, &OugaCommandRunner::completed, this,
          [this](int code, bool normal, const QString &output) {
            const QString complete = output.startsWith(m_streamedOutput)
                ? output : m_streamedOutput + output;
            if (m_executing)
              updateTransferOutput(complete.mid(m_streamedOutput.size()) + '\n');
            if (m_executing) {
              if (!m_channelOutputRecorded)
                for (const QString &line : complete.split(QRegularExpression("[\r\n]+"), Qt::SkipEmptyParts))
                  m_nativeOutput += QDateTime::currentDateTime().toString("[HH:mm:ss] ") + "[FASTBOOT OUTPUT] " + line + '\n';
              if (m_commandFlashTarget.size())
                m_nativeOutput += QDateTime::currentDateTime().toString("[HH:mm:ss] ") + "[FLASH END] " +
                    m_commandFlashTarget + (commandSucceeded(code, normal, complete) ? QString() :
                    QString(" (exit %1)").arg(code)) + '\n';
            }
            m_streamedOutput.clear();
            auto callback = std::move(m_callback);
            m_callback = {};
            if (callback) callback(code, normal, complete);
          });
}
void OugaFlashService::configure(const QString &tool, const QString &dir) {
  if (m_busy)
    return;
  m_tool = tool;
  m_logBase = dir;
}
void OugaFlashService::updateTransferOutput(const QString &text) {
  if (m_index >= m_plan.steps.size() || m_plan.steps[m_index].image.isEmpty()) return;
  static const QRegularExpression sending(
      "Sending(?:\\s+sparse)?\\s+'[^']+'(?:\\s+\\d+/\\d+)?\\s*\\((\\d+(?:\\.\\d+)?)\\s*(KB|MB|GB|B)\\)",
      QRegularExpression::CaseInsensitiveOption);
  static const QRegularExpression progress(
      "^\\s*\\S+:\\s+(\\d+(?:\\.\\d+)?)\\s*(KB|MB|GB|B)/",
      QRegularExpression::CaseInsensitiveOption);
  static const QRegularExpression failure("FAILED|\\berror\\b", QRegularExpression::CaseInsensitiveOption);
  auto bytes = [](const QRegularExpressionMatch &match) {
    const QString unit = match.captured(2).toUpper();
    const double factor = unit == "GB" ? 1073741824.0 : unit == "MB" ? 1048576.0 : unit == "KB" ? 1024.0 : 1.0;
    return qint64(match.captured(1).toDouble() * factor);
  };
  m_transferPending += text;
  int end;
  while ((end = m_transferPending.indexOf(QRegularExpression("[\\r\\n]"))) >= 0) {
    const QString line = m_transferPending.left(end);
    m_transferPending.remove(0, end + 1);
    const auto send = sending.match(line);
    if (send.hasMatch()) {
      m_chunkExpected = bytes(send);
      m_chunkReported = 0;
    }
    bool changed = false;
    const auto current = progress.match(line);
    if (current.hasMatch()) {
      const qint64 reported = qMin(bytes(current), m_chunkExpected > 0 ? m_chunkExpected : m_transferImageBytes);
      const qint64 delta = reported >= m_chunkReported ? reported - m_chunkReported : reported;
      if (delta > 0) {
        m_transferAccumulated += delta;
        m_chunkReported = reported;
        changed = true;
      }
    }
    m_transferFailed |= failure.match(line).hasMatch();
    if (!m_transferFailed && line.contains("Writing '", Qt::CaseInsensitive) && m_chunkExpected > m_chunkReported) {
      m_transferAccumulated += m_chunkExpected - m_chunkReported;
      m_chunkReported = m_chunkExpected;
      changed = true;
    }
    if (!m_transferFailed && line.startsWith("Finished.", Qt::CaseInsensitive)) {
      m_transferAccumulated = qMax(m_transferAccumulated, m_transferImageBytes);
      changed = true;
    }
    if (!changed || m_transferImageBytes <= 0) continue;
    m_transferAccumulated = qMin(m_transferAccumulated, m_transferImageBytes);
    const qint64 sent = qint64(qMin(double(m_transferStepBytes),
        m_transferAccumulated * (double(m_transferStepBytes) / m_transferImageBytes)));
    m_transferReported = qMax(m_transferReported, qMin(m_plan.totalBytes, m_transferCommitted + sent));
    emit transferProgress(m_transferReported, m_plan.totalBytes);
  }
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
  emit busyChanged(true);
  return true;
}
void OugaFlashService::setLogContext(const QString &source, const QString &interfaceLog) {
  if (m_busy) return;
  m_source = source;
  m_interfacePrefix = interfaceLog;
}
void OugaFlashService::setPostActions(bool clearData, bool autoReboot) {
  m_clearData = clearData;
  m_autoReboot = autoReboot;
}
void OugaFlashService::logPartitionSummary() {
  if (!m_fullPackage || m_summaryLogged) return;
  m_summaryLogged = true;
  writeLog(QString("刷写任务总结：成功 %1 个，失败 %2 个.")
               .arg(m_successful).arg(m_failedTargets.size()));
  if (!m_failedTargets.isEmpty()) {
    QStringList targets = m_failedTargets;
    targets.removeDuplicates();
    writeLog("刷写失败分区：" + targets.join("、"));
  }
}
void OugaFlashService::finish(bool success, const QString &message) {
  ++m_generation;
  m_wait.stop();
  m_modeClock.invalidate();
  QString result = message;
  if (m_executing) {
    if (m_fullPackage) {
      logPartitionSummary();
      if (m_stop) {
        result = "任务已按用户请求停止.";
        writeLog("线刷流程结束：" + result);
      } else if (success && m_plan.options.mode == FlashMode::OnlyFastbootd &&
                 !m_failedTargets.isEmpty()) {
        const QString count = m_failedTargets.size() == 1 ? "有一个分区刷写失败"
            : QString("有%1个分区刷写失败").arg(m_failedTargets.size());
        result = "所有任务执行完成；" + count + "，为了防止变砖，已自动取消自动重启!";
        writeLog("线刷流程结束：" + result);
      } else if (success && m_failedTargets.isEmpty() && !m_phaseFailed) {
        result = "所有任务执行成功！";
        writeLog("线刷流程结束：" + result);
      } else if (!success) {
        writeLog(message);
      }
    } else {
      result = m_stop ? "任务已按用户请求停止." : success ? QString() : message;
      if (!result.isEmpty()) writeLog(result);
    }
    if (m_fullPackage && success && !m_stop)
      emit transferProgress(m_plan.totalBytes, m_plan.totalBytes);
    success = success && !m_stop && m_failedTargets.isEmpty() && !m_phaseFailed;
    const bool save = m_fullPackage || m_plan.options.mode == FlashMode::AfterSalesFastbootd;
    if (save && !saveLog()) {
      emit log("生成日志文件失败: 无法写入日志文件");
    }
    emit progress(m_completed, m_plan.flashCount, result);
  }
  m_executing = false;
  m_busy = false;
  m_callback = {};
  DeviceOperationLease::release(this);
  emit busyChanged(false);
  emit finished(success && !m_stop, result);
}
void OugaFlashService::writeLog(const QString &text) {
  if (text.isEmpty()) return;
  if (m_executing)
    m_lines << QDateTime::currentDateTime().toString("[HH:mm:ss ] ") + text;
  emit log(text);
}
void OugaFlashService::beginStep(const QString &title) {
  m_stepLine = -1;
  m_transferStepBytes = m_transferImageBytes = 0;
  if (m_executing) {
    m_stepLine = m_lines.size();
    m_lines << QDateTime::currentDateTime().toString("[HH:mm:ss ] ") + title + " ...";
  }
  emit stepStarted(title);
}
void OugaFlashService::endStep(bool success) {
  if (m_stepLine >= 0 && m_stepLine < m_lines.size()) {
    const bool slot = m_lines[m_stepLine].contains("设置槽位：");
    m_lines[m_stepLine] += success ? (slot ? "Done" : "OK") : "Error";
  }
  m_stepLine = -1;
  emit stepFinished(success);
}
void OugaFlashService::appendNative(const QString &text, bool error) {
  QStringList lines = text.split('\n', Qt::SkipEmptyParts), diagnostics;
  if (error) {
    for (const QString &line : lines)
      if (line.contains(QRegularExpression("FAILED|error|cannot|invalid|remote:",
                                             QRegularExpression::CaseInsensitiveOption)))
        diagnostics << line;
    lines = diagnostics.isEmpty() ? lines.mid(qMax(0, lines.size() - 3)) : diagnostics;
  }
  for (QString line : lines) {
    line = line.trimmed();
    if (line.isEmpty() || line == "ERROR_DETECTED") continue;
    if (m_executing) m_lines << "    " + line;
    emit nativeLog("    " + line, error);
  }
}
bool OugaFlashService::saveLog() {
  const QString directory = m_logBase.isEmpty()
      ? QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) : m_logBase;
  if (!QDir().mkpath(directory)) return false;
  const bool sales = m_plan.options.mode == FlashMode::AfterSalesBootloader ||
                     m_plan.options.mode == FlashMode::AfterSalesFastbootd;
  QString content = "=== 紫罗兰工具箱欧加刷写日志 ===\n日志编号: " +
      QUuid::createUuid().toString(QUuid::Id128).toUpper() +
      "\n生成时间: " + QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss") +
      "\n工具版本: " + QCoreApplication::applicationVersion() +
      "\n刷写模式: " + (sales ? "售后包模式" : "全量包模式") +
      "\n刷写文件路径: " + (m_source.isEmpty() ? "未记录" : m_source) +
      "\n==============================\n\n=== 界面显示日志 ===\n";
  if (!m_interfacePrefix.isEmpty()) content += m_interfacePrefix + '\n';
  content += m_lines.join('\n') + "\n\n=== 完整Fastboot输出 ===\n" +
      (m_nativeOutput.isEmpty() ? "无fastboot输出记录" : m_nativeOutput.trimmed()) +
      "\n\n=== 日志结束 ===\n";
  content.replace("\r\n", "\n");
  content.replace("\n", "\r\n");
  const QByteArray hash = QCryptographicHash::hash(content.toUtf8(), QCryptographicHash::Sha256).toHex().toUpper();
  content += "\r\n=== 日志完整性信息 ===\r\nSHA-256: " + QString::fromLatin1(hash) + "\r\n";
  const QString file = QDir(directory).filePath(
      QString(sales ? "售后包刷机日志_" : "刷机日志_") +
      QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss") + ".txt");
  QSaveFile output(file);
  const QByteArray data = content.toUtf8();
  if (!output.open(QIODevice::WriteOnly) || output.write(data) != data.size() || !output.commit())
    return false;
  emit log("刷机日志已自动保存在: " + file);
  return true;
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
  m_channelOutputRecorded = false;
  m_callback = std::move(cb);
  m_commandFlashTarget = args.value(0) == "flash" ? args.value(1) : QString();
  if (m_executing) {
    const QString timestamp = QDateTime::currentDateTime().toString("[HH:mm:ss] ");
    if (!m_commandFlashTarget.isEmpty())
      m_nativeOutput += timestamp + "[FLASH BEGIN] " + m_commandFlashTarget + " <= \"" + args.value(2) + "\"\n";
    QStringList display = a;
    if (!m_commandFlashTarget.isEmpty()) display.last() = '"' + display.last() + '"';
    m_nativeOutput += timestamp + "[FASTBOOT CMD] " + display.join(' ') + '\n';
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
        const QStringList serials = connectedSerials(out);
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
void OugaFlashService::probe(const QString &serial, bool fullPackage) {
  if (!acquire())
    return;
  m_serial = serial;
  m_fullPackage = fullPackage;
  if (!fullPackage) {
    queryMode([this](bool userspace) {
      probeStable(userspace, [this](const Device &device) {
        emit deviceReady(device);
        finish(true, {});
      });
    });
    return;
  }
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
void OugaFlashService::queryMode(std::function<void(bool)> ready) {
  const auto mode = [](const QString &output) -> std::optional<bool> {
    const auto value = parseDevice({}, output).variables.value("is-userspace");
    if (QRegularExpression("^(?:yes|true|1)$", QRegularExpression::CaseInsensitiveOption).match(value).hasMatch())
      return true;
    if (QRegularExpression("^(?:no|false|0)$", QRegularExpression::CaseInsensitiveOption).match(value).hasMatch())
      return false;
    return {};
  };
  command({"getvar", "is-userspace"}, [this, ready, mode](int, bool, const QString &output) {
    const auto direct = mode(output);
    if (direct.has_value()) { ready(*direct); return; }
    command({"getvar", "all"}, [ready, mode](int, bool, const QString &all) {
      ready(mode(all).value_or(all.contains("super-partition-name", Qt::CaseInsensitive)));
    });
  });
}
void OugaFlashService::probeStable(bool userspace,
                                   std::function<void(const Device &)> ready,
                                   std::function<void()> failed) {
  m_expectedMode = userspace;
  m_stable = 0;
  m_connectionReady = false;
  m_modeClock.start();
  m_stableClock.invalidate();
  pollStable(std::move(ready), std::move(failed));
}
void OugaFlashService::pollStable(std::function<void(const Device &)> ready,
                                  std::function<void()> failed) {
  if (m_stop) {
    finish(false, "已在命令边界停止");
    return;
  }
  if ((!m_connectionReady && m_modeClock.elapsed() >= m_timing.modeTimeoutMs) ||
      (m_connectionReady && m_stableClock.elapsed() >= m_timing.stableWindowMs)) {
    if (failed) failed();
    else finish(false, "等待原设备模式超时（不接管其他设备）");
    return;
  }
  auto retry = [this, ready, failed] {
    const quint64 generation = m_generation;
    const int delay = m_connectionReady ? m_timing.pollMs : qMin(200, m_timing.pollMs);
    QTimer::singleShot(delay, this, [this, ready, failed, generation] {
      if (m_busy && generation == m_generation)
        pollStable(ready, failed);
    });
  };
  command({"devices"}, [this, ready, failed, retry](int, bool, const QString &out) {
    const bool connected = connectedSerials(out).contains(m_serial, Qt::CaseInsensitive);
    if (!connected) { m_stable = 0; retry(); return; }
    if (m_connectionReady && ++m_stable < m_timing.stableSamples) {
      retry();
      return;
    }
    queryMode([this, ready, failed, retry](bool userspace) {
      if (userspace != m_expectedMode) { m_stable = 0; retry(); return; }
      if (!m_connectionReady) {
        m_connectionReady = true;
        m_stable = 0;
        m_stableClock.start();
        // The reference begins its stability loop immediately after the wait.
        pollStable(ready, failed);
        return;
      }
      // Read the table once for subsequent planning, outside stability sampling.
      command({"getvar", "all"}, [this, ready, retry](int c, bool normal, const QString &output) {
        if (m_fullPackage && !commandSucceeded(c, normal, output)) {
          m_stable = 0; retry(); return;
        }
        m_modeClock.invalidate();
        Device device = parseDevice(m_serial, output);
        device.modeKnown = true;
        device.userspace = m_expectedMode;
        ready(device);
      });
    });
  }, false);
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
              finish(true, "设备模式准备完成");
            });
          });
}
void OugaFlashService::validateAndResume() {
  probeStable(m_expected.userspace, [this](const Device &device) {
    QString error;
    if (!compatible(device, true, &error)) {
      finish(false, error);
      return;
    }
    next();
  });
}
bool OugaFlashService::compatible(const Device &d, bool initial,
                                  QString *e) const {
  if (!m_fullPackage) {
    if (d.serial != m_serial || !d.modeKnown ||
        (initial && d.userspace != m_expected.userspace)) {
      *e = "设备模式已改变，请重新开始";
      return false;
    }
    return true;
  }
  if (d.serial != m_serial ||
      (!d.variables.value("serialno").isEmpty() &&
       d.variables.value("serialno") != m_serial) ||
      (!m_expected.product.isEmpty() && d.product != m_expected.product) || d.platform != m_expected.platform ||
      !d.unlockKnown || !d.unlocked || !d.modeKnown) {
    *e = "原设备身份、平台或解锁状态不一致/未知";
    return false;
  }
  if (d.slot != m_expected.slot) {
    *e = "活动槽发生非计划改变";
    return false;
  }
  if (initial &&
      d.userspace != m_expected.userspace) {
    *e = "设备模式已改变，请重新开始";
    return false;
  }
  return true;
}
void OugaFlashService::execute(const Plan &plan) {
  if (!acquire()) return;
  Plan rebuilt;
  QString error;
  if (!OugaFlashPlanner::build(plan.images, plan.device, plan.options, &rebuilt, &error) ||
      planText(rebuilt) != planText(plan)) {
    finish(false, "刷写任务无效：" + error);
    return;
  }
  m_plan = plan;
  m_fullPackage = plan.options.mode != FlashMode::RepairFastbootd &&
                  plan.options.mode != FlashMode::AfterSalesBootloader &&
                  plan.options.mode != FlashMode::AfterSalesFastbootd;
  m_expected = plan.device;
  m_serial = plan.device.serial;
  m_index = m_completed = m_successful = 0;
  m_failedTargets.clear();
  m_summaryLogged = false;
  m_lines.clear();
  m_nativeOutput.clear();
  m_phaseFailed = false;
  m_eraseUserdata = m_eraseMetadata = m_wipe = m_misc = true;
  m_clearData = plan.options.clearData;
  m_clearDataStarted = false;
  m_transferCommitted = 0;
  m_transferStepBytes = 0;
  m_autoReboot = plan.options.autoReboot;
  m_executing = true;
  if (m_fullPackage)
    writeLog("请将手机停留在选择语言界面，请勿触碰设备和数据线.");
  validateAndResume();
}
void OugaFlashService::requestStop() {
  if (!m_busy) return;
  m_stop = true;
  writeLog("已请求停止，当前分区刷写完成后将停止后续任务.");
  if (m_wait.isActive()) {
    m_wait.stop();
    finish(false, "任务已按用户请求停止.");
  }
}
void OugaFlashService::next() {
  if (!m_busy) return;
  if (m_stop) {
    finish(false, "任务已按用户请求停止.");
    return;
  }
  if (m_index >= m_plan.steps.size()) {
    finish(true, "线刷流程结束");
    return;
  }
  const Step step = m_plan.steps[m_index];
  const bool sales = m_plan.options.mode == FlashMode::AfterSalesBootloader ||
                     m_plan.options.mode == FlashMode::AfterSalesFastbootd;
  if (step.kind == Step::PostActions) {
    logPartitionSummary();
    // Keep the optional actions as execution gates: the reference reads each
    // checkbox after the preceding command, including FRP and the wipe.
    Options options = m_plan.options;
    options.clearData = options.autoReboot = !sales || m_clearData || m_autoReboot;
    m_plan.steps.resize(m_index);
    m_plan.steps += OugaFlashPlanner::postActions(m_expected, options);
    next();
    return;
  }
  const bool wipeCancelled = step.kind == Step::ClearData && !m_clearData;
  const bool forcedRebootCancel = m_plan.options.mode == FlashMode::OnlyFastbootd &&
                                 !m_failedTargets.isEmpty();
  const bool rebootCancelled = step.arguments == QStringList{"reboot"} &&
                               (!m_autoReboot || forcedRebootCancel);
  if (wipeCancelled || rebootCancelled ||
      (sales && step.target == "frp" && !m_clearDataStarted)) {
    if (!sales && wipeCancelled && m_plan.options.clearData)
      writeLog("用户已取消清除数据，本次任务将跳过该步骤.");
    if (!sales && rebootCancelled && !forcedRebootCancel && m_plan.options.autoReboot)
      writeLog("用户已取消自动重启，本次任务将保留当前设备模式.");
    ++m_index;
    next();
    return;
  }
  emit progress(m_completed, m_plan.flashCount, step.title);
  if (step.kind == Step::PreSwitchGuard) {
    if (!m_failedTargets.isEmpty()) {
      const QString message = m_plan.options.mode == FlashMode::BothSlots
          ? "AB通刷物理分区存在刷写失败，已停止后续切槽和逻辑分区重建."
          : "A槽非逻辑分区存在刷写失败，已保留B槽活动并终止强力线刷.";
      finish(false, message);
      return;
    }
    ++m_index;
    next();
    return;
  }
  if (step.kind == Step::CowCleanup) {
    beginStep(step.title);
    cleanCowPartitions();
    return;
  }
  if (step.kind == Step::SaleTable) {
    command({"getvar", "all"}, [this](int, bool, const QString &output) {
      const Device table = parseDevice(m_serial, output);
      Device device = m_expected;
      device.partitions = table.partitions;
      device.sizes = table.sizes;
      device.logical = table.logical;
      const bool bootloader = m_plan.options.mode == FlashMode::AfterSalesBootloader;
      // Rebuild only the remaining sale phase against the table read after COW.
      // Its Bootloader prefix has already executed and is discarded below.
      device.userspace = !bootloader;
      Options options = m_plan.options;
      options.afterSuper = bootloader;
      Plan updated;
      QString error;
      if (!OugaFlashPlanner::build(m_plan.images, device, options, &updated, &error)) {
        finish(false, error);
        return;
      }
      int first = 0;
      while (first < updated.steps.size() && updated.steps[first].kind != Step::SaleTable)
        ++first;
      m_plan.steps.resize(m_index + 1);
      m_plan.steps += updated.steps.mid(first + 1);
      m_plan.flashCount = m_completed;
      qint64 remaining = 0;
      for (int i = m_index + 1; i < m_plan.steps.size(); ++i)
        if (!m_plan.steps[i].image.isEmpty()) {
          ++m_plan.flashCount;
          remaining += m_plan.steps[i].bytes;
        }
      if (bootloader) m_plan.totalBytes = m_transferCommitted + remaining;
      if (bootloader) {
        beginStep("对分区刷写任务进行排序");
        endStep(true);
        writeLog(QString("总结刷写任务，售后包FastbootD阶段共 %1 个分区.")
                     .arg((m_plan.flashCount - m_completed) / 2));
      } else {
        int ordinary = 0, modem = 0;
        for (int i = m_index + 1; i < m_plan.steps.size(); ++i) {
          const Step &write = m_plan.steps[i];
          if (write.image.isEmpty()) continue;
          if (write.target == "modem_a") ++modem;
          else if (write.target != "modem_b") ++ordinary;
        }
        beginStep("正在匹配分区");
        endStep(true);
        writeLog(QString("总结刷写任务，售后包FastbootD阶段 %1 个分区，Fastboot阶段 %2 个分区.")
                     .arg(ordinary).arg(modem));
        beginStep("对分区刷写任务进行排序");
        endStep(true);
      }
      m_expected.partitions = table.partitions;
      m_expected.sizes = table.sizes;
      m_expected.logical = table.logical;
      ++m_index;
      next();
    });
    return;
  }
  if (step.kind == Step::ClearData) {
    m_clearDataStarted = true;
    beginStep(step.title);
    clearData();
    return;
  }
  if (step.kind == Step::ModeSwitch && m_plan.options.mode == FlashMode::RepairFastbootd && m_successful == 0) {
    finish(false, "没有任何镜像刷写成功，不重启到FastbootD.");
    return;
  }
  if (step.kind == Step::Wait) {
    if (sales && step.waitMs == 120000) {
      writeLog(step.title);
      const int line = m_lines.size();
      const QString timestamp = QDateTime::currentDateTime().toString("[HH:mm:ss ] ");
      m_lines << timestamp + "剩余时间：120秒";
      emit coolingProgress(120);
      disconnect(&m_wait, nullptr, this, nullptr);
      const auto remaining = std::make_shared<int>(120);
      connect(&m_wait, &QTimer::timeout, this, [this, remaining, line, timestamp] {
        --*remaining;
        emit coolingProgress(*remaining);
        m_lines[line] = timestamp + (*remaining > 0
            ? QString("剩余时间：%1秒").arg(*remaining) : QString("设备冷却完成"));
        if (*remaining > 0) m_wait.start(qMax(0, 1000 * m_timing.waitScale));
        else { ++m_index; next(); }
      });
      m_wait.start(qMax(0, 1000 * m_timing.waitScale));
      return;
    }
    if (!step.title.isEmpty()) beginStep(step.title);
    disconnect(&m_wait, nullptr, this, nullptr);
    connect(&m_wait, &QTimer::timeout, this, [this, step] {
      if (!step.title.isEmpty()) endStep(true);
      ++m_index;
      next();
    });
    m_wait.start(qMax(0, step.waitMs * m_timing.waitScale));
    return;
  }
  if (step.kind == Step::Checkpoint) {
    // Reference sale FB continues automatically after the cooling countdown.
    probeStable(step.userspace, [this](const Device &device) {
      QString error;
      if (!compatible(device, false, &error)) { finish(false, error); return; }
      Options options = m_plan.options;
      options.afterSuper = true;
      options.clearData = m_clearData;
      options.autoReboot = m_autoReboot;
      Plan tail;
      if (!OugaFlashPlanner::build(m_plan.images, device, options, &tail, &error)) {
        finish(false, error);
        return;
      }
      tail.flashCount += m_completed;
      tail.totalBytes += m_transferCommitted;
      m_plan = tail;
      m_expected = device;
      m_index = 0;
      next();
    });
    return;
  }
  if (step.kind == Step::Reprobe) {
    beginStep(step.title);
    command({"getvar", "all"}, [this](int, bool, const QString &output) {
      const Device device = parseDevice(m_serial, output);
      QString error;
      const QString slot = afterSalesSlot(device, &error);
      if (slot.isEmpty()) { endStep(false); finish(false, error); return; }
      m_expected.partitions = device.partitions;
      m_expected.sizes = device.sizes;
      m_expected.logical = device.logical;
      endStep(true);
      writeLog("判断最佳启动槽位为" + slot.toUpper() + "槽");
      beginStep("设置槽位：" + slot.toUpper());
      command({"set_active", slot}, [this, slot](int code, bool normal, const QString &output) {
        const bool success = commandSucceeded(code, normal, output);
        endStep(true);
        appendNative(output, !success);
        if (success) m_expected.slot = slot;
        ++m_index;
        next();
      });
    });
    return;
  }
  beginStep(step.title);
  if (!step.image.isEmpty()) {
    m_transferStepBytes = qMax<qint64>(0, step.bytes);
    m_transferImageBytes = QFileInfo(step.image).size();
    m_transferAccumulated = m_chunkExpected = m_chunkReported = 0;
    m_transferReported = m_transferCommitted;
    m_transferPending.clear();
    m_transferFailed = false;
  }
  auto run = [this, step] {
    if (!step.image.isEmpty() && !QFileInfo::exists(step.image)) {
      completeStep(step, false, "镜像文件不存在：" + step.image);
      return;
    }
    if (step.targetMissing && !m_expected.partitions.contains(step.target)) {
      completeStep(step, false, "无法解析目标分区：" + step.target);
      return;
    }
    command(step.arguments, [this, step](int code, bool normal, const QString &output) {
      const bool success = commandSucceeded(code, normal, output) ||
          (step.allowMissing && normal && partitionMissing(output));
      completeStep(step, success, output);
    });
  };
  run();
}
void OugaFlashService::skipSalePostActions() {
  writeLog("无法确认设备已进入FastbootD模式，跳过清除数据和自动重启操作");
  m_plan.steps.resize(m_index);
  next();
}
void OugaFlashService::skipSaleModem(const QString &message) {
  writeLog(message);
  while (m_index < m_plan.steps.size() && m_plan.steps[m_index].kind != Step::PostActions)
    ++m_index;
  next();
}
void OugaFlashService::completeStep(const Step &step, bool success, const QString &output) {
  const bool sales = m_plan.options.mode == FlashMode::AfterSalesBootloader ||
                     m_plan.options.mode == FlashMode::AfterSalesFastbootd;
  // Sale cleanup calls do not inspect the command result before LogStepEndOk.
  const bool saleCleanup = sales &&
      (step.target == "frp" || step.arguments == QStringList{"reboot"});
  endStep(saleCleanup || success);
  if (!success) appendNative(output, true);
  else if (step.image.isEmpty() && (step.kind == Step::ModeSwitch || step.arguments.value(0) == "erase" ||
                                    step.arguments == QStringList{"reboot"}))
    appendNative(output, false);
  if (!step.image.isEmpty()) {
    ++m_completed;
    if (success) ++m_successful;
    else m_failedTargets << step.target;
    // Reference progress accounts for a completed attempt even when it failed.
    const bool saleModem = m_plan.options.mode == FlashMode::AfterSalesFastbootd &&
        (step.target == "modem_a" || step.target == "modem_b");
    if (success || !saleModem)
      m_transferCommitted = qMin(m_plan.totalBytes, m_transferCommitted + step.bytes);
    emit transferProgress(m_transferCommitted, m_plan.totalBytes);
  } else if (!success && !saleCleanup) {
    if (step.target == "userdata") {
      if (step.arguments.value(0) == "-w") m_wipe = false;
      else m_eraseUserdata = false;
    } else if (step.target == "metadata") m_eraseMetadata = false;
    else if (step.target != "post-actions" && step.target != "sale-modem") m_phaseFailed = true;
  }
  if (step.target == "misc") m_misc = success;
  if (!success && step.kind == Step::ModeSwitch && step.target == "post-actions") {
    skipSalePostActions();
    return;
  }
  if (!success && step.kind == Step::ModeSwitch && step.target == "sale-modem") {
    skipSaleModem("无法重启到Fastboot模式");
    return;
  }
  if (!success && !step.continueOnFailure) {
    finish(false, "步骤失败：" + step.title);
    return;
  }
  if (step.kind == Step::ModeSwitch) {
    const bool salePostActions = step.target == "post-actions";
    const bool saleModem = step.target == "sale-modem";
    auto skipModem = [this] {
      skipSaleModem("等待Fastboot设备连接超时，跳过modem分区刷写");
    };
    probeStable(step.userspace, [this, salePostActions, saleModem, skipModem](const Device &device) {
      QString error;
      if (!compatible(device, false, &error)) {
        if (salePostActions) skipSalePostActions();
        else if (saleModem) skipModem();
        else finish(false, error);
        return;
      }
      m_expected = device;
      ++m_index;
      next();
    }, salePostActions ? std::function<void()>([this] { skipSalePostActions(); })
                       : saleModem ? std::function<void()>(skipModem)
                                   : std::function<void()>());
    return;
  }
  if (success && step.arguments.value(0) == "set_active")
    m_expected.slot = step.arguments.value(1);
  if (success && step.arguments.value(0) == "create-logical-partition") {
    m_expected.sizes[step.target] = step.arguments.value(2).toULongLong();
    m_expected.partitions.insert(step.target);
    m_expected.logical.insert(step.target);
  }
  if (success && step.arguments.value(0) == "delete-logical-partition") {
    m_expected.sizes.remove(step.target);
    m_expected.partitions.remove(step.target);
    m_expected.logical.remove(step.target);
  }
  if (step.arguments.value(0) == "-w" || step.target == "misc" ||
      (step.target == "metadata" && m_expected.platform == Platform::MediaTek)) {
    const bool data = m_expected.platform == Platform::MediaTek
        ? m_eraseUserdata && m_eraseMetadata : m_wipe || (m_eraseUserdata && m_eraseMetadata);
    m_phaseFailed |= !data || !m_misc;
  }
  ++m_index;
  if (m_fullPackage && step.arguments == QStringList{"reboot"}) {
    // The owned command has exited; never terminate another application's
    // Fastboot process while reproducing the reference's three-second finish.
    beginStep("结束fastboot.exe进程");
    disconnect(&m_wait, nullptr, this, nullptr);
    connect(&m_wait, &QTimer::timeout, this, [this] {
      endStep(true);
      next();
    });
    m_wait.start(qMax(0, 3000 * m_timing.waitScale));
    return;
  }
  const quint64 generation = m_generation;
  QTimer::singleShot(0, this, [this, generation] {
    if (generation == m_generation) next();
  });
}

void OugaFlashService::cleanCowPartitions() {
  command({"getvar", "all"}, [this](int code, bool normal, const QString &output) {
    if (!commandSucceeded(code, normal, output)) {
      writeLog("无法获取设备变量信息...");
      appendNative(output, true);
      completeCowCleanup(false);
      return;
    }
    QStringList names;
    const QRegularExpression partition("partition-size:([^:]+):");
    for (const QString &line : output.split('\n')) {
      if (!line.contains("partition-size:") ||
          !line.contains("cow", Qt::CaseInsensitive)) continue;
      const auto match = partition.match(line);
      const QString name = match.captured(1).trimmed();
      if (match.hasMatch() && !names.contains(name)) names << name;
    }
    deleteCowPartitions(names, 0);
  });
}
void OugaFlashService::deleteCowPartitions(const QStringList &names, int index) {
  if (index == names.size()) {
    completeCowCleanup(true);
    return;
  }
  const QString name = names[index];
  command({"delete-logical-partition", name},
          [this, names, index, name](int code, bool normal, const QString &output) {
    const bool missing = output.contains("not found", Qt::CaseInsensitive) ||
        output.contains("does not exist", Qt::CaseInsensitive) ||
        output.contains("no such partition", Qt::CaseInsensitive) ||
        output.contains("unknown partition", Qt::CaseInsensitive);
    if (!commandSucceeded(code, normal, output) && !missing) {
      appendNative(output, true);
      completeCowCleanup(false);
      return;
    }
    m_expected.partitions.remove(name);
    m_expected.sizes.remove(name);
    m_expected.logical.remove(name);
    deleteCowPartitions(names, index + 1);
  });
}
void OugaFlashService::completeCowCleanup(bool success) {
  endStep(success);
  const bool sales = m_plan.options.mode == FlashMode::AfterSalesBootloader ||
                     m_plan.options.mode == FlashMode::AfterSalesFastbootd;
  if (!success && !sales) {
    finish(false, "解析或清理COW快照分区失败");
    return;
  }
  beginStep("解析分区表");
  endStep(true);
  if (sales) {
    // Both sale branches wait one second before reading/flashing the table.
    disconnect(&m_wait, nullptr, this, nullptr);
    connect(&m_wait, &QTimer::timeout, this, [this] {
      ++m_index;
      next();
    });
    m_wait.start(qMax(0, 1000 * m_timing.waitScale));
  } else {
    ++m_index;
    next();
  }
}
void OugaFlashService::clearData() {
  auto results = std::make_shared<QVector<bool>>();
  auto erase = std::make_shared<std::function<void(int)>>();
  std::weak_ptr<std::function<void(int)>> weakErase = erase;
  *erase = [this, results, weakErase](int index) {
    const auto erase = weakErase.lock();
    if (!erase) return;
    if (m_stop) { finish(false, "任务已按用户请求停止."); return; }
    if (index < 2) {
      command({"erase", index == 0 ? "userdata" : "metadata"}, [this, results, erase, index](int code, bool normal, const QString &out) {
        const bool okay = commandSucceeded(code, normal, out);
        results->append(okay);
        appendNative(out, !okay);
        (*erase)(index + 1);
      });
      return;
    }
    m_eraseUserdata = results->value(0);
    m_eraseMetadata = results->value(1);
    const bool sales = m_plan.options.mode == FlashMode::AfterSalesBootloader ||
                       m_plan.options.mode == FlashMode::AfterSalesFastbootd;
    auto complete = [this, sales](bool okay) {
      m_phaseFailed |= !sales && !okay;
      endStep(sales || okay);
      ++m_index;
      next();
    };
    if (!sales && m_expected.platform == Platform::MediaTek) {
      complete(m_eraseUserdata && m_eraseMetadata);
      return;
    }
    command({"-w"}, [this, complete](int code, bool normal, const QString &out) {
      m_wipe = commandSucceeded(code, normal, out);
      appendNative(out, !m_wipe);
      const bool data = m_wipe || (m_eraseUserdata && m_eraseMetadata);
      const Partition misc = m_plan.options.miscImage;
      if (misc.path.isEmpty() || !QFileInfo::exists(misc.path)) { complete(data); return; }
      command({"flash", "misc", misc.path}, [this, complete, data](int c, bool n, const QString &out) {
        m_misc = commandSucceeded(c, n, out);
        appendNative(out, !m_misc);
        complete(data && m_misc);
      });
    });
  };
  (*erase)(0);
}
