#include "menuwidget.h"
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
#include <QFileInfo>
#include <QFont>
#include <QGuiApplication>
#include <QGridLayout>
#include <QKeyEvent>
#include <QList>
#include <QMouseEvent>
#include <QPushButton>
#include <QStackedWidget>
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
#include <QDialog>
#include <QDropEvent>
#include <QMimeData>
#include <QDynamicPropertyChangeEvent>
#include <algorithm>
#include <functional>

namespace {
// 控制中心行按钮：图标与文字由兄弟 QLabel 渲染（投屏 spinner/状态需要独立刷新），
// 按钮本体只负责点击区域、底色与选中描边，因此不绘制 QIcon 内容。
class RowButton : public QPushButton
{
public:
    using QPushButton::QPushButton;
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const bool dark = palette().color(QPalette::Window).lightness() < 128;
        QColor background = Qt::transparent;
        if (isDown()) background = dark ? QColor(66, 97, 110, 200) : QColor(206, 228, 238, 230);
        else if (underMouse()) background = dark ? QColor(52, 73, 80, 170) : QColor(224, 238, 244, 170);
        if (background.alpha()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(background);
            painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 12, 12);
        }
        if (property("menuSelected").toBool() && isEnabled()) {
            painter.setPen(QPen(QColor(131, 174, 202), 1.2));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 12, 12);
        }
    }
};

// 功能 tile：把图标与文字合成为一张 DPI 感知位图，保持预览稿“图标在上、文字在下”的观感，
// 同时按钮仍保留 tooltip/accessibleName 与原 objectName。
QIcon tileIcon(const QString &iconName, const QString &label, qreal ratio)
{
    const qreal dpr = qMax<qreal>(1.0, ratio);
    const int width = 124, height = 52;
    QPixmap pixmap(qRound(width * dpr), qRound(height * dpr));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    const QIcon glyph = UIHelper::menuIcon(iconName, dpr);
    glyph.paint(&painter, QRect(12, 7, 19, 19));
    const bool dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
    painter.setPen(QColor(dark ? "#dce8ef" : "#385565"));
    QFont font(QStringLiteral("Microsoft YaHei"));
    font.setPixelSize(12);
    painter.setFont(font);
    painter.drawText(QRect(12, 30, width - 20, 16), Qt::AlignLeft | Qt::AlignVCenter, label);
    painter.end();
    return QIcon(pixmap);
}

} // namespace

// 主入口悬浮球：空闲固定 command 图标，投屏启动中显示 spinner，
// 流式中切换为投屏图标并叠加绿色徽标。点击由 MenuWidget 接管面板显隐。
// 定义在全局作用域，与 menuwidget.h 中的前置声明（成员 ball）保持同一类型。
class FloatingBall : public QWidget
{
public:
    enum CastState { Idle, Loading, Streaming };

    FloatingBall()
    {
        setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        setAttribute(Qt::WA_TranslucentBackground);
        // 应用存活锚点：面板不承担 QuitOnClose，由悬浮球保持事件循环。
        setAttribute(Qt::WA_QuitOnClose);
        setFixedSize(46, 46);
        setWindowTitle(APP_NAME);
        setCursor(Qt::PointingHandCursor);
    }

    void setCastState(CastState value)
    {
        if (castState == value) return;
        castState = value;
        update();
    }

