#include "menuwidget.h"
#include "payloadwindow.h"
#include "configwindow.h"
#include "processmanager.h"
#include "resourceextractor.h"
#include "devicemanager.h"
#include "uihelper.h"
#include <QScreen>
#include <QGuiApplication>
#include <QApplication>
#include <QMessageBox>
#include <QProcess>
#include <QStandardPaths>
#include <QDir>
#include <QDesktopServices>
#include <QUrl>
#include <QCoreApplication>
#include <QThread>

#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
#endif

MenuWidget::MenuWidget(QWidget *parent)
    : QWidget(parent)
    , deviceInfoWindow(nullptr)
    , repairWindow(nullptr)
    , payloadWindow(nullptr)
    , deviceCheckWindow(nullptr)
    , configWindow(nullptr)
    , imgProcess(nullptr)
    , cleanupStep(0)
{
    // 设置窗口标志：无边框、置顶（不使用Tool，这样可以显示在任务栏）
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground);
    
    // 设置窗口标题
    setWindowTitle("秋白工作室");
    
    setupUI();
    updatePosition();
}

MenuWidget::~MenuWidget()
{
    if (deviceInfoWindow) {
        deviceInfoWindow->hide();
        deviceInfoWindow->deleteLater();
        deviceInfoWindow = nullptr;
    }
    
    if (repairWindow) {
        repairWindow->hide();
        repairWindow->deleteLater();
        repairWindow = nullptr;
    }
    
    if (payloadWindow) {
        payloadWindow->hide();
        payloadWindow->deleteLater();
        payloadWindow = nullptr;
    }
    
    if (deviceCheckWindow) {
        deviceCheckWindow->hide();
        deviceCheckWindow->deleteLater();
        deviceCheckWindow = nullptr;
    }
    
    if (configWindow) {
        configWindow->hide();
        configWindow->deleteLater();
        configWindow = nullptr;
    }
    
    if (imgProcess) {
        imgProcess->kill();
        imgProcess->deleteLater();
        imgProcess = nullptr;
    }
}

void MenuWidget::setupUI()
{
    mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(0);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    
    QStringList buttonTexts = {
        "投屏", "秋白工作室", "PAYLOAD", "提取IMG",
        "设备检测", "配置", "联系作者", "退出", "收起"
    };
    
    for (int i = 0; i < buttonTexts.size(); ++i) {
        QPushButton *btn = new QPushButton(buttonTexts[i], this);
        btn->setMinimumHeight(45);
        btn->setMaximumHeight(45);
        btn->setMinimumWidth(120);
        btn->setStyleSheet(UIHelper::getButtonStyle(i, buttonTexts.size()));
        
        connect(btn, &QPushButton::clicked, this, &MenuWidget::onButtonClicked);
        buttons.append(btn);
        mainLayout->addWidget(btn);
    }
    
    setStyleSheet(
        "MenuWidget {"
        "   background-color: rgba(180, 210, 220, 240);"
        "   border-radius: 12px;"
        "}"
    );
}

void MenuWidget::updatePosition()
{
    // 获取屏幕尺寸
    QScreen *screen = QGuiApplication::primaryScreen();
    QRect screenGeometry = screen->geometry();
    
    // 调整窗口大小以适应内容
    adjustSize();
    
    // 计算位置：屏幕最右侧，垂直居中
    int x = screenGeometry.width() - width(); // 紧贴右边缘
    int y = (screenGeometry.height() - height()) / 2;
    
    move(x, y);
}

