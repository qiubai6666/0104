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

signals:
    void stateChanged();

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
