#include "ougapreparation.h"
#include "deviceoperationlease.h"
#include "ougapayloadextractor.h"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include <QThread>
#include <QRegularExpression>
#include <QLocale>
#include <QUuid>
#include <QtConcurrent>
OugaPreparation::OugaPreparation(QObject *parent) : QObject(parent) {
  connect(&m_payloadProcess, &OugaPayloadProcess::output, this,
          &OugaPreparation::consumePayloadOutput);
  connect(&m_payloadProcess, &OugaPayloadProcess::finished, this,
          [this](int code, bool normal, const QString &error) {
    parsePayloadCounter();
    auto callback = std::move(m_callback);
    m_callback = {};
    if (m_cancel) {
      end(false, "准备已取消；保留生成的文件，不自动删除");
      return;
    }
    if (callback)
      callback(error.isEmpty() && Ouga::commandSucceeded(code, normal, m_output),
               error.isEmpty() ? m_output : error);
  });
  connect(&m_process, &QProcess::readyReadStandardOutput, this, [this] {
    consumeOutput(m_process.readAllStandardOutput(), false);
  });
  connect(&m_process, &QProcess::readyReadStandardError, this, [this] {
    consumeOutput(m_process.readAllStandardError(), true);
  });
  connect(
      &m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
      this, [this](int c, QProcess::ExitStatus status) {
        auto cb = std::move(m_callback);
        m_callback = {};
        consumeOutput(m_process.readAllStandardOutput(), false, true);
        consumeOutput(m_process.readAllStandardError(), true, true);
        m_reportArchiveProgress = false;
        if (m_cancel) {
          end(false, "准备已取消；保留生成的文件，不自动删除");
          return;
        }
        // AOSP lpmake prints valid --help text with exit status 1.
        // This exception is only for a read-only capability query; actual
        // generation and device commands keep strict failure handling.
        const bool lpmakeHelp =
            c == 1 && m_process.arguments() == QStringList{"--help"} &&
            m_output.contains("command-line tool for creating Android "
                              "Logical Partition images.") &&
            m_output.contains("Usage:");
        if (cb)
          cb(Ouga::commandSucceeded(lpmakeHelp ? 0 : c,
                                    status == QProcess::NormalExit, m_output),
             m_output);
      });
  connect(&m_process, &QProcess::errorOccurred, this,
          [this](QProcess::ProcessError e) {
            if (e == QProcess::FailedToStart) {
              auto cb = std::move(m_callback);
              m_callback = {};
              if (cb)
                cb(false, m_process.errorString());
            }
          });
}
bool OugaPreparation::begin() {
  if (m_busy)
    return false;
  m_busy = true;
  m_cancel = false;
  m_abort = false;
  emit busyChanged(true);
  return true;
}
void OugaPreparation::end(bool ok, const QString &s) {
  if (m_payloadActive) {
    for (auto i = m_payloadRows.cbegin(); i != m_payloadRows.cend(); ++i)
      if (i.value() == 1)
        emit payloadPartitionFinished(i.key(), false);
    m_payloadActive = false;
  }
  m_busy = false;
  m_quietOutput = false;
  m_reportArchiveProgress = false;
  m_process.setStandardOutputFile(
      QString()); // Also reset redirection after a cancelled ARB read.
  if (m_lease) {
    DeviceOperationLease::release(this);
    m_lease = false;
  }
  emit busyChanged(false);
  emit finished(ok, s);
}
void OugaPreparation::cancel() {
  m_cancel = true;
  m_abort = true;
  if (m_payloadActive) m_payloadProcess.cancel();
  if (m_process.state() != QProcess::NotRunning)
    m_process.kill();
}
void OugaPreparation::run(const QString &tool, const QStringList &args,
                          const QString &cwd,
                          std::function<void(bool, const QString &)> done,
                          bool reportArchiveProgress, bool quietOutput) {
  m_quietOutput = quietOutput || reportArchiveProgress;
  m_output.clear();
  m_reportArchiveProgress = reportArchiveProgress;
  for (auto &stream : m_archiveProgressStreams)
    stream = ArchiveProgressState();
  m_lastArchiveProgress = -1;
  m_callback = std::move(done);
  m_process.setWorkingDirectory(cwd);
  if (m_reportArchiveProgress)
    publishArchiveProgress(0);
  if (m_cancel) {
    m_callback = {};
    end(false, "准备已取消；保留输出文件");
    return;
  }
  m_process.start(tool, args);
}
void OugaPreparation::publishArchiveProgress(int percent) {
  // A tool's 100% is not completion: output safety checks must also succeed.
  percent = qMin(percent, 99);
  if (!m_cancel && percent > m_lastArchiveProgress) {
    m_lastArchiveProgress = percent;
    emit archiveProgress(percent);
  }
}
void OugaPreparation::consumeOutput(const QByteArray &bytes, bool standardError,
                                    bool final) {
  const QString text = QString::fromLocal8Bit(bytes);
  m_output += text;
  // Retain raw diagnostics for failure, without listing/banner/terminal noise.
  if (!text.isEmpty() && !m_quietOutput)
    emit log(text);
  if (!m_reportArchiveProgress)
    return;
  auto &stream = m_archiveProgressStreams[standardError ? 1 : 0];
  // A bounded streaming parser handles CR/backspace updates and tokens split
  // across reads, without combining stdout and stderr or retaining long lines.
  for (const char c : bytes) {
    if (c == '\r' || c == '\n' || c == '\b') {
      if (stream.phase == ArchiveProgressState::Percent)
        publishArchiveProgress(stream.percent);
      stream = ArchiveProgressState();
      continue;
    }
    switch (stream.phase) {
    case ArchiveProgressState::LeadingSpace:
      if (c == ' ' || c == '\t')
        break;
      if (c >= '0' && c <= '9') {
        stream.phase = ArchiveProgressState::Digits;
        stream.percent = c - '0';
        stream.digits = 1;
      } else
        stream.phase = ArchiveProgressState::Ignore;
      break;
    case ArchiveProgressState::Digits:
      if (c >= '0' && c <= '9' && stream.digits < 3) {
        stream.percent = stream.percent * 10 + c - '0';
        ++stream.digits;
      } else if (c == '%' && stream.percent <= 100)
        stream.phase = ArchiveProgressState::Percent;
      else
        stream.phase = ArchiveProgressState::Ignore;
      break;
    case ArchiveProgressState::Percent:
      if (c == ' ' || c == '\t')
        publishArchiveProgress(stream.percent);
      stream.phase = ArchiveProgressState::Ignore;
      break;
    case ArchiveProgressState::Ignore:
      break;
    }
  }
  if (final && stream.phase == ArchiveProgressState::Percent)
    publishArchiveProgress(stream.percent);
}
void OugaPreparation::startPayloadRow(const QString &name) {
  if (m_payloadRows.value(name) == 0) {
    m_payloadRows[name] = 1;
    emit payloadPartitionStarted(name);
  }
}
void OugaPreparation::parsePayloadCounter() {
  // Read completed/total operation counts, NOT rounded terminal percentages,
  // output-file lengths, elapsed time, or preallocated/sparse extents.
  static const QRegularExpression row(
      R"(^([A-Za-z0-9_]+) +[0-9]{1,3}%\|.*\| *([0-9]+) */ *([0-9]+)(?: |$))");
  const auto match = row.match(QString::fromUtf8(m_payloadLine));
  if (!match.hasMatch() || m_cancel) return;
  const QString name = match.captured(1);
  bool a = false, b = false;
  const quint64 done = match.captured(2).toULongLong(&a);
  const quint64 total = match.captured(3).toULongLong(&b);
  if (!a || !b || !total || !m_payloadOperations.contains(name) ||
      total != m_payloadOperations.value(name) || done > total ||
      done < m_payloadDone.value(name)) return;
  startPayloadRow(name);
  m_payloadDone[name] = done;
  quint64 all = 0, completed = 0;
  for (auto i = m_payloadOperations.cbegin(); i != m_payloadOperations.cend(); ++i) {
    all += i.value();
    completed += m_payloadDone.value(i.key());
  }
  const int percent = all ? qMin(99, int(completed * 100 / all)) : 0;
  if (percent > m_lastPayloadProgress) {
    m_lastPayloadProgress = percent;
    emit payloadProgress(percent);
  }
}
void OugaPreparation::consumePayloadOutput(const QByteArray &bytes) {
  m_output += QString::fromUtf8(bytes);
  for (const char c : bytes) {
    switch (m_terminalState) {
    case Escape:
      m_terminalState = c == '[' ? Csi : c == ']' ? Osc : Text;
      break;
    case Csi:
      if (c >= '@' && c <= '~') {
        m_terminalState = Text;
        if (c != 'm') { parsePayloadCounter(); m_payloadLine.clear(); }
      }
      break;
    case Osc:
      if (c == '\a') m_terminalState = Text;
      else if (c == '\x1b') m_terminalState = OscEscape;
      break;
    case OscEscape:
      m_terminalState = c == '\\' ? Text : Osc;
      break;
    case Text:
      if (c == '\x1b') m_terminalState = Escape;
      else if (c == '\r' || c == '\n') {
        parsePayloadCounter(); m_payloadLine.clear();
      } else if (c == '\b') {
        if (!m_payloadLine.isEmpty()) m_payloadLine.chop(1);
      } else if (m_payloadLine.size() < 4096) {
        m_payloadLine += c;
        if (c == '[') parsePayloadCounter();
      }
      break;
    }
  }
}
void OugaPreparation::work(std::function<QString()> job,
                           std::function<void()> done) {
  auto w = new QFutureWatcher<QString>(this);
  connect(w, &QFutureWatcher<QString>::finished, this, [this, w, done] {
    QString e = w->result();
    w->deleteLater();
    if (m_cancel)
      end(false, "准备已取消；保留输出文件");
    else if (!e.isEmpty())
      end(false, e);
    else
      done();
  });
  w->setFuture(QtConcurrent::run(std::move(job)));
}
bool OugaPreparation::newOutput(const QString &source, const QString &output,
                                QString *error) {
  QFileInfo info(output);
  QString parent = info.dir().canonicalPath(),
          src = QFileInfo(source).isDir()
                    ? QFileInfo(source).canonicalFilePath()
                    : QFileInfo(source).dir().canonicalPath();
  QString clean =
      QDir::cleanPath(QDir::fromNativeSeparators(info.absoluteFilePath()));
  if (parent.isEmpty() || src.isEmpty() ||
      clean.compare(src, Qt::CaseInsensitive) == 0 ||
      clean.startsWith(src + '/', Qt::CaseInsensitive) || info.isSymLink() ||
      info.isJunction() ||
      (info.exists() &&
       !QDir(output)
            .entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden |
                       QDir::System)
            .isEmpty())) {
    *error = "输出必须是源目录外的独立空目录，父目录必须存在，不能是链接";
    return false;
  }
  QString actual = QDir(parent).filePath(info.fileName());
  if (actual.compare(src, Qt::CaseInsensitive) == 0 ||
      actual.startsWith(src + '/', Qt::CaseInsensitive)) {
    *error = "输出实际路径落入源目录";
    return false;
  }
  if (!QDir().mkpath(output)) {
    *error = "无法创建输出目录";
    return false;
  }
  return true;
}
void OugaPreparation::scan(const QString &directory) {
  if (!begin())
    return;
  auto images = std::make_shared<QVector<Ouga::Partition>>();
  work(
      [images, directory] {
        QString error;
        *images = OugaPackage::scan(directory, &error);
        return error;
      },
      [this, images, directory] {
        emit prepared(*images, directory);
        end(true, "镜像扫描与 SHA-256 校验完成");
      });
}
void OugaPreparation::listPayload(const QString &tool, const QString &file) {
  if (!begin())
    return;
  auto entries = std::make_shared<QVector<OugaPayloadEntry>>();
  work(
      [entries, file] {
        QString e;
        bool delta;
        OugaPackage::payloadManifest(file, entries.get(), &delta, &e);
        return e;
      },
      [this, entries, tool, file] {
        const QString workdir =
            QDir(QDir::tempPath())
                .filePath("OrangeTools-payload-list-" +
                          QUuid::createUuid().toString(QUuid::Id128));
        if (!QDir().mkpath(workdir)) {
          end(false, "无法创建独立 Payload 列表工作目录");
          return;
        }
        emit log("Payload 列表工作目录（保留，不自动清理）：" + workdir);
        run(tool, {"--list", file}, workdir,
            [this, entries](bool ok, const QString &out) {
              if (!ok) {
                end(false, "Payload 列表失败：" + out);
                return;
              }
              QStringList names;
              for (const auto &i : *entries)
                names << i.name;
              emit payloadListed(names);
              end(true, "Payload 列表已读取（尚未提取/刷写）");
            });
      });
}
void OugaPreparation::payload(const QString &tool, const QString &file,
                              const QString &output,
                              const QStringList &selected,
                              const QString &oldDirectory) {
  if (!begin())
    return;
  auto entries = std::make_shared<QVector<OugaPayloadEntry>>();
  auto delta = std::make_shared<bool>(false);
  auto layout = std::make_shared<OugaPayloadLayout>();
  auto native = std::make_shared<bool>(false);
  work(
      [entries, delta, layout, native, file, output, selected, oldDirectory] {
        QString e;
        if (!newOutput(file, output, &e))
          return e;
        if (!OugaPackage::payloadManifest(file, entries.get(), delta.get(), &e,
                                          layout.get()))
          return e;
        QSet<QString> names;
        for (const auto &p : *entries)
          names.insert(p.name);
        for (const QString &n : selected)
          if (!names.contains(n))
            return QString("Payload 没有分区：") + n;
        *delta = false;
        for (const auto &p : *entries) {
          if ((!selected.isEmpty() && !selected.contains(p.name)) ||
              !p.requiresOldImage)
            continue;
          *delta = true;
          if (!p.oldSize || p.oldHash.size() != 32)
            return QString("增量 Payload 没有可信旧镜像信息，拒绝提取：") + p.name;
          const QString f = QDir(oldDirectory).filePath(p.name + ".img");
          if (oldDirectory.isEmpty() ||
              !OugaPackage::inside(oldDirectory, f) ||
              quint64(QFileInfo(f).size()) != p.oldSize ||
              OugaPackage::digest(f, &e) != p.oldHash)
            return QString("增量 Payload 缺少匹配的旧镜像：") + p.name;
        }
        *native = !*delta &&
            OugaPayloadExtractor::supported(*entries, selected, *layout);
        return QString();
      },
      [this, entries, delta, layout, native, tool, file, output, selected,
       oldDirectory] {
        if (*native) {
          nativePayload(file, *layout, *entries, output, selected);
          return;
        }
        if (tool.isEmpty() || layout->base != 0) {
          end(false, "此 Payload 需要 payload.exe 提取（增量或不支持的操作）");
          return;
        }
        // Match CPU parallelism without unbounded disk contention on large hosts.
        const int workers = qBound(2, QThread::idealThreadCount(), 8);
        QStringList args = {"--out", output, "--workers", QString::number(workers)};
        if (*delta)
          args << "--diff" << "--old" << oldDirectory;
        if (!selected.isEmpty())
          args << "--partitions" << selected.join(',');
        args << file;
        m_payloadActive = true;
        m_output.clear();
        m_payloadOperations.clear();
        m_payloadDone.clear();
        m_payloadRows.clear();
        m_payloadLine.clear();
        m_terminalState = Text;
        m_lastPayloadProgress = 0;
        QStringList names;
        for (const auto &p : *entries)
          if (selected.isEmpty() || selected.contains(p.name)) {
            m_payloadOperations.insert(p.name, p.operations);
            names << p.name;
          }
        emit payloadListed(names);
        emit payloadProgress(0);
        if (m_cancel) { end(false, "准备已取消；保留输出文件"); return; }
        m_callback = [this, entries, output, selected](bool ok, const QString &out) {
              if (!ok) {
                end(false, "Payload 提取失败（未刷写）：" + out);
                return;
              }
              for (const auto &p : *entries)
                if (selected.isEmpty() || selected.contains(p.name))
                  startPayloadRow(p.name);
              auto images = std::make_shared<QVector<Ouga::Partition>>();
              work(
                  [this, entries, output, selected, images] {
                    QString e;
                    // scan() validates mappings, sparse ranges and hashes once.
                    // Reuse only this invocation's results for manifest comparison,
                    // never a persistent cache or a size/mtime shortcut before flash.
                    *images = OugaPackage::scan(output, &e);
                    if (!e.isEmpty())
                      return e;
                    QMap<QString, QByteArray> hashes;
                    for (const auto &image : *images)
                      hashes.insert(image.path, image.sha256);
                    for (const auto &p : *entries) {
                      if (!selected.isEmpty() && !selected.contains(p.name))
                        continue;
                      const QString f = QDir(output).filePath(p.name + ".img");
                      if (!OugaPackage::inside(output, f) ||
                          quint64(QFileInfo(f).size()) != p.size)
                        return QString("Payload 输出缺失/长度/SHA-256 不符：") +
                               p.name;
                      const QString canonical = QFileInfo(f).canonicalFilePath();
                      // Excluded partitions (e.g. misc/FRP) are still verified,
                      // but must not be added to the flashable image list.
                      const QByteArray hash = hashes.contains(canonical)
                          ? hashes.value(canonical) : OugaPackage::digest(f, &e);
                      if (hash != p.hash)
                        return QString("Payload 输出缺失/长度/SHA-256 不符：") +
                               p.name;
                      QMetaObject::invokeMethod(this, [this, name = p.name] {
                        if (!m_cancel && m_payloadActive) {
                          m_payloadRows[name] = 2;
                          emit payloadPartitionFinished(name, true);
                        }
                      }, Qt::QueuedConnection);
                    }
                    return e;
                  },
                  [this, images, output] {
                    emit payloadProgress(100);
                    emit prepared(*images, output);
                    end(true, "Payload 已提取并重新校验；尚未写入设备");
                  });
            };
        m_payloadProcess.start(tool, args, output);
      });
}
void OugaPreparation::nativePayload(const QString &file,
                                    const OugaPayloadLayout &layout,
                                    const QVector<OugaPayloadEntry> &entries,
                                    const QString &output,
                                    const QStringList &selected) {
  QVector<OugaPayloadEntry> chosen;
  QStringList names;
  for (const auto &p : entries)
    if (selected.isEmpty() || selected.contains(p.name)) {
      chosen << p;
      names << p.name;
    }
  m_payloadActive = true;
  m_payloadRows.clear();
  m_lastPayloadProgress = 0;
  emit payloadListed(names);
  emit payloadProgress(0);
  if (m_cancel) {
    end(false, "准备已取消；保留输出文件");
    return;
  }
  auto images = std::make_shared<QVector<Ouga::Partition>>();
  auto last = std::make_shared<std::atomic_int>(0);
  // Same parallelism as VioletToolBox: Environment.ProcessorCount.
  const int workers = qMax(1, QThread::idealThreadCount());
  work(
      [this, file, layout, chosen, output, images, last, workers] {
        OugaPayloadExtractor::Callbacks callbacks;
        callbacks.started = [this](const QString &name) {
          QMetaObject::invokeMethod(this, [this, name] {
            if (m_payloadActive && !m_cancel)
              startPayloadRow(name);
          }, Qt::QueuedConnection);
        };
        callbacks.finished = [this](const QString &name) {
          QMetaObject::invokeMethod(this, [this, name] {
            if (m_payloadActive && !m_cancel) {
              m_payloadRows[name] = 2;
              emit payloadPartitionFinished(name, true);
            }
          }, Qt::QueuedConnection);
        };
        callbacks.progress = [this, last](quint64 done, quint64 total) {
          // Written bytes, not time; 100% only after the final checks below.
          const int percent =
              total ? int(qMin<quint64>(99, done * 100 / total)) : 0;
          int previous = last->load();
          while (percent > previous &&
                 !last->compare_exchange_weak(previous, percent)) {
          }
          if (percent > previous)
            QMetaObject::invokeMethod(this, [this, percent] {
              if (m_payloadActive && !m_cancel &&
                  percent > m_lastPayloadProgress) {
                m_lastPayloadProgress = percent;
                emit payloadProgress(percent);
              }
            }, Qt::QueuedConnection);
        };
        QString e = OugaPayloadExtractor::extract(file, layout, chosen, output,
                                                  workers, m_abort, callbacks);
        if (!e.isEmpty())
          return e;
        // Every byte came from hash-verified operations that tile the image
        // exactly, so the manifest digest is this output's expected SHA-256;
        // flashing still re-hashes the file before every write.
        QMap<QString, QByteArray> known;
        for (const auto &p : chosen) {
          const QString f = QDir(output).filePath(p.name + ".img");
          if (!OugaPackage::inside(output, f) ||
              quint64(QFileInfo(f).size()) != p.size)
            return QString("Payload 输出缺失/长度不符：") + p.name;
          known.insert(QFileInfo(f).canonicalFilePath(), p.hash);
        }
        *images = OugaPackage::scan(output, &e, known);
        return e;
      },
      [this, images, output] {
        emit payloadProgress(100);
        emit prepared(*images, output);
        end(true, "Payload 已提取（逐操作 SHA-256 校验）；尚未写入设备");
      });
}
void OugaPreparation::extractArchive(const QString &tool, const QString &file,
                                     const QString &output, bool scanImages) {
  if (!begin())
    return;
  QString e;
  if (!newOutput(file, output, &e)) {
    end(false, e);
    return;
  }
  run(tool, {"l", "-slt", "-ba", "-sccUTF-8", file}, output,
      [this, tool, file, output, scanImages](bool ok, const QString &listing) {
        QString e;
        if (!ok || !OugaPackage::safeArchiveListing(listing, &e)) {
          end(false, "解压前校验失败：" + (ok ? e : listing));
          return;
        }
        QStringList arguments = {"x", "-y", "-aos", "-sccUTF-8", "-bsp1", "-bso2", "-o" + output};
        if (!scanImages) {
          QStringList payloads;
          quint64 payloadSize = 0;
          for (const QString &block : listing.split(QRegularExpression("\\r?\\n\\r?\\n"))) {
            QString path;
            quint64 size = 0;
            for (const QString &line : block.split('\n')) {
              if (line.startsWith("Path = ")) path = line.mid(7).trimmed();
              if (line.startsWith("Size = ")) size = line.mid(7).trimmed().toULongLong();
            }
            if (QFileInfo(QDir::fromNativeSeparators(path)).fileName().compare("payload.bin", Qt::CaseInsensitive) == 0) {
              payloads << path;
              payloadSize = size;
            }
          }
          if (payloads.size() != 1) {
            end(false, payloads.isEmpty() ? "ZIP中没有payload.bin" : "ZIP中有多个payload.bin，请消除歧义");
            return;
          }
          emit log("正在预解压 payload.bin...");
          emit log(QString("正在从 ZIP 解压 payload.bin (%1)，可能需要几分钟...")
                       .arg(QLocale().formattedDataSize(payloadSize)));
          // Exact member match, no wildcard/option interpretation of package paths.
          arguments << "-spd" << ("-i!" + payloads.first());
        } else {
          emit log("正在解压售后包...");
        }
        arguments << file;
        run(tool, arguments,
            output,
            [this, output, scanImages](bool success, const QString &out) {
              if (!success) {
                end(false, "解压失败：" + out);
                return;
              }
              auto images = std::make_shared<QVector<Ouga::Partition>>();
              auto root = std::make_shared<QString>(output);
              work(
                  [images, root, output, scanImages] {
                    QDirIterator it(output,
                                    QDir::AllEntries | QDir::NoDotAndDotDot |
                                        QDir::Hidden | QDir::System,
                                    QDirIterator::Subdirectories);
                    while (it.hasNext()) {
                      QFileInfo f(it.next());
                      if (f.isSymLink() || f.isJunction() ||
                          !OugaPackage::inside(output, f.absoluteFilePath()))
                        return QString("解压输出包含链接/越界");
                    }
                    QStringList dirs = QDir(output).entryList(
                        QDir::Dirs | QDir::NoDotAndDotDot);
                    if (dirs.size() == 1 &&
                        QDir(QDir(output).filePath(dirs[0] + "/IMAGES"))
                            .exists())
                      *root = QDir(output).filePath(dirs[0]);
                    if (!scanImages)
                      return QString();
                    QString e;
                    *images = OugaPackage::scan(*root, &e);
                    return e;
                  },
                  [this, images, root, scanImages] {
                    m_lastArchiveProgress = 100;
                    emit archiveProgress(100);
                    if (scanImages)
                      emit prepared(*images, *root);
                    emit archiveExtracted(*root);
                    if (!scanImages) emit log("payload.bin 解压完成");
                    end(true, scanImages
                                  ? "解压及重新扫描成功，尚未写入设备"
                                  : "解压及路径校验成功，尚未提取或写入设备");
                  });
            }, true);
      }, false, true);
}
void OugaPreparation::makeSuper(const QString &tool, const QString &directory,
                                const QString &output) {
  if (!begin())
    return;
  QString e;
  if (!newOutput(directory, output, &e)) {
    end(false, e);
    return;
  }
  auto args = std::make_shared<QStringList>();
  auto merged = std::make_shared<QSet<QString>>();
  auto images = std::make_shared<QVector<Ouga::Partition>>();
  QString target = QDir(output).filePath("super.img");
  work(
      [args, merged, images, directory, target] {
        QString e;
        if (!OugaPackage::lpmakeArguments(directory, target, args.get(),
                                          merged.get(), &e))
          return e;
        *images = OugaPackage::scan(directory, &e);
        return e;
      },
      [this, tool, args, merged, images, output, directory, target] {
        run(tool, {"--help"}, output,
            [this, tool, args, merged, images, output, directory,
             target](bool ok, const QString &help) {
              if (!ok) {
                end(false, "无法查询 lpmake 参数能力");
                return;
              }
              for (const QString &a : *args)
                if (a.startsWith("--") && !help.contains(a)) {
                  end(false, "lpmake 不支持所需参数：" + a);
                  return;
                }
              work(
                  [args, output] {
                    QString e;
                    QMap<QString, quint64> allocation;
                    for (int i = 0; i < args->size() - 1; ++i)
                      if (args->at(i) == "--partition") {
                        auto f = args->at(i + 1).split(':');
                        allocation[f[0]] = f.value(2).toULongLong();
                      }
                    for (int i = 0; i < args->size() - 1; ++i)
                      if (args->at(i) == "--image") {
                        QString value = args->at(i + 1);
                        int eq = value.indexOf('=');
                        QString n = value.left(eq), src = value.mid(eq + 1),
                                dst = QDir(output).filePath(n + ".raw.img");
                        if (!OugaPackage::toRaw(src, dst, allocation[n], &e))
                          return e;
                        // All files are in the independent output directory.
                        // Pass validated ASCII basenames to the old tool;
                        // QProcess supplies its Unicode working directory.
                        (*args)[i + 1] = n + "=" + QFileInfo(dst).fileName();
                      } else if (args->at(i) == "--output") {
                        (*args)[i + 1] = QFileInfo(args->at(i + 1)).fileName();
                      }
                    return QString();
                  },
                  [this, tool, args, merged, images, output, directory,
                   target] {
                    run(tool, *args, output,
                        [this, merged, images, directory,
                         target](bool success, const QString &out) {
                          if (!success) {
                            end(false, "Super 生成失败：" + out);
                            return;
                          }
                          work(
                              [merged, images, target] {
                                QString e;
                                Ouga::Partition image;
                                if (!OugaPackage::inspect("super", target,
                                                          &image, &e))
                                  return e;
                                if (image.merged != *merged)
                                  return QString(
                                      "生成的 Super 分区清单与定义不一致");
                                for (int i = images->size() - 1; i >= 0; --i)
                                  if (Ouga::baseName(images->at(i).name) ==
                                      "super")
                                    images->removeAt(i);
                                images->append(image);
                                return QString();
                              },
                              [this, images, directory] {
                                emit prepared(*images, directory);
                                end(true, "Super 已生成，源包/源 JSON 未修改");
                              });
                        });
                  });
            });
      });
}
void OugaPreparation::discoverAdb(const QString &adb) {
  if (!begin())
    return;
  QString error;
  if (!DeviceOperationLease::acquire(this, &error)) {
    end(false, error);
    return;
  }
  m_lease = true;
  run(adb, {"devices"}, QFileInfo(adb).absolutePath(),
      [this](bool ok, const QString &output) {
        if (!ok) {
          end(false, "ADB设备列表读取失败");
          return;
        }
        QStringList serials;
        for (const QString &line : output.split('\n')) {
          QStringList fields = line.simplified().split(' ');
          if (fields.size() == 2 && fields[1] == "device" &&
              !serials.contains(fields[0]))
            serials << fields[0];
        }
        emit adbDevicesFound(serials);
        end(true, serials.isEmpty() ? "没有已授权且在线的ADB设备"
                                    : "已读取授权ADB设备列表");
      });
}
void OugaPreparation::readCurrentArb(const QString &adb, const QString &serial,
                                     const QString &output) {
  if (!begin())
    return;
  QString error;
  if (serial.trimmed().isEmpty() ||
      !DeviceOperationLease::acquire(this, &error)) {
    end(false, error.isEmpty() ? "必须明确指定已授权 ADB 序列号" : error);
    return;
  }
  m_lease = true;
  if (QFileInfo::exists(output) || !QFileInfo(output).dir().exists()) {
    end(false, "请选择不存在的独立输出文件");
    return;
  }
  run(adb, {"-s", serial, "shell", "getprop", "ro.boot.slot_suffix"},
      QFileInfo(output).absolutePath(),
      [this, adb, serial, output](bool ok, const QString &out) {
        QString slot = out.trimmed();
        if (!ok || (slot != "_a" && slot != "_b")) {
          end(false, "ADB 当前槽未知；不主动提权或解锁");
          return;
        }
        m_process.setStandardOutputFile(output, QIODevice::WriteOnly |
                                                    QIODevice::NewOnly);
        run(adb,
            {"-s", serial, "exec-out", "cat",
             "/dev/block/by-name/xbl_config" + slot},
            QFileInfo(output).absolutePath(),
            [this, output](bool success, const QString &text) {
              m_process.setStandardOutputFile(QString());
              quint32 index = 0;
              QString error;
              if (!success || !OugaPackage::readArb(output, &index, &error)) {
                end(false, "当前镜像读取/解析失败（需要已有读取权限）：" +
                               text + error);
                return;
              }
              emit arbRead(output, index);
              end(true, "已读取镜像 ARB；这不代表硬件熔断状态");
            });
      });
}
