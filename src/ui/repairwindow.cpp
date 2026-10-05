#include "processmanager.h"
#include "ougaflashwindow.h"
#include "xiaomiflashwindow.h"
#include "deviceoperationlease.h"
#include <QCloseEvent>
#include "shellcommand.h"
#include "repairwindow.h"
#include "resourceextractor.h"
#include "devicemanager.h"
#include "uihelper.h"
#include <QApplication>
#include <QScreen>
#include <QGuiApplication>
#include <QMessageBox>
#include <QFileDialog>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

RepairWindow::RepairWindow(QWidget *parent)
    : QWidget(parent)
    , repairProcess(nullptr)
    , usbFixStep(UsbFixStep::Push)
    , tmpFixStep(TmpFixStep::CreateDirectory)
    , usbFixExecutionFailed(false)
    , currentApkIndex(0)
    , successCount(0)
    , failCount(0)
    , apkInstallStep(ApkInstallStep::Push)
    , currentModuleIndex(0)
    , moduleSuccessCount(0)
    , moduleFailCount(0)
    , moduleInstallStep(ModuleInstallStep::Push)
    , rootManagerType(RootManager::Magisk)
{
    // 设置窗口标志：无边框、置顶
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground);

    // 设置窗口标题
    setWindowTitle("修复");

    setupUI();
    DeviceManager::instance()->ensureAdbOnlyMonitoring();
}

RepairWindow::~RepairWindow()
{
    DeviceManager::instance()->releaseAdbOnlyMonitoring();
    if (repairProcess) {
        repairProcess->kill();
        repairProcess->deleteLater();
    }
}

void RepairWindow::setupUI()
{
    mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(0);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    QStringList buttonTexts = {
        "USB修复",
        "修复TMP",
        "安装APK",
        "安装模块",
        "欧加线刷",
        "小米线刷"
    };

    buttons = UIHelper::createMenuButtons(this, mainLayout, buttonTexts);
    for (QPushButton *button : buttons) {
        connect(button, &QPushButton::clicked, this, &RepairWindow::onButtonClicked);
    }

    setStyleSheet(
        "RepairWindow {"
        "   background-color: rgba(195, 219, 228, 245);"
        "   border-radius: 10px;"
        "}"
    );

    // 调整窗口大小
    adjustSize();
}

void RepairWindow::setPosition(int mainMenuX, int mainMenuY, int mainMenuHeight)
{
    Q_UNUSED(mainMenuHeight);

    // 计算位置：主菜单左侧，顶部对齐，无间隔
    int x = mainMenuX - width();  // 紧贴主菜单左侧
    int y = mainMenuY;  // 顶部对齐

    move(x, y);
}

void RepairWindow::onButtonClicked()
{
    QPushButton *button = qobject_cast<QPushButton *>(sender());
    const int index = buttons.indexOf(button);
    if (index < 0) {
        return;
    }

    switch (static_cast<RepairAction>(index)) {
    case UsbFix:
        executeUsbFix();
        break;
    case TmpFix:
        fixTmpFolder();
        break;
    case InstallApk:
        installApk();
        break;
    case InstallModule:
        installModule();
        break;
    case XiaomiFlash:
        if (!xiaomiWindow) xiaomiWindow = new XiaomiFlashWindow(this);
        xiaomiWindow->showNormal();
        xiaomiWindow->raise();
        xiaomiWindow->activateWindow();
        break;
    case OugaFlash:
        if (!ougaWindow) ougaWindow = new OugaFlashWindow(this);
        ougaWindow->showNormal();
        ougaWindow->raise();
        ougaWindow->activateWindow();
        break;
    }
}

bool RepairWindow::setButtonsEnabled(bool enabled)
{
    if (!enabled && !DeviceOperationLease::acquire(this)) return false;
    if (enabled) DeviceOperationLease::release(this);
    for (QPushButton *btn : buttons) {
        btn->setEnabled(enabled);
    }
    return true;
}

