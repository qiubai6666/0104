#ifndef SCREENCASTNAVBAR_H
#define SCREENCASTNAVBAR_H

#include <QWidget>
#include <QTimer>

// 投屏导航条：贴在 scrcpy 窗口右侧的竖排「返回 / 主页 / 多任务 / 亮屏·息屏」
// 按钮，通过 adb input keyevent 驱动手机。窗口自身不抢焦点（WS_EX_NOACTIVATE），
// 生命周期完全跟随投屏会话：attachToSession 等待 scrcpy 窗口出现后跟随定位，
// detach 停止跟踪并隐藏，不独立存活。
class ScreenCastNavBar : public QWidget
{
    Q_OBJECT
public:
    enum class NavAction { Back, Home, Recents, Power };
    Q_ENUM(NavAction)

    explicit ScreenCastNavBar(QWidget *parent = nullptr);

    // 跟随投屏会话：pid 为 scrcpy 进程；窗口尚未出现时持续轮询等待。
    void attachToSession(qint64 pid);
    // 会话结束：停止跟踪并隐藏。
    void detach();

    bool isTracking() const { return m_targetPid != 0; }
    bool hasCastWindow() const { return m_castHwnd != 0; }

    // 按键语义 → Android keycode（纯逻辑，供测试）。
    static int androidKeyCode(NavAction action);
    // 导航条逻辑尺寸（物理像素由调用方按显示器 DPI 换算）。
    static QSize preferredSize();
    // 物理像素下计算导航条位置：首选投屏窗口右侧、次选左侧、
    // 兜底贴工作区右缘，垂直居中并整体钳制在工作区内（纯逻辑，供测试）。
    static QRect computePlacement(const QRect &castRect, const QRect &workArea, const QSize &navSize);
    // 投屏窗口候选过滤（纯逻辑，供测试）：控制台、最小化、工具窗口和屏外
    // 窗口都不属于投屏画面，避免跟随到 scrcpy 的控制台或隐藏辅助窗口。
    static bool isCastWindowCandidate(bool visible, bool iconic, bool toolWindow,
                                      const QRect &windowRect, const QString &className);

signals:
    void keyRequested(int keyCode);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void buildButtons();
#ifdef Q_OS_WIN
    void *findCastWindow() const;
    void tryAttach();
    void updatePlacement();
    // 把导航条插到投屏窗口正上方：既不浮到无关应用之上，也不会被画面盖住。
    void raiseAboveCast(void *navWindow, void *castWindow);
#endif
    QTimer m_attachTimer;
    QTimer m_pollTimer;
    qint64 m_targetPid = 0;
    void *m_castHwnd = nullptr;
    bool m_castTopMost = false;
};

#endif
