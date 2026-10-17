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
#include <dwmapi.h>
#endif

namespace {
// 紧凑竖条布局：容器 6px 边距、36px 图标按钮、间距 4，整体 48×128。
constexpr int kMargin = 6;
constexpr int kSpacing = 4;
constexpr int kButtonSize = 36;
constexpr int kPlacementGap = 8; // 与投屏窗口的间隙（物理像素）

// 跟随窗口拖动/缩放需要接近鼠标事件的刷新率；等待窗口出现时用较慢的轮询。
constexpr int kFollowIntervalMs = 16;
constexpr int kAttachIntervalMs = 100;
// Win32 把最小化窗口放在 -32000 附近；这类窗口不能当作投屏画面。
constexpr int kMinimizedCoordinate = -10000;

int actionKeyCode(ScreenCastNavBar::NavAction action)
{
    switch (action) {
    case ScreenCastNavBar::NavAction::Back: return 4;    // KEYCODE_BACK
    case ScreenCastNavBar::NavAction::Home: return 3;    // KEYCODE_HOME
    case ScreenCastNavBar::NavAction::Recents: return 187; // KEYCODE_APP_SWITCH
    }
    return 0;
}

#ifdef Q_OS_WIN
void applyNoActivate(HWND hwnd)
{
    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (!(exStyle & WS_EX_NOACTIVATE))
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle | WS_EX_NOACTIVATE);
}

bool hasTopMostStyle(HWND hwnd)
{
    return (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
}

// 其他虚拟桌面上的窗口仍然 IsWindowVisible，但 DWM 会将其 cloaked；
// 不排除就会在切换桌面后只看到按钮、看不到投屏。
bool isCloaked(HWND hwnd)
{
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
           cloaked != 0;
}

// 可见画面边界：GetWindowRect 含不可见的缩放边框，最大化时还会超出工作区；
// DWM 扩展边框才是用户实际看到、也真正挡住导航条的区域。
QRect extendedFrameRect(HWND hwnd)
{
    RECT rc{};
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rc, sizeof(rc)))) {
        const QRect frame(rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top);
        if (frame.width() > 0 && frame.height() > 0)
            return frame;
    }
    if (GetWindowRect(hwnd, &rc))
        return QRect(rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top);
    return QRect();
}

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
    if (pid != ctx->pid) return TRUE;
    if (isCloaked(hwnd)) return TRUE;
    RECT rc{};
    const bool hasRect = GetWindowRect(hwnd, &rc) != FALSE;
    const QRect rect = hasRect
        ? QRect(rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top) : QRect();
    wchar_t className[128]{};
    GetClassNameW(hwnd, className, 127);
    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (!ScreenCastNavBar::isCastWindowCandidate(
            IsWindowVisible(hwnd) != FALSE, IsIconic(hwnd) != FALSE,
            (exStyle & WS_EX_TOOLWINDOW) != 0, rect, QString::fromWCharArray(className)))
        return TRUE;
    const long area = rect.width() * rect.height();
    if (area > ctx->bestArea) {
        ctx->bestArea = area;
        ctx->best = hwnd;
    }
    return TRUE;
}

void *largestPidWindow(DWORD pid)
{
    EnumContext ctx;
    ctx.pid = pid;
    EnumWindows(collectPidWindows, reinterpret_cast<LPARAM>(&ctx));
    return ctx.best;
}
#endif
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
    applyNoActivate(reinterpret_cast<HWND>(winId()));
#endif
    setCursor(Qt::PointingHandCursor);
    buildButtons();

    m_attachTimer.setInterval(kAttachIntervalMs);
    connect(&m_attachTimer, &QTimer::timeout, this, &ScreenCastNavBar::tryAttach);
    m_pollTimer.setInterval(kFollowIntervalMs);
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

bool ScreenCastNavBar::isCastWindowCandidate(bool visible, bool iconic, bool toolWindow,
                                             const QRect &windowRect, const QString &className)
{
    if (!visible || iconic || toolWindow) return false;
    if (windowRect.width() <= 0 || windowRect.height() <= 0) return false;
    if (windowRect.left() <= kMinimizedCoordinate || windowRect.top() <= kMinimizedCoordinate)
        return false;
    // scrcpy 是控制台程序；手动从终端启动时控制台可能比画面窗口更大。
    if (className == QLatin1String("ConsoleWindowClass") ||
        className == QLatin1String("PseudoConsoleWindow"))
        return false;
    return true;
}