void RepairWindow::executeUsbFix()
{
    if (DeviceOperationLease::busyFor(this)) return;
    // 检查设备连接
    if (DeviceManager::instance()->currentMode() != DeviceManager::ADB) {
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误", "未检测到设备，请检查设备连接与授权。", this);
        return;
    }

    QString adbPath = ResourceExtractor::getAdbPath();
    QString usbShPath = ResourceExtractor::getResourcePath() + "/usb.sh";

    // 检查文件是否存在
    if (!QFile::exists(usbShPath)) {
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误", "找不到 usb.sh 文件！", this);
        return;
    }

    if (!setButtonsEnabled(false)) return;
    buttons[UsbFix]->setText("修复中...");

    // 创建进程对象
    if (repairProcess) {
        repairProcess->deleteLater();
    }
    repairProcess = ProcessManager::createProcess(this);
    connect(repairProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) { if (e==QProcess::FailedToStart) setButtonsEnabled(true); });
    repairProcess->setWorkingDirectory(ResourceExtractor::getResourcePath());
    usbFixStep = UsbFixStep::Push;
    usbFixExecutionFailed = false;
    usbFixError.clear();

    // 第1步：推送 usb.sh 到手机
    connect(repairProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &RepairWindow::onUsbFixStep1Finished);
    connect(repairProcess, &QProcess::errorOccurred,
            this, &RepairWindow::onUsbFixProcessError);

    repairProcess->start(adbPath, QStringList() << "push" << usbShPath << "/storage/emulated/0/usb.sh");
}

void RepairWindow::onUsbFixStep1Finished(int exitCode, QProcess::ExitStatus exitStatus)
{
    disconnect(repairProcess, nullptr, this, nullptr);

    if (exitStatus != QProcess::NormalExit || exitCode != 0) {
        finishUsbFix(false, "传送 usb.sh 失败，请检查设备连接与授权。\n" +
            QString::fromLocal8Bit(repairProcess->readAllStandardError()).trimmed());
        return;
    }

    // 第2步：执行 usb.sh
    usbFixStep = UsbFixStep::Execute;
    QString adbPath = ResourceExtractor::getAdbPath();
    connect(repairProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &RepairWindow::onUsbFixStep2Finished);
    connect(repairProcess, &QProcess::errorOccurred,
            this, &RepairWindow::onUsbFixProcessError);

    repairProcess->start(adbPath, QStringList() << "shell" << "su" << "-c" << "sh /storage/emulated/0/usb.sh");
}

void RepairWindow::onUsbFixStep2Finished(int exitCode, QProcess::ExitStatus exitStatus)
{
    disconnect(repairProcess, nullptr, this, nullptr);

    if (exitStatus != QProcess::NormalExit || exitCode != 0) {
        usbFixExecutionFailed = true;
        usbFixError = QString::fromLocal8Bit(repairProcess->readAllStandardError()).trimmed();
        if (usbFixError.isEmpty()) {
            usbFixError = "执行 usb.sh 失败。";
        }
    }

    // 即使执行失败也尝试删除临时脚本，避免残留在设备上。
    startUsbFixCleanup();
}

void RepairWindow::onUsbFixStep3Finished(int exitCode, QProcess::ExitStatus exitStatus)
{
    disconnect(repairProcess, nullptr, this, nullptr);

    if (exitStatus != QProcess::NormalExit || exitCode != 0) {
        const QString cleanupError = QString::fromLocal8Bit(repairProcess->readAllStandardError()).trimmed();
        if (!cleanupError.isEmpty()) {
            usbFixError += (usbFixError.isEmpty() ? QString() : "\n") +
                "清理 usb.sh 失败：" + cleanupError;
        } else if (usbFixError.isEmpty()) {
            usbFixError = "清理 usb.sh 失败。";
        }
        usbFixExecutionFailed = true;
    }

    finishUsbFix(!usbFixExecutionFailed, usbFixError);
}

void RepairWindow::onUsbFixProcessError(QProcess::ProcessError error)
{
    if (error != QProcess::FailedToStart) {
        return;
    }

    const QString errorText = repairProcess->errorString().trimmed();
    if (usbFixStep == UsbFixStep::Execute) {
        usbFixExecutionFailed = true;
        usbFixError = errorText.isEmpty() ? "无法启动 usb.sh 执行命令。" : errorText;
        startUsbFixCleanup();
    } else if (usbFixStep == UsbFixStep::Cleanup) {
        usbFixExecutionFailed = true;
        usbFixError += (usbFixError.isEmpty() ? QString() : "\n") +
            (errorText.isEmpty() ? "无法启动清理命令。" : "清理 usb.sh 失败：" + errorText);
        finishUsbFix(false, usbFixError);
    } else {
        finishUsbFix(false, errorText.isEmpty() ? "无法启动 ADB 传送命令。" : errorText);
    }
}

