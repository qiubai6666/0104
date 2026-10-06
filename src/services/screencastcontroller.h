#ifndef SCREENCASTCONTROLLER_H
#define SCREENCASTCONTROLLER_H

#include <QObject>
#include <QProcess>
#include <QTimer>

// A button-owned session: wait for an authorized ADB device, then launch only
// this session's scrcpy. Never stop a shared ADB server or unrelated tools.
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
    void setState(State state, const QString &status);
    void finish(QProcess *process, const QString &status);
    State m_state = Idle;
    QString m_statusText = QStringLiteral("点击开始投屏");
    QString m_serial;
    QString m_stopStatus;
    QProcess *m_process = nullptr;
    QTimer m_startTimer;
};

#endif