void ScreenCastNavBar::attachToSession(qint64 pid)
{
#ifdef Q_OS_WIN
    m_targetPid = pid;
    m_castHwnd = nullptr;
    m_castTopMost = false;
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
#ifdef Q_OS_WIN
    if (m_castTopMost) {
        SetWindowPos(reinterpret_cast<HWND>(winId()), HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        m_castTopMost = false;
    }
#endif
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
void *ScreenCastNavBar::findCastWindow() const
{
    return largestPidWindow(static_cast<DWORD>(m_targetPid));
}

void ScreenCastNavBar::tryAttach()
{
    if (!m_targetPid) return;
    if (void *found = findCastWindow()) {
        m_attachTimer.stop();
        m_castHwnd = found;
        m_castTopMost = false;
        updatePlacement();
        if (m_castHwnd && !m_pollTimer.isActive()) m_pollTimer.start();
    } else if (isVisible()) {
        hide();
    }
}

void ScreenCastNavBar::updatePlacement()
{
    HWND cast = static_cast<HWND>(m_castHwnd);
    if (!m_targetPid) {
        if (isVisible()) hide();
        return;
    }
    if (!cast) {
        m_pollTimer.stop();
        if (!m_attachTimer.isActive()) m_attachTimer.start();
        if (isVisible()) hide();
        return;
    }
    if (!IsWindow(cast)) {
        // scrcpy 退出或重建窗口：重新按进程查找，避免导航条从此停在原地。
        m_castHwnd = nullptr;
        if (isVisible()) hide();
        m_pollTimer.stop();
        m_attachTimer.start();
        return;
    }
    DWORD windowPid = 0;
    GetWindowThreadProcessId(cast, &windowPid);
    if (windowPid != static_cast<DWORD>(m_targetPid)) {
        // 窗口句柄被系统复用给其他进程：不能继续跟随。
        m_castHwnd = nullptr;
        if (isVisible()) hide();
        m_pollTimer.stop();
        m_attachTimer.start();
        return;
    }
    if (!IsWindowVisible(cast) || IsIconic(cast) || isCloaked(cast)) {
        if (isVisible()) hide();
        return;
    }
    const QRect castRect = extendedFrameRect(cast);
    if (castRect.isEmpty()) {
        if (isVisible()) hide();
        return;
    }
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

    // 位置全部走原生 SetWindowPos（物理像素），绕开 Qt 逻辑像素换算。
    const HWND nav = reinterpret_cast<HWND>(winId());
    if (!isVisible()) {
        show();
        applyNoActivate(nav); // 原生窗口重建后扩展样式可能丢失
    }
    RECT current{};
    const bool hasCurrent = GetWindowRect(nav, &current) != FALSE;
    if (!hasCurrent || current.left != placement.left() || current.top != placement.top() ||
        current.right - current.left != placement.width() ||
        current.bottom - current.top != placement.height()) {
        SetWindowPos(nav, nullptr, placement.x(), placement.y(),
                     placement.width(), placement.height(),
                     SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
    raiseAboveCast(nav, cast);
}

void ScreenCastNavBar::raiseAboveCast(void *navWindow, void *castWindow)
{
    const HWND nav = static_cast<HWND>(navWindow);
    const HWND cast = static_cast<HWND>(castWindow);
    const bool castTopMost = hasTopMostStyle(cast);
    if (castTopMost != m_castTopMost) {
        // 投屏窗口被人为置顶时导航条也要置顶，否则 SetWindowPos 无法越过置顶层。
        SetWindowPos(nav, castTopMost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        m_castTopMost = castTopMost;
    }
    if (GetWindow(cast, GW_HWNDPREV) == nav)
        return;
    HWND above = GetWindow(cast, GW_HWNDPREV);
    // 投屏窗口是普通窗口中的最上层时，above 属于置顶层，不能把导航条也带成置顶。
    if (!castTopMost && above && hasTopMostStyle(above))
        above = nullptr;
    // 插到“投屏窗口上方”而不是 HWND_TOP：用户切到其他应用时导航条仍留在其下，
    // 但任何情况下都不会被投屏画面盖住。
    SetWindowPos(nav, above ? above : HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}
#endif