void RepairWindow::finishUsbFix(bool success, const QString &detail)
{
    disconnect(repairProcess, nullptr, this, nullptr);
    setButtonsEnabled(true);
    buttons[UsbFix]->setText("USB修复");

    if (success) {
        UIHelper::showCenteredMessageBox(QMessageBox::Information, "完成", "USB修复执行完成！", this);
    } else {
        const QString message = detail.isEmpty() ? "USB修复失败。" : "USB修复失败：\n" + detail;
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误", message, this);
    }
}

void RepairWindow::startUsbFixCleanup()
{
    disconnect(repairProcess, nullptr, this, nullptr);
    usbFixStep = UsbFixStep::Cleanup;

    const QString adbPath = ResourceExtractor::getAdbPath();
    connect(repairProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &RepairWindow::onUsbFixStep3Finished);
    connect(repairProcess, &QProcess::errorOccurred,
            this, &RepairWindow::onUsbFixProcessError);
    repairProcess->start(adbPath, QStringList() << "shell" << "su" << "-c" << "rm -f /storage/emulated/0/usb.sh");
}

void RepairWindow::fixTmpFolder()
{
    if (DeviceOperationLease::busyFor(this)) return;
    // 检查设备连接
    if (DeviceManager::instance()->currentMode() != DeviceManager::ADB) {
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误", "未检测到设备，请检查设备连接与授权。", this);
        return;
    }

    if (!setButtonsEnabled(false)) return;
    buttons[TmpFix]->setText("修复中...");

    // 创建进程对象
    if (repairProcess) {
        repairProcess->deleteLater();
    }
    repairProcess = ProcessManager::createProcess(this);
    connect(repairProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) { if (e==QProcess::FailedToStart) setButtonsEnabled(true); });
    repairProcess->setWorkingDirectory(ResourceExtractor::getResourcePath());

    // 第1步：创建目录
    startTmpFixCommand(QStringList() << "shell" << "su" << "-c" << "mkdir -p /data/local/tmp",
                       TmpFixStep::CreateDirectory);
}

void RepairWindow::onTmpFixStepFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    disconnect(repairProcess, nullptr, this, nullptr);
    QString adbPath = ResourceExtractor::getAdbPath();
    const bool succeeded = exitStatus == QProcess::NormalExit && exitCode == 0;

    switch (tmpFixStep) {
    case TmpFixStep::CreateDirectory:
        if (succeeded) {
            startTmpFixCommand(QStringList() << "shell" << "su" << "-c" << "chcon -R u:object_r:shell_data_file:s0 /data/local/tmp",
                               TmpFixStep::SetContext);
        } else {
            startTmpFixCommand(QStringList() << "shell" << "su" << "-s" << "mkdir -p /data/local/tmp",
                               TmpFixStep::CreateDirectoryFallback);
        }
        break;
    case TmpFixStep::CreateDirectoryFallback:
        if (succeeded) {
            startTmpFixCommand(QStringList() << "shell" << "su" << "-c" << "chcon -R u:object_r:shell_data_file:s0 /data/local/tmp",
                               TmpFixStep::SetContext);
        } else {
            finishTmpFix(false, "创建 /data/local/tmp 失败：" +
                QString::fromLocal8Bit(repairProcess->readAllStandardError()).trimmed());
        }
        break;
    case TmpFixStep::SetContext:
        if (succeeded) {
            startTmpFixCommand(QStringList() << "shell" << "su" << "-c" << "chmod 777 /data/local/tmp",
                               TmpFixStep::SetPermissions);
        } else {
            startTmpFixCommand(QStringList() << "shell" << "su" << "-s" << "chcon -R u:object_r:shell_data_file:s0 /data/local/tmp",
                               TmpFixStep::SetContextFallback);
        }
        break;
    case TmpFixStep::SetContextFallback:
        if (succeeded) {
            startTmpFixCommand(QStringList() << "shell" << "su" << "-c" << "chmod 777 /data/local/tmp",
                               TmpFixStep::SetPermissions);
        } else {
            finishTmpFix(false, "设置 /data/local/tmp 的 SELinux 上下文失败：" +
                QString::fromLocal8Bit(repairProcess->readAllStandardError()).trimmed());
        }
        break;
    case TmpFixStep::SetPermissions:
        if (succeeded) {
            finishTmpFix(true, QString());
        } else {
            startTmpFixCommand(QStringList() << "shell" << "su" << "-s" << "chmod 777 /data/local/tmp",
                               TmpFixStep::SetPermissionsFallback);
        }
        break;
    case TmpFixStep::SetPermissionsFallback:
        if (succeeded) {
            finishTmpFix(true, QString());
        } else {
            finishTmpFix(false, "设置 /data/local/tmp 权限失败：" +
                QString::fromLocal8Bit(repairProcess->readAllStandardError()).trimmed());
        }
        break;
    }
}

