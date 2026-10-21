#include "menuwidget.h"
#include "deviceoperationlease.h"
#include "devicepicker.h"
#include "elidedlabel.h"
#include "configwindow.h"
#include "devicecheckwindow.h"
#include "screencastcontroller.h"
#include "screencastnavbar.h"
#include "payloadwindow.h"
#include "repairwindow.h"
#include "openlistservice.h"
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
#include <QMovie>
#include <QMouseEvent>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QScreen>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QCloseEvent>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
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
        if (property("castCard").toBool()) {
            paintCastCard();
            return;
        }
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const bool dark = palette().color(QPalette::Window).lightness() < 128;
        const QColor accent = dark ? QColor(63, 140, 242) : QColor(47, 128, 237);
        QColor background = Qt::transparent;
        if (property("menuSelected").toBool() && isEnabled())
            background = dark ? QColor(63, 140, 242, 34) : QColor(47, 128, 237, 26);
        else if (isDown()) background = dark ? QColor(66, 97, 110, 200) : QColor(206, 228, 238, 230);
        else if (underMouse()) background = dark ? QColor(74, 98, 114, 128) : QColor(212, 230, 237, 128);
        if (background.alpha()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(background);
            painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 14, 14);
        }
        if ((underMouse() || (property("menuSelected").toBool() && isEnabled())) && isEnabled()) {
            painter.setPen(QPen(accent, 1.1));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 14, 14);
        }
    }

private:
    void paintCastCard()
    {
        const bool dark = palette().color(QPalette::Window).lightness() < 128;
        const QString tone = property("castTone").toString();
        QColor top;
        QColor bottom;
        QColor border;
        if (tone == QStringLiteral("pending")) {
            top = dark ? QColor("#33301f") : QColor("#FBF8F1");
            bottom = dark ? QColor("#26303a") : QColor("#F4F8FB");
            border = dark ? QColor(226, 154, 45, 64) : QColor(210, 139, 34, 61);
        } else if (tone == QStringLiteral("live")) {
            top = dark ? QColor("#1f332a") : QColor("#F3FAF7");
            bottom = dark ? QColor("#24333c") : QColor("#F1F8FB");
            border = dark ? QColor(69, 181, 126, 64) : QColor(28, 157, 91, 56);
        } else {
            top = dark ? QColor("#263543") : QColor("#F4F9FD");
            bottom = dark ? QColor("#212e39") : QColor("#F8FBFD");
            border = dark ? QColor(146, 166, 181, 46) : QColor(113, 149, 173, 36);
        }
        const QColor hoverBorder = dark ? QColor(63, 140, 242, 110) : QColor(47, 126, 235, 64);
        const QRectF card = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QLinearGradient gradient(card.topLeft(), card.bottomRight());
        gradient.setColorAt(0.0, top);
        gradient.setColorAt(0.56, dark ? bottom.lighter(104) : QColor("#EEF5FA"));
        gradient.setColorAt(1.0, bottom);
        painter.setPen(QPen(underMouse() && isEnabled() ? hoverBorder : border, 1.0));
        painter.setBrush(gradient);
        painter.drawRoundedRect(card, 14, 14);

        QPainterPath clip;
        clip.addRoundedRect(card, 14, 14);
        painter.save();
        painter.setClipPath(clip);
        painter.setPen(QPen(QColor(255, 255, 255, dark ? 26 : 235), 1.0));
        painter.drawLine(QPointF(card.left() + 12, card.top() + 1.2),
                         QPointF(card.right() - 12, card.top() + 1.2));
        if (isDown()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(47, 128, 237, dark ? 26 : 12));
            painter.drawRect(card);
        }
        painter.restore();
    }
};

QPixmap rotatedChevron(const QColor &color, qreal ratio, int angle)
{
    const qreal dpr = qMax<qreal>(1.0, ratio);
    QPixmap pixmap(qRound(14 * dpr), qRound(14 * dpr));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(7, 7);
    painter.rotate(angle);
    painter.translate(-7, -7);
    UIHelper::menuIcon(QStringLiteral("chevron-right"), color, dpr)
        .paint(&painter, QRect(0, 0, 14, 14));
    painter.end();
    return pixmap;
}

