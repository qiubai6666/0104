#include "ougacommandrunner.h"
#include <QDir>
#include <QFileInfo>
OugaProcessRunner::OugaProcessRunner(QObject *p) : OugaCommandRunner(p) {
  // Not registered with stopAllProcesses: an in-flight flash must never be
  // killed by cleanup.
  m_process.setProcessChannelMode(QProcess::MergedChannels);
  m_readDeadline.setSingleShot(true);
  connect(&m_readDeadline, &QTimer::timeout, this, [this] {
    m_output += "\nERROR: read-only query timed out\n";
    m_process
        .kill(); // Only armed for getvar/devices, never for a write or reboot.
  });
  m_watchdog.setInterval(120000);
  m_watchdog.setSingleShot(true);
  connect(&m_process, &QProcess::readyReadStandardOutput, this, [this] {
    QString s = QString::fromLocal8Bit(m_process.readAllStandardOutput());
    m_output += s;
    emit output(s);
    m_watchdog.start();
  });
  connect(&m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
          this, [this](int c, QProcess::ExitStatus s) {
            finish(c, s == QProcess::NormalExit);
          });
  connect(&m_process, &QProcess::errorOccurred, this,
          [this](QProcess::ProcessError e) {
            if (e == QProcess::FailedToStart) {
              m_output += m_process.errorString();
              finish(-1, false);
            }
          });
  connect(&m_watchdog, &QTimer::timeout, this, &OugaCommandRunner::stalled);
}
void OugaProcessRunner::run(const QString &p, const QStringList &a,
                            int readTimeoutMs) {
  if (running()) {
    emit completed(-1, false, "执行器忙，拒绝并发命令");
    return;
  }
  m_output.clear();
  m_done = false;
  m_process.setWorkingDirectory(QFileInfo(p).absolutePath());
  m_watchdog.start();
  const QString verb = a.value(0) == "-s" ? a.value(2) : a.value(0);
  if (readTimeoutMs > 0 && (verb == "getvar" || verb == "devices"))
    m_readDeadline.start(readTimeoutMs);
  m_process.start(p, a);
}
bool OugaProcessRunner::running() const { return !m_done; }
void OugaProcessRunner::finish(int c, bool normal) {
  if (m_done)
    return;
  m_output += QString::fromLocal8Bit(m_process.readAllStandardOutput());
  m_done = true;
  m_watchdog.stop();
  m_readDeadline.stop();
  emit completed(c, normal, m_output);
}
QString OugaProcessRunner::formatToolsError(const QString &fastbootPath) {
  const QDir directory(QFileInfo(fastbootPath).absolutePath());
  for (const QString name : {"mke2fs.exe", "make_f2fs.exe", "mke2fs.conf"}) {
    const QFileInfo file(directory.filePath(name));
    if (!file.isFile() || !file.isReadable() || file.size() == 0)
      return "格式化依赖缺失/不可读：" + name;
  }
  return {};
}

QString OugaProcessRunner::bundledToolPath(const QString &resourceDirectory,
                                           const QString &key,
                                           const QString &fallback) {
  if (resourceDirectory.isEmpty())
    return fallback;
  QString relative;
  if (key == "7z")
    relative = "bin/7zip/7z.exe";
  else if (key == "lpmake")
    relative = "bin/lpmake/lpmake.exe";
  else if (key == "fastboot" || key == "adb")
    // Share the root tools with all existing device operations.
    relative = key + ".exe";
  else
    return fallback;
  const QFileInfo file(QDir(resourceDirectory).filePath(relative));
  return file.isFile() && file.isReadable() && file.size() > 0
             ? file.absoluteFilePath()
             : fallback;
}