void RepairWindow::onTmpFixProcessError(QProcess::ProcessError error)
{
    if (error == QProcess::FailedToStart) {
        const QString errorText = repairProcess->errorString().trimmed();
        finishTmpFix(false, errorText.isEmpty() ? "无法启动修复命令。" : errorText);
    }
}

void RepairWindow::finishTmpFix(bool success, const QString &detail)
{
    disconnect(repairProcess, nullptr, this, nullptr);
    setButtonsEnabled(true);
    buttons[TmpFix]->setText("修复TMP");

    if (success) {
        UIHelper::showCenteredMessageBox(QMessageBox::Information, "完成", "/data/local/tmp 修复完成！", this);
    } else {
        const QString message = detail.isEmpty() ? "/data/local/tmp 修复失败。" : "/data/local/tmp 修复失败：\n" + detail;
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误", message, this);
    }
}

void RepairWindow::startTmpFixCommand(const QStringList &arguments, TmpFixStep step)
{
    disconnect(repairProcess, nullptr, this, nullptr);
    tmpFixStep = step;
    connect(repairProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &RepairWindow::onTmpFixStepFinished);
    connect(repairProcess, &QProcess::errorOccurred,
            this, &RepairWindow::onTmpFixProcessError);
    repairProcess->start(ResourceExtractor::getAdbPath(), arguments);
}

void RepairWindow::installApk()
{
    if (DeviceOperationLease::busyFor(this)) return;
    // 检查设备连接
    if (DeviceManager::instance()->currentMode() != DeviceManager::ADB) {
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误", "未检测到设备，请检查设备连接与授权。", this);
        return;
    }

    // 让用户选择APK文件或文件夹
    QMessageBox::StandardButton choice = UIHelper::showCenteredQuestion("选择安装方式",
        "请选择安装方式：\n\n"
        "点击 [是] - 选择单个APK文件\n"
        "点击 [否] - 选择包含APK的文件夹", this);

    apkFilesToInstall.clear();
    QString desktopPath = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);

    if (choice == QMessageBox::Yes) {
        // 选择单个APK文件
        QStringList files = QFileDialog::getOpenFileNames(
            this,
            "选择APK文件",
            desktopPath,
            "APK文件 (*.apk);;All Files (*.*)"
        );

        if (files.isEmpty()) {
            return;
        }

        apkFilesToInstall = files;
    } else if (choice == QMessageBox::No) {
        // 选择文件夹
        QString folder = QFileDialog::getExistingDirectory(
            this,
            "选择包含APK的文件夹",
            desktopPath,
            QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks
        );

        if (folder.isEmpty()) {
            return;
        }

        // 扫描文件夹中的所有APK文件
        QDir dir(folder);
        QStringList apkFiles = dir.entryList(QStringList() << "*.apk", QDir::Files);

        if (apkFiles.isEmpty()) {
            UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误", "所选文件夹中没有找到APK文件！", this);
            return;
        }

        for (const QString &apk : apkFiles) {
            apkFilesToInstall.append(folder + "/" + apk);
        }
    } else {
        // 用户取消
        return;
    }

    // 确认安装
    QString confirmMsg = QString("即将安装 %1 个APK文件：\n\n").arg(apkFilesToInstall.size());
    for (int i = 0; i < qMin(apkFilesToInstall.size(), 5); ++i) {
        confirmMsg += QFileInfo(apkFilesToInstall[i]).fileName() + "\n";
    }
    if (apkFilesToInstall.size() > 5) {
        confirmMsg += QString("...等共 %1 个文件").arg(apkFilesToInstall.size());
    }
    confirmMsg += "\n是否继续？";

    QMessageBox::StandardButton reply = UIHelper::showCenteredQuestion("确认安装", confirmMsg, this);
    if (reply != QMessageBox::Yes) {
        return;
    }

    // 开始安装
    if (!setButtonsEnabled(false)) return;
    buttons[InstallApk]->setText("安装中...");

    currentApkIndex = 0;
    successCount = 0;
    failCount = 0;
    apkInstallStep = ApkInstallStep::Push;

    // 创建进程对象
    if (repairProcess) {
        repairProcess->deleteLater();
    }
    repairProcess = ProcessManager::createProcess(this);
    connect(repairProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) { if (e==QProcess::FailedToStart) setButtonsEnabled(true); });
    repairProcess->setWorkingDirectory(ResourceExtractor::getResourcePath());

    connect(repairProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &RepairWindow::onApkInstallStepFinished);

    // 第一步：推送APK到设备
    QString adbPath = ResourceExtractor::getAdbPath();
    QString apkPath = apkFilesToInstall[currentApkIndex];
    QString apkName = QFileInfo(apkPath).fileName();
    QString remotePath = "/data/local/tmp/" + apkName;

    buttons[InstallApk]->setText(QString("推送中(%1/%2)").arg(1).arg(apkFilesToInstall.size()));
    repairProcess->start(adbPath, QStringList() << "push" << apkPath << remotePath);
}