// 功能 tile：把图标与文字合成为一张 DPI 感知位图，保持预览稿“图标在左、文字在右”的观感，
// 同时按钮仍保留 tooltip/accessibleName 与原 objectName。
QIcon tileIcon(const QString &iconName, const QString &label, qreal ratio)
{
    const qreal dpr = qMax<qreal>(1.0, ratio);
    const int width = 128, height = 68;
    QPixmap pixmap(qRound(width * dpr), qRound(height * dpr));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool dark = QApplication::palette().color(QPalette::Window).lightness() < 128;

    const QRectF well(11, 19, 30, 30);
    QPainterPath wellPath;
    wellPath.addRoundedRect(well, 9, 9);
    QLinearGradient wellFill(well.topLeft(), well.bottomRight());
    wellFill.setColorAt(0, dark ? QColor("#2c3c49") : QColor("#FFFFFF"));
    wellFill.setColorAt(1, dark ? QColor("#263541") : QColor("#EEF5FA"));
    painter.fillPath(wellPath, wellFill);
    painter.setPen(QPen(dark ? QColor(146, 166, 181, 40) : QColor(110, 148, 173, 33), 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(wellPath);
    UIHelper::menuIcon(iconName, QColor(dark ? "#6fa6cc" : "#4B7E9E"), dpr)
        .paint(&painter, QRect(18, 26, 16, 16));

    painter.setPen(QColor(dark ? "#dbe6ee" : "#3C5568"));
    QFont font(QStringLiteral("Microsoft YaHei"));
    font.setPixelSize(12);
    font.setWeight(QFont::Medium);
    painter.setFont(font);
    painter.drawText(QRect(50, 0, width - 58, height), Qt::AlignLeft | Qt::AlignVCenter, label);
    painter.end();
    return QIcon(pixmap);
}

// 柔和状态胶囊：浅色底 + 同色圆点，用于投屏卡与目标设备行。
class StatusPill : public QLabel
{
public:
    explicit StatusPill(QWidget *parent = nullptr) : QLabel(parent)
    {
        setAlignment(Qt::AlignCenter);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        setFixedHeight(19);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        QFont labelFont(QStringLiteral("Microsoft YaHei"));
        labelFont.setPixelSize(10);
        labelFont.setWeight(QFont::DemiBold);
        setFont(labelFont);
    }

    QSize sizeHint() const override
    {
        return QSize(26 + fontMetrics().horizontalAdvance(text()), 19);
    }

    void setState(const QString &value)
    {
        setProperty("state", value);
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        const bool dark = palette().color(QPalette::Window).lightness() < 128;
        const QString state = property("state").toString();
        QColor background;
        QColor foreground;
        QColor dot;
        if (state == QStringLiteral("ok")) {
            background = dark ? QColor(34, 163, 90, 36) : QColor("#EAF7F0");
            foreground = dark ? QColor("#7ee2a8") : QColor("#127645");
            dot = dark ? QColor("#42c983") : QColor("#1C9D5B");
        } else if (state == QStringLiteral("loading") || state == QStringLiteral("warn")) {
            background = dark ? QColor(226, 154, 45, 34) : QColor("#FBF3E3");
            foreground = dark ? QColor("#f4c66a") : QColor("#8B5A0B");
            dot = dark ? QColor("#e2a33e") : QColor("#D28B22");
        } else {
            background = dark ? QColor(154, 168, 181, 28) : QColor("#EDF2F6");
            foreground = dark ? QColor("#93a7b6") : QColor("#6E8290");
            dot = dark ? QColor("#8fa3b2") : QColor("#8A99A5");
        }

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF pill = QRectF(rect());
        painter.setPen(Qt::NoPen);
        painter.setBrush(background);
        painter.drawRoundedRect(pill, pill.height() / 2.0, pill.height() / 2.0);
        painter.setBrush(dot);
        painter.drawEllipse(QPointF(10.5, pill.center().y()), 2.5, 2.5);
        painter.setPen(foreground);
        painter.setFont(font());
        painter.drawText(QRectF(18, 0, pill.width() - 26, pill.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, text());
    }
};

// 目标行尾部锁图标：投屏中替代 chevron，提示目标已被锁定。
QPixmap lockPixmap(const QColor &color, qreal ratio)
{
    const qreal dpr = qMax<qreal>(1.0, ratio);
    QPixmap pixmap(qRound(14 * dpr), qRound(14 * dpr));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(color, 1.35, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    QPainterPath shackle;
    shackle.moveTo(4.5, 6.2);
    shackle.lineTo(4.5, 5.1);
    shackle.cubicTo(4.5, 2.7, 9.5, 2.7, 9.5, 5.1);
    shackle.lineTo(9.5, 6.2);
    painter.drawPath(shackle);
    painter.drawRoundedRect(QRectF(3.2, 6.1, 7.6, 5.4), 1.8, 1.8);
    painter.end();
    return pixmap;
}

QString castWellStyle(const QString &tone, bool dark)
{
    QColor top;
    QColor bottom;
    QColor border;
    if (tone == QStringLiteral("pending")) {
        top = dark ? QColor("#342f21") : QColor("#FFFFFF");
        bottom = dark ? QColor("#2b3027") : QColor("#FDF6E9");
        border = dark ? QColor(226, 154, 45, 48) : QColor(210, 139, 34, 46);
    } else if (tone == QStringLiteral("live")) {
        top = dark ? QColor("#24362e") : QColor("#FFFFFF");
        bottom = dark ? QColor("#22332f") : QColor("#EDF9F3");
        border = dark ? QColor(69, 181, 126, 48) : QColor(28, 157, 91, 36);
    } else {
        top = dark ? QColor("#2d3d4a") : QColor("#FFFFFF");
        bottom = dark ? QColor("#27343f") : QColor("#EEF6FC");
        border = dark ? QColor(146, 166, 181, 44) : QColor(108, 147, 173, 41);
    }
    return QStringLiteral(
        "background:qlineargradient(x1:0,y1:0,x2:1,y2:1, stop:0 %1, stop:1 %2);"
        " border:1px solid %3; border-radius:9px;")
        .arg(top.name(), bottom.name(),
             QStringLiteral("rgba(%1,%2,%3,%4)").arg(border.red()).arg(border.green())
                 .arg(border.blue()).arg(border.alpha()));
}

} // namespace

// 主入口悬浮球：浅玻璃外壳内常驻播放 Gengar 动画；悬停加内圈蓝环，
// 投屏启动中压暗动画并叠加琥珀进度环，流式中显示绿色徽标。
// 点击由 MenuWidget 接管面板显隐。
// 定义在全局作用域，与 menuwidget.h 中的前置声明（成员 ball）保持同一类型。
class FloatingBall : public QWidget
{
public:
    enum CastState { Idle, Loading, Streaming };

    FloatingBall()
        : mascot(new QMovie(QStringLiteral(":/mascot/gengar.gif"), QByteArrayLiteral("gif"), this))
    {
        // Qt::Tool：悬浮球不占用任务栏图标，主菜单单独保留任务栏入口。
        setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                       Qt::Tool);
        setAttribute(Qt::WA_TranslucentBackground);
        // 应用存活锚点：面板不承担 QuitOnClose，由悬浮球保持事件循环。
        setAttribute(Qt::WA_QuitOnClose);
        setFixedSize(46, 46);
        setWindowTitle(APP_NAME);
        setCursor(Qt::PointingHandCursor);
        mascot->setCacheMode(QMovie::CacheAll);
        connect(mascot, &QMovie::frameChanged, this, [this] { update(); });
        // 旧环境缺少 qgif 插件时保持外壳与 command 回退，不显示空球。
        if (mascot->isValid())
            mascot->start();
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
    void enterEvent(QEnterEvent *) override
    {
        hovered = true;
        update();
    }
    void leaveEvent(QEvent *) override
    {
        hovered = false;
        update();
    }
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
        const QColor accent = dark ? QColor("#3f8cf2") : QColor("#2F80ED");
        // 预览稿：球面与面板同为半透明 surface 底 + 白色内描边。
        QColor fill = dark ? QColor(18, 27, 37, 153) : QColor(252, 253, 255, 168);
        painter.setPen(QPen(dark ? QColor(255, 255, 255, 23) : QColor(255, 255, 255, 128), 1.0));
        painter.setBrush(fill);
        painter.drawEllipse(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5));

        // 46px 窗口不能像预览稿那样外扩光晕，改用壳内蓝环表达悬停。
        if (hovered && castState != Loading) {
            painter.setPen(QPen(QColor(accent.red(), accent.green(), accent.blue(),
                                        dark ? 140 : 120), 1.6));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(QRectF(rect()).adjusted(2.8, 2.8, -2.8, -2.8));
        }

        // 92x87 原图等比缩至 34px，悬停只放大 1px，避免撑破外壳。
        const qreal artWidth = hovered ? 35.0 : 34.0;
        const qreal artHeight = artWidth * 87.0 / 92.0;
        const QRectF artRect((width() - artWidth) / 2.0, (height() - artHeight) / 2.0,
                             artWidth, artHeight);
        const QPixmap frame = mascot->currentPixmap();
        if (!frame.isNull()) {
            if (castState == Loading)
                painter.setOpacity(0.55);
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            painter.drawPixmap(artRect, frame, QRectF(frame.rect()));
            painter.setOpacity(1.0);
        } else {
            const QRectF iconRect((width() - 22) / 2.0, (height() - 22) / 2.0, 22, 22);
            UIHelper::menuIcon(QStringLiteral("command"), accent, devicePixelRatioF())
                .paint(&painter, iconRect.toRect());
        }

        if (castState == Loading) {
            const QColor amber("#C58A2B");
            const QRectF ring = QRectF(rect()).adjusted(3.2, 3.2, -3.2, -3.2);
            painter.setPen(QPen(QColor(amber.red(), amber.green(), amber.blue(), 46), 2.0));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(ring);
            painter.setPen(QPen(amber, 2.0, Qt::SolidLine, Qt::RoundCap));
            painter.drawArc(ring, spinnerAngle * 16, 260 * 16);
        }
        if (castState == Streaming) {
            const QRectF badge(width() - 16, 4, 12, 12);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor("#22c55e"));
            painter.drawEllipse(badge);
            painter.setPen(QPen(dark ? QColor(20, 30, 40) : QColor(255, 255, 255), 1.5));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(badge);
        }
    }

private:
    CastState castState = Idle;
    bool pressed = false;
    bool hovered = false;
    QMovie *mascot = nullptr;
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

    // 启动预热：后台预下载配置文件，首次打开配置窗口无需等待网络。
    QTimer::singleShot(500, this, [] { ConfigWindow::preloadConfigFile(); });
}