    int spinnerAngle = 0;
    std::function<void()> activated;

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        pressed = event->button() == Qt::LeftButton;
        event->accept();
    }
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (pressed && event->button() == Qt::LeftButton && rect().contains(event->pos()) && activated)
            activated();
        pressed = false;
        event->accept();
    }
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const bool dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
        QColor fill = dark ? QColor(39, 56, 64) : QColor(195, 219, 228);
        fill.setAlpha(102);
        painter.setPen(QPen(dark ? QColor(255, 255, 255, 32) : QColor(255, 255, 255, 180), 1.0));
        painter.setBrush(fill);
        painter.drawEllipse(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5));
        const QRectF iconRect((width() - 22) / 2.0, (height() - 22) / 2.0, 22, 22);
        if (castState == Loading) {
            painter.setPen(QPen(dark ? QColor("#dce8ef") : QColor("#385565"), 2.5, Qt::SolidLine, Qt::RoundCap));
            painter.setBrush(Qt::NoBrush);
            painter.drawArc(iconRect, spinnerAngle * 16, 260 * 16);
        } else {
            const QIcon icon = UIHelper::menuIcon(
                castState == Streaming ? QStringLiteral("monitor-smartphone") : QStringLiteral("command"),
                devicePixelRatioF());
            icon.paint(&painter, iconRect.toRect());
        }
        if (castState == Streaming) {
            const QRectF badge(width() - 16, 4, 12, 12);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor("#3fbf6f"));
            painter.drawEllipse(badge);
            painter.setPen(QPen(dark ? QColor(35, 42, 48) : QColor(255, 255, 255), 1.5));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(badge);
        }
    }

private:
    CastState castState = Idle;
    bool pressed = false;
};

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
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowTitle(APP_NAME);

    setupUI();
    // Scope the application filter to our submenus, including their controls
    // and lazily-created tool windows. Unrelated windows keep their own drops.
    qApp->installEventFilter(this);
    updatePosition();
    // 主菜单持续持有 ADB 监控，修复/提取入口不再依赖先打开其他窗口。
    DeviceManager::instance()->ensureAdbOnlyMonitoring();
}

MenuWidget::~MenuWidget()
{
    qApp->removeEventFilter(this);
    screenCastSpinnerTimer->stop();
    delete screenCastController;
    DeviceManager::instance()->releaseAdbOnlyMonitoring();
    const QList<QWidget *> windows = {ball, repairWindow, payloadWindow, deviceCheckWindow, configWindow};
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
    setFixedSize(UIHelper::ControlCenterWidth, UIHelper::ControlCenterHeight);
    UIHelper::applyControlCenterSurface(this);
    setAcceptDrops(true);
    const bool dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
    setStyleSheet(QString("QLabel {background:transparent; color:%1; font-family:'Segoe UI','Microsoft YaHei';}")
                  .arg(dark ? "#e9edf3" : "#2c3e50"));

    mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // 共享子页头部：主页隐藏，仅翻页显示。
    header = new QWidget(this);
    header->setFixedHeight(46);
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(12, 8, 14, 6);
    headerLayout->setSpacing(8);
    backButton = new QPushButton(header);
    backButton->setObjectName("panelBackButton");
    backButton->setFixedSize(28, 28);
    backButton->setIcon(UIHelper::menuIcon(QStringLiteral("chevron-left"), devicePixelRatioF()));
    backButton->setIconSize(QSize(20, 20));
    backButton->setCursor(Qt::PointingHandCursor);
    backButton->setAccessibleName(QStringLiteral("返回"));
    backButton->setStyleSheet("QPushButton { background:transparent; border:none; border-radius:8px; }"
                              "QPushButton:hover { background:rgba(212,230,237,120); }"
                              "QPushButton:pressed { background:rgba(212,230,237,220); }");
    headerLayout->addWidget(backButton);
    pageTitle = new QLabel(header);
    pageTitle->setStyleSheet("background:transparent; font-size:15px; font-weight:normal;");
    headerLayout->addWidget(pageTitle);
    headerLayout->addStretch(1);
    mainLayout->addWidget(header);
    connect(backButton, &QPushButton::clicked, this, &MenuWidget::showHome);

    pages = new QStackedWidget(this);
    mainLayout->addWidget(pages, 1);

    homePage = buildHomePage();
    pages->addWidget(homePage);
    pageTitles.insert(homePage, QString());

    // 悬浮球：点击在面板显隐之间切换。
    ball = new FloatingBall;
    ball->activated = [this] { togglePanel(); };

    screenCastSpinnerTimer->setInterval(40);
    connect(screenCastSpinnerTimer, &QTimer::timeout, this, [this] {
        screenCastSpinnerAngle = (screenCastSpinnerAngle + 16) % 360;
        paintScreenCastSpinner();
        if (ball) {
            ball->spinnerAngle = screenCastSpinnerAngle;
            ball->update();
        }
    });
    connect(screenCastController, &ScreenCastController::stateChanged,
            this, &MenuWidget::updateScreenCastButton);
    connect(DeviceManager::instance(), &DeviceManager::deviceModeChanged,
            this, &MenuWidget::updateDeviceRow);
    updateDeviceRow();
    updateScreenCastButton();
    // 主页不显示返回头部，仅进入子页时展示。
    showHome();
}