void RepairWindow::onApkInstallStepFinished()
{
    int exitCode = repairProcess->exitCode();
    QString output = QString::fromLocal8Bit(repairProcess->readAllStandardOutput());
    QString error = QString::fromLocal8Bit(repairProcess->readAllStandardError());
    QString adbPath = ResourceExtractor::getAdbPath();
    QString apkPath = apkFilesToInstall[currentApkIndex];
    QString apkName = QFileInfo(apkPath).fileName();
    QString remotePath = "/data/local/tmp/" + apkName;

    switch (apkInstallStep) {
    case ApkInstallStep::Push:  // push完成
        if (exitCode != 0) {
            qDebug() << "推送失败:" << apkName << error;
            failCount++;
            // 跳过这个APK，继续下一个
            currentApkIndex++;
            apkInstallStep = ApkInstallStep::Push;
            if (currentApkIndex < apkFilesToInstall.size()) {
                QString nextApk = apkFilesToInstall[currentApkIndex];
                QString nextName = QFileInfo(nextApk).fileName();
                QString nextRemote = "/data/local/tmp/" + nextName;
                buttons[InstallApk]->setText(QString("推送中(%1/%2)").arg(currentApkIndex + 1).arg(apkFilesToInstall.size()));
                repairProcess->start(adbPath, QStringList() << "push" << nextApk << nextRemote);
            } else {
                goto finished;
            }
        } else {
            // push成功，执行pm install (su -c)
            apkInstallStep = ApkInstallStep::InstallWithSuC;
            buttons[InstallApk]->setText(QString("安装中(%1/%2)").arg(currentApkIndex + 1).arg(apkFilesToInstall.size()));
            // 路径先按内层 shell 引用，再引用完整的 su 命令字符串
            repairProcess->start(adbPath, QStringList() << "shell" << ShellCommand::asRoot("pm install -r " + ShellCommand::quote(remotePath)));
        }
        break;

    case ApkInstallStep::InstallWithSuC:  // install with -c 完成
        if (output.contains("Success") || output.contains("success") || error.contains("Success") || error.contains("success")) {
            successCount++;
            qDebug() << "安装成功:" << apkName;
            // 删除临时文件
            apkInstallStep = ApkInstallStep::DeleteTemporaryFile;
            repairProcess->start(adbPath, QStringList() << "shell" << ShellCommand::asRoot("rm -f " + ShellCommand::quote(remotePath)));
        } else {
            // -c 失败，尝试使用 -s
            qDebug() << "su -c 失败，尝试 su -s:" << apkName;
            apkInstallStep = ApkInstallStep::InstallWithSuS;
            repairProcess->start(adbPath, QStringList() << "shell" << ShellCommand::asRoot("pm install -r " + ShellCommand::quote(remotePath), "-s"));
        }
        break;

    case ApkInstallStep::InstallWithSuS:  // install with -s 完成
        if (output.contains("Success") || output.contains("success") || error.contains("Success") || error.contains("success")) {
            successCount++;
            qDebug() << "安装成功(su -s):" << apkName;
        } else {
            failCount++;
            qDebug() << "安装失败:" << apkName << output << error;
        }
        // 删除临时文件
        apkInstallStep = ApkInstallStep::DeleteTemporaryFile;
        repairProcess->start(adbPath, QStringList() << "shell" << ShellCommand::asRoot("rm -f " + ShellCommand::quote(remotePath)));
        break;

    case ApkInstallStep::DeleteTemporaryFile:  // delete完成
        // 继续下一个APK
        currentApkIndex++;
        apkInstallStep = ApkInstallStep::Push;

        if (currentApkIndex < apkFilesToInstall.size()) {
            QString nextApk = apkFilesToInstall[currentApkIndex];
            QString nextName = QFileInfo(nextApk).fileName();
            QString nextRemote = "/data/local/tmp/" + nextName;
            buttons[InstallApk]->setText(QString("推送中(%1/%2)").arg(currentApkIndex + 1).arg(apkFilesToInstall.size()));
            repairProcess->start(adbPath, QStringList() << "push" << nextApk << nextRemote);
        } else {
            goto finished;
        }
        break;
    }
    return;

finished:
    // 全部安装完成
    disconnect(repairProcess, nullptr, this, nullptr);
    setButtonsEnabled(true);
    buttons[InstallApk]->setText("安装APK");

    // 只有失败时才显示结果
    if (failCount > 0) {
        QString resultMsg = QString("安装完成！\n\n"
            "成功：%1 个\n"
            "失败：%2 个").arg(successCount).arg(failCount);
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "安装结果", resultMsg, this);
    }
}

