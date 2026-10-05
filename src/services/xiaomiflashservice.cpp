#include "xiaomiflashservice.h"
#include "deviceoperationlease.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QtMath>

QString Xiaomi::scriptName(Mode mode) {
    switch (mode) {
    case Mode::KeepData: return "flash_all_except_storage.bat";
    case Mode::WipeAndLock: return "flash_all_lock.bat";
    default: return "flash_all.bat";
    }
}
QString Xiaomi::modeName(Mode mode) {
    switch (mode) {
    case Mode::KeepData: return "保留数据刷机";
    case Mode::WipeAndLock: return "清除数据并回锁 BL";
    default: return "清除数据刷机";
    }
}

bool Xiaomi::inspectPackage(const QString &directory, Mode mode, Package *package, QString *error) {
    auto fail = [&](const QString &message) { if (error) *error = message; return false; };
    if (!package) return fail("刷机包输出无效。");
    *package = {};
    const QString trimmed = directory.trimmed();
    const QFileInfo root(trimmed);
    if (trimmed.isEmpty() || !root.isDir())
        return fail("请选择已解压的小米 Fastboot 刷机包文件夹（不是 ZIP/TGZ 压缩包）。");

    const QString scriptPath = QDir(root.absoluteFilePath()).filePath(scriptName(mode));
    QFile script(scriptPath);
    if (!QFileInfo(scriptPath).isFile() || !script.open(QIODevice::ReadOnly))
        return fail("所选模式需要 " + scriptName(mode) + "，请直接选择包含该脚本的刷机包根目录。");
    const QByteArray bytes = script.readAll();
    if (script.error() != QFile::NoError)
        return fail("无法完整读取刷机脚本。");

    Package result;
    result.mode = mode;
    result.directory = root.absoluteFilePath();
    result.script = QFileInfo(scriptPath).absoluteFilePath();
    result.scriptSha256 = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);

    // This is deliberately best-effort. VioletToolBox uses this information
    // only for progress; it does not reject a valid BAT because it contains a
    // variable, a vendor command, or an image that cannot be inspected.
    const QString scriptDirectory = QFileInfo(result.script).absolutePath();
    const QRegularExpression flash(
        R"((?i)\bfastboot(?:\.exe)?\b.*?\bflash\s+(?<partition>[^\s"']+)\s+(?<image>"[^"]+"|[^\s|&]+))");
    bool foundFlash = false;
    bool completePlan = true;
    for (QString line : QString::fromLocal8Bit(bytes).split('\n')) {
        line = line.trimmed();
        if (line.startsWith('@')) line.remove(0, 1);
        if (line.startsWith("rem ", Qt::CaseInsensitive) || line.startsWith("::") ||
            line.startsWith("echo ", Qt::CaseInsensitive)) continue;
        const auto match = flash.match(line);
        if (!match.hasMatch()) continue;
        foundFlash = true;
        QString imageToken = match.captured("image").trimmed();
        if (imageToken.size() >= 2 && imageToken.startsWith('"') && imageToken.endsWith('"'))
            imageToken = imageToken.mid(1, imageToken.size() - 2);
        QString unresolved = imageToken;
        unresolved.remove(QRegularExpression("%~dp0", QRegularExpression::CaseInsensitiveOption));
        if (unresolved.contains('%')) { completePlan = false; continue; }
        QString imagePath = imageToken;
        imagePath.replace(QRegularExpression("%~dp0", QRegularExpression::CaseInsensitiveOption),
                          scriptDirectory + QDir::separator());
        if (QDir::isRelativePath(imagePath)) imagePath = QDir(scriptDirectory).filePath(imagePath);
        const QFileInfo image(QDir::cleanPath(imagePath));
        if (!image.isFile() || image.size() <= 0) {
            completePlan = false;
            continue;
        }
        result.images.append({match.captured("partition"), image.size()});
        result.totalBytes += image.size();
    }
    result.progressPlanValid = foundFlash && completePlan && !result.images.isEmpty() && result.totalBytes > 0;
    *package = result;
    return true;
}