QWidget *MenuWidget::buildHomePage()
{
    const bool dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(8);

    // 品牌行
    auto *brandRow = new QHBoxLayout;
    brandRow->setSpacing(6);
    auto *brandIcon = new QLabel(page);
    brandIcon->setPixmap(UIHelper::menuIcon(QStringLiteral("command"), devicePixelRatioF()).pixmap(14, 14));
    brandIcon->setFixedSize(14, 14);
    brandIcon->setScaledContents(true);
    auto *brandText = new QLabel(QStringLiteral("ORANGETOOLS"), page);
    brandText->setStyleSheet("font-size:11px; font-weight:bold; letter-spacing:2px; color:#68818e;");
    brandRow->addSpacing(2);
    brandRow->addWidget(brandIcon);
    brandRow->addWidget(brandText);
    brandRow->addStretch(1);
    layout->addLayout(brandRow);
    layout->addSpacing(2);

    // 投屏行：点击直接开始/停止投屏（buttons[ScreenCast]）。
    auto *castCard = new QFrame(page);
    castCard->setFixedHeight(58);
    auto *castGrid = new QGridLayout(castCard);
    castGrid->setContentsMargins(10, 0, 10, 0);
    castGrid->setSpacing(8);
    screenCastIconLabel = new QLabel(castCard);
    screenCastIconLabel->setFixedSize(24, 24);
    screenCastIconLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto *castTitle = new QLabel(QStringLiteral("手机投屏"), castCard);
    castTitle->setAttribute(Qt::WA_TransparentForMouseEvents);
    castTitle->setStyleSheet("font-size:13px;");
    screenCastSubtitle = new QLabel(castCard);
    screenCastSubtitle->setAttribute(Qt::WA_TransparentForMouseEvents);
    screenCastSubtitle->setStyleSheet("font-size:11px; color:#71808d;");
    screenCastPill = new QLabel(castCard);
    screenCastPill->setObjectName("statusPill");
    screenCastPill->setAttribute(Qt::WA_TransparentForMouseEvents);
    screenCastPill->setProperty("state", "off");
    screenCastPill->setStyleSheet(
        "QLabel#statusPill { border-radius:8px; padding:2px 8px; font-size:10px; }"
        "QLabel#statusPill[state=\"off\"] { background:rgba(146,166,181,60); color:#71808d; }"
        "QLabel#statusPill[state=\"ok\"] { background:rgba(63,191,111,60); color:#1d7a43; }"
        "QLabel#statusPill[state=\"loading\"] { background:rgba(230,162,60,70); color:#a3661a; }");
    auto *castTexts = new QVBoxLayout;
    castTexts->setSpacing(1);
    castTexts->addWidget(castTitle);
    castTexts->addWidget(screenCastSubtitle);
    castGrid->addWidget(screenCastIconLabel, 0, 0, Qt::AlignVCenter);
    castGrid->addLayout(castTexts, 0, 1, Qt::AlignVCenter);
    castGrid->setColumnStretch(1, 1);
    castGrid->addWidget(screenCastPill, 0, 2, Qt::AlignVCenter | Qt::AlignRight);
    auto *castButton = new RowButton(castCard);
    castButton->setObjectName("screenCastButton");
    castButton->setAccessibleName(QStringLiteral("投屏"));
    castButton->setCursor(Qt::PointingHandCursor);
    castGrid->addWidget(castButton, 0, 0, 1, 3);
    layout->addWidget(castCard);
    buttons.append(castButton);
    connect(castButton, &QPushButton::clicked, this, &MenuWidget::onButtonClicked);

    // 设备行：展示连接状态，点击进入设备检测页。
    auto *deviceCard = new QFrame(page);
    deviceCard->setFixedHeight(46);
    auto *deviceGrid = new QGridLayout(deviceCard);
    deviceGrid->setContentsMargins(10, 0, 10, 0);
    deviceGrid->setSpacing(8);
    auto *deviceIcon = new QLabel(page);
    deviceIcon->setPixmap(UIHelper::menuIcon(QStringLiteral("smartphone"), devicePixelRatioF()).pixmap(20, 20));
    deviceIcon->setFixedSize(20, 20);
    deviceIcon->setScaledContents(true);
    deviceIcon->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto *deviceTitle = new QLabel(QStringLiteral("设备"), page);
    deviceTitle->setAttribute(Qt::WA_TransparentForMouseEvents);
    deviceTitle->setStyleSheet("font-size:13px;");
    devicePill = new QLabel(page);
    devicePill->setObjectName("statusPill");
    devicePill->setAttribute(Qt::WA_TransparentForMouseEvents);
    devicePill->setProperty("state", "off");
    devicePill->setStyleSheet(screenCastPill->styleSheet());
    deviceGrid->addWidget(deviceIcon, 0, 0, Qt::AlignVCenter);
    deviceGrid->addWidget(deviceTitle, 0, 1, Qt::AlignVCenter);
    deviceGrid->setColumnStretch(1, 1);
    deviceGrid->addWidget(devicePill, 0, 2, Qt::AlignVCenter | Qt::AlignRight);
    auto *deviceRow = new QPushButton(deviceCard);
    deviceRow->setObjectName("deviceRowButton");
    deviceRow->setAccessibleName(QStringLiteral("设备检测"));
    deviceRow->setCursor(Qt::PointingHandCursor);
    deviceRow->setToolTip(QStringLiteral("打开设备检测"));
    deviceGrid->addWidget(deviceRow, 0, 0, 1, 3);
    layout->addWidget(deviceCard);
    connect(deviceRow, &QPushButton::clicked, this, [this] {
        showSubmenu(DeviceCheck, ensureDeviceCheckWindow());
    });

    // 功能 tile 2×3（buttons[RepairTools..ContactAuthor]）。
    auto *tileGrid = new QGridLayout;
    tileGrid->setHorizontalSpacing(8);
    tileGrid->setVerticalSpacing(8);
    const QVector<QPair<int, QString>> tiles = {
        {RepairTools, QStringLiteral("秋白工作室")}, {Payload, QStringLiteral("PAYLOAD")},
        {ExtractImg, QStringLiteral("提取IMG")}, {DeviceCheck, QStringLiteral("设备检测")},
        {Configuration, QStringLiteral("配置")}, {ContactAuthor, QStringLiteral("联系作者")}};
    const QStringList tileIcons = {QStringLiteral("wrench"), QStringLiteral("package-open"),
        QStringLiteral("file-down"), QStringLiteral("smartphone"), QStringLiteral("sliders-horizontal"),
        QStringLiteral("heart")};
    const QString tileStyle = QStringLiteral(
        "QPushButton { background:rgba(146,166,181,30); border:1px solid rgba(146,166,181,60);"
        " border-radius:12px; }"
        "QPushButton:hover { background:rgba(212,230,237,110); border-color:rgba(131,174,202,140); }"
        "QPushButton:pressed { background:rgba(195,219,228,200); }");
    for (int index = 0; index < tiles.size(); ++index) {
        auto *tile = new QPushButton(page);
        tile->setObjectName(QString("menuAction%1").arg(tiles[index].first));
        tile->setFixedHeight(58);
        tile->setIconSize(QSize(124, 52));
        tile->setIcon(tileIcon(tileIcons[index], tiles[index].second, devicePixelRatioF()));
        tile->setToolTip(tiles[index].second);
        tile->setAccessibleName(tiles[index].second);
        tile->setCursor(Qt::PointingHandCursor);
        tile->setStyleSheet(tileStyle);
        tile->setProperty("menuDirectAction", tiles[index].first == ExtractImg ||
                                              tiles[index].first == ContactAuthor);
        tileGrid->addWidget(tile, index / 2, index % 2);
        buttons.append(tile);
        connect(tile, &QPushButton::clicked, this, &MenuWidget::onButtonClicked);
    }
    layout->addLayout(tileGrid);
    layout->addStretch(1);

    // 底部：收起（buttons[Minimize]）与退出（buttons[Exit]）。
    const QString footerStyle = QString(
        "QPushButton { background:rgba(146,166,181,30); border:none; border-radius:10px;"
        " color:%1; font-size:12px; padding:8px; }"
        "QPushButton:hover { background:rgba(146,166,181,60); }"
        "QPushButton:pressed { background:rgba(146,166,181,90); }")
        .arg(dark ? "#e9edf3" : "#2c3e50");
    auto *footerRow = new QHBoxLayout;
    footerRow->setSpacing(8);
    const struct { MenuAction action; const char *label; const char *icon; } footer[] = {
        {Minimize, "收起", "panel-right-close"}, {Exit, "退出", "power"}};
    QPushButton *footerButtons[2] = {nullptr, nullptr};
    for (int index = 0; index < 2; ++index) {
        const auto &entry = footer[index];
        auto *button = new QPushButton(QString::fromUtf8(entry.label), page);
        button->setObjectName(QString("menuAction%1").arg(int(entry.action)));
        button->setFixedHeight(36);
        button->setIcon(UIHelper::menuIcon(QString::fromLatin1(entry.icon), devicePixelRatioF()));
        button->setIconSize(QSize(15, 15));
        button->setCursor(Qt::PointingHandCursor);
        button->setAccessibleName(QString::fromUtf8(entry.label));
        button->setStyleSheet(footerStyle);
        footerRow->addWidget(button);
        connect(button, &QPushButton::clicked, this, &MenuWidget::onButtonClicked);
        footerButtons[index] = button;
    }
    // 视觉顺序为「收起、退出」，buttons 向量下标必须与 MenuAction 枚举一致
    // （Exit=7、Minimize=8），故按枚举顺序补入向量。
    buttons.append(footerButtons[1]);
    buttons.append(footerButtons[0]);
    layout->addLayout(footerRow);
    return page;
}