void RepairWindow::installModule()
{
    if (DeviceOperationLease::busyFor(this)) return;
    // 检查设备连接
    if (DeviceManager::instance()->currentMode() != DeviceManager::ADB) {
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误", "未检测到设备，请检查设备连接与授权。", this);
        return;
    }

    // 让用户选择模块文件或文件夹
    QMessageBox::StandardButton choice = UIHelper::showCenteredQuestion("选择安装方式",
        "请选择安装方式：\n\n"
        "点击 [是] - 选择单个或多个ZIP模块文件\n"
        "点击 [否] - 选择包含模块的文件夹", this);

    moduleFilesToInstall.clear();
    QString desktopPath = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);

    if (choice == QMessageBox::Yes) {
        // 选择ZIP文件
        QStringList files = QFileDialog::getOpenFileNames(
            this,
            "选择模块文件",
            desktopPath,
            "模块文件 (*.zip);;All Files (*.*)"
        );

        if (files.isEmpty()) {
            return;
        }

        moduleFilesToInstall = files;
    } else if (choice == QMessageBox::No) {
        // 选择文件夹
        QString folder = QFileDialog::getExistingDirectory(
            this,
            "选择包含模块的文件夹",
            desktopPath,
            QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks
        );

        if (folder.isEmpty()) {
            return;
        }

        // 扫描文件夹中的所有ZIP文件
        QDir dir(folder);
        QStringList zipFiles = dir.entryList(QStringList() << "*.zip", QDir::Files);

        if (zipFiles.isEmpty()) {
            UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误", "所选文件夹中没有找到ZIP模块文件！", this);
            return;
        }

        for (const QString &zip : zipFiles) {
            moduleFilesToInstall.append(folder + "/" + zip);
        }
    } else {
        return;
    }

    // 禁用按钮，开始检测Root管理器
    if (!setButtonsEnabled(false)) return;
    buttons[InstallModule]->setText("检测中...");

    // 创建进程对象
    if (repairProcess) {
        repairProcess->deleteLater();
    }
    repairProcess = ProcessManager::createProcess(this);
    connect(repairProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) { if (e==QProcess::FailedToStart) setButtonsEnabled(true); });
    repairProcess->setWorkingDirectory(ResourceExtractor::getResourcePath());

    connect(repairProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &RepairWindow::onRootDetectFinished);

    // 检测Root管理器类型：检查特定目录是否存在（需要root权限）
    QString adbPath = ResourceExtractor::getAdbPath();
    // Magisk: /data/adb/magisk
    // APatch: /data/adb/ap
    // KernelSU: /data/adb/ksu
    // 使用 su -c 执行，因为 /data/adb 需要root权限访问
    repairProcess->start(adbPath, QStringList() << "shell"
        << "su -c 'echo MAGISK:$([ -d /data/adb/magisk ] && echo YES || echo NO):AP:$([ -d /data/adb/ap ] && echo YES || echo NO):KSU:$([ -d /data/adb/ksu ] && echo YES || echo NO)'");
}

