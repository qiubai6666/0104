#include "openlistinstaller.h"
#include "openlistservice.h"
#include "processmanager.h"
#include "deviceoperationlease.h"
#include "resourceextractor.h"
#include "shellcommand.h"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QRegularExpression>
#include <QUuid>

OpenListProcessRunner::OpenListProcessRunner(QObject *parent)
    : OugaCommandRunner(parent), m_process(ProcessManager::createProcess(this)) {
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    m_deadline.setSingleShot(true); m_watchdog.setSingleShot(true); m_watchdog.setInterval(120000);
    connect(&m_deadline,&QTimer::timeout,this,[this] {
        m_output += QStringLiteral("\nERROR: 只读检测超时\n"); m_process->kill();
    });
    connect(&m_watchdog,&QTimer::timeout,this,[this] { emit stalled(); });
    connect(m_process,&QProcess::readyReadStandardOutput,this,&OpenListProcessRunner::readOutput);
    connect(m_process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,
            [this](int code,QProcess::ExitStatus status) { finish(code,status == QProcess::NormalExit); });
    connect(m_process,&QProcess::errorOccurred,this,[this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) finish(-1,false);
    });
}
void OpenListProcessRunner::run(const QString &program,const QStringList &args,int timeout) {
    if (m_running) return;
    m_running = true; m_truncated = false; m_output.clear(); m_decoder.resetState();
    m_process->setWorkingDirectory(QFileInfo(program).absolutePath());
    if (timeout > 0) m_deadline.start(timeout);
    m_watchdog.start(); m_process->start(program,args);
}
void OpenListProcessRunner::readOutput() {
    // adb and Android shell output UTF-8; retain decoder state across byte boundaries.
    const QString text = m_decoder(m_process->readAllStandardOutput());
    if (m_output.size() + text.size() <= 2 * 1024 * 1024) {
        m_output += text;
        if (!text.isEmpty()) emit output(text);
    } else m_truncated = true;
    m_watchdog.start();
}
void OpenListProcessRunner::finish(int code,bool normal) {
    if (!m_running) return;
    readOutput();
    m_running = false; m_deadline.stop(); m_watchdog.stop();
    if (m_truncated) { normal = false; m_output += QStringLiteral("\nERROR: 输出过长，结果无法确认\n"); }
    emit completed(code,normal,m_output);
}
OpenListInstaller::OpenListInstaller(QObject *parent,OugaCommandRunner *runner,const QString &adbPath,const QString &sevenZip)
    : QObject(parent), m_runner(runner ? runner : new OpenListProcessRunner(this)),
      m_adb(adbPath.isEmpty() ? ResourceExtractor::getAdbPath() : adbPath),
      m_sevenZip(sevenZip.isEmpty() ? QDir(ResourceExtractor::getResourcePath()).filePath("bin/7zip/7z.exe") : sevenZip) {
    setProperty(DeviceOperationLease::ApplicationExitExemptProperty, true);
    m_deviceWait.setInterval(100);
    connect(&m_deviceWait, &QTimer::timeout, this, &OpenListInstaller::startDeviceDiscovery);
    connect(m_runner,&OugaCommandRunner::completed,this,&OpenListInstaller::onCompleted);
    connect(m_runner,&OugaCommandRunner::output,this,[this](const QString &text) { streamLog(text); });
    connect(m_runner,&OugaCommandRunner::stalled,this,[this] {
        emit log(m_transferOnly ? QStringLiteral("传输暂时没有新输出，请检查手机连接；不会强制停止传输。") : QStringLiteral("安装器暂时没有新输出，请检查手机授权；不会强制停止安装。"));
    });
}
void OpenListInstaller::streamLog(const QString &text,bool flush) {
    // Buffer complete lines: process chunks can split UTF-8 text, paths and internal markers.
    if (m_stage != ApkInstall && m_stage != ModuleInstall && m_stage != ApkRootInstall) return;
    if (!text.isEmpty()) m_receivedOutput = true;
    m_logPending += text;
    QStringList lines;
    int newline;
    while ((newline = m_logPending.indexOf('\n')) >= 0) {
        lines.append(m_logPending.left(newline)); m_logPending.remove(0,newline+1);
    }
    if (flush && !m_logPending.isEmpty()) { lines.append(m_logPending); m_logPending.clear(); }
    for (QString line : lines) {
        line = line.trimmed();
        if (line.isEmpty() || line == "Success" || line == "OL_INSTALL_SUCCESS") continue;
        const bool extraction = line.startsWith("Archive:") || line.startsWith("inflating:") || line.startsWith("creating:");
        if (extraction && success(0,true,line,false)) continue;
        line.replace(m_file,QStringLiteral("[本次安装包]"));
        if (!m_remote.isEmpty()) line.replace(m_remote,QStringLiteral("[手机临时文件]"));
        emit log(OpenList::redact(line));
    }
    // Bound a misbehaving installer's line without exposing a partial path or marker.
    if (m_logPending.size() > 65536) m_logPending.clear();
}
QStringList OpenListInstaller::authorizedDevices(const QString &output) {
    QStringList result;
    const QRegularExpression re("^([^\\s]+)\\s+device(?:\\s|$)");
    for (const QString &line : output.split('\n')) {
        const auto match = re.match(line.trimmed());
        if (match.hasMatch() && !result.contains(match.captured(1))) result.append(match.captured(1));
    }
    return result;
}
bool OpenListInstaller::success(int code,bool normal,const QString &output,bool apk) {
    if (!normal || code != 0) return false;
    if (QRegularExpression("\\b(failure|failed|error|abort|aborted|denied|not found)\\b|失败|错误|拒绝",
                           QRegularExpression::CaseInsensitiveOption).match(output).hasMatch()) return false;
    return !apk || QRegularExpression("(?:^|[\\r\\n])\\s*Success\\s*(?:$|[\\r\\n])").match(output).hasMatch();
}
bool OpenListInstaller::validModuleListing(const QString &output) {
    int rootProps = 0;
    for (const QString &line : output.split('\n')) {
        const QString s = line.trimmed();
        if (s == "Path = module.prop") ++rootProps;
        if (s == "Encrypted = +") return false;
        if (s.startsWith("Path = ")) {
            QString path = s.mid(7); path.replace('\\','/');
            if (path.split('/').contains("..") || path.split('/').contains(".") || path.startsWith('/') || path.contains(':')) return false;
        }
    }
    return rootProps == 1;
}
bool OpenListInstaller::validModuleProperties(const QString &output) {
    if (output.size() > 65536) return false;
    QMap<QString,QString> properties;
    for (const auto &line : output.split('\n')) {
        const QString s = line.trimmed(); if (s.startsWith('#')) continue;
        const int eq = s.indexOf('='); if (eq > 0) properties.insert(s.left(eq).trimmed(),s.mid(eq+1).trimmed());
    }
    return QRegularExpression("^[A-Za-z][A-Za-z0-9._-]*$").match(properties.value("id")).hasMatch() && !properties.value("name").isEmpty();
}
bool OpenListInstaller::cleanupDownloadedFile(const QString &file) {
    const QFileInfo info(file);
    return info.isFile() && !info.isSymLink() && OpenList::safeDirectory(info.absolutePath(),false) && QFile::remove(file);
}
bool OpenListInstaller::inspectDevices() {
    if (busy()) return false;
    if (DeviceOperationLease::owner() && DeviceOperationLease::owner() != this) {
        emit completed(false,QStringLiteral("其他设备操作尚未结束，请等待后重试。")); return false;
    }
    m_stage = WaitingDevice; m_deviceWaitAttempts = 0;
    startDeviceDiscovery(); return true;
}
void OpenListInstaller::startDeviceDiscovery() {
    if (m_stage != WaitingDevice) { m_deviceWait.stop(); return; }
    if (DeviceOperationLease::acquire(this)) {
        m_deviceWait.stop(); command(Discover,m_adb,{"devices"},5000); return;
    }
    if (DeviceOperationLease::owner() || ++m_deviceWaitAttempts >= 50) {
        m_deviceWait.stop(); m_stage = Idle;
        emit completed(false,QStringLiteral("设备通道仍被其他操作占用，请等待后重试。")); return;
    }
    if (!m_deviceWait.isActive()) {
        emit log(QStringLiteral("正在等待设备通道空闲…")); m_deviceWait.start();
    }
}
bool OpenListInstaller::install(const QString &file,bool module,const QString &serial) {
    if (busy()) return false;
    if (serial.isEmpty() || serial.contains(QRegularExpression("\\s")) || !DeviceOperationLease::acquire(this)) {
        emit completed(false,QStringLiteral("目标设备无效或设备操作被占用，文件已保留：%1").arg(file)); return false;
    }
    m_transferOnly = false;
    m_apkRootMode = ShellRoot;
    m_file = file; m_serial = serial; m_module = module; m_remoteTouched = false; m_canTransferFailure = false; m_fallbackRemote.clear(); m_managers.clear();
    const QFileInfo info(file);
    if (!info.isFile() || info.isSymLink() || !OpenList::safeDirectory(info.absolutePath(),false)) {
        end(false,QStringLiteral("安装包不可用或路径不安全。")); return false;
    }
    if (module) {
        QFile archive(file);
        if (!archive.open(QIODevice::ReadOnly) || archive.read(4) != QByteArray("PK\x03\x04",4)) {
            end(false,QStringLiteral("不是有效的 ZIP 模块文件。")); return false;
        }
    }
    m_remote = "/data/local/tmp/orange-openlist-" + QUuid::createUuid().toString(QUuid::Id128) + (module ? ".zip" : ".apk");
    command(Verify,m_adb,{"devices"},5000); return true;
}
bool OpenListInstaller::transfer(const QString &file,const QString &serial) {
    if (busy()) return false;
    if (serial.isEmpty() || serial.contains(QRegularExpression("\\s")) || !DeviceOperationLease::acquire(this)) {
        emit completed(false,QStringLiteral("目标设备无效或设备操作被占用，文件已保留：%1").arg(file)); return false;
    }
    m_transferOnly = true; m_file = file; m_serial = serial;
    m_remoteTouched = false; m_canTransferFailure = false; m_fallbackRemote.clear(); m_result.clear();
    const QFileInfo info(file);
    if (!info.isFile() || info.isSymLink() || !OpenList::safeDirectory(info.absolutePath(),false)) {
        finish(false,QStringLiteral("文件不可用或路径不安全，无法传输。")); return false;
    }
    emit log(QStringLiteral("正在传输到手机内部存储：%1").arg(info.fileName()));
    command(TransferVerify,m_adb,{"devices"},5000); return true;
}
void OpenListInstaller::command(Stage stage,const QString &program,const QStringList &args,int timeout) {
    m_logPending.clear(); m_receivedOutput = false;
    m_stage = stage; m_runner->run(program,args,timeout);
}
void OpenListInstaller::adb(Stage stage,const QStringList &args,int timeout) {
    command(stage,m_adb,QStringList{"-s",m_serial} + args,timeout);
}
void OpenListInstaller::onCompleted(int code,bool normal,const QString &out) {
    streamLog(m_receivedOutput ? QString() : out,true);
    if (m_stage == Idle || m_stage == RootChoice) return;
    if (m_stage == Discover) {
        const auto serials = authorizedDevices(out);
        m_stage = Idle; DeviceOperationLease::release(this);
        if (!normal || code != 0) emit completed(false,QStringLiteral("设备检测失败或超时。"));
        else if (serials.isEmpty()) emit completed(false,QStringLiteral("没有已授权的 ADB 设备，请检查连接和手机授权。"));
        else emit devicesReady(serials);
        return;
    }
    if (handleTransfer(code,normal,out) || handleApkRoot(code,normal,out)) return;
    if (m_stage == RemoteCleanup) {
        if (code != 0 || !normal) emit log(QStringLiteral("手机临时文件清理失败，请检查连接；不会改变已确认的安装结果。"));
        end(m_installSuccess,m_result); return;
    }
    if (!success(code,normal,out,m_stage == ApkInstall)) {
        QString reason;
        if (m_stage == RootAccess || m_stage == Managers) reason = QStringLiteral("Root 授权被拒绝或管理器检测失败。");
        else if (m_stage == ZipList || m_stage == ZipTest || m_stage == ZipProperties) reason = QStringLiteral("模块 ZIP 无效、损坏或无法检查。");
        else if (m_stage == Verify) reason = QStringLiteral("安装前设备重新检测失败。");
        else reason = QStringLiteral("安装器执行失败或结果无法确认。");
        if (m_remoteTouched) cleanRemote(false,reason); else end(false,reason);
        return;
    }
    if (m_stage == ModuleInstall && !out.split(QRegularExpression("[\\r\\n]+"),Qt::SkipEmptyParts).contains("OL_INSTALL_SUCCESS")) {
        cleanRemote(false,QStringLiteral("模块安装结果无法确认。")); return;
    }
    switch (m_stage) {
    case Verify:
        if (!authorizedDevices(out).contains(m_serial)) { end(false,QStringLiteral("原目标手机已断开或未授权，不会切换设备。")); break; }
        if (!m_module) probeApkRoot();
        else command(ZipList,m_sevenZip,{"l","-sccUTF-8","-slt","-ba","-pOrangeOpenListNoPassword","--",m_file},60000);
        break;
    case ZipList:
        if (!validModuleListing(out)) { end(false,QStringLiteral("不是有效 Root 模块：需要唯一的根目录 module.prop，且不能加密。")); break; }
        command(ZipTest,m_sevenZip,{"t","-sccUTF-8","-bd","-pOrangeOpenListNoPassword","--",m_file},60000); break;
    case ZipTest:
        command(ZipProperties,m_sevenZip,{"e","-sccUTF-8","-so","-pOrangeOpenListNoPassword","--",m_file,"module.prop"},60000); break;
    case ZipProperties:
        if (!validModuleProperties(out)) { end(false,QStringLiteral("模块描述缺少有效 id 或 name，拒绝安装。")); break; }
        m_canTransferFailure = true;
        adb(RootAccess,{"shell",ShellCommand::asRoot("id -u")},30000); break;
    case RootAccess:
        if (!out.split(QRegularExpression("[\\r\\n]+"),Qt::SkipEmptyParts).contains("0")) { end(false,QStringLiteral("未获得 Root 授权，禁止模块安装。")); break; }
        adb(Managers,{"shell",ShellCommand::asRoot(
            "m=$(command -v magisk 2>/dev/null); if [ -n \"$m\" ] && [ -x \"$m\" ]; then printf 'OL_MAGISK=%s\\n' \"$m\"; "
            "elif [ -x /data/adb/magisk/magisk ]; then echo OL_MAGISK=/data/adb/magisk/magisk; fi; "
            "[ ! -x /data/adb/ap/bin/apd ] || echo OL_APATCH=/data/adb/ap/bin/apd; "
            "[ ! -x /data/adb/ksu/bin/ksud ] || echo OL_KERNELSU=/data/adb/ksu/bin/ksud; true")},30000); break;
    case Managers: {
        const QRegularExpression re("^OL_(MAGISK|APATCH|KERNELSU)=(/[^\\r\\n]+)$");
        for (const auto &line : out.split('\n')) {
            const auto match = re.match(line.trimmed()); if (!match.hasMatch()) continue;
            const QString key = match.captured(1), path = match.captured(2);
            if (path.contains(QRegularExpression("[\\x00-\\x1f]"))) continue;
            const QString name = key == "MAGISK" ? "Magisk / Alpha" : key == "APATCH" ? "APatch" : "KernelSU";
            m_managers.insert(name,ShellCommand::quote(path) + (key == "MAGISK" ? " --install-module" : " module install"));
        }
        if (m_managers.isEmpty()) { end(false,QStringLiteral("未检测到可用的 Magisk、APatch 或 KernelSU 安装命令。")); break; }
        m_stage = RootChoice;
        if (m_managers.size() == 1) chooseRoot(m_managers.firstKey());
        else emit rootChoiceRequired(m_managers.keys());
        break;
    }
    case Push:
        emit log(QStringLiteral("正在安装模块，请留意手机 Root 授权。"));
        adb(ModuleInstall,{"shell",ShellCommand::asRoot(m_managers.value(m_selectedManager) + ' ' + ShellCommand::quote(m_remote) + QStringLiteral("; rc=$?; if [ \"$rc\" -eq 0 ]; then printf '\\nOL_INSTALL_SUCCESS\\n'; fi; exit \"$rc\""))}); break;
    case ModuleInstall: cleanRemote(true,QStringLiteral("模块安装成功；可能需要重启手机，尚未验证模块生效。")); break;
    case ApkInstall: end(true,QStringLiteral("APK 安装成功。")); break;
    default: break;
    }
}
QString OpenListInstaller::apkRootCommand(const QString &command) const {
    if (m_apkRootMode == ShellRoot) return command;
    return ShellCommand::asRoot(command,m_apkRootMode == SuC ? "-c" : "-s");
}
void OpenListInstaller::probeApkRoot() {
    adb(ApkRootProbe,{"shell",apkRootCommand("id -u")},10000);
}
bool OpenListInstaller::handleApkRoot(int code,bool normal,const QString &out) {
    if (m_stage == ApkRootProbe) {
        if (success(code,normal,out,false) && out.split(QRegularExpression("[\\r\\n]+"),Qt::SkipEmptyParts).contains("0")) {
            m_canTransferFailure = true;
            m_remoteTouched = true;
            adb(ApkRootPush,{"push",m_file,m_remote});
        } else if (m_apkRootMode != SuS) {
            m_apkRootMode = m_apkRootMode == ShellRoot ? SuC : SuS;
            probeApkRoot();
        } else {
            m_canTransferFailure = true;
            emit log(QStringLiteral("未获得 Shell Root 权限，使用普通 ADB 安装 APK。"));
            adb(ApkInstall,{"install","-r",m_file});
        }
        return true;
    }
    if (m_stage == ApkRootPush) {
        if (!success(code,normal,out,false)) {
            cleanRemote(false,QStringLiteral("Root APK 安装前推送失败。"));
        } else {
            emit log(QStringLiteral("正在通过 Root 安装 APK，请留意手机授权。"));
            adb(ApkRootInstall,{"shell",apkRootCommand("pm install -r " + ShellCommand::quote(m_remote))});
        }
        return true;
    }
    if (m_stage == ApkRootInstall) {
        if (success(code,normal,out,true)) {
            cleanRemote(true,QStringLiteral("APK 安装成功（Root）。"));
        } else if (m_apkRootMode == SuC && normal &&
                   QRegularExpression("\\b(failure|failed|error|denied)\\b",QRegularExpression::CaseInsensitiveOption).match(out).hasMatch()) {
            // Match the local APK button's compatibility retry, but never retry an unknown result.
            m_apkRootMode = SuS;
            emit log(QStringLiteral("su -c 安装失败，尝试“APK安装”按钮的 su -s 兼容方式。"));
            adb(ApkRootInstall,{"shell",apkRootCommand("pm install -r " + ShellCommand::quote(m_remote))});
        } else {
            cleanRemote(false,QStringLiteral("Root APK 安装失败或结果无法确认。"));
        }
        return true;
    }
    return false;
}
bool OpenListInstaller::handleTransfer(int code,bool normal,const QString &out) {
    // A successful fallback transfer is not installation success; neither transfer mode retries itself.
    if (m_stage == TransferVerify) {
        if (!success(code,normal,out,false) || !authorizedDevices(out).contains(m_serial)) {
            finish(false,m_result + QStringLiteral(" 原目标手机已断开、未授权或检测失败，无法推送安装包；不会切换设备。"));
            return true;
        }
        const QFileInfo info(m_file);
        QString name = info.completeBaseName();
        name.replace(QRegularExpression("[\\x00-\\x1f\\x7f]"),QStringLiteral("_"));
        m_fallbackRemote = QStringLiteral("/sdcard/") + name + '-' +
            QUuid::createUuid().toString(QUuid::Id128) + '.' + info.suffix();
        // Reserve a unique file with shell noclobber; never replace existing user files.
        adb(TransferDestination,{"shell","set -C; : > " + ShellCommand::quote(m_fallbackRemote)});
        return true;
    }
    if (m_stage == TransferDestination) {
        if (!success(code,normal,out,false))
            finish(false,m_result + QStringLiteral(" 无法写入手机内部存储根目录，文件传输失败。"));
        else adb(TransferPush,{"push",m_file,m_fallbackRemote});
        return true;
    }
    if (m_stage == TransferPush) {
        if (success(code,normal,out,false)) {
            finish(m_transferOnly,m_transferOnly ? QStringLiteral("传输完成：%1").arg(m_fallbackRemote) : m_result + QStringLiteral(" 安装包已推送到手机：%1，可手动安装。").arg(m_fallbackRemote));
        } else {
            m_result += QStringLiteral(" 文件传输失败，请检查手机连接和存储空间。");
            adb(TransferCleanup,{"shell","rm -f " + ShellCommand::quote(m_fallbackRemote)});
        }
        return true;
    }
    if (m_stage == TransferCleanup) {
        if (!success(code,normal,out,false))
            m_result += QStringLiteral(" 手机端不完整文件清理失败：%1").arg(m_fallbackRemote);
        finish(false,m_result);
        return true;
    }
    return false;
}
void OpenListInstaller::chooseRoot(const QString &manager) {
    if (m_stage != RootChoice) return;
    if (!m_managers.contains(manager)) { end(false,QStringLiteral("未选择 Root 管理器，文件已保留。")); return; }
    m_selectedManager = manager; m_remoteTouched = true;
    adb(Push,{"push",m_file,m_remote});
}
void OpenListInstaller::cleanRemote(bool ok,const QString &message) {
    m_installSuccess = ok; m_result = message;
    const QString cleanup = "rm -f " + ShellCommand::quote(m_remote);
    adb(RemoteCleanup,{"shell",m_module ? ShellCommand::asRoot(cleanup) : apkRootCommand(cleanup)},10000);
}
void OpenListInstaller::end(bool ok,const QString &message) {
    if (!ok && m_canTransferFailure) {
        m_canTransferFailure = false;
        const QFileInfo info(m_file);
        if (!info.isFile() || info.isSymLink() || !OpenList::safeDirectory(info.absolutePath(),false)) {
            finish(false,message + QStringLiteral(" 安装包不可用或路径不安全，无法推送。"));
            return;
        }
        m_result = message;
        emit log(QStringLiteral("安装未成功，正在尝试将安装包推送到原目标手机的内部存储根目录。"));
        command(TransferVerify,m_adb,{"devices"},5000);
        return;
    }
    finish(ok,message);
}
void OpenListInstaller::finish(bool ok,const QString &message) {
    QString result = message;
    if (ok && !m_transferOnly) {
        if (!cleanupDownloadedFile(m_file))
            result += QStringLiteral(" 安装成功，但下载文件清理失败：%1").arg(m_file);
    } else if (!m_file.isEmpty()) result += (m_transferOnly ? QStringLiteral(" 下载文件保留在：%1") : QStringLiteral(" 安装包保留在：%1")).arg(m_file);
    m_stage = Idle; DeviceOperationLease::release(this); m_file.clear(); m_remoteTouched = false; m_canTransferFailure = false; m_fallbackRemote.clear(); m_transferOnly = false;
    emit completed(ok,result);
}
