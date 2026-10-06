#include "xiaomiflashservice.h"
#include "deviceoperationlease.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>
#include <QtMath>
#include <algorithm>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

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
    m_probe.setProcessChannelMode(QProcess::MergedChannels);
    m_probeTimer.setSingleShot(true);
    m_probeTimer.setInterval(5000);
    connect(&m_probeTimer, &QTimer::timeout, this, [this] {
        if (!m_checking) return;
        m_probeTimedOut = true;
        m_probe.kill(); // Read-only devices/getvar probe, never the flashing process.
    });
    connect(&m_probe, &QProcess::started, this, [this] {
        if (m_probeCancelled || m_probeTimedOut) m_probe.kill();
    });
    connect(&m_probe, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, &XiaomiFlashService::completeProbe);
    connect(&m_probe, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && m_checking)
            finish(false, "Fastboot 检测工具无法启动，未执行刷机：" + m_probe.errorString());
    });
    m_mismatchTimer.setSingleShot(true);
    m_mismatchTimer.setInterval(10000);
    m_statusTimer.setInterval(1000);
    connect(&m_statusTimer, &QTimer::timeout, this, [this] {
        if (m_busy && !m_checking) emit progressInfo(QString("%1  |  Time:%2s").arg(m_transferRate).arg(m_elapsed.elapsed() / 1000));
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
    m_package = package;
    if (!scriptUnchanged(error)) return false;
    if (!DeviceOperationLease::acquire(this, error)) return false;

    m_package = package;
    m_busy = true;
    m_failed = false;
    m_scriptStarted = false;
    m_probeCancelled = m_probeTimedOut = false;
    m_detectedSerial.clear();
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
    m_runtimeScript.clear();
    m_pendingSendingBytes = 0;
    m_pendingOutput.clear();
    m_elapsed.restart();
    emit log("正在检测 Fastboot 设备，尚未执行刷机脚本。\n");
    m_checking = true;
    emit checkingChanged(true);
    beginProbe(false);
    return true;
}
bool XiaomiFlashService::scriptUnchanged(QString *error) const {
    QFile script(m_package.script);
    if (!script.open(QIODevice::ReadOnly)) {
        if (error) *error = "无法读取刷机脚本：" + script.errorString();
        return false;
    }
    const QByteArray bytes = script.readAll();
    if (script.error() != QFile::NoError) {
        if (error) *error = "无法完整读取刷机脚本。";
        return false;
    }
    if (!m_package.scriptSha256.isEmpty() &&
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256) != m_package.scriptSha256) {
        if (error) *error = "确认后刷机脚本已变化，已拒绝执行，请重新选择并确认。";
        return false;
    }
    return true;
}
void XiaomiFlashService::beginProbe(bool product) {
    m_checkProduct = product;
    emit progressInfo(product ? "正在检查设备响应（未开始刷机，可取消）" : "正在检测 Fastboot（未开始刷机，可取消）");
    m_probe.setProgram(m_fastboot);
    m_probe.setWorkingDirectory(QFileInfo(m_fastboot).absolutePath());
    // Select only the read-only probe. The original BAT still receives no -s.
    m_probe.setArguments(product ? QStringList{"-s", m_detectedSerial, "getvar", "product"} : QStringList{"devices"});
    m_probeTimer.start();
    m_probe.start();
    m_probe.closeWriteChannel();
}
void XiaomiFlashService::cancelCheck() {
    if (!m_checking) return;
    m_probeCancelled = true;
    m_probeTimer.stop();
    m_probe.kill();
}
void XiaomiFlashService::completeProbe(int code, QProcess::ExitStatus status) {
    if (!m_checking) return;
    m_probeTimer.stop();
    const QString output = QString::fromLocal8Bit(m_probe.readAllStandardOutput());
    if (m_probeCancelled) { finish(false, "已取消设备检测，未执行刷机脚本。"); return; }
    if (m_probeTimedOut) { finish(false, "Fastboot 设备检测超时，未执行刷机脚本。请检查驱动、连接和 Fastboot 模式后重试。"); return; }
    if (status != QProcess::NormalExit || code != 0) {
        emit log(output);
        finish(false, "Fastboot 设备检测失败，未执行刷机脚本。请检查驱动及设备连接。"); return;
    }
    if (!m_checkProduct) {
        static const QRegularExpression device(R"(^\s*(\S+)\s+fastboot\s*$)", QRegularExpression::CaseInsensitiveOption);
        QStringList serials;
        for (const QString &line : output.split('\n')) {
            const auto match = device.match(line);
            if (match.hasMatch() && !serials.contains(match.captured(1))) serials.append(match.captured(1));
        }
        if (serials.size() != 1) {
            finish(false, serials.isEmpty() ? "未检测到 Fastboot 设备，未执行刷机脚本。请进入 Fastboot 模式、检查驱动并连接设备后重试。" :
                   "检测到多台 Fastboot 设备，未执行刷机脚本。请只连接目标设备后重试。"); return;
        }
        m_detectedSerial = serials.first();
        beginProbe(true);
        return;
    }
    static const QRegularExpression product(R"((?:^|\n)\s*(?:\(bootloader\)\s*)?product:\s*(\S+))",
                                           QRegularExpression::CaseInsensitiveOption);
    const auto match = product.match(output);
    if (!match.hasMatch() || output.contains("FAILED", Qt::CaseInsensitive)) {
        emit log(output);
        finish(false, "设备未返回有效的 Fastboot product，未执行刷机脚本。请检查设备状态后重试。"); return;
    }
    emit log(QString("检测到 Fastboot 设备：%1，product：%2\n").arg(m_detectedSerial, match.captured(1)));
    // Recheck the script after both asynchronous probes (not just after the dialog).
    QString error;
    if (!scriptUnchanged(&error)) { finish(false, error); return; }
    m_checking = false;
    emit checkingChanged(false);
    m_elapsed.restart();
    emit progress(m_package.progressPlanValid ? 0 : -1);
    emit log("开始执行小米官方线刷脚本：" + m_package.script + "\n保留脚本原有的机型、防回滚和 BL 检查；刷写中禁止中断。");
    emit progressInfo("0MB/s  |  Time:0s");
    m_statusTimer.start();
    launchScript();
}
void XiaomiFlashService::launchScript() {
    QString error;
    if (!scriptUnchanged(&error)) { finish(false, error); return; }
    if (!prepareRuntimeScript(&error)) { finish(false, error); return; }
    m_scriptStarted = true;
    const QString toolDirectory = QFileInfo(m_fastboot).absolutePath();
    auto env = QProcessEnvironment::systemEnvironment();
    const QString scriptToRun = m_runtimeScript.isEmpty() ? m_package.script : m_runtimeScript;
    env.insert("ORANGE_XIAOMI_SCRIPT", QDir::toNativeSeparators(scriptToRun));
    env.insert("PATH", QDir::toNativeSeparators(toolDirectory) + ';' + env.value("PATH"));
    m_process.setProcessEnvironment(env);
    m_process.setWorkingDirectory(toolDirectory);
    const QString cmd = QDir(env.value("SystemRoot", "C:/Windows")).filePath("System32/cmd.exe");
    m_process.setProgram(cmd);
    m_process.setArguments({});
    // No -s is appended: the selected official BAT receives exactly the same
    // command line as in the reference implementation.
    m_process.setNativeArguments("/d /v:off /s /c \"@echo off & \"%ORANGE_XIAOMI_SCRIPT%\"\"");
    m_process.start();
    // Avoid an accidental PAUSE keeping the operation and lease alive forever.
    m_process.closeWriteChannel();
}

