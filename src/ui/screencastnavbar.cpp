#include "screencastnavbar.h"
#include "uihelper.h"

#include <QApplication>
#include <QColor>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPushButton>
#include <QVBoxLayout>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
// 紧凑竖条布局：容器 6px 边距、36px 图标按钮、间距 4，整体 48×128。
constexpr int kMargin = 6;
constexpr int kSpacing = 4;
constexpr int kButtonSize = 36;
constexpr int kPlacementGap = 8; // 与投屏窗口的间隙（物理像素）

int actionKeyCode(ScreenCastNavBar::NavAction action)
{
    switch (action) {
    case ScreenCastNavBar::NavAction::Back: return 4;    // KEYCODE_BACK
    case ScreenCastNavBar::NavAction::Home: return 3;    // KEYCODE_HOME
    case ScreenCastNavBar::NavAction::Recents: return 187; // KEYCODE_APP_SWITCH
    }
    return 0;
}
}

ScreenCastNavBar::ScreenCastNavBar(QWidget *parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFixedSize(preferredSize());
#ifdef Q_OS_WIN
    // 提前创建原生窗口并加上不抢焦点扩展样式：点击按钮不会把键盘焦点
    // 从 scrcpy 窗口夺走（否则投屏的键盘控制会“莫名失效”）。
    if (HWND hwnd = reinterpret_cast<HWND>(winId())) {
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
                          GetWindowLongPtrW(hwnd, GWL_EXSTYLE) | WS_EX_NOACTIVATE);
    }
#endif
    setCursor(Qt::PointingHandCursor);
    buildButtons();

    m_attachTimer.setInterval(200);
    connect(&m_attachTimer, &QTimer::timeout, this, &ScreenCastNavBar::tryAttach);
    m_pollTimer.setInterval(500);
    connect(&m_pollTimer, &QTimer::timeout, this, &ScreenCastNavBar::updatePlacement);
}

void ScreenCastNavBar::buildButtons()
{
    const bool dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
    const QColor ink(dark ? "#e9edf3" : "#1f1f1f");
    const QColor buttonBorder(dark ? "#4a5a68" : "#e0e0e0");
    const QColor hover(dark ? "#42616e" : "rgba(47,128,237,30)");
    const QColor pressed(dark ? "#4c6e7d" : "rgba(47,128,237,50)");
    const QString style = QStringLiteral(
        "QPushButton { background:transparent; border:1px solid %1; border-radius:9px;"
        " color:%2; }"
        "QPushButton:hover { border-color:%3; background:%4; }"
        "QPushButton:pressed { background:%5; }")
        .arg(buttonBorder.name(), ink.name(), ink.name(), hover.name(QColor::HexArgb),
             pressed.name(QColor::HexArgb));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(kMargin, kMargin, kMargin, kMargin);
    layout->setSpacing(kSpacing);
    struct Entry { const char *icon; QString label; NavAction action; };
    const Entry entries[] = {
        {"corner-up-left", QStringLiteral("返回"), NavAction::Back},
        {"home", QStringLiteral("主页"), NavAction::Home},
        {"recents", QStringLiteral("多任务"), NavAction::Recents},
    };
    for (const Entry &entry : entries) {
        auto *button = new QPushButton(this);
        button->setAccessibleName(entry.label);
        button->setToolTip(entry.label);
        button->setIcon(UIHelper::menuIcon(QString::fromLatin1(entry.icon), ink, devicePixelRatioF()));
        button->setIconSize(QSize(16, 16));
        button->setFixedSize(kButtonSize, kButtonSize);
        button->setCursor(Qt::PointingHandCursor);
        button->setStyleSheet(style);
        connect(button, &QPushButton::clicked, this, [this, entry] {
            emit keyRequested(actionKeyCode(entry.action));
        });
        layout->addWidget(button);
    }
}

int ScreenCastNavBar::androidKeyCode(NavAction action)
{
    return actionKeyCode(action);
}

QSize ScreenCastNavBar::preferredSize()
{
    return QSize(kButtonSize + kMargin * 2,
                 kButtonSize * 3 + kSpacing * 2 + kMargin * 2);
}

QRect ScreenCastNavBar::computePlacement(const QRect &castRect, const QRect &workArea, const QSize &navSize)
{
    int x = castRect.right() + 1 + kPlacementGap;
    if (x + navSize.width() - 1 > workArea.right()) {
        x = castRect.left() - kPlacementGap - navSize.width();
        if (x < workArea.left())
            x = workArea.right() + 1 - navSize.width(); // 兜底：贴工作区右缘
    }
    int y = castRect.top() + (castRect.height() - navSize.height()) / 2;
    y = qBound(workArea.top(), y, workArea.bottom() + 1 - navSize.height());
    return QRect(QPoint(x, y), navSize);
}