MenuWidget::~MenuWidget()
{
    qApp->removeEventFilter(this);
    screenCastSpinnerTimer->stop();
    delete screenCastController;
    DeviceManager::instance()->releaseAdbOnlyMonitoring();
    const QList<QWidget *> windows = {ball, repairWindow, payloadWindow, deviceCheckWindow,
                                      configWindow, screenCastNavBar};
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
    backButton->setIcon(UIHelper::menuIcon(QStringLiteral("chevron-left"),
                                           QColor(dark ? "#93a7b6" : "#647585"),
                                           devicePixelRatioF()));
    backButton->setIconSize(QSize(15, 15));
    backButton->setCursor(Qt::PointingHandCursor);
    backButton->setAccessibleName(QStringLiteral("返回"));
    backButton->setStyleSheet("QPushButton { background:rgba(146,166,181,40); border:none; border-radius:9px; }"
                              "QPushButton:hover { background:rgba(47,128,237,30); }"
                              "QPushButton:pressed { background:rgba(47,128,237,50); }");
    headerLayout->addWidget(backButton);
    pageTitle = new QLabel(header);
    pageTitle->setStyleSheet(QString("background:transparent; font-size:15px; font-weight:600; color:%1;")
                             .arg(dark ? "#e9edf3" : "#22364a"));
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
    connect(screenCastController, &ScreenCastController::stateChanged,
            this, &MenuWidget::updateDeviceRow);
    // 导航条跟随投屏会话：按键请求转回控制器发送，会话窗口出现/结束驱动显隐。
    screenCastNavBar = new ScreenCastNavBar;
    connect(screenCastNavBar, &ScreenCastNavBar::keyRequested,
            screenCastController, &ScreenCastController::sendNavigationKey);
    connect(screenCastController, &ScreenCastController::sessionWindowAppeared,
            screenCastNavBar, &ScreenCastNavBar::attachToSession);
    connect(screenCastController, &ScreenCastController::sessionEnded,
            screenCastNavBar, &ScreenCastNavBar::detach);
    auto *deviceManager = DeviceManager::instance();
    connect(deviceManager, &DeviceManager::deviceModeChanged,
            this, &MenuWidget::updateDeviceRow);
    connect(deviceManager, &DeviceManager::deviceListChanged,
            this, &MenuWidget::updateDeviceRow);
    connect(deviceManager, &DeviceManager::deviceSelectionChanged,
            this, [this](const QString &) { updateDeviceRow(); });
    connect(deviceManager, &DeviceManager::extendedDeviceDetailsUpdated,
            this, [this](const QString &) { updateDeviceRow(); });
    connect(DeviceOperationLease::instance(), &DeviceOperationLease::changed,
            this, [this](bool held) { if (held) closeDevicePicker(); });
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
    layout->setSpacing(0);

    // 品牌行
    auto *brandRow = new QHBoxLayout;
    brandRow->setContentsMargins(0, 0, 0, 0);
    brandRow->setSpacing(7);
    auto *brandIcon = new QLabel(page);
    brandIcon->setPixmap(UIHelper::menuIcon(QStringLiteral("command"),
        QColor(dark ? "#3f8cf2" : "#43779D"), devicePixelRatioF()).pixmap(14, 14));
    brandIcon->setFixedSize(14, 14);
    brandIcon->setScaledContents(true);
    auto *brandText = new QLabel(QStringLiteral("ORANGETOOLS"), page);
    brandText->setStyleSheet(QString("font-size:10px; font-weight:700; letter-spacing:1.65px; color:%1;")
                             .arg(dark ? "#93a7b6" : "#617B8D"));
    brandRow->addWidget(brandIcon);
    brandRow->addWidget(brandText);
    brandRow->addStretch(1);
    layout->addLayout(brandRow);
    layout->addSpacing(10);

    // 投屏主卡：点击直接开始/停止投屏（buttons[ScreenCast]）。
    // 卡片本体由 RowButton 绘制渐变与状态描边，图标 well 与文案分别排版。
    auto *castButton = new RowButton(page);
    castButton->setObjectName("screenCastButton");
    castButton->setAccessibleName(QStringLiteral("投屏"));
    castButton->setCursor(Qt::PointingHandCursor);
    castButton->setFixedHeight(58);
    castButton->setProperty("castCard", true);
    castButton->setProperty("castTone", "idle");
    auto *castGrid = new QGridLayout(castButton);
    castGrid->setContentsMargins(11, 0, 11, 0);
    castGrid->setSpacing(10);
    screenCastIconLabel = new QLabel(castButton);
    screenCastIconLabel->setObjectName("castWell");
    screenCastIconLabel->setFixedSize(30, 30);
    screenCastIconLabel->setAlignment(Qt::AlignCenter);
    screenCastIconLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto *castTitle = new QLabel(QStringLiteral("手机投屏"), castButton);
    castTitle->setAttribute(Qt::WA_TransparentForMouseEvents);
    castTitle->setStyleSheet(QString("font-size:12.5px; font-weight:600; color:%1;")
                             .arg(dark ? "#e9edf3" : "#22364a"));
    screenCastSubtitle = new ElidedLabel(castButton);
    screenCastSubtitle->setAttribute(Qt::WA_TransparentForMouseEvents);
    screenCastSubtitle->setStyleSheet(QString("font-size:10px; color:%1;")
                                      .arg(dark ? "#93a7b6" : "#6F8494"));
    screenCastPill = new StatusPill(castButton);
    screenCastPill->setObjectName("statusPill");
    screenCastPill->setProperty("state", "off");
    screenCastPill->setText(QStringLiteral("未投屏"));
    auto *castTexts = new QVBoxLayout;
    castTexts->setContentsMargins(0, 0, 0, 0);
    castTexts->setSpacing(1);
    castTexts->addWidget(castTitle);
    castTexts->addWidget(screenCastSubtitle);
    castGrid->addWidget(screenCastIconLabel, 0, 0, Qt::AlignVCenter);
    castGrid->addLayout(castTexts, 0, 1, Qt::AlignVCenter);
    castGrid->setColumnStretch(1, 1);
    castGrid->addWidget(screenCastPill, 0, 2, Qt::AlignVCenter | Qt::AlignRight);
    layout->addWidget(castButton);
    layout->addSpacing(8);
    buttons.append(castButton);
    connect(castButton, &QPushButton::clicked, this, &MenuWidget::onButtonClicked);

    // 设备行：显示投屏与设备检测的共同目标，点击展开选择器。
    const QColor ink(dark ? "#e9edf3" : "#22364a");
    const QColor inkDim(dark ? "#93a7b6" : "#647585");
    const QColor inkFaint(dark ? "#7f95a6" : "#95a5b2");
    deviceRowButton = new QPushButton(page);
    deviceRowButton->setObjectName("deviceRowButton");
    deviceRowButton->setAccessibleName(QStringLiteral("选择目标设备"));
    deviceRowButton->setCursor(Qt::PointingHandCursor);
    deviceRowButton->setToolTip(QStringLiteral("选择投屏与设备检测使用的设备"));
    deviceRowButton->setFixedHeight(48);
    auto *deviceRowLayout = new QHBoxLayout(deviceRowButton);
    deviceRowLayout->setContentsMargins(9, 0, 9, 0);
    deviceRowLayout->setSpacing(8);

    // 设备头像：28px 渐变 well + 在线状态点。
    deviceAvatar = new QLabel(deviceRowButton);
    deviceAvatar->setFixedSize(28, 28);
    deviceAvatar->setAlignment(Qt::AlignCenter);
    deviceAvatar->setAttribute(Qt::WA_TransparentForMouseEvents);
    deviceAvatar->setPixmap(UIHelper::menuIcon(QStringLiteral("smartphone"),
                                              QColor(dark ? "#9db0be" : "#50738A"),
                                              devicePixelRatioF()).pixmap(14, 14));
    devicePresenceDot = new QLabel(deviceAvatar);
    devicePresenceDot->setFixedSize(7, 7);
    devicePresenceDot->move(20, 20);
    devicePresenceDot->setAttribute(Qt::WA_TransparentForMouseEvents);
    devicePresenceDot->setStyleSheet(QString(
        "background:#1C9D5B; border:1px solid %1; border-radius:3px;")
        .arg(dark ? "#1a2732" : "#FBFDFE"));
    devicePresenceDot->hide();

    auto *deviceTexts = new QVBoxLayout;
    deviceTexts->setContentsMargins(0, 0, 0, 0);
    deviceTexts->setSpacing(1);
    auto *eyebrowRow = new QHBoxLayout;
    eyebrowRow->setContentsMargins(0, 0, 0, 0);
    eyebrowRow->setSpacing(4);
    auto *eyebrow = new QLabel(QStringLiteral("目标设备"), deviceRowButton);
    eyebrow->setAttribute(Qt::WA_TransparentForMouseEvents);
    eyebrow->setStyleSheet(QString("font-size:8.5px; font-weight:600; letter-spacing:.8px; color:%1;")
                           .arg(dark ? "#7f95a6" : "#93A2AE"));
    auto *eyebrowDot = new QLabel(QStringLiteral("·"), deviceRowButton);
    eyebrowDot->setAttribute(Qt::WA_TransparentForMouseEvents);
    eyebrowDot->setStyleSheet(QString("font-size:8.5px; color:%1;")
                              .arg(dark ? "#5d7181" : "#C1CCD4"));
    deviceSerialLabel = new ElidedLabel(deviceRowButton);
    deviceSerialLabel->setText(QStringLiteral("未连接"));
    deviceSerialLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    deviceSerialLabel->setStyleSheet(QString(
        "font-family:'Consolas'; font-size:8px; letter-spacing:.4px; color:%1;")
        .arg(dark ? "#7f95a6" : "#8B9BA8"));
    eyebrowRow->addWidget(eyebrow);
    eyebrowRow->addWidget(eyebrowDot);
    eyebrowRow->addWidget(deviceSerialLabel, 1);
    deviceNameLabel = new ElidedLabel(deviceRowButton);
    deviceNameLabel->setText(QStringLiteral("未检测到设备"));
    deviceNameLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    deviceNameLabel->setStyleSheet(QString("font-size:12.5px; font-weight:600; color:%1;")
                                   .arg(ink.name()));
    deviceTexts->addLayout(eyebrowRow);
    deviceTexts->addWidget(deviceNameLabel);

    devicePill = new StatusPill(deviceRowButton);
    devicePill->setObjectName("statusPill");
    devicePill->setProperty("state", "off");
    devicePill->setText(QStringLiteral("未连接"));
    deviceEndmark = new QLabel(deviceRowButton);
    deviceEndmark->setAttribute(Qt::WA_TransparentForMouseEvents);
    deviceEndmark->setFixedSize(18, 18);
    deviceEndmark->setAlignment(Qt::AlignCenter);
    deviceEndmark->setPixmap(rotatedChevron(inkFaint, devicePixelRatioF(), 90));

    deviceRowLayout->addWidget(deviceAvatar, 0, Qt::AlignVCenter);
    deviceRowLayout->addLayout(deviceTexts, 1);
    deviceRowLayout->addWidget(devicePill, 0, Qt::AlignVCenter);
    deviceRowLayout->addWidget(deviceEndmark, 0, Qt::AlignVCenter);
    layout->addWidget(deviceRowButton);
    layout->addSpacing(8);
    connect(deviceRowButton, &QPushButton::clicked, this, &MenuWidget::toggleDevicePicker);
    refreshDeviceRowChrome();

    // 功能组 2×3：六个入口共享一张圆角面板，单元格用 1px 分隔线切分。
    auto *toolGroup = new QFrame(page);
    toolGroup->setObjectName("toolGroup");
    toolGroup->setFixedHeight(206);
    toolGroup->setStyleSheet(dark
        ? QStringLiteral("QFrame#toolGroup { background:qlineargradient(x1:0,y1:0,x2:0,y2:1,"
                         " stop:0 rgba(38,52,66,150), stop:1 rgba(34,47,60,140));"
                         " border:1px solid rgba(146,166,181,48); border-radius:14px; }")
        : QStringLiteral("QFrame#toolGroup { background:qlineargradient(x1:0,y1:0,x2:0,y2:1,"
                         " stop:0 rgba(254,255,255,184), stop:1 rgba(248,252,254,173));"
                         " border:1px solid rgba(111,144,166,43); border-radius:14px; }"));
    auto *tileGrid = new QGridLayout(toolGroup);
    tileGrid->setContentsMargins(1, 1, 1, 1);
    tileGrid->setHorizontalSpacing(0);
    tileGrid->setVerticalSpacing(0);
    tileGrid->setColumnStretch(0, 1);
    tileGrid->setColumnStretch(1, 1);
    const QVector<QPair<int, QString>> tiles = {
        {RepairTools, QStringLiteral("秋白工作室")}, {Payload, QStringLiteral("PAYLOAD")},
        {ExtractImg, QStringLiteral("提取IMG")}, {DeviceCheck, QStringLiteral("设备检测")},
        {Configuration, QStringLiteral("配置")}, {ContactAuthor, QStringLiteral("联系作者")}};
    const QStringList tileIcons = {QStringLiteral("wrench"), QStringLiteral("package-open"),
        QStringLiteral("file-down"), QStringLiteral("smartphone"), QStringLiteral("sliders-horizontal"),
        QStringLiteral("heart")};
    const QString divider = dark ? QStringLiteral("rgba(146,166,181,36)")
                                 : QStringLiteral("rgba(120,150,170,31)");
    const QString hoverBg = dark ? QStringLiteral("rgba(63,140,242,26)")
                                 : QStringLiteral("rgba(47,128,237,13)");
    const QString pressedBg = dark ? QStringLiteral("rgba(63,140,242,40)")
                                   : QStringLiteral("rgba(47,128,237,22)");
    for (int index = 0; index < tiles.size(); ++index) {
        auto *tile = new QPushButton(toolGroup);
        tile->setObjectName(QString("menuAction%1").arg(tiles[index].first));
        tile->setFixedHeight(68);
        tile->setIconSize(QSize(128, 68));
        tile->setIcon(tileIcon(tileIcons[index], tiles[index].second, devicePixelRatioF()));
        tile->setToolTip(tiles[index].second);
        tile->setAccessibleName(tiles[index].second);
        tile->setCursor(Qt::PointingHandCursor);
        QString edges;
        if (index % 2 == 0)
            edges += QStringLiteral(" border-right:1px solid %1;").arg(divider);
        if (index < 4)
            edges += QStringLiteral(" border-bottom:1px solid %1;").arg(divider);
        QString cornerRadius;
        if (index == 0) cornerRadius = QStringLiteral("border-top-left-radius:13px;");
        else if (index == 1) cornerRadius = QStringLiteral("border-top-right-radius:13px;");
        else if (index == 4) cornerRadius = QStringLiteral("border-bottom-left-radius:13px;");
        else if (index == 5) cornerRadius = QStringLiteral("border-bottom-right-radius:13px;");
        tile->setStyleSheet(QStringLiteral(
            "QPushButton { background:transparent; border:none; padding:0;%1 }"
            "QPushButton:hover { background:%2; %3 }"
            "QPushButton:pressed { background:%4; %3 }")
            .arg(edges, hoverBg, cornerRadius, pressedBg));
        tile->setProperty("menuDirectAction", tiles[index].first == ExtractImg ||
                                              tiles[index].first == ContactAuthor);
        tileGrid->addWidget(tile, index / 2, index % 2);
        buttons.append(tile);
        connect(tile, &QPushButton::clicked, this, &MenuWidget::onButtonClicked);
    }
    layout->addWidget(toolGroup);
    layout->addStretch(1);
    layout->addSpacing(8);

    // 底部：收起（buttons[Minimize]）与退出（buttons[Exit]）共用一段式描边底栏。
    auto *footer = new QFrame(page);
    footer->setObjectName("panelFooter");
    footer->setFixedHeight(36);
    footer->setStyleSheet(dark
        ? QStringLiteral("QFrame#panelFooter { background:rgba(255,255,255,12);"
                         " border:1px solid rgba(146,166,181,56); border-radius:11px; }")
        : QStringLiteral("QFrame#panelFooter { background:rgba(255,255,255,128);"
                         " border:1px solid rgba(117,149,171,56); border-radius:11px; }"));
    auto *footerRow = new QHBoxLayout(footer);
    footerRow->setContentsMargins(1, 1, 1, 1);
    footerRow->setSpacing(0);
    const struct { MenuAction action; const char *label; const char *icon; } footerEntries[] = {
        {Minimize, "收起", "panel-right-close"}, {Exit, "退出", "power"}};
    QPushButton *footerButtons[2] = {nullptr, nullptr};
    for (int index = 0; index < 2; ++index) {
        const auto &entry = footerEntries[index];
        auto *button = new QPushButton(QString::fromUtf8(entry.label), footer);
        button->setObjectName(QString("menuAction%1").arg(int(entry.action)));
        button->setFixedHeight(34);
        button->setIcon(UIHelper::menuIcon(QString::fromLatin1(entry.icon),
                                           QColor(dark ? "#93a7b6" : "#607687"),
                                           devicePixelRatioF()));
        button->setIconSize(QSize(14, 14));
        button->setCursor(Qt::PointingHandCursor);
        button->setAccessibleName(QString::fromUtf8(entry.label));
        const QString sideRadius = index == 0
            ? QStringLiteral("border-top-left-radius:10px; border-bottom-left-radius:10px;")
            : QStringLiteral("border-left:1px solid rgba(117,149,171,41);"
                             " border-top-right-radius:10px; border-bottom-right-radius:10px;");
        button->setStyleSheet(QStringLiteral(
            "QPushButton { background:transparent; border:none; color:%1;"
            " font-size:11.5px; padding:0 6px; %2 }"
            "QPushButton:hover { background:%3; color:%4; }"
            "QPushButton:pressed { background:%5; }")
            .arg(dark ? "#93a7b6" : "#607687", sideRadius,
                 dark ? "rgba(63,140,242,22)" : "rgba(47,128,237,15)",
                 dark ? "#8fc0f5" : "#1F63C7",
                 dark ? "rgba(63,140,242,38)" : "rgba(47,128,237,26)"));
        footerRow->addWidget(button);
        connect(button, &QPushButton::clicked, this, &MenuWidget::onButtonClicked);
        footerButtons[index] = button;
    }
    // 视觉顺序为「收起、退出」，buttons 向量下标必须与 MenuAction 枚举一致
    // （Exit=7、Minimize=8），故按枚举顺序补入向量。
    buttons.append(footerButtons[1]);
    buttons.append(footerButtons[0]);
    layout->addWidget(footer);

    // 目标设备浮层与切换提示：浮层不参与布局，覆盖在设备行下方。
    devicePicker = new DevicePicker(page);
    devicePicker->hide();
    connect(devicePicker, &DevicePicker::deviceSelected, this, [this](const QString &serial) {
        closeDevicePicker();
        QString name = targetDisplayName();
        if (name.isEmpty()) name = serial;
        showDeviceToast(QStringLiteral("已切换：%1").arg(name));
    });
    connect(devicePicker, &DevicePicker::closeRequested, this, &MenuWidget::closeDevicePicker);
    connect(devicePicker, &DevicePicker::refreshRequested, this, [this] {
        devicePicker->setScanning(true);
        devicePicker->showHint(QStringLiteral("正在扫描已连接的设备…"));
        DeviceManager::instance()->refreshDevices();
        QTimer::singleShot(900, this, [this] {
            if (!devicePicker) return;
            devicePicker->setScanning(false);
            devicePicker->showHint(QStringLiteral("刚刚刷新 · %1 台设备在线")
                                   .arg(devicePicker->onlineCount()));
        });
    });

    deviceToast = new QLabel(page);
    deviceToast->setObjectName("deviceTargetToast");
    deviceToast->setAlignment(Qt::AlignCenter);
    deviceToast->setFixedHeight(28);
    deviceToast->setTextFormat(Qt::RichText);
    deviceToast->setStyleSheet(QString(
        "QLabel#deviceTargetToast { background:rgba(28,48,64,242); color:#FFFFFF;"
        " border-radius:10px; padding:0 12px; font-size:10px; }"));
    deviceToast->hide();
    deviceToastTimer = new QTimer(this);
    deviceToastTimer->setSingleShot(true);
    deviceToastTimer->setInterval(1900);
    connect(deviceToastTimer, &QTimer::timeout, deviceToast, &QWidget::hide);
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
    closeDevicePicker();
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
    closeDevicePicker();
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
    }
    return repairWindow;
}