void RepairWindow::onRootDetectFinished()
{
    disconnect(repairProcess, nullptr, this, nullptr);

    QString output = QString::fromLocal8Bit(repairProcess->readAllStandardOutput());
    QString error = QString::fromLocal8Bit(repairProcess->readAllStandardError());
    QString combined = output + error;

    qDebug() << "Root检测结果:" << combined;

    // 解析检测结果
    bool hasMagisk = combined.contains("MAGISK:YES");
    bool hasApatch = combined.contains("AP:YES");
    bool hasKsu = combined.contains("KSU:YES");

    // 确定使用哪个Root管理器（优先级：APatch > KSU > Magisk）
    QString detectedRoot;
    if (hasApatch) {
        rootManagerType = RootManager::APatch;
        detectedRoot = "APatch";
    } else if (hasKsu) {
        rootManagerType = RootManager::KernelSU;
        detectedRoot = "KernelSU";
    } else if (hasMagisk) {
        rootManagerType = RootManager::Magisk;
        detectedRoot = "Magisk";
    } else {
        // 未检测到Root管理器
        setButtonsEnabled(true);
        buttons[InstallModule]->setText("安装模块");
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误",
            "未检测到支持的Root管理器！\n\n"
            "支持的类型：Magisk/Alpha、APatch、KernelSU", this);
        return;
    }

    // 确认安装
    QString confirmMsg = QString("检测到 %1，即将安装 %2 个模块：\n\n").arg(detectedRoot).arg(moduleFilesToInstall.size());
    for (int i = 0; i < qMin(moduleFilesToInstall.size(), 5); ++i) {
        confirmMsg += QFileInfo(moduleFilesToInstall[i]).fileName() + "\n";
    }
    if (moduleFilesToInstall.size() > 5) {
        confirmMsg += QString("...等共 %1 个文件").arg(moduleFilesToInstall.size());
    }
    confirmMsg += "\n是否继续？";

    QMessageBox::StandardButton reply = UIHelper::showCenteredQuestion("确认安装", confirmMsg, this);
    if (reply != QMessageBox::Yes) {
        setButtonsEnabled(true);
        buttons[InstallModule]->setText("安装模块");
        return;
    }

    // 开始安装
    startModuleInstall();
}

void RepairWindow::startModuleInstall()
{
    if (DeviceOperationLease::busyFor(this)) return;
    buttons[InstallModule]->setText("安装中...");

    currentModuleIndex = 0;
    moduleSuccessCount = 0;
    moduleFailCount = 0;
    moduleInstallStep = ModuleInstallStep::Push;

    connect(repairProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &RepairWindow::onModuleInstallStepFinished);

    // 第一步：推送模块到设备
    QString adbPath = ResourceExtractor::getAdbPath();
    QString modulePath = moduleFilesToInstall[currentModuleIndex];
    QString moduleName = QFileInfo(modulePath).fileName();
    QString remotePath = "/data/local/tmp/" + moduleName;

    buttons[InstallModule]->setText(QString("推送中(%1/%2)").arg(1).arg(moduleFilesToInstall.size()));
    repairProcess->start(adbPath, QStringList() << "push" << modulePath << remotePath);
}

