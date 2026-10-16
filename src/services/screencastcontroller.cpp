#include "screencastcontroller.h"
#include "devicemanager.h"
#include "deviceoperationlease.h"
#include "processmanager.h"
#include "resourceextractor.h"
#include <QDir>

ScreenCastController::ScreenCastController(QObject *parent) : QObject(parent)
{
    // Failed/disconnected sessions still back off while ADB may report a
    // rebooting device online. Normal window closes do not need this delay.
    m_retryTimer.setSingleShot(true);
    m_retryTimer.setInterval(2000);
    connect(&m_retryTimer, &QTimer::timeout, this, &ScreenCastController::tryStart);
    m_startTimer.setSingleShot(true);
    m_startTimer.setInterval(10000);
    connect(&m_startTimer, &QTimer::timeout, this, [this] {
        if (m_state == Starting) {
            m_stopStatus = QStringLiteral("投屏启动超时，将自动重试；再次点击取消自动投屏");
            stopCurrentProcess();
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
    m_autoCastEnabled = false;
    m_retryTimer.stop();
    m_startTimer.stop();
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
    if (m_autoCastEnabled || m_state != Idle) { stop(); return; }
    m_autoCastEnabled = true;
    m_stopStatus.clear();
    setState(WaitingForDevice, QStringLiteral("自动投屏已开启，等待 ADB 设备；请允许 USB 调试，再次点击取消自动投屏"));
    // Queue startup so the button paints its spinner before launching scrcpy.
    QTimer::singleShot(0, this, &ScreenCastController::tryStart);
}

void ScreenCastController::tryStart()
{
    auto *manager = DeviceManager::instance();
    if (!m_autoCastEnabled || m_state != WaitingForDevice || m_retryTimer.isActive() ||
        DeviceOperationLease::busyFor() ||
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
        setState(Streaming, QStringLiteral("正在投屏；断开后自动重连，再次点击取消自动投屏"));
        emit sessionWindowAppeared(process->processId());
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish(process, m_state == Stopping ? QStringLiteral("点击开始投屏") :
                   QStringLiteral("无法启动投屏，将自动重试；请检查 scrcpy 文件，再次点击取消自动投屏"));
    });
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, process](int code, QProcess::ExitStatus exit) {
        const bool normalExit = code == 0 && exit == QProcess::NormalExit;
        finish(process, normalExit ?
               QStringLiteral("投屏窗口已关闭，正在自动恢复；再次点击取消自动投屏") :
               QStringLiteral("投屏已结束或启动失败，将自动重试；再次点击取消自动投屏"), normalExit);
    });
    setState(Starting, QStringLiteral("正在启动投屏；再次点击取消自动投屏"));
    if (m_process != process || m_state != Starting) return;
    m_startTimer.start();
    process->start(QDir(ResourceExtractor::getResourcePath()).filePath("scrcpy.exe"),
                   {"--max-size", "1024", "--video-bit-rate", "4M", "--serial", m_serial});
}

void ScreenCastController::finish(QProcess *process, const QString &status, bool normalExit)
{
    if (process != m_process) return;
    auto *manager = DeviceManager::instance();
    const bool restartSameDevice = normalExit && m_state == Streaming &&
        manager->currentMode() == DeviceManager::ADB && manager->deviceSerial() == m_serial;
    m_startTimer.stop();
    disconnect(process, nullptr, this, nullptr);
    m_process = nullptr;
    m_serial.clear();
    process->deleteLater();
    const QString finalStatus = m_stopStatus.isEmpty() ? status : m_stopStatus;
    m_stopStatus.clear();
    if (m_autoCastEnabled) {
        // Schedule before stateChanged so a reentrant manual stop cannot leave
        // a live failure timer. The queued fast path also rechecks user intent,
        // device availability and operation leases without recursive startup.
        if (restartSameDevice) QTimer::singleShot(0, this, &ScreenCastController::tryStart);
        else m_retryTimer.start();
        setState(WaitingForDevice, finalStatus);
    } else {
        setState(Idle, QStringLiteral("点击开始投屏"));
    }
    emit sessionEnded();
}

void ScreenCastController::sendNavigationKey(int keyCode)
{
    if (m_state != Streaming || m_serial.isEmpty()) return;
    auto *manager = DeviceManager::instance();
    if (manager->currentMode() != DeviceManager::ADB) return;
    QProcess *process = ProcessManager::createProcess(this);
    process->setWorkingDirectory(ResourceExtractor::getResourcePath());
    connect(process, &QProcess::readyReadStandardOutput, process, [process] { process->readAllStandardOutput(); });
    connect(process, &QProcess::readyReadStandardError, process, [process] { process->readAllStandardError(); });
    // 一次性按键命令：即发即忘，结束后自清理，不阻塞 UI。
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            process, &QObject::deleteLater);
    connect(process, &QProcess::errorOccurred, process, [process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) process->deleteLater();
    });
    process->start(ResourceExtractor::getAdbPath(),
                   {QStringLiteral("-s"), m_serial, QStringLiteral("shell"),
                    QStringLiteral("input"), QStringLiteral("keyevent"), QString::number(keyCode)});
}

void ScreenCastController::stop()
{
    // Only an explicit stop (button / application exit) clears auto-cast intent.
    m_autoCastEnabled = false;
    m_retryTimer.stop();
    m_stopStatus.clear();
    stopCurrentProcess();
}

void ScreenCastController::stopCurrentProcess()
{
    m_startTimer.stop();
    if (!m_process) {
        m_serial.clear();
        setState(Idle, QStringLiteral("点击开始投屏"));
        return;
    }
    if (m_state == Stopping) return;
    setState(Stopping, m_autoCastEnabled ? QStringLiteral("连接中断，正在等待自动重连") :
                                          QStringLiteral("正在停止投屏"));
    if (m_process->state() == QProcess::NotRunning)
        finish(m_process, QStringLiteral("点击开始投屏"));
    else m_process->kill();
}

void ScreenCastController::checkDevice()
{
    if (m_state == WaitingForDevice) { tryStart(); return; }
    if (m_state != Starting && m_state != Streaming) return;
    auto *manager = DeviceManager::instance();
    if (manager->currentMode() != DeviceManager::ADB || manager->deviceSerial() != m_serial) {
        m_stopStatus = QStringLiteral("设备已断开或切换，等待 ADB 恢复后自动投屏；再次点击取消自动投屏");
        stopCurrentProcess();
    }
}