PayloadWindow *MenuWidget::ensurePayloadWindow()
{
    if (!payloadWindow) {
        payloadWindow = new PayloadWindow(pages);
        pages->addWidget(payloadWindow);
        pageTitles.insert(payloadWindow, QStringLiteral("Payload 分区提取"));
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
    if (qobject_cast<QDialog *>(widget->window())) return nullptr;
    if (deviceCheckWindow &&
        (widget == deviceCheckWindow || deviceCheckWindow->isAncestorOf(widget)))
        return deviceCheckWindow;
    return nullptr;
}

bool MenuWidget::canAcceptFileTransfer() const
{
    return deviceCheckWindow && deviceCheckWindow->canAcceptFileTransfer();
}

bool MenuWidget::handleFileDrop(QWidget *surface, QEvent *event)
{
    auto *drop = static_cast<QDropEvent *>(event);
    const QStringList files = DeviceCheckWindow::droppedFiles(drop->mimeData());
    if (files.isEmpty()) {
        const auto *mime = drop->mimeData();
        if (!mime || !mime->hasUrls()) return false;
        drop->ignore();
        return true;
    }
    if (!drop->possibleActions().testFlag(Qt::CopyAction) || !canAcceptFileTransfer()) {
        drop->ignore();
        return true;
    }
    if (event->type() == QEvent::Drop) {
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
        const QByteArray name = static_cast<QDynamicPropertyChangeEvent *>(event)->propertyName();
        // Qt 的内部样式属性和业务元数据不能触发再次 polish。
        if (name != "state" && name != "menuSelected")
            return QWidget::eventFilter(watched, event);
        auto *widget = qobject_cast<QWidget *>(watched);
        if (widget && isAncestorOf(widget)) {
            if (name == "state" && widget->objectName() == "statusPill") {
                widget->style()->unpolish(widget);
                widget->style()->polish(widget);
                widget->update();
            } else if (name == "menuSelected") {
                // 选中态由按钮绘制，不需要重算样式表及字体。
                widget->update();
            }
        }
        return QWidget::eventFilter(watched, event);
    }
    if (type == QEvent::MouseButtonPress) {
        // 面板可见时，点击面板与悬浮球之外的区域收起面板。
        auto *widget = qobject_cast<QWidget *>(watched);
        if (widget && isVisible()) {
            // 选择器展开时，面板内其他位置的点击先收起浮层，不吞掉本次点击。
            if (devicePicker && devicePicker->isVisible()) {
                const bool insidePicker = widget == devicePicker || devicePicker->isAncestorOf(widget);
                const bool insideRow = widget == deviceRowButton || deviceRowButton->isAncestorOf(widget);
                if (!insidePicker && !insideRow && isAncestorOf(widget)) closeDevicePicker();
            }
            const bool insidePanel = widget == this || isAncestorOf(widget);
            const bool insideBall = ball && (widget == ball || ball->isAncestorOf(widget));
            // 下拉列表、对话框等独立顶层窗口不在 isAncestorOf 判定链内，
            // 其宿主控件属于面板/悬浮球时视为内部点击，不收起面板。
            bool insideOwnedWindow = false;
            if (!insidePanel && !insideBall && widget->window() != widget) {
                for (const QWidget *owner = widget->window()->parentWidget();
                     owner; owner = owner->parentWidget()) {
                    if (owner == this || (ball && owner == ball) ||
                        isAncestorOf(owner) || (ball && ball->isAncestorOf(owner))) {
                        insideOwnedWindow = true;
                        break;
                    }
                }
            }
            if (!insidePanel && !insideBall && !insideOwnedWindow) collapsePanel();
        }
        return QWidget::eventFilter(watched, event);
    }
    if (type == QEvent::KeyPress) {
        auto *widget = qobject_cast<QWidget *>(watched);
        if (widget && isVisible() && widget->window() == this &&
            static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            if (devicePicker && devicePicker->isVisible()) closeDevicePicker();
            else collapsePanel();
            return true;
        }
        return QWidget::eventFilter(watched, event);
    }
    // Do not walk widget ancestry for paint, timer or ordinary input events.
    if (type != QEvent::Show && type != QEvent::Hide && type != QEvent::WindowStateChange &&
        type != QEvent::DragEnter && type != QEvent::DragMove && type != QEvent::Drop)
        return QWidget::eventFilter(watched, event);
    auto *widget = qobject_cast<QWidget *>(watched);
    if (type == QEvent::Show || type == QEvent::Hide || type == QEvent::WindowStateChange) {
        const QList<QWidget *> roots = {this, repairWindow, payloadWindow,
                                       deviceCheckWindow, configWindow};
        if (roots.contains(widget)) updateMenuSelection();
        if (type == QEvent::Hide && widget != homePage && pageTitles.contains(widget) &&
            pages->currentWidget() == widget && isVisible())
            showHome();
    }
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
    if (!visible) {
        closeDevicePicker();
        if (deviceToast) deviceToast->hide();
        if (deviceToastTimer) deviceToastTimer->stop();
    }
    QWidget::setVisible(visible);
    updateMenuSelection();
}

void MenuWidget::updateScreenCastButton()
{
    auto *button = buttons[ScreenCast];
    const auto state = screenCastController->state();
    const bool loading = state == ScreenCastController::WaitingForDevice ||
                         state == ScreenCastController::Starting ||
                         state == ScreenCastController::Stopping;
    const bool streaming = state == ScreenCastController::Streaming;
    const bool dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
    const QString tone = streaming ? QStringLiteral("live")
                                   : loading ? QStringLiteral("pending")
                                             : QStringLiteral("idle");
    button->setProperty("screenCastLoading", loading);
    button->setProperty("castTone", tone);
    button->setProperty("menuSelected", loading || streaming);
    button->setToolTip(screenCastController->statusText());
    // Remain clickable while a disconnected session is stopping so the user
    // can immediately cancel auto-reconnect as well.
    button->setEnabled(true);
    button->setAccessibleName(QStringLiteral("投屏"));
    button->setAccessibleDescription(screenCastController->statusText());
    button->update();

    QString castSubtitle = screenCastController->statusText();
    if (state == ScreenCastController::Idle) {
        const QString target = targetDisplayName();
        if (!target.isEmpty())
            castSubtitle = QStringLiteral("%1 · %2").arg(target, castSubtitle);
    }
    screenCastSubtitle->setText(castSubtitle);

    auto *pill = static_cast<StatusPill *>(screenCastPill);
    pill->setText(streaming ? QStringLiteral("投屏中")
                            : loading ? QStringLiteral("连接中") : QStringLiteral("未投屏"));
    pill->setState(streaming ? QStringLiteral("ok")
                             : loading ? QStringLiteral("loading") : QStringLiteral("off"));

    screenCastIconLabel->setStyleSheet(castWellStyle(tone, dark));
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
        const QColor iconColor = streaming
            ? (dark ? QColor("#7ee2a8") : QColor("#127645"))
            : (dark ? QColor("#63a7e6") : QColor("#2E74B7"));
        const QPixmap pixmap = UIHelper::menuIcon(QStringLiteral("monitor-smartphone"),
                                                  iconColor,
                                                  devicePixelRatioF()).pixmap(17, 17);
        button->setIcon(pixmap);
        screenCastIconLabel->setPixmap(pixmap);
    }
}