void MenuWidget::onButtonClicked()
{
    QPushButton *btn = qobject_cast<QPushButton*>(sender());
    if (!btn) return;
    
    int index = buttons.indexOf(btn);
    
    // 投屏功能
    if (index == 0) {
        if (!deviceInfoWindow) {
            deviceInfoWindow = new DeviceInfoWindow();
            deviceInfoWindow->show();
            deviceInfoWindow->raise();
            deviceInfoWindow->activateWindow();
        } else {
            if (deviceInfoWindow->isVisible()) {
                deviceInfoWindow->hide();
                deviceInfoWindow->deleteLater();
                deviceInfoWindow = nullptr;
            } else {
                deviceInfoWindow->show();
                deviceInfoWindow->raise();
                deviceInfoWindow->activateWindow();
            }
        }
        return;
    }
    
    // 修复功能
    if (index == 1) {
        if (!repairWindow) {
            repairWindow = new RepairWindow();
            repairWindow->setPosition(x(), y(), height());
            repairWindow->show();
        } else {
            if (repairWindow->isVisible()) {
                repairWindow->hide();
                repairWindow->deleteLater();
                repairWindow = nullptr;
            } else {
                repairWindow->setPosition(x(), y(), height());
                repairWindow->show();
            }
        }
        return;
    }
    
    // PAYLOAD 功能
    if (index == 2) {
        if (!payloadWindow) {
            payloadWindow = new PayloadWindow();
            payloadWindow->show();
        } else {
            if (payloadWindow->isVisible()) {
                payloadWindow->hide();
                payloadWindow->deleteLater();
                payloadWindow = nullptr;
            } else {
                payloadWindow->show();
            }
        }
        return;
    }
    
    // 提取IMG 功能
    if (index == 3) {
        extractImg();
        return;
    }
    
    // 设备检测功能
    if (index == 4) {
        if (!deviceCheckWindow) {
            deviceCheckWindow = new DeviceCheckWindow();
            deviceCheckWindow->setPosition(x(), y());
            deviceCheckWindow->show();
        } else {
            if (deviceCheckWindow->isVisible()) {
                deviceCheckWindow->hide();
                deviceCheckWindow->deleteLater();
                deviceCheckWindow = nullptr;
            } else {
                deviceCheckWindow->setPosition(x(), y());
                deviceCheckWindow->show();
            }
        }
        return;
    }
    
    // 配置功能
    if (index == 5) {
        if (!configWindow) {
            configWindow = new ConfigWindow();
            configWindow->setPosition(x(), y(), height());
            configWindow->show();
        } else {
            if (configWindow->isVisible()) {
                configWindow->hide();
                configWindow->deleteLater();
                configWindow = nullptr;
            } else {
                configWindow->setPosition(x(), y(), height());
                configWindow->show();
            }
        }
        return;
    }
    
    // 联系作者（打开 Neil.jpg）
    if (index == 6) {
        QString neilImagePath = ResourceExtractor::getNeilImagePath();
        
        // 检查文件是否存在
        if (QFile::exists(neilImagePath)) {
            // 使用系统默认图片查看器打开
            if (QDesktopServices::openUrl(QUrl::fromLocalFile(neilImagePath))) {
                qDebug() << "成功打开 Neil.jpg";
            } else {
                // 如果系统默认程序打开失败，尝试用浏览器打开
                qDebug() << "系统默认程序打开失败，尝试使用浏览器打开";
                QString browserPath = "file:///" + neilImagePath;
                if (QDesktopServices::openUrl(QUrl(browserPath))) {
                    qDebug() << "使用浏览器打开成功";
                } else {
                    UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误", 
                        "无法打开图片，请检查系统是否有图片查看器或浏览器。");
                }
            }
        } else {
            UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误", 
                "找不到作者图片文件！\n路径: " + neilImagePath);
        }
        return;
    }
    
    // 退出程序
    if (index == 7) {
        cleanupAndExit();
        return;
    }
    
    // 最小化窗口
    if (index == 8) {
        // 所有子窗口也一起最小化
        if (deviceInfoWindow && deviceInfoWindow->isVisible()) {
            deviceInfoWindow->showMinimized();
        }
        if (repairWindow && repairWindow->isVisible()) {
            repairWindow->showMinimized();
        }
        if (payloadWindow && payloadWindow->isVisible()) {
            payloadWindow->showMinimized();
        }
        if (deviceCheckWindow && deviceCheckWindow->isVisible()) {
            deviceCheckWindow->showMinimized();
        }
        if (configWindow && configWindow->isVisible()) {
            configWindow->showMinimized();
        }
        showMinimized();
        return;
    }
}

