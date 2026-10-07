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
    connect(m_process,&QProcess::readyReadStandardOutput,this,[this] {
        QString text = QString::fromLocal8Bit(m_process->readAllStandardOutput());
        if (m_output.size() + text.size() <= 2 * 1024 * 1024) m_output += text;
        else m_truncated = true;
        emit output(text.left(4000)); m_watchdog.start();
    });
    connect(m_process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,
            [this](int code,QProcess::ExitStatus status) { finish(code,status == QProcess::NormalExit); });
    connect(m_process,&QProcess::errorOccurred,this,[this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) finish(-1,false);
    });
}
void OpenListProcessRunner::run(const QString &program,const QStringList &args,int timeout) {
    if (m_running) return;
    m_running = true; m_truncated = false; m_output.clear();
    m_process->setWorkingDirectory(QFileInfo(program).absolutePath());
    if (timeout > 0) m_deadline.start(timeout);
    m_watchdog.start(); m_process->start(program,args);
}
void OpenListProcessRunner::finish(int code,bool normal) {
    if (!m_running) return;
    const auto tail = QString::fromLocal8Bit(m_process->readAllStandardOutput());
    if (m_output.size() + tail.size() <= 2 * 1024 * 1024) m_output += tail;
    else m_truncated = true;
    m_running = false; m_deadline.stop(); m_watchdog.stop();
    if (m_truncated) { normal = false; m_output += QStringLiteral("\nERROR: 输出过长，结果无法确认\n"); }
    emit completed(code,normal,m_output);
}
OpenListInstaller::OpenListInstaller(QObject *parent,OugaCommandRunner *runner,const QString &adbPath,const QString &sevenZip)
    : QObject(parent), m_runner(runner ? runner : new OpenListProcessRunner(this)),
      m_adb(adbPath.isEmpty() ? ResourceExtractor::getAdbPath() : adbPath),
      m_sevenZip(sevenZip.isEmpty() ? QDir(ResourceExtractor::getResourcePath()).filePath("bin/7zip/7z.exe") : sevenZip) {
    connect(m_runner,&OugaCommandRunner::completed,this,&OpenListInstaller::onCompleted);
    connect(m_runner,&OugaCommandRunner::output,this,[this](QString text) {
        // ZIP listing and root probes contain implementation paths, not user logs.
        if (m_stage == ApkInstall || m_stage == Push || m_stage == ModuleInstall) {
            text.replace(m_file,QStringLiteral("[本次安装包]"));
            if (!m_remote.isEmpty()) text.replace(m_remote,QStringLiteral("[手机临时文件]"));
            emit log(OpenList::redact(text));
        }
    });
    connect(m_runner,&OugaCommandRunner::stalled,this,[this] {
        emit log(QStringLiteral("安装器暂时没有新输出，请检查手机授权；不会强制停止安装。"));
    });
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
    if (!DeviceOperationLease::acquire(this)) { emit completed(false,QStringLiteral("其他设备操作尚未结束。")); return false; }
    command(Discover,m_adb,{"devices"},5000); return true;
}
bool OpenListInstaller::install(const QString &file,bool module,const QString &serial) {
    if (busy()) return false;
    if (serial.isEmpty() || serial.contains(QRegularExpression("\\s")) || !DeviceOperationLease::acquire(this)) {
        emit completed(false,QStringLiteral("目标设备无效或设备操作被占用，文件已保留：%1").arg(file)); return false;
    }
    m_file = file; m_serial = serial; m_module = module; m_remoteTouched = false; m_managers.clear();
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
    m_remote = "/data/local/tmp/orange-openlist-" + QUuid::createUuid().toString(QUuid::Id128) + ".zip";
    command(Verify,m_adb,{"devices"},5000); return true;
}
void OpenListInstaller::command(Stage stage,const QString &program,const QStringList &args,int timeout) {
    m_stage = stage; m_runner->run(program,args,timeout);
}
void OpenListInstaller::adb(Stage stage,const QStringList &args,int timeout) {
    command(stage,m_adb,QStringList{"-s",m_serial} + args,timeout);
}
void OpenListInstaller::onCompleted(int code,bool normal,const QString &out) {
    if (m_stage == Idle || m_stage == RootChoice) return;
    if (m_stage == Discover) {
        const auto serials = authorizedDevices(out);
        m_stage = Idle; DeviceOperationLease::release(this);
        if (!normal || code != 0) emit completed(false,QStringLiteral("设备检测失败或超时。"));
        else if (serials.isEmpty()) emit completed(false,QStringLiteral("没有已授权的 ADB 设备，请检查连接和手机授权。"));
        else emit devicesReady(serials);
        return;
    }
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
        const QString detail = OpenList::redact(out);
        if (m_stage == ApkInstall || m_stage == ModuleInstall) emit log(detail);
        if (m_remoteTouched) cleanRemote(false,reason); else end(false,reason);
        return;
    }
    if (m_stage == ModuleInstall && !out.split(QRegularExpression("[\\r\\n]+"),Qt::SkipEmptyParts).contains("OL_INSTALL_SUCCESS")) {
        cleanRemote(false,QStringLiteral("模块安装结果无法确认。")); return;
    }
    switch (m_stage) {
    case Verify:
        if (!authorizedDevices(out).contains(m_serial)) { end(false,QStringLiteral("原目标手机已断开或未授权，不会切换设备。")); break; }
        if (!m_module) { emit log(QStringLiteral("正在安装 APK。")); adb(ApkInstall,{"install","-r",m_file}); }
        else command(ZipList,m_sevenZip,{"l","-slt","-ba","-pOrangeOpenListNoPassword","--",m_file},60000);
        break;
    case ZipList:
        if (!validModuleListing(out)) { end(false,QStringLiteral("不是有效 Root 模块：需要唯一的根目录 module.prop，且不能加密。")); break; }
        command(ZipTest,m_sevenZip,{"t","-bd","-pOrangeOpenListNoPassword","--",m_file},60000); break;
    case ZipTest:
        command(ZipProperties,m_sevenZip,{"e","-so","-pOrangeOpenListNoPassword","--",m_file,"module.prop"},60000); break;
    case ZipProperties:
        if (!validModuleProperties(out)) { end(false,QStringLiteral("模块描述缺少有效 id 或 name，拒绝安装。")); break; }
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
void OpenListInstaller::chooseRoot(const QString &manager) {
    if (m_stage != RootChoice) return;
    if (!m_managers.contains(manager)) { end(false,QStringLiteral("未选择 Root 管理器，文件已保留。")); return; }
    m_selectedManager = manager; m_remoteTouched = true;
    adb(Push,{"push",m_file,m_remote});
}
void OpenListInstaller::cleanRemote(bool ok,const QString &message) {
    m_installSuccess = ok; m_result = message;
    adb(RemoteCleanup,{"shell",ShellCommand::asRoot("rm -f " + ShellCommand::quote(m_remote))},10000);
}
void OpenListInstaller::end(bool ok,const QString &message) {
    QString result = message;
    if (ok) {
        if (cleanupDownloadedFile(m_file)) result += QStringLiteral(" 下载文件已自动删除。");
        else result += QStringLiteral(" 安装成功，但下载文件清理失败：%1").arg(m_file);
    } else if (!m_file.isEmpty()) result += QStringLiteral(" 安装包保留在：%1").arg(m_file);
    m_stage = Idle; DeviceOperationLease::release(this); m_file.clear(); m_remoteTouched = false;
    emit completed(ok,result);
}
