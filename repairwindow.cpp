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
    , tmpFixStep(0)
    , currentApkIndex(0)
    , successCount(0)
    , failCount(0)
    , apkInstallStep(0)
    , currentModuleIndex(0)
    , moduleSuccessCount(0)
    , moduleFailCount(0)
    , moduleInstallStep(0)
    , rootManagerType(0)
{
    // 设置窗口标志：无边框、置顶
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground);
    
    // 设置窗口标题
    setWindowTitle("修复");
    
    setupUI();
}

RepairWindow::~RepairWindow()
{
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
        "安装模块"
    };
    
    for (int i = 0; i < buttonTexts.size(); ++i) {
        QPushButton *btn = new QPushButton(buttonTexts[i], this);
        btn->setMinimumHeight(45);
        btn->setMaximumHeight(45);
        btn->setMinimumWidth(120);
        btn->setStyleSheet(UIHelper::getButtonStyle(i, buttonTexts.size()));
        
        connect(btn, &QPushButton::clicked, this, &RepairWindow::onButtonClicked);
        buttons.append(btn);
        mainLayout->addWidget(btn);
    }
    
    setStyleSheet(
        "RepairWindow {"
        "   background-color: rgba(180, 210, 220, 240);"
        "   border-radius: 12px;"
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
    QPushButton *btn = qobject_cast<QPushButton*>(sender());
    if (!btn) return;
    
    int index = buttons.indexOf(btn);
    
    // 修复选项1：USB修复
    if (index == 0) {
        executeUsbFix();
        return;
    }
    
    // 修复选项2：修复 /data/local/tmp
    if (index == 1) {
        fixTmpFolder();
        return;
    }
    
    // 安装APK
    if (index == 2) {
        installApk();
        return;
    }
    
    // 安装模块
    if (index == 3) {
        installModule();
        return;
    }
}

void RepairWindow::setButtonsEnabled(bool enabled)
{
    for (QPushButton *btn : buttons) {
        btn->setEnabled(enabled);
    }
}

