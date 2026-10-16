#ifndef SCREENCASTCONTROLLER_H
#define SCREENCASTCONTROLLER_H

#include <QObject>
#include <QProcess>
#include <QTimer>

// A button-owned auto-cast session: retain intent across disconnections and
// retry scrcpy until explicitly stopped. Never stop shared ADB or unrelated tools.
class ScreenCastController : public QObject
{
    Q_OBJECT
public:
    enum State { Idle, WaitingForDevice, Starting, Streaming, Stopping };
    Q_ENUM(State)
    explicit ScreenCastController(QObject *parent = nullptr);
    ~ScreenCastController() override;
    State state() const { return m_state; }
    QString statusText() const { return m_statusText; }
    void toggle();
    void stop();
    // 导航条按键：向当前会话设备发送 adb input keyevent；
    // 仅在流式会话中有效，其余状态静默忽略。
    void sendNavigationKey(int keyCode);

signals:
    void stateChanged();
    // 会话窗口（scrcpy 进程）出现/结束，供投屏导航条跟随显示与隐藏。
    void sessionWindowAppeared(qint64 pid);
    void sessionEnded();

private:
    friend class ScreenCastTests;
    void tryStart();
    void checkDevice();
    void stopCurrentProcess();
    void setState(State state, const QString &status);
    void finish(QProcess *process, const QString &status, bool normalExit = false);
    bool m_autoCastEnabled = false;
    State m_state = Idle;
    QString m_statusText = QStringLiteral("点击开始投屏");
    QString m_serial;
    QString m_stopStatus;
    QProcess *m_process = nullptr;
    QTimer m_startTimer;
    QTimer m_retryTimer;
};

#endif
