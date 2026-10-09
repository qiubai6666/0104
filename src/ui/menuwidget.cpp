#include "menuwidget.h"
#include "mainmenubutton.h"
#include "deviceoperationlease.h"
#include "configwindow.h"
#include "devicecheckwindow.h"
#include "screencastcontroller.h"
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
#include <QCloseEvent>
#include <QPainter>
#include <QIcon>
#include <QLabel>
#include <QEvent>


MenuWidget::MenuWidget(QWidget *parent)
    : QWidget(parent)
    , screenCastController(new ScreenCastController(this))
    , screenCastSpinnerTimer(new QTimer(this))
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
    // 主菜单持续持有 ADB 监控，修复/提取入口不再依赖先打开其他窗口。
    DeviceManager::instance()->ensureAdbOnlyMonitoring();
}

MenuWidget::~MenuWidget()
{
    screenCastSpinnerTimer->stop();
    delete screenCastController;
    DeviceManager::instance()->releaseAdbOnlyMonitoring();
    const QList<QWidget *> windows = {
        screenCastWindow, repairWindow, payloadWindow, deviceCheckWindow, configWindow
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
    mainLayout->setSpacing(1);
    mainLayout->setContentsMargins(5, 7, 5, 7);
    setFixedWidth(UIHelper::MainMenuWidth);
    UIHelper::applyFloatingSurface(this, true);
    const QStringList labels = {"投屏", "秋白工作室", "PAYLOAD", "提取IMG",
        "设备检测", "配置", "联系作者", "退出", "收起"};
    const QStringList icons = {"monitor-smartphone", "wrench", "package-open", "file-down",
        "smartphone", "sliders-horizontal", "message-circle", "power", "panel-right-close"};
    for (int index = 0; index < labels.size(); ++index) {
        if (index == Exit) {
            auto *line = new QWidget(this);
            line->setFixedSize(20, 1);
            line->setStyleSheet("background:rgba(146,166,181,100);");
            mainLayout->addSpacing(3);
            mainLayout->addWidget(line, 0, Qt::AlignHCenter);
            mainLayout->addSpacing(3);
        }
        auto *button = new MainMenuButton(this);
        button->setFixedSize(UIHelper::MainMenuButtonSize, UIHelper::MainMenuButtonSize);
        button->setIcon(UIHelper::menuIcon(icons[index], devicePixelRatioF()));
        button->setIconSize(QSize(UIHelper::MainMenuIconSize, UIHelper::MainMenuIconSize));
        button->setToolTip(labels[index]);
        button->setAccessibleName(labels[index]);
        button->setObjectName(QString("menuAction%1").arg(index));
        button->setCursor(Qt::PointingHandCursor);
        button->setProperty("menuDirectAction", index == ExtractImg || index == ContactAuthor);
        mainLayout->addWidget(button, 0, Qt::AlignHCenter);
        buttons.append(button);
        connect(button, &QPushButton::clicked, this, &MenuWidget::onButtonClicked);
    }
    setupScreenCastWindow();
    buttons[ScreenCast]->setObjectName("screenCastButton");
    screenCastSpinnerTimer->setInterval(40);
    connect(screenCastSpinnerTimer, &QTimer::timeout, this, [this] {
        screenCastSpinnerAngle = (screenCastSpinnerAngle + 16) % 360;
        paintScreenCastSpinner();
    });
    connect(screenCastController, &ScreenCastController::stateChanged,
            this, &MenuWidget::updateScreenCastButton);
    updateScreenCastButton();
}

void MenuWidget::setupScreenCastWindow()
{
    screenCastWindow = new QWidget;
    screenCastWindow->setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    screenCastWindow->setWindowTitle(QStringLiteral("手机投屏"));
    UIHelper::applyFloatingSurface(screenCastWindow);
    screenCastWindow->installEventFilter(this);
    auto *layout = new QVBoxLayout(screenCastWindow);
    layout->setContentsMargins(22, 20, 22, 20);
    layout->setSpacing(12);
    auto *title = new QLabel(QStringLiteral("手机投屏"), screenCastWindow);
    title->setStyleSheet("background:transparent; color:#2c3e50; font-size:17px;");
    layout->addWidget(title);
    screenCastStatusLabel = new QLabel(screenCastWindow);
    screenCastStatusLabel->setWordWrap(true);
    screenCastStatusLabel->setStyleSheet("background:transparent; color:#71808d; font-size:12px;");
    layout->addWidget(screenCastStatusLabel);
    screenCastActionButton = new QPushButton(screenCastWindow);
    screenCastActionButton->setObjectName("screenCastActionButton");
    screenCastActionButton->setStyleSheet(UIHelper::getStandardButtonStyle());
    screenCastActionButton->setMinimumHeight(36);
    screenCastActionButton->setCursor(Qt::PointingHandCursor);
    layout->addWidget(screenCastActionButton, 0, Qt::AlignLeft);
    connect(screenCastActionButton, &QPushButton::clicked, screenCastController, &ScreenCastController::toggle);
    screenCastWindow->adjustSize();
}

void MenuWidget::toggleSubmenu(MenuAction action, QWidget *window)
{
    const bool wasVisible = window->isVisible() && !window->isMinimized();
    const QList<QWidget *> windows = {screenCastWindow, repairWindow, payloadWindow, deviceCheckWindow, configWindow};
    for (QWidget *other : windows) if (other && other != window) other->hide();
    if (wasVisible) window->hide();
    else {
        window->installEventFilter(this);
        UIHelper::positionDrawer(window, geometry());
        window->showNormal();
        window->raise();
        window->activateWindow();
    }
    Q_UNUSED(action);
    updateMenuSelection();
}

void MenuWidget::updateMenuSelection()
{
    const QList<QPair<MenuAction, QWidget *>> windows = {
        {RepairTools, repairWindow}, {Payload, payloadWindow},
        {DeviceCheck, deviceCheckWindow}, {Configuration, configWindow}};
    for (const auto &entry : windows) {
        const bool selected = entry.second && entry.second->isVisible() && !entry.second->isMinimized();
        auto *button = buttons[entry.first];
        button->setProperty("menuSelected", selected);
    }
}

bool MenuWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != this && (event->type() == QEvent::Show || event->type() == QEvent::Hide ||
                           event->type() == QEvent::WindowStateChange)) {
        updateMenuSelection();
    }
    return QWidget::eventFilter(watched, event);
}