XiaomiFlashService::XiaomiFlashService(QObject *parent) : QObject(parent) {
    m_process.setProcessChannelMode(QProcess::MergedChannels);
    m_mismatchTimer.setSingleShot(true);
    m_mismatchTimer.setInterval(10000);
    m_statusTimer.setInterval(1000);
    connect(&m_statusTimer, &QTimer::timeout, this, [this] {
        if (m_busy) emit progressInfo(QString("%1  |  Time:%2s").arg(m_transferRate).arg(m_elapsed.elapsed() / 1000));
    });
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &XiaomiFlashService::readOutput);
    connect(&m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, &XiaomiFlashService::completeProcess);
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && m_busy)
            finish(false, "无法启动线刷脚本：" + m_process.errorString());
    });
    connect(&m_mismatchTimer, &QTimer::timeout, this, [this] {
        if (!m_busy || !m_mismatchPending) return;
        m_mismatchPending = false;
        emit warning("检测到 Missmatching，设备可能未正确进入 Fastboot 或刷机包与机型不匹配。请核对机型；如需重新进入 Fastboot，请在确认当前操作已结束后按电源键 + 音量减。当前脚本仍在运行，不会开放重复开始；请先查看日志，待脚本结束后再重试。");
    });
}
void XiaomiFlashService::configure(const QString &fastbootPath) {
    if (!m_busy) m_fastboot = fastbootPath;
}
bool XiaomiFlashService::start(const Xiaomi::Package &package, QString *error) {
    auto fail = [&](const QString &message) { if (error) *error = message; return false; };
    if (m_busy) return fail("小米线刷操作尚未结束。");
    if (!QFileInfo(m_fastboot).isFile()) return fail("程序内置 fastboot.exe 不存在，请重新启动并检查资源提取。");
    if (!QFileInfo(package.script).isFile()) return fail("刷机脚本不存在，请重新选择刷机包。");
    if (!DeviceOperationLease::acquire(this, error)) return false;

    m_package = package;
    m_busy = true;
    m_failed = false;
    m_mismatchPending = false;
    m_item = -1;
    m_lastPercent = 0;
    m_completedBytes = 0;
    m_completedChunkBytes = 0;
    m_currentChunkExpectedBytes = 0;
    m_currentChunkReportedBytes = 0;
    m_currentItemTransferredBytes = 0;
    m_currentCommandFinished = false;
    m_transferRate = "0MB/s";
    m_pendingOutput.clear();
    m_elapsed.restart();
    emit progress(m_package.progressPlanValid ? 0 : -1);
    emit log("开始执行小米官方线刷脚本：" + m_package.script + "\n保留脚本原有的机型、防回滚和 BL 检查；刷写中禁止中断。");
    emit progressInfo("0MB/s  |  Time:0s");
    m_statusTimer.start();
    launchScript();
    return true;
}
void XiaomiFlashService::launchScript() {
    QFile script(m_package.script);
    if (!script.open(QIODevice::ReadOnly)) { finish(false, "无法读取刷机脚本：" + script.errorString()); return; }
    const QByteArray bytes = script.readAll();
    if (script.error() != QFile::NoError) { finish(false, "无法完整读取刷机脚本。"); return; }
    const auto hash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    if (!m_package.scriptSha256.isEmpty() && hash != m_package.scriptSha256) {
        finish(false, "确认后刷机脚本已变化，已拒绝执行，请重新选择并确认。"); return;
    }

    const QString toolDirectory = QFileInfo(m_fastboot).absolutePath();
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert("ORANGE_XIAOMI_SCRIPT", QDir::toNativeSeparators(m_package.script));
    env.insert("PATH", QDir::toNativeSeparators(toolDirectory) + ';' + env.value("PATH"));
    m_process.setProcessEnvironment(env);
    m_process.setWorkingDirectory(toolDirectory);
    const QString cmd = QDir(env.value("SystemRoot", "C:/Windows")).filePath("System32/cmd.exe");
    m_process.setProgram(cmd);
    m_process.setArguments({});
    // No -s is appended: the selected official BAT receives exactly the same
    // command line as in the reference implementation.
    m_process.setNativeArguments("/d /v:off /s /c \"\"%ORANGE_XIAOMI_SCRIPT%\"\"");
    m_process.start();
    // Avoid an accidental PAUSE keeping the operation and lease alive forever.
    m_process.closeWriteChannel();
}
void XiaomiFlashService::readOutput() {
    // Decode complete lines, not arbitrary pipe chunks (Chinese/multibyte log
    // characters can straddle readyRead signals).
    m_pendingOutput += m_process.readAllStandardOutput();
    m_pendingOutput.replace('\r', '\n');
    qsizetype end = -1;
    while ((end = m_pendingOutput.indexOf('\n')) >= 0) {
        const QByteArray bytes = m_pendingOutput.left(end);
        m_pendingOutput.remove(0, end + 1);
        handleLine(QString::fromLocal8Bit(bytes));
    }
}
void XiaomiFlashService::handleLine(const QString &line) {
    if (line.trimmed().isEmpty()) return;
    emit log(line + "\n");
    if (m_mismatchPending) { m_mismatchTimer.stop(); m_mismatchPending = false; }
    if (line.contains("Missmatching", Qt::CaseInsensitive)) {
        m_mismatchPending = true;
        m_mismatchTimer.start();
    }
    static const QRegularExpression failed(
        R"(\bFAILED\b|\berror\b|失败|错误|missmatch|mismatch|anti.?rollback.*(?:error|fail))",
        QRegularExpression::CaseInsensitiveOption);
    if (failed.match(line).hasMatch()) {
        if (!m_failed) emit warning("脚本报告错误：" + line + "\n当前脚本尚未结束，请勿重复开始或强制中断。");
        m_failed = true;
    }

    static const QRegularExpression speed(
        R"((?<speed>\d+(?:\.\d+)?)\s*(?<unit>GB/s|MB/s|KB/s|B/s))",
        QRegularExpression::CaseInsensitiveOption);
    const auto speedMatch = speed.match(line);
    if (speedMatch.hasMatch()) m_transferRate = speedMatch.captured("speed") + speedMatch.captured("unit");
    emit progressInfo(QString("%1  |  Time:%2s").arg(m_transferRate).arg(m_elapsed.elapsed() / 1000));
    if (m_failed) return;
    if (!m_package.progressPlanValid) { parseFallbackProgress(line); return; }

    static const QRegularExpression sending(
        R"(Sending(?:\s+sparse)?\s+'(?<partition>[^']+)'(?:\s+\d+/\d+)?\s*\((?<size>\d+(?:\.\d+)?)\s*(?<unit>GB|MB|KB|B)\))",
        QRegularExpression::CaseInsensitiveOption);
    const auto sendingMatch = sending.match(line);
    if (sendingMatch.hasMatch() && activateItem(sendingMatch.captured("partition"))) {
        m_currentChunkExpectedBytes = sizeToBytes(sendingMatch.captured("size"), sendingMatch.captured("unit"));
        m_currentChunkReportedBytes = 0;
    }

    static const QRegularExpression transfer(
        R"(^\s*(?<partition>[^:]+):\s*(?<current>\d+(?:\.\d+)?)\s*(?<currentUnit>GB|MB|KB|B)\s*/\s*(?<total>\d+(?:\.\d+)?)\s*(?<totalUnit>GB|MB|KB|B)\s*\()", QRegularExpression::CaseInsensitiveOption);
    const auto transferMatch = transfer.match(line);
    if (transferMatch.hasMatch() && ensureItemActive(transferMatch.captured("partition"))) {
        const qint64 current = sizeToBytes(transferMatch.captured("current"), transferMatch.captured("currentUnit"));
        const qint64 total = sizeToBytes(transferMatch.captured("total"), transferMatch.captured("totalUnit"));
        if (m_currentChunkExpectedBytes <= 0) m_currentChunkExpectedBytes = total;
        m_currentChunkReportedBytes = qMax(m_currentChunkReportedBytes, current);
        setCurrentTransferred(m_completedChunkBytes + m_currentChunkReportedBytes);
    }
    if (QRegularExpression(R"(^\s*Writing\s+')", QRegularExpression::CaseInsensitiveOption).match(line).hasMatch() && m_item >= 0 && !m_currentCommandFinished) {
        m_currentChunkReportedBytes = qMax(m_currentChunkReportedBytes, m_currentChunkExpectedBytes);
        setCurrentTransferred(m_completedChunkBytes + m_currentChunkReportedBytes);
    }
    if (QRegularExpression(R"(^\s*Finished\.)", QRegularExpression::CaseInsensitiveOption).match(line).hasMatch())
        completeItem();
    updateProgress();
}
void XiaomiFlashService::completeProcess(int code, QProcess::ExitStatus status) {
    if (!m_busy) return;
    readOutput();
    if (!m_pendingOutput.isEmpty()) {
        const QByteArray tail = m_pendingOutput;
        m_pendingOutput.clear();
        handleLine(QString::fromLocal8Bit(tail));
    }
    const bool success = status == QProcess::NormalExit && code == 0 && !m_failed;
    finish(success, success ? "小米线刷脚本执行完成（退出代码 0）。请结合日志确认设备状态。" : QString("小米线刷未成功（退出代码 %1），请检查日志。").arg(code));
}
void XiaomiFlashService::finish(bool success, const QString &message) {
    if (!m_busy) return;
    m_mismatchTimer.stop();
    m_statusTimer.stop();
    m_mismatchPending = false;
    m_busy = false;
    m_transferRate = "0MB/s";
    emit progressInfo(QString("0MB/s  |  Time:%1s").arg(m_elapsed.elapsed() / 1000));
    DeviceOperationLease::release(this);
    if (success) emit progress(100);
    emit log(message + "\n");
    emit finished(success, message);
}
qint64 XiaomiFlashService::sizeToBytes(const QString &value, const QString &unit) {
    bool ok = false; const double number = value.toDouble(&ok); if (!ok) return 0;
    const double multiplier = unit.compare("GB", Qt::CaseInsensitive) == 0 ? 1024.0 * 1024.0 * 1024.0 :
        unit.compare("MB", Qt::CaseInsensitive) == 0 ? 1024.0 * 1024.0 :
        unit.compare("KB", Qt::CaseInsensitive) == 0 ? 1024.0 : 1.0;
    return qMax<qint64>(0, qRound64(number * multiplier));
}
bool XiaomiFlashService::activateItem(const QString &partition) {
    if (m_item >= 0 && !m_currentCommandFinished && m_package.images[m_item].partition.compare(partition, Qt::CaseInsensitive) == 0) {
        completeChunk(); return true;
    }
    if (m_item >= 0 && !m_currentCommandFinished) completeItem();
    for (int i = qMax(0, m_item + 1); i < m_package.images.size(); ++i) {
        if (m_package.images[i].partition.compare(partition, Qt::CaseInsensitive) == 0) {
            m_item = i; m_currentItemTransferredBytes = 0; m_completedChunkBytes = 0;
            m_currentChunkExpectedBytes = m_currentChunkReportedBytes = 0; m_currentCommandFinished = false; return true;
        }
    }
    return false;
}
bool XiaomiFlashService::ensureItemActive(const QString &partition) {
    if (m_item >= 0 && !m_currentCommandFinished && m_package.images[m_item].partition.compare(partition, Qt::CaseInsensitive) == 0) return true;
    return activateItem(partition);
}
void XiaomiFlashService::completeChunk() {
    if (m_item < 0 || m_currentCommandFinished) return;
    const qint64 length = m_package.images[m_item].bytes;
    m_completedChunkBytes = qMin(length, m_completedChunkBytes + qMax(m_currentChunkExpectedBytes, m_currentChunkReportedBytes));
    m_currentItemTransferredBytes = qMax(m_currentItemTransferredBytes, m_completedChunkBytes);
    m_currentChunkExpectedBytes = m_currentChunkReportedBytes = 0;
}
void XiaomiFlashService::completeItem() {
    if (m_item < 0 || m_currentCommandFinished) return;
    m_completedBytes = qMin(m_package.totalBytes, m_completedBytes + m_package.images[m_item].bytes);
    m_currentItemTransferredBytes = 0; m_completedChunkBytes = 0;
    m_currentChunkExpectedBytes = m_currentChunkReportedBytes = 0; m_currentCommandFinished = true;
}
void XiaomiFlashService::setCurrentTransferred(qint64 bytes) {
    if (m_item < 0 || m_currentCommandFinished) return;
    m_currentItemTransferredBytes = qMax(m_currentItemTransferredBytes, qMin(m_package.images[m_item].bytes, qMax<qint64>(0, bytes)));
}
void XiaomiFlashService::updateProgress() {
    if (!m_package.progressPlanValid || m_package.totalBytes <= 0) return;
    const qint64 transferred = qMax(m_completedBytes, qMin(m_package.totalBytes, m_completedBytes + m_currentItemTransferredBytes));
    const int percent = qBound(0, int(double(transferred) * 100.0 / double(m_package.totalBytes)), 99);
    if (percent > m_lastPercent) { m_lastPercent = percent; emit progress(percent); }
}
void XiaomiFlashService::parseFallbackProgress(const QString &line) {
    static const QRegularExpression percent(R"(\((?<value>\d+(?:\.\d+)?)%\))");
    const auto match = percent.match(line);
    if (!match.hasMatch()) {
        if (m_lastPercent >= 5) return;
        int stage = m_lastPercent;
        if (line.contains("target reported max download size", Qt::CaseInsensitive)) stage = 1;
        else if (line.contains("erasing", Qt::CaseInsensitive) && !line.contains("erase successfully", Qt::CaseInsensitive)) stage = 3;
        else if (line.contains("sending", Qt::CaseInsensitive) && !line.contains("okay", Qt::CaseInsensitive)) stage = 5;
        // Rebooting is not proof of success; 100 is reserved for process exit.
        if (stage > m_lastPercent) { m_lastPercent = stage; emit progress(stage); }
        return;
    }
    bool ok = false; const double value = match.captured("value").toDouble(&ok); if (!ok) return;
    if (qFuzzyCompare(value, 100.0) && QRegularExpression(R"(\d+\s*(?:B|KB)/\d+\s*(?:B|KB)\s*\(100\.0%\))").match(line).hasMatch()) return;
    const int next = qBound(0, qRound(value), 99);
    // Match the reference fallback when a new large partition starts.
    if (m_lastPercent > 90 && next < 50) m_lastPercent = 0;
    if (next > m_lastPercent) { m_lastPercent = next; emit progress(next); }
}