void RepairWindow::onModuleInstallStepFinished()
{
    int exitCode = repairProcess->exitCode();
    QString output = QString::fromLocal8Bit(repairProcess->readAllStandardOutput());
    QString error = QString::fromLocal8Bit(repairProcess->readAllStandardError());
    QString adbPath = ResourceExtractor::getAdbPath();
    QString modulePath = moduleFilesToInstall[currentModuleIndex];
    QString moduleName = QFileInfo(modulePath).fileName();
    QString remotePath = "/data/local/tmp/" + moduleName;

    switch (moduleInstallStep) {
    case ModuleInstallStep::Push:  // push完成
        if (exitCode != 0) {
            qDebug() << "推送失败:" << moduleName << error;
            moduleFailCount++;
            // 跳过这个模块，继续下一个
            currentModuleIndex++;
            moduleInstallStep = ModuleInstallStep::Push;
            if (currentModuleIndex < moduleFilesToInstall.size()) {
                QString nextModule = moduleFilesToInstall[currentModuleIndex];
                QString nextName = QFileInfo(nextModule).fileName();
                QString nextRemote = "/data/local/tmp/" + nextName;
                buttons[InstallModule]->setText(QString("推送中(%1/%2)").arg(currentModuleIndex + 1).arg(moduleFilesToInstall.size()));
                repairProcess->start(adbPath, QStringList() << "push" << nextModule << nextRemote);
            } else {
                goto module_finished;
            }
        } else {
            // push成功，执行安装命令
            moduleInstallStep = ModuleInstallStep::Install;
            buttons[InstallModule]->setText(QString("安装中(%1/%2)").arg(currentModuleIndex + 1).arg(moduleFilesToInstall.size()));

            // 根据Root管理器类型执行不同的安装命令
            QString installCmd;
            switch (rootManagerType) {
                case RootManager::Magisk:  // Magisk/Alpha
                    installCmd = QString("magisk --install-module %1").arg(ShellCommand::quote(remotePath));
                    break;
                case RootManager::APatch:  // APatch
                    installCmd = QString("/data/adb/ap/bin/apd module install %1").arg(ShellCommand::quote(remotePath));
                    break;
                case RootManager::KernelSU:  // KernelSU
                    installCmd = QString("/data/adb/ksu/bin/ksud module install %1").arg(ShellCommand::quote(remotePath));
                    break;
            }
            repairProcess->start(adbPath, QStringList() << "shell" << ShellCommand::asRoot(installCmd));
        }
        break;

    case ModuleInstallStep::Install:  // install完成
        {
            // 检查安装结果
            bool success = false;
            QString combined = output + error;

            // Magisk 成功标志
            if (combined.contains("Done") || combined.contains("Success") ||
                combined.contains("success") || combined.contains("installed") ||
                (exitCode == 0 && !combined.contains("Error") && !combined.contains("error") && !combined.contains("failed"))) {
                success = true;
            }

            if (success) {
                moduleSuccessCount++;
                qDebug() << "模块安装成功:" << moduleName;
            } else {
                moduleFailCount++;
                qDebug() << "模块安装失败:" << moduleName << combined;
            }

            // 删除临时文件
            moduleInstallStep = ModuleInstallStep::DeleteTemporaryFile;
            repairProcess->start(adbPath, QStringList() << "shell" << ShellCommand::asRoot("rm -f " + ShellCommand::quote(remotePath)));
        }
        break;

    case ModuleInstallStep::DeleteTemporaryFile:  // delete完成
        // 继续下一个模块
        currentModuleIndex++;
        moduleInstallStep = ModuleInstallStep::Push;

        if (currentModuleIndex < moduleFilesToInstall.size()) {
            QString nextModule = moduleFilesToInstall[currentModuleIndex];
            QString nextName = QFileInfo(nextModule).fileName();
            QString nextRemote = "/data/local/tmp/" + nextName;
            buttons[InstallModule]->setText(QString("推送中(%1/%2)").arg(currentModuleIndex + 1).arg(moduleFilesToInstall.size()));
            repairProcess->start(adbPath, QStringList() << "push" << nextModule << nextRemote);
        } else {
            goto module_finished;
        }
        break;
    }
    return;

module_finished:
    // 全部安装完成
    disconnect(repairProcess, nullptr, this, nullptr);

    if (moduleFailCount > 0) {
        // 有失败，显示结果
        setButtonsEnabled(true);
        buttons[InstallModule]->setText("安装模块");
        QString resultMsg = QString("模块安装完成！\n\n"
            "成功：%1 个\n"
            "失败：%2 个").arg(moduleSuccessCount).arg(moduleFailCount);
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "安装结果", resultMsg, this);
    } else {
        // 全部成功，自动重启设备
        buttons[InstallModule]->setText("重启中...");

        connect(repairProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this](int, QProcess::ExitStatus) {
            disconnect(repairProcess, nullptr, this, nullptr);
            setButtonsEnabled(true);
            buttons[InstallModule]->setText("安装模块");
        });

        QString adbPath = ResourceExtractor::getAdbPath();
        repairProcess->start(adbPath, QStringList() << "reboot");
    }
}

bool RepairWindow::hasActiveOugaTask() const {
    if ((xiaomiWindow && xiaomiWindow->isBusy()) || (ougaWindow && ougaWindow->isBusy())) return true;
    // Taskbar flash windows have no QObject parent: protect both launchers
    // explicitly, including read-only discovery and modal operations.
    for (QObject *owner = DeviceOperationLease::owner(); owner; owner = owner->parent())
        if (owner == ougaWindow || owner == xiaomiWindow) return true;
    return false;
}
void RepairWindow::closeEvent(QCloseEvent *event) {
    if (hasActiveOugaTask() || DeviceOperationLease::owner()==this) {
        event->ignore();
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "操作进行中", "请等待设备操作结束；不能销毁所属菜单。", this);
        return;
    }
    QWidget::closeEvent(event);
}