void MenuWidget::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange && !isMinimized()) updateMenuSelection();
}

void MenuWidget::updateScreenCastButton()
{
    auto *button = buttons[ScreenCast];
    const auto state = screenCastController->state();
    const bool loading = state == ScreenCastController::WaitingForDevice || state == ScreenCastController::Starting;
    button->setProperty("screenCastLoading", loading);
    button->setProperty("menuSelected", loading || state == ScreenCastController::Streaming ||
                        state == ScreenCastController::Stopping);
    button->setToolTip(screenCastController->statusText());
    // Remain clickable while a disconnected session is stopping so the user
    // can immediately cancel auto-reconnect as well.
    button->setEnabled(true);
    button->setAccessibleName(QStringLiteral("投屏"));
    button->setAccessibleDescription(screenCastController->statusText());
    screenCastStatusLabel->setText(screenCastController->statusText());
    const QString action = state == ScreenCastController::WaitingForDevice ? QStringLiteral("取消等待")
        : state == ScreenCastController::Starting ? QStringLiteral("取消启动")
        : state == ScreenCastController::Streaming || state == ScreenCastController::Stopping ? QStringLiteral("停止投屏")
        : QStringLiteral("开始投屏");
    screenCastActionButton->setText(action);
    if (screenCastWindow->isVisible()) UIHelper::positionDrawer(screenCastWindow, geometry());
    if (loading) {
        button->setText(QString());
        paintScreenCastSpinner();
        screenCastSpinnerTimer->start();
    } else {
        screenCastSpinnerTimer->stop();
        button->setIcon(UIHelper::menuIcon(QStringLiteral("monitor-smartphone"), devicePixelRatioF()));
        button->setText(QString());
    }
}

void MenuWidget::paintScreenCastSpinner()
{
    auto *button = buttons[ScreenCast];
    const qreal ratio = button->devicePixelRatioF();
    QPixmap pixmap(qRound(24 * ratio), qRound(24 * ratio));
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(44, 62, 80, 45), 2.5));
    painter.drawEllipse(QRectF(3, 3, 18, 18));
    painter.setPen(QPen(QColor("#2c3e50"), 2.5, Qt::SolidLine, Qt::RoundCap));
    painter.drawArc(QRectF(3, 3, 18, 18), screenCastSpinnerAngle * 16, 260 * 16);
    painter.end();
    button->setIcon(QIcon(pixmap));
}