QString MenuWidget::targetDisplayName() const
{
    auto *manager = DeviceManager::instance();
    const QString serial = manager->deviceSerial();
    if (serial.isEmpty()) return QString();
    const QString display = manager->deviceDisplayName();
    if (!display.isEmpty()) return display;
    for (const auto &entry : manager->deviceEntries())
        if (entry.serial == serial && !entry.model.isEmpty()) return entry.model;
    return QString();
}

void MenuWidget::updateDeviceRow()
{
    auto *manager = DeviceManager::instance();
    const QVector<DeviceManager::DeviceEntry> entries = manager->deviceEntries();
    const QString serial = manager->deviceSerial();
    const DeviceManager::DeviceEntry *target = nullptr;
    for (const auto &entry : entries)
        if (!serial.isEmpty() && entry.serial == serial) target = &entry;

    int operable = 0;
    bool hasUnauthorized = false;
    bool hasOtherUnavailable = false;
    for (const auto &entry : entries) {
        if (entry.isOperable()) {
            ++operable;
        } else if (entry.state == QStringLiteral("unauthorized")) {
            hasUnauthorized = true;
        } else {
            hasOtherUnavailable = true;
        }
    }
    // 仅有未授权设备时也在行内展示它，点击即可看到具体原因。
    if (!target && operable == 0 && !entries.isEmpty()) target = &entries.constFirst();

    QString name;
    if (target) {
        if (!serial.isEmpty() && target->serial == serial) name = manager->deviceDisplayName();
        if (name.isEmpty()) name = target->model;
        if (name.isEmpty()) {
            name = target->state == QStringLiteral("fastboot")
                ? QStringLiteral("Fastboot 设备") : QStringLiteral("Android 设备");
        }
    }

    deviceSerialLabel->setText(target ? target->serial : QStringLiteral("未连接"));
    deviceSerialLabel->setToolTip(target ? target->serial : QString());
    deviceNameLabel->setText(target ? name : QStringLiteral("未检测到设备"));
    deviceNameLabel->setToolTip(target ? name : QString());
    deviceNameLabel->update();

    auto *pill = static_cast<StatusPill *>(devicePill);
    if (operable > 1) {
        pill->setText(QStringLiteral("%1 台在线").arg(operable));
        pill->setState(QStringLiteral("ok"));
    } else if (operable == 1) {
        pill->setText(QStringLiteral("已连接"));
        pill->setState(QStringLiteral("ok"));
    } else if (!entries.isEmpty()) {
        pill->setText(hasUnauthorized && !hasOtherUnavailable
                          ? QStringLiteral("待授权") : QStringLiteral("不可用"));
        pill->setState(QStringLiteral("loading"));
    } else {
        pill->setText(QStringLiteral("未连接"));
        pill->setState(QStringLiteral("off"));
    }

    const bool locked = isTargetLocked();
    if (locked && devicePicker && devicePicker->isVisible()) closeDevicePicker();
    if (devicePresenceDot) devicePresenceDot->setVisible(target && target->isOperable());
    const bool streaming = screenCastController &&
                           screenCastController->state() == ScreenCastController::Streaming;
    deviceRowButton->setAccessibleDescription(
        target ? QStringLiteral("目标设备 %1，序列号 %2%3")
                     .arg(name, target->serial,
                          locked ? QStringLiteral("，投屏流程进行中已锁定") : QString())
               : QStringLiteral("未检测到可操作设备"));
    deviceRowButton->setToolTip(target
        ? (locked ? (streaming ? QStringLiteral("投屏中，目标设备已锁定：%1").arg(name)
                               : QStringLiteral("正在连接，目标设备已锁定：%1").arg(name))
                  : QStringLiteral("当前目标：%1\n点击选择其他设备").arg(name))
        : QStringLiteral("选择投屏与设备检测使用的设备"));

    if (devicePicker) {
        const QString selectedDisplay = (!serial.isEmpty() && target && target->serial == serial)
            ? name : QString();
        devicePicker->setDevices(entries, serial, selectedDisplay);
        if (devicePicker->isVisible()) positionDevicePicker();
    }
    refreshDeviceRowChrome();
    updateScreenCastButton();
}