bool XiaomiFlashService::prepareRuntimeScript(QString *error) {
    if (m_package.mode != Xiaomi::Mode::KeepData) return true;

    QFile input(m_package.script);
    if (!input.open(QIODevice::ReadOnly)) {
        if (error) *error = "无法读取保留数据刷机脚本：" + input.errorString();
        return false;
    }
    const QByteArray source = input.readAll();
    if (input.error() != QFile::NoError) {
        if (error) *error = "无法完整读取保留数据刷机脚本。";
        return false;
    }

    if (!m_package.scriptSha256.isEmpty() &&
        QCryptographicHash::hash(source, QCryptographicHash::Sha256) != m_package.scriptSha256) {
        if (error) *error = "确认后刷机脚本已变化，已拒绝执行，请重新选择并确认。";
        return false;
    }

    // Match only the known vendor template: a raw partition-type probe piped
    // through findstr, an errorlevel==0 guard, then a single guarded erase.
    // Never infer an erase target from comments or unrelated/unguarded probes.
    static const QRegularExpression cotaBlock(
        R"cota(^[ \t]*@?fastboot(?:\.exe)?[ \t]+%\*[ \t]+getvar[ \t]+partition-type:(?<probe>opcust|opconfig)[ \t]+2>&1[ \t]*\|[ \t]*findstr[ \t]+/r[ \t]+/c:"\^partition-type:(?<filter>opcust|opconfig): raw"[ \t]*\r?\n[ \t]*if[ \t]+%errorlevel%[ \t]+equ[ \t]+0[ \t]*\([ \t]*\r?\n[ \t]*@?fastboot(?:\.exe)?[ \t]+%\*[ \t]+erase[ \t]+(?<erase>opcust|opconfig)[ \t]+\|\|[^\r\n]+\r?\n[ \t]*\)[ \t]*\r?$)cota",
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::MultilineOption);
    QByteArray patched = source;
    struct Edit { qsizetype offset; qsizetype length; QByteArray replacement; };
    QVector<Edit> replacements;
    int corrections = 0;
    // Latin1 gives each source byte one code unit, so edits preserve the original
    // encoding, BOM, comments and line endings instead of re-encoding the BAT.
    const QString byteView = QString::fromLatin1(source);
    auto blocks = cotaBlock.globalMatch(byteView);
    while (blocks.hasNext()) {
        const auto block = blocks.next();
        const QString expected = block.captured("probe");
        if (expected.compare(block.captured("filter"), Qt::CaseInsensitive) != 0 ||
            expected.compare(block.captured("erase"), Qt::CaseInsensitive) == 0) continue;
        replacements.append({block.capturedStart("erase"), block.capturedLength("erase"),
                             expected.toLatin1()});
        ++corrections;
    }
    if (corrections == 0) return true;

    static const QRegularExpression dp0(R"(%~dp0)", QRegularExpression::CaseInsensitiveOption);
    auto paths = dp0.globalMatch(byteView);
    QByteArray packageDirectory;
    if (paths.hasNext()) {
#ifdef Q_OS_WIN
        // The runtime BAT is stored in TEMP. Its %~dp0 would therefore point
        // to TEMP instead of the extracted ROM, and cmd.exe cannot reliably
        // resolve Chinese paths in every legacy Xiaomi script. Resolve the ROM
        // directory to its 8.3 path only when a COTA repair is actually needed.
        QString compatiblePath = QDir::toNativeSeparators(QDir::cleanPath(m_package.directory));
        const std::wstring packageWide = compatiblePath.toStdWString();
        const DWORD shortLength = GetShortPathNameW(packageWide.c_str(), nullptr, 0);
        if (shortLength > 0) {
            std::wstring shortWide(shortLength, L'\0');
            const DWORD written = GetShortPathNameW(packageWide.c_str(), shortWide.data(), shortLength);
            if (written > 0 && written < shortLength)
                compatiblePath = QString::fromWCharArray(shortWide.c_str(), int(written));
        }
        // 8.3 names may be disabled. An already-safe ASCII path also works;
        // reject lossy encodings or shell metacharacters rather than running a
        // rewritten unquoted vendor command against an ambiguous image path.
        static const QRegularExpression safePath(R"(^[A-Za-z]:[\\/][A-Za-z0-9_.~\\/\-]+$)");
        if (safePath.match(compatiblePath).hasMatch()) packageDirectory = compatiblePath.toLatin1();
#else
        packageDirectory = QDir::toNativeSeparators(QDir::cleanPath(m_package.directory)).toLocal8Bit();
#endif
        if (packageDirectory.isEmpty()) {
            if (error) *error = "无法为修正版脚本生成安全的刷机包路径，未执行刷机。请将刷机包解压到纯英文、无空格或特殊字符的目录后重试。";
            return false;
        }
        packageDirectory += QDir::separator() == QChar('\\') ? '\\' : '/';
        while (paths.hasNext()) {
            const auto path = paths.next();
            replacements.append({path.capturedStart(), path.capturedLength(), packageDirectory});
        }
    }
    // Apply edits from right to left so earlier byte offsets stay valid.
    std::sort(replacements.begin(), replacements.end(), [](const Edit &a, const Edit &b) {
        return a.offset > b.offset;
    });
    for (const Edit &edit : replacements)
        patched.replace(edit.offset, edit.length, edit.replacement);

    const QString tempRoot = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    QDir tempDir(tempRoot);
    if (tempRoot.isEmpty() || !tempDir.mkpath("OrangeTools_Xiaomi")) {
        if (error) *error = "无法创建保留数据刷机临时脚本目录。";
        return false;
    }
    const QString path = tempDir.filePath("OrangeTools_Xiaomi/flash_all_except_storage_" +
                                          QUuid::createUuid().toString(QUuid::WithoutBraces) + ".bat");
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly) || output.write(patched) != patched.size() || !output.commit()) {
        if (error) *error = "无法创建修正版保留数据刷机临时脚本：" + output.errorString();
        return false;
    }
    m_runtimeScript = path;
    emit log(QString("保留数据模式已修正脚本中的 COTA 分区目标（%1 处），原始脚本未修改。\n").arg(corrections));
    return true;
}