void MenuWidget::updatePosition()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (!screen) return;
    adjustSize();
    const QRect area = screen->availableGeometry();
    move(area.right() - width() - 15, area.center().y() - height() / 2);
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
        screenCastController->toggle();
        break;
    case RepairTools:
        if (!repairWindow) repairWindow = new RepairWindow();
        toggleSubmenu(RepairTools, repairWindow);
        break;
    case Payload:
        if (!payloadWindow) payloadWindow = new PayloadWindow();
        toggleSubmenu(Payload, payloadWindow);
        break;
    case ExtractImg:
        extractImg();
        break;
    case DeviceCheck:
        if (!deviceCheckWindow) deviceCheckWindow = new DeviceCheckWindow();
        toggleSubmenu(DeviceCheck, deviceCheckWindow);
        break;
    case Configuration:
        if (!configWindow) configWindow = new ConfigWindow();
        toggleSubmenu(Configuration, configWindow);
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
        screenCastWindow, repairWindow, payloadWindow, deviceCheckWindow, configWindow
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
    if (DeviceOperationLease::busyFor(this)) return;
    // 检查设备连接
    if (DeviceManager::instance()->currentMode() != DeviceManager::ADB) {
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误", "未检测到设备，请检查设备连接与授权。", this);
        return;
    }

    if (!DeviceOperationLease::acquire(this)) return;

    // 禁用提取IMG按钮
    buttons[ExtractImg]->setEnabled(false);
    buttons[ExtractImg]->setToolTip("检索中...");

    // 创建进程对象
    if (imgProcess) {
        imgProcess->deleteLater();
    }
    imgProcess = ProcessManager::createProcess(this);
    connect(imgProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) { if (e==QProcess::FailedToStart) { DeviceOperationLease::release(this); buttons[ExtractImg]->setEnabled(true); buttons[ExtractImg]->setToolTip("提取IMG"); } });
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
        DeviceOperationLease::release(this);
        buttons[ExtractImg]->setEnabled(true);
        buttons[ExtractImg]->setToolTip("提取IMG");
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

    buttons[ExtractImg]->setToolTip("提取中...");

    QString adbPath = ResourceExtractor::getAdbPath();

    connect(imgProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &MenuWidget::onPullProcessFinished);

    connect(imgProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) { if(e==QProcess::FailedToStart){ DeviceOperationLease::release(this); buttons[ExtractImg]->setEnabled(true); } });
    imgProcess->start(adbPath, QStringList() << "pull" << latestImgFile << imgOutputPath);
}

void MenuWidget::onPullProcessFinished(int exitCode, QProcess::ExitStatus)
{
    DeviceOperationLease::release(this);
    disconnect(imgProcess, nullptr, this, nullptr);

    buttons[ExtractImg]->setEnabled(true);
    buttons[ExtractImg]->setToolTip("提取IMG");

    if (exitCode == 0) {
        // 提取成功，直接打开 IMG 文件夹
        QDesktopServices::openUrl(QUrl::fromLocalFile(imgOutputPath));
    } else {
        UIHelper::showCenteredMessageBox(QMessageBox::Critical, "错误", "提取失败，请检查设备连接或权限。");
    }
}

void MenuWidget::closeEvent(QCloseEvent *event)
{
    if (DeviceOperationLease::blocksApplicationExit() || (repairWindow && repairWindow->hasActiveOugaTask()) || (deviceCheckWindow && deviceCheckWindow->isOperationInProgress())) {
        event->ignore();
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "操作进行中",
            "正在重启或刷写设备，请等待操作结束后再关闭主菜单。", this);
        return;
    }
    QWidget::closeEvent(event);
}

void MenuWidget::cleanupAndExit()
{
    if (DeviceOperationLease::blocksApplicationExit() || (repairWindow && repairWindow->hasActiveOugaTask()) || (deviceCheckWindow && deviceCheckWindow->isOperationInProgress())) {
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "操作进行中",
            "正在重启或刷写设备，请等待操作结束后再退出。", this);
        return;
    }
    qDebug() << "开始清理并退出...";

    screenCastController->stop();
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

