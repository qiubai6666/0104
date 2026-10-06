#include "screencastcontroller.h"
#include "devicemanager.h"
#include "deviceoperationlease.h"
#include "processmanager.h"
#include "resourceextractor.h"
#include <QDir>

ScreenCastController::ScreenCastController(QObject *parent) : QObject(parent)
{
    m_startTimer.setSingleShot(true);
    m_startTimer.setInterval(10000);
    connect(&m_startTimer, &QTimer::timeout, this, [this] {
        if (m_state == Starting) {
            m_stopStatus = QStringLiteral("投屏启动超时，请重试");
            stop();
        }
    });
    auto *manager = DeviceManager::instance();
    connect(manager, &DeviceManager::deviceModeChanged, this, [this] { checkDevice(); });
    // Includes serial replacement without a mode change.
    connect(manager, &DeviceManager::deviceInfoUpdated, this, [this] { checkDevice(); });
    connect(DeviceOperationLease::instance(), &DeviceOperationLease::changed,
            this, [this](bool held) { if (!held) tryStart(); });
}

ScreenCastController::~ScreenCastController()
{
    if (!m_process) return;
    disconnect(m_process, nullptr, this, nullptr);
    if (m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_process->waitForFinished(1000);
    }
}

void ScreenCastController::setState(State state, const QString &status)
{
    m_state = state;
    m_statusText = status;
    emit stateChanged();
}

void ScreenCastController::toggle()
{
    if (m_state != Idle) { stop(); return; }
    m_stopStatus.clear();
    setState(WaitingForDevice, QStringLiteral("等待 ADB 设备，请连接手机并允许 USB 调试；再次点击取消"));
    // Queue startup so the button paints its spinner before launching scrcpy.
    QTimer::singleShot(0, this, &ScreenCastController::tryStart);
}

void ScreenCastController::tryStart()
{
    auto *manager = DeviceManager::instance();
    if (m_state != WaitingForDevice || DeviceOperationLease::busyFor() ||
        manager->currentMode() != DeviceManager::ADB || manager->deviceSerial().isEmpty()) return;

    m_serial = manager->deviceSerial();
    QProcess *process = ProcessManager::createProcess(this);
    m_process = process;
    process->setProperty("orangeNonBlockingTool", true);
    process->setWorkingDirectory(ResourceExtractor::getResourcePath());
    // Drain output throughout the session; scrcpy logs must not accumulate.
    connect(process, &QProcess::readyReadStandardOutput, this, [process] { process->readAllStandardOutput(); });
    connect(process, &QProcess::readyReadStandardError, this, [process] { process->readAllStandardError(); });
    connect(process, &QProcess::started, this, [this, process] {
        if (process != m_process) return;
        if (m_state == Stopping) { process->kill(); return; }
        m_startTimer.stop();
        setState(Streaming, QStringLiteral("正在投屏；再次点击停止投屏"));
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish(process, m_state == Stopping ? QStringLiteral("点击开始投屏") :
                   QStringLiteral("无法启动投屏，请检查 scrcpy 文件后重试"));
    });
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, process](int code, QProcess::ExitStatus exit) {
        finish(process, m_state == Stopping || (code == 0 && exit == QProcess::NormalExit) ?
               QStringLiteral("点击开始投屏") : QStringLiteral("投屏已结束或启动失败，请检查连接后重试"));
    });
    setState(Starting, QStringLiteral("正在启动投屏；再次点击取消"));
    if (m_process != process || m_state != Starting) return;
    m_startTimer.start();
    process->start(QDir(ResourceExtractor::getResourcePath()).filePath("scrcpy.exe"),
                   {"--max-size", "1024", "--video-bit-rate", "4M", "--serial", m_serial});
}

void ScreenCastController::finish(QProcess *process, const QString &status)
{
    if (process != m_process) return;
    m_startTimer.stop();
    disconnect(process, nullptr, this, nullptr);
    m_process = nullptr;
    m_serial.clear();
    process->deleteLater();
    const QString finalStatus = m_stopStatus.isEmpty() ? status : m_stopStatus;
    m_stopStatus.clear();
    setState(Idle, finalStatus);
}

void ScreenCastController::stop()
{
    m_startTimer.stop();
    if (!m_process) {
        m_serial.clear();
        setState(Idle, QStringLiteral("点击开始投屏"));
        return;
    }
    if (m_state == Stopping) return;
    setState(Stopping, QStringLiteral("正在停止投屏"));
    if (m_process->state() == QProcess::NotRunning)
        finish(m_process, QStringLiteral("点击开始投屏"));
    else m_process->kill();
}

void ScreenCastController::checkDevice()
{
    if (m_state == WaitingForDevice) { tryStart(); return; }
    if (m_state != Starting && m_state != Streaming) return;
    auto *manager = DeviceManager::instance();
    if (manager->currentMode() != DeviceManager::ADB || manager->deviceSerial() != m_serial) stop();
}