QString MenuWidget::getLatestImgFile()
{
    // 异步版本已不需要此函数，保留空实现以兼容
    return QString();
}

void MenuWidget::extractImg()
{
    // 检查设备连接
    if (!DeviceManager::instance()->isDeviceConnected()) {
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误", "未检测到设备，请检查设备连接与授权。", this);
        return;
    }
    
    // 禁用提取IMG按钮
    buttons[3]->setEnabled(false);
    buttons[3]->setText("检索中...");
    
    // 创建进程对象
    if (imgProcess) {
        imgProcess->deleteLater();
    }
    imgProcess = new QProcess(this);
    imgProcess->setWorkingDirectory(ResourceExtractor::getResourcePath());
    
    QString adbPath = ResourceExtractor::getAdbPath();
    
    connect(imgProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &MenuWidget::onLsProcessFinished);
    
    imgProcess->start(adbPath, QStringList() << "shell" << "ls" << "-t" << "-1" << "/sdcard/Download/*.img");
}

void MenuWidget::onLsProcessFinished(int exitCode, QProcess::ExitStatus)
{
    Q_UNUSED(exitCode);
    
    disconnect(imgProcess, nullptr, this, nullptr);
    
    QString output = imgProcess->readAllStandardOutput();
    QStringList files = output.split('\n', Qt::SkipEmptyParts);
    
    if (files.isEmpty()) {
        buttons[3]->setEnabled(true);
        buttons[3]->setText("提取IMG");
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误", "手机 /sdcard/Download 目录下没有 .img 文件。");
        return;
    }
    
    latestImgFile = files.first().trimmed();
    
    // 获取桌面路径并创建 IMG 文件夹
    QString desktopPath = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    imgOutputPath = desktopPath + "/IMG";
    
    QDir dir;
    if (!dir.exists(imgOutputPath)) {
        dir.mkpath(imgOutputPath);
    }
    
    buttons[3]->setText("提取中...");
    
    QString adbPath = ResourceExtractor::getAdbPath();
    
    connect(imgProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &MenuWidget::onPullProcessFinished);
    
    imgProcess->start(adbPath, QStringList() << "pull" << latestImgFile << imgOutputPath);
}

void MenuWidget::onPullProcessFinished(int exitCode, QProcess::ExitStatus)
{
    disconnect(imgProcess, nullptr, this, nullptr);
    
    buttons[3]->setEnabled(true);
    buttons[3]->setText("提取IMG");
    
    if (exitCode == 0) {
        // 提取成功，直接打开 IMG 文件夹
        QDesktopServices::openUrl(QUrl::fromLocalFile(imgOutputPath));
    } else {
        UIHelper::showCenteredMessageBox(QMessageBox::Critical, "错误", "提取失败，请检查设备连接或权限。");
    }
}

void MenuWidget::cleanupAndExit()
{
    qDebug() << "开始清理并退出...";
    
    // 1. 结束所有记录的子进程
    ProcessManager::killAllRecordedProcesses();
    
    // 2. 强制结束 adb.exe 和 fastboot.exe
    ProcessManager::killProcessByName("adb.exe");
    ProcessManager::killProcessByName("fastboot.exe");
    
    cleanupStep = 0;
    
    // 3. 使用定时器等待进程释放文件
    QTimer::singleShot(500, this, &MenuWidget::onCleanupProcessFinished);
}

void MenuWidget::onCleanupProcessFinished()
{
    // 删除qiubai文件夹
    QString qiubaiPath = ResourceExtractor::getResourcePath();
    QDir qiubaiDir(qiubaiPath);
    if (qiubaiDir.exists()) {
        qDebug() << "删除qiubai文件夹:" << qiubaiPath;
        if (qiubaiDir.removeRecursively()) {
            qDebug() << "删除成功";
        } else {
            qDebug() << "删除失败，可能有文件被占用";
        }
    }
    
    // 退出应用程序
    qDebug() << "退出应用程序";
    QApplication::quit();
}