void XiaomiFlashService::cleanupRuntimeScript() {
    if (m_runtimeScript.isEmpty()) return;
    QFile::remove(m_runtimeScript);
    m_runtimeScript.clear();
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
bool XiaomiFlashService::isCommandEcho(const QString &line) {
    // ECHO ON scripts may print: C:\...>fastboot ... || echo Missmatching...
    // Ignore only shell command syntax, never actual Fastboot error/result lines.
    static const QRegularExpression prompt(R"(^\s*(?:[A-Za-z]:[\\/]|\\\\)[^>]*>)");
    static const QRegularExpression command(
        R"(^\s*(?:[|&]{2}\s*)?@?(?:echo|if|for|set|exit|findstr|rem|cd|pause|fastboot(?:\.exe)?)\b(?:\s|$))",
        QRegularExpression::CaseInsensitiveOption);
    return prompt.match(line).hasMatch() || command.match(line).hasMatch();
}
void XiaomiFlashService::handleLine(const QString &line) {
    if (line.trimmed().isEmpty()) return;
    emit log(line + "\n");
    const bool commandEcho = isCommandEcho(line);
    if (commandEcho) return; // A displayed shell command is not an executed result.
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
    if (speedMatch.hasMatch()) {
        bool ok = false;
        const double value = speedMatch.captured("speed").toDouble(&ok);
        if (ok) {
            QString unit = speedMatch.captured("unit");
            unit.chop(2); // remove "/s"
            updateTransferRate(sizeToBytes(QString::number(value), unit), 1.0);
        }
    }

    // Standard fastboot reports size in Sending and duration in OKAY, either
    // on the same line or on the next line. This also works without a complete
    // image plan. Sparse chunks use their own size/time, not the last chunk's
    // size divided by the whole command's total (which includes disk writes).
    static const QRegularExpression sending(
        R"(Sending(?:\s+sparse)?\s+'(?<partition>[^']+)'(?:\s+\d+/\d+)?\s*\((?<size>\d+(?:\.\d+)?)\s*(?<unit>GB|MB|KB|B)\))",
        QRegularExpression::CaseInsensitiveOption);
    const auto sendingMatch = sending.match(line);
    if (sendingMatch.hasMatch())
        m_pendingSendingBytes = sizeToBytes(sendingMatch.captured("size"), sendingMatch.captured("unit"));
    static const QRegularExpression sendingOkay(
        R"(\bOKAY\s*\[\s*(?<seconds>\d+(?:\.\d+)?)\s*s\s*\])",
        QRegularExpression::CaseInsensitiveOption);
    const auto okay = sendingOkay.match(line);
    const bool sendingResult = sendingMatch.hasMatch() || line.trimmed().startsWith("OKAY", Qt::CaseInsensitive);
    if (sendingResult && okay.hasMatch() && m_pendingSendingBytes > 0 && !m_failed) {
        bool ok = false;
        const double seconds = okay.captured("seconds").toDouble(&ok);
        if (ok) updateTransferRate(m_pendingSendingBytes, seconds);
        m_pendingSendingBytes = 0;
    }
    if (m_failed || line.trimmed().startsWith("Writing", Qt::CaseInsensitive) ||
        line.contains("Finished.", Qt::CaseInsensitive)) m_pendingSendingBytes = 0;

    if (!m_failed) {
        if (!m_package.progressPlanValid) {
            parseFallbackProgress(line);
        } else {
            if (sendingMatch.hasMatch() && activateItem(sendingMatch.captured("partition"))) {
                m_currentChunkExpectedBytes = sizeToBytes(sendingMatch.captured("size"), sendingMatch.captured("unit"));
                m_currentChunkReportedBytes = 0;
            }

            static const QRegularExpression transfer(
                "^\\s*(?<partition>[^:]+):\\s*(?<current>\\d+(?:\\.\\d+)?)\\s*(?<currentUnit>GB|MB|KB|B)\\s*/\\s*(?<total>\\d+(?:\\.\\d+)?)\\s*(?<totalUnit>GB|MB|KB|B)\\s*\\(",
                QRegularExpression::CaseInsensitiveOption);
            const auto transferMatch = transfer.match(line);
            if (transferMatch.hasMatch() && ensureItemActive(transferMatch.captured("partition"))) {
                const qint64 current = sizeToBytes(transferMatch.captured("current"), transferMatch.captured("currentUnit"));
                const qint64 total = sizeToBytes(transferMatch.captured("total"), transferMatch.captured("totalUnit"));
                if (m_currentChunkExpectedBytes <= 0) m_currentChunkExpectedBytes = total;
                m_currentChunkReportedBytes = qMax(m_currentChunkReportedBytes, current);
                setCurrentTransferred(m_completedChunkBytes + m_currentChunkReportedBytes);
            }
            static const QRegularExpression writing(
                "^\\s*Writing\\s+'", QRegularExpression::CaseInsensitiveOption);
            if (writing.match(line).hasMatch() && m_item >= 0 && !m_currentCommandFinished) {
                m_currentChunkReportedBytes = qMax(m_currentChunkReportedBytes, m_currentChunkExpectedBytes);
                setCurrentTransferred(m_completedChunkBytes + m_currentChunkReportedBytes);
            }
            if (line.contains("Finished.", Qt::CaseInsensitive)) completeItem();
            updateProgress();
        }
    }
    emit progressInfo(QString("%1  |  Time:%2s").arg(m_transferRate).arg(m_elapsed.elapsed() / 1000));
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
    m_probeTimer.stop();
    const bool wasChecking = m_checking;
    m_checking = false;
    m_mismatchPending = false;
    m_busy = false;
    const QString finalRate = m_transferRate;
    cleanupRuntimeScript();
    if (wasChecking) emit checkingChanged(false);
    emit progressInfo(m_scriptStarted ? QString("%1  |  Time:%2s").arg(finalRate).arg(m_elapsed.elapsed() / 1000) : "未开始刷机");
    DeviceOperationLease::release(this);
    if (success) emit progress(100);
    emit log(message + "\n");
    emit finished(success, message);
}
QString XiaomiFlashService::formatTransferRate(double bytesPerSecond) {
    if (!(bytesPerSecond > 0.0) || !qIsFinite(bytesPerSecond)) return {};
    const double kib = 1024.0;
    const double mib = kib * kib;
    const double gib = mib * kib;
    double value = bytesPerSecond;
    QString unit = "B/s";
    if (value >= gib) { value /= gib; unit = "GB/s"; }
    else if (value >= mib) { value /= mib; unit = "MB/s"; }
    else if (value >= kib) { value /= kib; unit = "KB/s"; }
    QString number = value >= 100.0 ? QString::number(value, 'f', 0) :
        value >= 10.0 ? QString::number(value, 'f', 1) : QString::number(value, 'f', 2);
    while (number.contains('.') && number.endsWith('0')) number.chop(1);
    if (number.endsWith('.')) number.chop(1);
    return number + unit;
}

void XiaomiFlashService::updateTransferRate(qint64 bytes, double seconds) {
    if (bytes <= 0 || !(seconds > 0.0) || !qIsFinite(seconds)) return;
    const QString formatted = formatTransferRate(static_cast<double>(bytes) / seconds);
    if (!formatted.isEmpty()) m_transferRate = formatted;
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