void MenuWidget::showSubmenu(MenuAction action, QWidget *window)
{
    if (window->isWindow()) {
        // 独立窗口（回归测试替身或旧用法）保持抽屉显隐路径。
        toggleSubmenu(action, window);
        return;
    }
    // 面板内嵌页：同页再点返回主页。
    if (pages->currentWidget() == window && isVisible()) {
        showHome();
        return;
    }
    pages->setCurrentWidget(window);
    pageTitle->setText(pageTitles.value(window));
    header->setVisible(true);
    if (!isVisible()) {
        positionPanel();
        show();
        raise();
        activateWindow();
    }
    Q_UNUSED(action);
    updateMenuSelection();
}

void MenuWidget::toggleSubmenu(MenuAction action, QWidget *window)
{
    const bool wasVisible = window->isVisible() && !window->isMinimized();
    const QList<QWidget *> windows = {repairWindow, payloadWindow, deviceCheckWindow, configWindow};
    for (QWidget *other : windows)
        if (other && other != window && other->isWindow()) other->hide();
    if (wasVisible) window->hide();
    else {
        UIHelper::positionDrawer(window, geometry());
        window->showNormal();
        window->raise();
        window->activateWindow();
    }
    Q_UNUSED(action);
    updateMenuSelection();
}

void MenuWidget::showHome()
{
    pages->setCurrentWidget(homePage);
    header->setVisible(false);
    updateMenuSelection();
}