void ScreenCastNavBar::attachToSession(qint64 pid)
{
#ifdef Q_OS_WIN
    m_targetPid = pid;
    m_castHwnd = nullptr;
    m_pollTimer.stop();
    if (isVisible()) hide();
    m_attachTimer.start();
    tryAttach(); // 窗口可能已经创建，立即尝试一次
#else
    Q_UNUSED(pid);
#endif
}

void ScreenCastNavBar::detach()
{
    m_targetPid = 0;
    m_castHwnd = nullptr;
    m_attachTimer.stop();
    m_pollTimer.stop();
    if (isVisible()) hide();
}

void ScreenCastNavBar::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    const bool dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath path;
    path.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 12, 12);
    painter.fillPath(path, dark ? QColor(43, 57, 71, 245) : QColor(245, 249, 250, 245));
    painter.setPen(QPen(dark ? QColor("#42525f") : QColor("#d8d8d8"), 1));
    painter.drawPath(path);
}

#ifdef Q_OS_WIN
namespace {
struct EnumContext {
    DWORD pid = 0;
    void *best = nullptr;
    long bestArea = 0;
};

BOOL CALLBACK collectPidWindows(HWND hwnd, LPARAM lparam)
{
    auto *ctx = reinterpret_cast<EnumContext *>(lparam);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    // 只认该进程可见的非工具窗口中面积最大的顶层窗口，避免误认隐藏辅助窗口。
    if (pid != ctx->pid || !IsWindowVisible(hwnd)) return TRUE;
    if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
    RECT rc;
    if (!GetWindowRect(hwnd, &rc)) return TRUE;
    const long area = (rc.right - rc.left) * (rc.bottom - rc.top);
    if (area > ctx->bestArea) {
        ctx->bestArea = area;
        ctx->best = hwnd;
    }
    return TRUE;
}
}

void *ScreenCastNavBar::findCastWindow() const
{
    EnumContext ctx;
    ctx.pid = static_cast<DWORD>(m_targetPid);
    EnumWindows(collectPidWindows, reinterpret_cast<LPARAM>(&ctx));
    return ctx.best;
}

void ScreenCastNavBar::tryAttach()
{
    if (!m_targetPid) return;
    if (HWND hwnd = static_cast<HWND>(findCastWindow())) {
        m_attachTimer.stop();
        m_castHwnd = hwnd;
        updatePlacement();
        m_pollTimer.start();
    }
}

void ScreenCastNavBar::updatePlacement()
{
    HWND cast = static_cast<HWND>(m_castHwnd);
    if (!cast || !IsWindow(cast) || !IsWindowVisible(cast) || IsIconic(cast)) {
        if (isVisible()) hide();
        return;
    }
    RECT rc;
    if (!GetWindowRect(cast, &rc)) {
        if (isVisible()) hide();
        return;
    }
    const QRect castRect(rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (HMONITOR monitor = MonitorFromWindow(cast, MONITOR_DEFAULTTONEAREST))
        GetMonitorInfoW(monitor, &info);
    const QRect workArea(info.rcWork.left, info.rcWork.top,
                         info.rcWork.right - info.rcWork.left,
                         info.rcWork.bottom - info.rcWork.top);
    const UINT dpi = GetDpiForWindow(cast);
    const qreal scale = dpi > 0 ? dpi / 96.0 : 1.0;
    const QSize physical(qRound(preferredSize().width() * scale),
                         qRound(preferredSize().height() * scale));
    const QRect placement = computePlacement(castRect, workArea, physical);
    // 位置全部走原生 SetWindowPos（物理像素），绕开 Qt 逻辑像素换算；
    // 不改 Z 序，避免把投屏窗口顶到其他应用之上。
    if (!isVisible()) show();
    SetWindowPos(reinterpret_cast<HWND>(winId()), nullptr,
                 placement.x(), placement.y(), placement.width(), placement.height(),
                 SWP_NOACTIVATE | SWP_NOZORDER);
    // 兜底位置与投屏窗口重叠时，仅当 scrcpy 本身是前台窗口才把导航条提到
    // 其上，避免盖住画面；不与用户正在使用的其他应用争抢层级。
    if (placement.intersects(castRect) && GetForegroundWindow() == cast)
        SetWindowPos(reinterpret_cast<HWND>(winId()), HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}
#endif
