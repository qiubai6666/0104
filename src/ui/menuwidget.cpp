#include "menuwidget.h"
#include "configwindow.h"
#include "devicecheckwindow.h"
#include "deviceinfowindow.h"
#include "payloadwindow.h"
#include "repairwindow.h"
#include "processmanager.h"
#include "resourceextractor.h"
#include "devicemanager.h"
#include "uihelper.h"
#include "version.h"

#include <QApplication>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QList>
#include <QPushButton>
#include <QVBoxLayout>
#include <QScreen>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

namespace {
// 可见窗口再次点击时销毁；不可见窗口再次点击时复用。
// prepare 在显示前定位窗口，activate 仅用于投屏窗口。
template <typename Window, typename Prepare>
void toggleWindow(Window *&window, Prepare prepare, bool activate = false)
{
    if (window && window->isVisible()) {
        window->hide();
        window->deleteLater();
        window = nullptr;
        return;
    }
    if (!window) {
        window = new Window();
    }
    prepare(window);
    window->show();
    if (activate) {
        window->raise();
        window->activateWindow();
    }
}
}

MenuWidget::MenuWidget(QWidget *parent)
    : QWidget(parent)
    , deviceInfoWindow(nullptr)
    , repairWindow(nullptr)
    , payloadWindow(nullptr)
    , deviceCheckWindow(nullptr)
    , configWindow(nullptr)
    , imgProcess(nullptr)
{
    // 设置窗口标志：无边框、置顶（不使用Tool，这样可以显示在任务栏）
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground);

    // 设置窗口标题
    setWindowTitle(APP_NAME);

    setupUI();
    updatePosition();
}

MenuWidget::~MenuWidget()
{
    const QList<QWidget *> windows = {
        deviceInfoWindow, repairWindow, payloadWindow, deviceCheckWindow, configWindow
    };
    for (QWidget *window : windows) {
        if (window) {
            window->hide();
            window->deleteLater();
        }
    }
    if (imgProcess) {
        imgProcess->kill();
        imgProcess->deleteLater();
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

    buttons = UIHelper::createMenuButtons(this, mainLayout, buttonTexts);
    for (QPushButton *button : buttons) {
        connect(button, &QPushButton::clicked, this, &MenuWidget::onButtonClicked);
    }

    setStyleSheet(
        "MenuWidget {"
        "   background-color: rgba(195, 219, 228, 245);"
        "   border-radius: 10px;"
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
    QPushButton *button = qobject_cast<QPushButton *>(sender());
    const int index = buttons.indexOf(button);
    if (index < 0) {
        return;
    }

    switch (static_cast<MenuAction>(index)) {
    case ScreenCast:
        toggleWindow(deviceInfoWindow, [](DeviceInfoWindow *) {}, true);
        break;
    case RepairTools:
        toggleWindow(repairWindow, [this](RepairWindow *window) {
            window->setPosition(x(), y(), height());
        });
        break;
    case Payload:
        toggleWindow(payloadWindow, [](PayloadWindow *) {});
        break;
    case ExtractImg:
        extractImg();
        break;
    case DeviceCheck:
        toggleWindow(deviceCheckWindow, [this](DeviceCheckWindow *window) {
            window->setPosition(x(), y());
        });
        break;
    case Configuration:
        toggleWindow(configWindow, [this](ConfigWindow *window) {
            window->setPosition(x(), y(), height());
        });
        break;
    case ContactAuthor:
        openAuthorImage();
        break;
    case Exit:
        cleanupAndExit();
        break;
    case Minimize:
        minimizeWindows();
        break;
    }
}

void MenuWidget::openAuthorImage()
{
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
}

void MenuWidget::minimizeWindows()
{
    const QList<QWidget *> windows = {
        deviceInfoWindow, repairWindow, payloadWindow, deviceCheckWindow, configWindow
    };
    for (QWidget *window : windows) {
        if (window && window->isVisible()) {
            window->showMinimized();
        }
    }
    showMinimized();
}

void MenuWidget::extractImg()
{
    // 检查设备连接
    if (!DeviceManager::instance()->isDeviceConnected()) {
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误", "未检测到设备，请检查设备连接与授权。", this);
        return;
    }

    // 禁用提取IMG按钮
    buttons[ExtractImg]->setEnabled(false);
    buttons[ExtractImg]->setText("检索中...");

    // 创建进程对象
    if (imgProcess) {
        imgProcess->deleteLater();
    }
    imgProcess = ProcessManager::createProcess(this);
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
        buttons[ExtractImg]->setEnabled(true);
        buttons[ExtractImg]->setText("提取IMG");
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

    buttons[ExtractImg]->setText("提取中...");

    QString adbPath = ResourceExtractor::getAdbPath();

    connect(imgProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &MenuWidget::onPullProcessFinished);

    imgProcess->start(adbPath, QStringList() << "pull" << latestImgFile << imgOutputPath);
}

void MenuWidget::onPullProcessFinished(int exitCode, QProcess::ExitStatus)
{
    disconnect(imgProcess, nullptr, this, nullptr);

    buttons[ExtractImg]->setEnabled(true);
    buttons[ExtractImg]->setText("提取IMG");

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

    // 先停止轮询，再仅结束本程序持有的进程；共享 ADB Server 保持运行。
    DeviceManager::instance()->stopMonitoring();
    ProcessManager::stopAllProcesses();

    // 使用定时器等待进程释放文件
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