void MenuWidget::collapsePanel()
{
    showHome();
    hide();
}

void MenuWidget::togglePanel()
{
    if (isVisible()) {
        collapsePanel();
        return;
    }
    positionPanel();
    show();
    raise();
    activateWindow();
}

RepairWindow *MenuWidget::ensureRepairWindow()
{
    if (!repairWindow) {
        repairWindow = new RepairWindow(pages);
        pages->addWidget(repairWindow);
        pageTitles.insert(repairWindow, QStringLiteral("秋白工作室"));
        repairWindow->setAcceptDrops(true);
    }
    return repairWindow;
}

PayloadWindow *MenuWidget::ensurePayloadWindow()
{
    if (!payloadWindow) {
        payloadWindow = new PayloadWindow(pages);
        pages->addWidget(payloadWindow);
        pageTitles.insert(payloadWindow, QStringLiteral("Payload 分区提取"));
        payloadWindow->setAcceptDrops(true);
        connect(payloadWindow, &QDialog::finished, this, [this](int) { showHome(); });
    }
    return payloadWindow;
}

DeviceCheckWindow *MenuWidget::ensureDeviceCheckWindow()
{
    if (!deviceCheckWindow) {
        deviceCheckWindow = new DeviceCheckWindow(pages);
        pages->addWidget(deviceCheckWindow);
        pageTitles.insert(deviceCheckWindow, QStringLiteral("设备检测"));
        deviceCheckWindow->setAcceptDrops(true);
    }
    return deviceCheckWindow;
}