void MenuWidget::toggleDevicePicker()
{
    if (!devicePicker) return;
    if (devicePicker->isVisible()) {
        closeDevicePicker();
        return;
    }
    if (isTargetLocked()) {
        showDeviceToast(screenCastController->state() == ScreenCastController::Streaming
                            ? QStringLiteral("投屏中，目标设备已锁定")
                            : QStringLiteral("正在连接，目标设备已锁定"), true);
        return;
    }
    updateDeviceRow();
    devicePicker->resetHint();
    setDeviceRowOpen(true);
    positionDevicePicker();
    devicePicker->show();
    devicePicker->raise();
}

void MenuWidget::closeDevicePicker()
{
    if (!devicePicker) return;
    if (devicePicker->isVisible()) devicePicker->hide();
    devicePicker->setScanning(false);
    setDeviceRowOpen(false);
}

void MenuWidget::positionDevicePicker()
{
    if (!devicePicker || !homePage || !deviceRowButton) return;
    devicePicker->adjustSize();
    const int x = deviceRowButton->x();
    int y = deviceRowButton->y() + deviceRowButton->height() + 4;
    const int maxBottom = homePage->height() - 6;
    if (y + devicePicker->height() > maxBottom)
        y = qMax(6, maxBottom - devicePicker->height());
    devicePicker->move(x, y);
}