void RepairWindow::executeUsbFix()
{
    // 检查设备连接
    if (!DeviceManager::instance()->isDeviceConnected()) {
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
    
    setButtonsEnabled(false);
    buttons[0]->setText("修复中...");
    
    // 创建进程对象
    if (repairProcess) {
        repairProcess->deleteLater();
    }
    repairProcess = new QProcess(this);
    repairProcess->setWorkingDirectory(ResourceExtractor::getResourcePath());
    
    // 第1步：推送 usb.sh 到手机
    connect(repairProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &RepairWindow::onUsbFixStep1Finished);
    
    repairProcess->start(adbPath, QStringList() << "push" << usbShPath << "/storage/emulated/0/usb.sh");
}

void RepairWindow::onUsbFixStep1Finished(int exitCode, QProcess::ExitStatus)
{
    disconnect(repairProcess, nullptr, this, nullptr);
    
    if (exitCode != 0) {
        setButtonsEnabled(true);
        buttons[0]->setText("USB修复");
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误", "传送 usb.sh 失败，请检查设备连接与授权。", this);
        return;
    }
    
    // 第2步：执行 usb.sh
    QString adbPath = ResourceExtractor::getAdbPath();
    connect(repairProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &RepairWindow::onUsbFixStep2Finished);
    
    repairProcess->start(adbPath, QStringList() << "shell" << "su" << "-c" << "sh /storage/emulated/0/usb.sh");
}

void RepairWindow::onUsbFixStep2Finished(int, QProcess::ExitStatus)
{
    disconnect(repairProcess, nullptr, this, nullptr);
    
    // 第3步：删除 usb.sh
    QString adbPath = ResourceExtractor::getAdbPath();
    connect(repairProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &RepairWindow::onUsbFixStep3Finished);
    
    repairProcess->start(adbPath, QStringList() << "shell" << "su" << "-c" << "rm -r /storage/emulated/0/usb.sh");
}

void RepairWindow::onUsbFixStep3Finished(int, QProcess::ExitStatus)
{
    disconnect(repairProcess, nullptr, this, nullptr);
    
    setButtonsEnabled(true);
    buttons[0]->setText("USB修复");
    UIHelper::showCenteredMessageBox(QMessageBox::Information, "完成", "USB修复执行完成！", this);
}

void RepairWindow::fixTmpFolder()
{
    // 检查设备连接
    if (!DeviceManager::instance()->isDeviceConnected()) {
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误", "未检测到设备，请检查设备连接与授权。", this);
        return;
    }
    
    setButtonsEnabled(false);
    buttons[1]->setText("修复中...");
    
    // 创建进程对象
    if (repairProcess) {
        repairProcess->deleteLater();
    }
    repairProcess = new QProcess(this);
    repairProcess->setWorkingDirectory(ResourceExtractor::getResourcePath());
    
    tmpFixStep = 0;
    connect(repairProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &RepairWindow::onTmpFixStepFinished);
    
    // 第1步：创建目录
    QString adbPath = ResourceExtractor::getAdbPath();
    repairProcess->start(adbPath, QStringList() << "shell" << "su" << "-c" << "mkdir -p /data/local/tmp");
}

void RepairWindow::onTmpFixStepFinished()
{
    QString adbPath = ResourceExtractor::getAdbPath();
    int exitCode = repairProcess->exitCode();
    
    tmpFixStep++;
    
    switch (tmpFixStep) {
    case 1:
        // 如果第1步失败，尝试使用 -s 参数
        if (exitCode != 0) {
            repairProcess->start(adbPath, QStringList() << "shell" << "su" << "-s" << "mkdir -p /data/local/tmp");
        } else {
            // 成功，继续第2步
            tmpFixStep++;
            repairProcess->start(adbPath, QStringList() << "shell" << "su" << "-c" << "chcon -R u:object_r:shell_data_file:s0 /data/local/tmp");
        }
        break;
    case 2:
        // 第2步：设置 SELinux 上下文
        if (exitCode != 0) {
            repairProcess->start(adbPath, QStringList() << "shell" << "su" << "-s" << "chcon -R u:object_r:shell_data_file:s0 /data/local/tmp");
        } else {
            tmpFixStep++;
            repairProcess->start(adbPath, QStringList() << "shell" << "su" << "-c" << "chmod 777 /data/local/tmp");
        }
        break;
    case 3:
        // 第3步：设置权限
        if (exitCode != 0) {
            repairProcess->start(adbPath, QStringList() << "shell" << "su" << "-s" << "chmod 777 /data/local/tmp");
        } else {
            // 完成
            disconnect(repairProcess, nullptr, this, nullptr);
            setButtonsEnabled(true);
            buttons[1]->setText("修复TMP");
            UIHelper::showCenteredMessageBox(QMessageBox::Information, "完成", "/data/local/tmp 修复完成！", this);
        }
        break;
    default:
        // 完成
        disconnect(repairProcess, nullptr, this, nullptr);
        setButtonsEnabled(true);
        buttons[1]->setText("修复TMP");
        UIHelper::showCenteredMessageBox(QMessageBox::Information, "完成", "/data/local/tmp 修复完成！", this);
        break;
    }
}

void RepairWindow::installApk()
{
    // 检查设备连接
    if (!DeviceManager::instance()->isDeviceConnected()) {
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
    setButtonsEnabled(false);
    buttons[2]->setText("安装中...");
    
    currentApkIndex = 0;
    successCount = 0;
    failCount = 0;
    apkInstallStep = 0;
    
    // 创建进程对象
    if (repairProcess) {
        repairProcess->deleteLater();
    }
    repairProcess = new QProcess(this);
    repairProcess->setWorkingDirectory(ResourceExtractor::getResourcePath());
    
    connect(repairProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &RepairWindow::onApkInstallStepFinished);
    
    // 第一步：推送APK到设备
    QString adbPath = ResourceExtractor::getAdbPath();
    QString apkPath = apkFilesToInstall[currentApkIndex];
    QString apkName = QFileInfo(apkPath).fileName();
    QString remotePath = "/data/local/tmp/" + apkName;
    
    buttons[2]->setText(QString("推送中(%1/%2)").arg(1).arg(apkFilesToInstall.size()));
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
    case 0:  // push完成
        if (exitCode != 0) {
            qDebug() << "推送失败:" << apkName << error;
            failCount++;
            // 跳过这个APK，继续下一个
            currentApkIndex++;
            apkInstallStep = 0;
            if (currentApkIndex < apkFilesToInstall.size()) {
                QString nextApk = apkFilesToInstall[currentApkIndex];
                QString nextName = QFileInfo(nextApk).fileName();
                QString nextRemote = "/data/local/tmp/" + nextName;
                buttons[2]->setText(QString("推送中(%1/%2)").arg(currentApkIndex + 1).arg(apkFilesToInstall.size()));
                repairProcess->start(adbPath, QStringList() << "push" << nextApk << nextRemote);
            } else {
                goto finished;
            }
        } else {
            // push成功，执行pm install (su -c)
            apkInstallStep = 1;
            buttons[2]->setText(QString("安装中(%1/%2)").arg(currentApkIndex + 1).arg(apkFilesToInstall.size()));
            // 把完整命令作为单个字符串传递，避免参数被shell拆分
            repairProcess->start(adbPath, QStringList() << "shell" << QString("su -c 'pm install -r %1'").arg(remotePath));
        }
        break;
        
    case 1:  // install with -c 完成
        if (output.contains("Success") || output.contains("success") || error.contains("Success") || error.contains("success")) {
            successCount++;
            qDebug() << "安装成功:" << apkName;
            // 删除临时文件
            apkInstallStep = 3;
            repairProcess->start(adbPath, QStringList() << "shell" << QString("su -c 'rm -f %1'").arg(remotePath));
        } else {
            // -c 失败，尝试使用 -s
            qDebug() << "su -c 失败，尝试 su -s:" << apkName;
            apkInstallStep = 2;
            repairProcess->start(adbPath, QStringList() << "shell" << QString("su -s 'pm install -r %1'").arg(remotePath));
        }
        break;
        
    case 2:  // install with -s 完成
        if (output.contains("Success") || output.contains("success") || error.contains("Success") || error.contains("success")) {
            successCount++;
            qDebug() << "安装成功(su -s):" << apkName;
        } else {
            failCount++;
            qDebug() << "安装失败:" << apkName << output << error;
        }
        // 删除临时文件
        apkInstallStep = 3;
        repairProcess->start(adbPath, QStringList() << "shell" << QString("su -c 'rm -f %1'").arg(remotePath));
        break;
        
    case 3:  // delete完成
        // 继续下一个APK
        currentApkIndex++;
        apkInstallStep = 0;
        
        if (currentApkIndex < apkFilesToInstall.size()) {
            QString nextApk = apkFilesToInstall[currentApkIndex];
            QString nextName = QFileInfo(nextApk).fileName();
            QString nextRemote = "/data/local/tmp/" + nextName;
            buttons[2]->setText(QString("推送中(%1/%2)").arg(currentApkIndex + 1).arg(apkFilesToInstall.size()));
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
    buttons[2]->setText("安装APK");
    
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
    // 检查设备连接
    if (!DeviceManager::instance()->isDeviceConnected()) {
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
    setButtonsEnabled(false);
    buttons[3]->setText("检测中...");
    
    // 创建进程对象
    if (repairProcess) {
        repairProcess->deleteLater();
    }
    repairProcess = new QProcess(this);
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
        rootManagerType = 1;
        detectedRoot = "APatch";
    } else if (hasKsu) {
        rootManagerType = 2;
        detectedRoot = "KernelSU";
    } else if (hasMagisk) {
        rootManagerType = 0;
        detectedRoot = "Magisk";
    } else {
        // 未检测到Root管理器
        setButtonsEnabled(true);
        buttons[3]->setText("安装模块");
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
        buttons[3]->setText("安装模块");
        return;
    }
    
    // 开始安装
    startModuleInstall();
}

void RepairWindow::startModuleInstall()
{
    buttons[3]->setText("安装中...");
    
    currentModuleIndex = 0;
    moduleSuccessCount = 0;
    moduleFailCount = 0;
    moduleInstallStep = 0;
    
    connect(repairProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &RepairWindow::onModuleInstallStepFinished);
    
    // 第一步：推送模块到设备
    QString adbPath = ResourceExtractor::getAdbPath();
    QString modulePath = moduleFilesToInstall[currentModuleIndex];
    QString moduleName = QFileInfo(modulePath).fileName();
    QString remotePath = "/data/local/tmp/" + moduleName;
    
    buttons[3]->setText(QString("推送中(%1/%2)").arg(1).arg(moduleFilesToInstall.size()));
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
    case 0:  // push完成
        if (exitCode != 0) {
            qDebug() << "推送失败:" << moduleName << error;
            moduleFailCount++;
            // 跳过这个模块，继续下一个
            currentModuleIndex++;
            moduleInstallStep = 0;
            if (currentModuleIndex < moduleFilesToInstall.size()) {
                QString nextModule = moduleFilesToInstall[currentModuleIndex];
                QString nextName = QFileInfo(nextModule).fileName();
                QString nextRemote = "/data/local/tmp/" + nextName;
                buttons[3]->setText(QString("推送中(%1/%2)").arg(currentModuleIndex + 1).arg(moduleFilesToInstall.size()));
                repairProcess->start(adbPath, QStringList() << "push" << nextModule << nextRemote);
            } else {
                goto module_finished;
            }
        } else {
            // push成功，执行安装命令
            moduleInstallStep = 1;
            buttons[3]->setText(QString("安装中(%1/%2)").arg(currentModuleIndex + 1).arg(moduleFilesToInstall.size()));
            
            // 根据Root管理器类型执行不同的安装命令
            QString installCmd;
            switch (rootManagerType) {
                case 0:  // Magisk/Alpha
                    installCmd = QString("magisk --install-module %1").arg(remotePath);
                    break;
                case 1:  // APatch
                    installCmd = QString("/data/adb/ap/bin/apd module install %1").arg(remotePath);
                    break;
                case 2:  // KernelSU
                    installCmd = QString("/data/adb/ksu/bin/ksud module install %1").arg(remotePath);
                    break;
            }
            repairProcess->start(adbPath, QStringList() << "shell" << QString("su -c '%1'").arg(installCmd));
        }
        break;
        
    case 1:  // install完成
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
            moduleInstallStep = 2;
            repairProcess->start(adbPath, QStringList() << "shell" << QString("su -c 'rm -f %1'").arg(remotePath));
        }
        break;
        
    case 2:  // delete完成
        // 继续下一个模块
        currentModuleIndex++;
        moduleInstallStep = 0;
        
        if (currentModuleIndex < moduleFilesToInstall.size()) {
            QString nextModule = moduleFilesToInstall[currentModuleIndex];
            QString nextName = QFileInfo(nextModule).fileName();
            QString nextRemote = "/data/local/tmp/" + nextName;
            buttons[3]->setText(QString("推送中(%1/%2)").arg(currentModuleIndex + 1).arg(moduleFilesToInstall.size()));
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
        buttons[3]->setText("安装模块");
        QString resultMsg = QString("模块安装完成！\n\n"
            "成功：%1 个\n"
            "失败：%2 个").arg(moduleSuccessCount).arg(moduleFailCount);
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "安装结果", resultMsg, this);
    } else {
        // 全部成功，自动重启设备
        buttons[3]->setText("重启中...");
        
        connect(repairProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this](int, QProcess::ExitStatus) {
            disconnect(repairProcess, nullptr, this, nullptr);
            setButtonsEnabled(true);
            buttons[3]->setText("安装模块");
        });
        
        QString adbPath = ResourceExtractor::getAdbPath();
        repairProcess->start(adbPath, QStringList() << "reboot");
    }
}