ConfigWindow *MenuWidget::ensureConfigWindow()
{
    if (!configWindow) {
        configWindow = new ConfigWindow(pages);
        pages->addWidget(configWindow);
        pageTitles.insert(configWindow, QStringLiteral("配置与工具"));
        configWindow->setAcceptDrops(true);
    }
    return configWindow;
}

void MenuWidget::updateMenuSelection()
{
    const QList<QPair<MenuAction, QWidget *>> windows = {
        {RepairTools, repairWindow}, {Payload, payloadWindow},
        {DeviceCheck, deviceCheckWindow}, {Configuration, configWindow}};
    for (const auto &entry : windows) {
        bool selected = false;
        if (entry.second) {
            if (entry.second->isWindow())
                selected = entry.second->isVisible() && !entry.second->isMinimized();
            else
                selected = isVisible() && pages->currentWidget() == entry.second;
        }
        auto *button = buttons[entry.first];
        button->setProperty("menuSelected", selected);
    }
}

QWidget *MenuWidget::fileDropSurface(QWidget *widget)
{
    // Message boxes and file pickers keep their own interaction semantics.
    if (!widget) return nullptr;
    if (qobject_cast<QDialog *>(widget->window()) && widget->window() != payloadWindow) return nullptr;
    if (repairWindow && repairWindow->ownsToolWindow(widget->window())) return widget->window();
    // 从最具体到最泛匹配：面板是各嵌入页的祖先，必须放在最后，
    // 否则子页内的拖放会被误归到面板本体。
    const QList<QWidget *> roots = {repairWindow, payloadWindow,
                                   deviceCheckWindow, configWindow, this};
    for (QWidget *root : roots) {
        if (root && (widget == root || root->isAncestorOf(widget))) return root;
    }
    return nullptr;
}