bool MenuWidget::isTargetLocked() const
{
    if (!screenCastController) return false;
    const auto state = screenCastController->state();
    return state == ScreenCastController::WaitingForDevice ||
           state == ScreenCastController::Starting ||
           state == ScreenCastController::Streaming;
}

void MenuWidget::refreshDeviceRowChrome()
{
    if (!deviceRowButton || !deviceAvatar || !deviceEndmark) return;
    const bool dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
    const bool locked = isTargetLocked();
    const bool open = deviceRowOpen && !locked;
    const QColor accent = dark ? QColor("#63a7e6") : QColor("#1F63C7");

    QString rowStyle;
    if (open) {
        rowStyle = dark
            ? QStringLiteral("QPushButton#deviceRowButton { background:qlineargradient(x1:0,y1:0,x2:0,y2:1,"
                             " stop:0 rgba(63,140,242,40), stop:1 rgba(63,140,242,22));"
                             " border:1px solid rgba(63,140,242,90); border-radius:12px; }"
                             "QPushButton#deviceRowButton:hover { background:qlineargradient(x1:0,y1:0,x2:0,y2:1,"
                             " stop:0 rgba(63,140,242,54), stop:1 rgba(63,140,242,30)); }"
                             "QPushButton#deviceRowButton:pressed { background:rgba(63,140,242,64); }")
            : QStringLiteral("QPushButton#deviceRowButton { background:qlineargradient(x1:0,y1:0,x2:0,y2:1,"
                             " stop:0 rgba(47,126,235,19), stop:1 rgba(47,126,235,9));"
                             " border:1px solid rgba(47,126,235,61); border-radius:12px; }"
                             "QPushButton#deviceRowButton:hover { background:qlineargradient(x1:0,y1:0,x2:0,y2:1,"
                             " stop:0 rgba(47,126,235,23), stop:1 rgba(47,126,235,12)); }"
                             "QPushButton#deviceRowButton:pressed { background:rgba(47,126,235,31); }");
    } else if (locked) {
        rowStyle = dark
            ? QStringLiteral("QPushButton#deviceRowButton { background:qlineargradient(x1:0,y1:0,x2:0,y2:1,"
                             " stop:0 rgba(255,255,255,8), stop:1 rgba(255,255,255,4));"
                             " border:1px solid rgba(146,166,181,40); border-radius:12px; }"
                             "QPushButton#deviceRowButton:hover, QPushButton#deviceRowButton:pressed {"
                             " background:qlineargradient(x1:0,y1:0,x2:0,y2:1,"
                             " stop:0 rgba(255,255,255,8), stop:1 rgba(255,255,255,4)); }")
            : QStringLiteral("QPushButton#deviceRowButton { background:qlineargradient(x1:0,y1:0,x2:0,y2:1,"
                             " stop:0 rgba(255,255,255,87), stop:1 rgba(248,252,254,51));"
                             " border:1px solid rgba(104,139,162,43); border-radius:12px; }"
                             "QPushButton#deviceRowButton:hover, QPushButton#deviceRowButton:pressed {"
                             " background:qlineargradient(x1:0,y1:0,x2:0,y2:1,"
                             " stop:0 rgba(255,255,255,87), stop:1 rgba(248,252,254,51)); }");
    } else {
        rowStyle = dark
            ? QStringLiteral("QPushButton#deviceRowButton { background:qlineargradient(x1:0,y1:0,x2:0,y2:1,"
                             " stop:0 rgba(255,255,255,10), stop:1 rgba(255,255,255,4));"
                             " border:1px solid rgba(146,166,181,56); border-radius:12px; }"
                             "QPushButton#deviceRowButton:hover { background:rgba(63,140,242,30);"
                             " border-color:rgba(63,140,242,90); }"
                             "QPushButton#deviceRowButton:pressed { background:rgba(63,140,242,44); }")
            : QStringLiteral("QPushButton#deviceRowButton { background:qlineargradient(x1:0,y1:0,x2:0,y2:1,"
                             " stop:0 rgba(255,255,255,87), stop:1 rgba(248,252,254,51));"
                             " border:1px solid rgba(104,139,162,59); border-radius:12px; }"
                             "QPushButton#deviceRowButton:hover { background:rgba(139,165,182,26);"
                             " border-color:rgba(47,126,235,69); }"
                             "QPushButton#deviceRowButton:pressed { background:rgba(139,165,182,41); }");
    }
    deviceRowButton->setStyleSheet(rowStyle);
    deviceRowButton->setCursor(locked ? Qt::ArrowCursor : Qt::PointingHandCursor);

    if (open) {
        deviceAvatar->setStyleSheet(dark
            ? QStringLiteral("background:qlineargradient(x1:0,y1:0,x2:1,y2:1, stop:0 #2b3a48, stop:1 #24384a);"
                             " border:1px solid rgba(63,140,242,60); border-radius:9px;")
            : QStringLiteral("background:qlineargradient(x1:0,y1:0,x2:1,y2:1, stop:0 #F8FBFF, stop:1 #E7F1FD);"
                             " border:1px solid rgba(47,126,235,41); border-radius:9px;"));
    } else {
        deviceAvatar->setStyleSheet(dark
            ? QStringLiteral("background:qlineargradient(x1:0,y1:0,x2:1,y2:1, stop:0 #2b3a46, stop:1 #26343f);"
                             " border:1px solid rgba(146,166,181,40); border-radius:9px;")
            : QStringLiteral("background:qlineargradient(x1:0,y1:0,x2:1,y2:1, stop:0 #F9FBFD, stop:1 #EAF2F8);"
                             " border:1px solid rgba(107,144,168,41); border-radius:9px;"));
    }
    deviceAvatar->setPixmap(UIHelper::menuIcon(QStringLiteral("smartphone"),
        open ? accent : (dark ? QColor("#9db0be") : QColor("#50738A")),
        devicePixelRatioF()).pixmap(14, 14));

    if (locked) {
        deviceEndmark->setPixmap(lockPixmap(accent, devicePixelRatioF()));
        deviceEndmark->setStyleSheet(dark
            ? QStringLiteral("background:rgba(63,140,242,34); border:1px solid rgba(63,140,242,52); border-radius:6px;")
            : QStringLiteral("background:rgba(47,126,235,26); border:1px solid rgba(47,126,235,31); border-radius:6px;"));
    } else {
        const QColor chevronColor = open ? (dark ? QColor("#63a7e6") : QColor("#2F7EEB"))
                                         : (dark ? QColor("#7f95a6") : QColor("#95A6B2"));
        deviceEndmark->setPixmap(rotatedChevron(chevronColor, devicePixelRatioF(), open ? 270 : 90));
        deviceEndmark->setStyleSheet(QStringLiteral("background:transparent; border:none; border-radius:6px;"));
    }
}