bool MenuWidget::canAcceptFileTransfer() const
{
    if (deviceCheckWindow) return deviceCheckWindow->canAcceptFileTransfer();
    const auto *manager = DeviceManager::instance();
    return manager->currentMode() == DeviceManager::ADB && !manager->deviceSerial().isEmpty()
           && !DeviceOperationLease::busyFor();
}

bool MenuWidget::handleFileDrop(QWidget *surface, QEvent *event)
{
    auto *drop = static_cast<QDropEvent *>(event);
    const QStringList files = DeviceCheckWindow::droppedFiles(drop->mimeData());
    if (files.isEmpty()) {
        const auto *mime = drop->mimeData();
        if (!mime || !mime->hasUrls()) return false;
        // Preserve the flash-package selector only for pure local-folder drops.
        // Invalid/mixed batches must not fall through to native text editors.
        bool foldersOnly = !mime->urls().isEmpty();
        for (const QUrl &url : mime->urls())
            foldersOnly &= url.isLocalFile() && QFileInfo(url.toLocalFile()).isDir();
        if (foldersOnly) return false;
        drop->ignore();
        return true;
    }
    if (!drop->possibleActions().testFlag(Qt::CopyAction) || !canAcceptFileTransfer()) {
        drop->ignore();
        return true;
    }
    if (event->type() == QEvent::Drop) {
        ensureDeviceCheckWindow();
        if (!deviceCheckWindow->transferFiles(files, surface)) {
            drop->ignore();
            return true;
        }
    }
    // Always copy, including when Explorer proposes MoveAction.
    drop->setDropAction(Qt::CopyAction);
    drop->accept();
    return true;
}

bool MenuWidget::eventFilter(QObject *watched, QEvent *event)
{
    const auto type = event->type();
    if (type == QEvent::DynamicPropertyChange) {
        // 样式表驱动的状态（tile 选中、状态 pill）需要重新求值选择器。
        auto *widget = qobject_cast<QWidget *>(watched);
        if (widget && isAncestorOf(widget)) {
            widget->style()->unpolish(widget);
            widget->style()->polish(widget);
        }
        return QWidget::eventFilter(watched, event);
    }
    if (type == QEvent::MouseButtonPress) {
        // 面板可见时，点击面板与悬浮球之外的区域收起面板。
        auto *widget = qobject_cast<QWidget *>(watched);
        if (widget && isVisible()) {
            const bool insidePanel = widget == this || isAncestorOf(widget);
            const bool insideBall = ball && (widget == ball || ball->isAncestorOf(widget));
            if (!insidePanel && !insideBall) collapsePanel();
        }
        return QWidget::eventFilter(watched, event);
    }
    if (type == QEvent::KeyPress) {
        auto *widget = qobject_cast<QWidget *>(watched);
        if (widget && isVisible() && widget->window() == this &&
            static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            collapsePanel();
            return true;
        }
        return QWidget::eventFilter(watched, event);
    }
    // Do not walk widget ancestry for paint, timer or ordinary input events.
    if (type != QEvent::Show && type != QEvent::Hide && type != QEvent::WindowStateChange &&
        type != QEvent::DragEnter && type != QEvent::DragMove && type != QEvent::Drop)
        return QWidget::eventFilter(watched, event);
    auto *widget = qobject_cast<QWidget *>(watched);
    QWidget *surface = fileDropSurface(widget);
    if (surface) {
        if (event->type() == QEvent::Show && widget == surface) {
            surface->setAcceptDrops(true);
            if (!surface->property("phoneFileDropTarget").toBool()) {
                surface->setProperty("phoneFileDropTarget", true);
                const QString hint = QStringLiteral("ADB 模式下拖入文件传到手机 /sdcard/（同名文件会覆盖）");
                surface->setToolTip(surface->toolTip().isEmpty() ? hint : surface->toolTip() + '\n' + hint);
            }
        }
        if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove ||
            event->type() == QEvent::Drop) {
            return handleFileDrop(surface, event);
        }
        if (event->type() == QEvent::Show || event->type() == QEvent::Hide ||
            event->type() == QEvent::WindowStateChange) {
            const QList<QWidget *> roots = {this, repairWindow, payloadWindow,
                                           deviceCheckWindow, configWindow};
            if (roots.contains(widget)) updateMenuSelection();
            // 子页被关闭或隐藏时面板回主页，避免停留在空页。
            if (type == QEvent::Hide && widget != homePage && pageTitles.contains(widget) &&
                pages->currentWidget() == widget && isVisible())
                showHome();
        }
    }
    return QWidget::eventFilter(watched, event);
}