void MenuWidget::setDeviceRowOpen(bool open)
{
    deviceRowOpen = open;
    refreshDeviceRowChrome();
}

void MenuWidget::showDeviceToast(const QString &text, bool warning)
{
    if (!deviceToast || !homePage) return;
    const QString dotColor = warning ? QStringLiteral("#F4BF57") : QStringLiteral("#7EE0A7");
    deviceToast->setText(QStringLiteral("<span style=\"color:%1;\">●</span>&nbsp;&nbsp;%2")
                             .arg(dotColor, text.toHtmlEscaped()));
    deviceToast->setToolTip(text);
    deviceToast->setMaximumWidth(240);
    deviceToast->adjustSize();
    const int width = deviceToast->width();
    deviceToast->move((homePage->width() - width) / 2,
                      homePage->height() - 56 - deviceToast->height());
    deviceToast->raise();
    deviceToast->show();
    if (deviceToastTimer) deviceToastTimer->start();
}

void MenuWidget::paintScreenCastSpinner()
{
    auto *button = buttons[ScreenCast];
    const bool dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
    const qreal ratio = button->devicePixelRatioF();
    QPixmap pixmap(qRound(17 * ratio), qRound(17 * ratio));
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor spinner = dark ? QColor("#F2C879") : QColor("#C58A2B");
    painter.setPen(QPen(QColor(spinner.red(), spinner.green(), spinner.blue(), 56), 2.2));
    painter.drawEllipse(QRectF(1.4, 1.4, 14.2, 14.2));
    painter.setPen(QPen(spinner, 2.2, Qt::SolidLine, Qt::RoundCap));
    painter.drawArc(QRectF(1.4, 1.4, 14.2, 14.2), screenCastSpinnerAngle * 16, 260 * 16);
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

    // 退出时一并删除 OpenList 桌面传输目录；不跟随符号链接/目录联接。
    const QString toolsFolder = OpenList::desktopTransferDirectory();
    const QFileInfo toolsInfo(toolsFolder);
    if (toolsInfo.exists()) {
        if (!toolsInfo.isDir() || toolsInfo.isSymLink()) {
            qDebug() << "跳过桌面 Tools Folder 清理：不是普通目录" << toolsFolder;
        } else {
            qDebug() << "删除桌面 Tools Folder:" << toolsFolder;
            if (QDir(toolsFolder).removeRecursively()) {
                qDebug() << "删除成功";
            } else {
                qDebug() << "删除失败，可能有文件被占用";
            }
        }
    }

    // 退出应用程序
    qDebug() << "退出应用程序";
    QApplication::quit();
}