void MenuWidget::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange && !isMinimized()) updateMenuSelection();
}

void MenuWidget::setVisible(bool visible)
{
    QWidget::setVisible(visible);
    updateMenuSelection();
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
    screenCastSubtitle->setText(screenCastController->statusText());
    const bool streaming = state == ScreenCastController::Streaming || state == ScreenCastController::Stopping;
    screenCastPill->setText(loading ? QStringLiteral("启动中")
                          : streaming ? QStringLiteral("流式中") : QStringLiteral("未连接"));
    screenCastPill->setProperty("state", loading ? "loading" : streaming ? "ok" : "off");
    if (ball) {
        ball->setCastState(loading ? FloatingBall::Loading
                         : streaming ? FloatingBall::Streaming : FloatingBall::Idle);
    }
    if (loading) {
        button->setText(QString());
        paintScreenCastSpinner();
        screenCastSpinnerTimer->start();
    } else {
        screenCastSpinnerTimer->stop();
        button->setText(QString());
        const QPixmap pixmap = UIHelper::menuIcon(QStringLiteral("monitor-smartphone"),
                                                  devicePixelRatioF()).pixmap(24, 24);
        button->setIcon(pixmap);
        screenCastIconLabel->setPixmap(pixmap);
    }
}

void MenuWidget::updateDeviceRow()
{
    const auto mode = DeviceManager::instance()->currentMode();
    const bool adb = mode == DeviceManager::ADB;
    const bool fastboot = mode == DeviceManager::Fastboot;
    devicePill->setText(adb ? QStringLiteral("已连接 · ADB")
                      : fastboot ? QStringLiteral("已连接 · Fastboot") : QStringLiteral("未连接"));
    devicePill->setProperty("state", adb ? "ok" : fastboot ? "loading" : "off");
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
    screenCastIconLabel->setPixmap(pixmap);
}

void MenuWidget::updatePosition()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (!screen || !ball) return;
    const QRect area = screen->availableGeometry();
    ball->move(area.right() - ball->width() - 15, area.center().y() - ball->height() / 2);
    positionPanel();
}

void MenuWidget::positionPanel()
{
    if (!ball) return;
    QScreen *screen = QGuiApplication::screenAt(ball->geometry().center());
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (!screen) return;
    const QRect area = screen->availableGeometry();
    int x = ball->geometry().left() - 10 - width();
    x = std::max(area.left(), std::min(x, area.right() - width() + 1));
    const int y = std::max(area.top(), std::min(ball->geometry().center().y() - height() / 2,
                                                area.bottom() - height() + 1));
    move(x, y);
}

void MenuWidget::showBall()
{
    if (!ball) return;
    updatePosition();
    ball->show();
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
        showSubmenu(RepairTools, ensureRepairWindow());
        break;
    case Payload:
        showSubmenu(Payload, ensurePayloadWindow());
        break;
    case ExtractImg:
        extractImg();
        break;
    case DeviceCheck:
        showSubmenu(DeviceCheck, ensureDeviceCheckWindow());
        break;
    case Configuration:
        showSubmenu(Configuration, ensureConfigWindow());
        break;
    case ContactAuthor:
        openAuthorImage();
        break;
    case Exit:
        cleanupAndExit();
        break;
    case Minimize:
        collapsePanel();
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
