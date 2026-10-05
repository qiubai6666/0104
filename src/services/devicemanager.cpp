#include "deviceoperationlease.h"
#include "processmanager.h"
#include "devicemanager.h"
#include "resourceextractor.h"
#include "version.h"
#include <QDebug>
#include <QRegularExpression>

DeviceManager* DeviceManager::m_instance = nullptr;

DeviceManager* DeviceManager::instance()
{
    if (!m_instance) {
        m_instance = new DeviceManager();
    }
    return m_instance;
}

DeviceManager::DeviceManager(QObject *parent)
    : QObject(parent)
    , m_currentMode(None)
    , m_isChecking(false)
    , m_adbOnly(false)
    , m_fullModeRefCount(0)
    , m_adbOnlyRefCount(0)
    , m_isPaused(false)
{
    // 创建定时器
    m_checkTimer = new QTimer(this);
    connect(m_checkTimer, &QTimer::timeout, this, &DeviceManager::checkDeviceStatus);

    // 创建进程对象
    m_adbCheckProcess = ProcessManager::createProcess(this);
    m_fastbootCheckProcess = ProcessManager::createProcess(this);
    m_infoProcess = ProcessManager::createProcess(this);
    m_infoProcess->setWorkingDirectory(ResourceExtractor::getResourcePath());

    m_infoTimer = new QTimer(this);
    m_infoTimer->setSingleShot(true);
    m_infoTimer->setInterval(5000);
    qDebug() << "DeviceManager 单例已创建";
}

DeviceManager::~DeviceManager()
{
    stopMonitoring();
    if (m_checkTimer && m_checkTimer->isActive()) {
        m_checkTimer->stop();
    }

    if (m_adbCheckProcess) {
        if (m_adbCheckProcess->state() == QProcess::Running) {
            m_adbCheckProcess->kill();
            m_adbCheckProcess->waitForFinished(100);
        }
    }

    if (m_fastbootCheckProcess) {
        if (m_fastbootCheckProcess->state() == QProcess::Running) {
            m_fastbootCheckProcess->kill();
            m_fastbootCheckProcess->waitForFinished(100);
        }
    }

    if (m_infoProcess) {
        if (m_infoProcess->state() == QProcess::Running) {
            m_infoProcess->kill();
            m_infoProcess->waitForFinished(100);
        }
    }

    qDebug() << "DeviceManager 已销毁";
}

void DeviceManager::ensureMonitoring()
{
    // 增加全模式引用计数
    m_fullModeRefCount++;
    qDebug() << "DeviceManager 全模式引用计数:" << m_fullModeRefCount;

    // 只要有全模式请求，就必须切换到全模式
    if (m_adbOnly) {
        m_adbOnly = false;
        qDebug() << "DeviceManager 切换到全模式监控（ADB + Fastboot）";
    }

    if (!m_checkTimer->isActive()) {
        qDebug() << "DeviceManager 开始监控设备状态（ADB + Fastboot）";
        m_checkTimer->start(DEVICE_CHECK_INTERVAL);  // 每1秒检测一次
        checkDeviceStatus();  // 立即检测一次
    }
}

void DeviceManager::releaseFullModeMonitoring()
{
    // 减少全模式引用计数
    if (m_fullModeRefCount > 0) {
        m_fullModeRefCount--;
        qDebug() << "DeviceManager 释放全模式，引用计数:" << m_fullModeRefCount;
    }

    // 检查是否所有引用都已释放
    if (m_fullModeRefCount == 0 && m_adbOnlyRefCount == 0) {
        // 所有窗口都关闭了，停止监控
        stopMonitoring();
        qDebug() << "DeviceManager 所有引用已释放，停止监控";
    } else if (m_fullModeRefCount == 0 && !m_adbOnly) {
        // 没有全模式请求了，但还有 ADB-only 请求，切换到 ADB-only
        m_adbOnly = true;
        qDebug() << "DeviceManager 无全模式请求，切换到仅 ADB 监控";
    }
}

void DeviceManager::ensureAdbOnlyMonitoring()
{
    // 增加 ADB-only 引用计数
    m_adbOnlyRefCount++;
    qDebug() << "DeviceManager ADB-only 引用计数:" << m_adbOnlyRefCount;

    if (!m_checkTimer->isActive()) {
        // 定时器未运行，根据是否有全模式请求来决定模式
        if (m_fullModeRefCount == 0) {
            m_adbOnly = true;
            qDebug() << "DeviceManager 开始监控设备状态（仅 ADB）";
        } else {
            m_adbOnly = false;
            qDebug() << "DeviceManager 开始监控设备状态（全模式，因有" << m_fullModeRefCount << "个全模式请求）";
        }
        m_checkTimer->start(DEVICE_CHECK_INTERVAL);  // 每1秒检测一次
        // 直接触发一次检测
        if (!m_isChecking) {
            m_isChecking = true;
            QString adbPath = ResourceExtractor::getAdbPath();
            m_adbCheckProcess->setWorkingDirectory(ResourceExtractor::getResourcePath());
            disconnect(m_adbCheckProcess, nullptr, this, nullptr);
            connect(m_adbCheckProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                    this, &DeviceManager::onAdbCheckFinished);
            connect(m_adbCheckProcess, &QProcess::errorOccurred,
                    this, &DeviceManager::onAdbCheckError);
            m_adbCheckProcess->start(adbPath, QStringList() << "devices");
        }
    }
    // 如果定时器已运行，不改变当前模式（尊重全模式优先级）
}

void DeviceManager::releaseAdbOnlyMonitoring()
{
    // 减少 ADB-only 引用计数
    if (m_adbOnlyRefCount > 0) {
        m_adbOnlyRefCount--;
        qDebug() << "DeviceManager 释放 ADB-only，引用计数:" << m_adbOnlyRefCount;
    }

    // 检查是否所有引用都已释放
    if (m_fullModeRefCount == 0 && m_adbOnlyRefCount == 0) {
        // 所有窗口都关闭了，停止监控
        stopMonitoring();
        qDebug() << "DeviceManager 所有引用已释放，停止监控";
    }
}

void DeviceManager::stopMonitoring()
{
    m_checkTimer->stop();
    cancelInfoQuery();
    // Disconnect before killing, including processes still in Starting state.
    for (QProcess *process : {m_adbCheckProcess, m_fastbootCheckProcess}) {
        disconnect(process, nullptr, this, nullptr);
        if (process->state() != QProcess::NotRunning) {
            process->kill();
            process->waitForFinished(1000);
        }
    }
    m_isChecking = false;
}

void DeviceManager::pauseMonitoring()
{
    if (m_isPaused) return;
    m_isPaused = true;
    cancelInfoQuery();
    for (QProcess *process : {m_adbCheckProcess, m_fastbootCheckProcess}) {
        disconnect(process, nullptr, this, nullptr);
        if (process->state() != QProcess::NotRunning) {
            process->kill();
            process->waitForFinished(1000);
        }
    }
    m_isChecking = false;
}

void DeviceManager::resumeMonitoring(QObject *waitingOwner)
{
    if (DeviceOperationLease::owner() && DeviceOperationLease::owner() != waitingOwner) return;
    if (m_isPaused) {
        m_isPaused = false;
        qDebug() << "DeviceManager 恢复监控";
    }
    if (waitingOwner && (m_fullModeRefCount > 0 || m_adbOnlyRefCount > 0) && !m_checkTimer->isActive()) {
        m_checkTimer->start(DEVICE_CHECK_INTERVAL);
    }
}

bool DeviceManager::isDeviceConnected() const
{
    return m_currentMode != None;
}

QString DeviceManager::getDeviceInfo() const
{
    return m_deviceInfo;
}

void DeviceManager::checkDeviceStatus()
{
    // 如果正在检测或已暂停，跳过本次
    if (m_isChecking || m_isPaused) {
        return;
    }

    m_isChecking = true;

    QString adbPath = ResourceExtractor::getAdbPath();
    m_adbCheckProcess->setWorkingDirectory(ResourceExtractor::getResourcePath());

    // 断开之前的连接
    disconnect(m_adbCheckProcess, nullptr, this, nullptr);

    // 连接完成信号
    connect(m_adbCheckProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &DeviceManager::onAdbCheckFinished);
    connect(m_adbCheckProcess, &QProcess::errorOccurred,
            this, &DeviceManager::onAdbCheckError);

    m_adbCheckProcess->start(adbPath, QStringList() << "devices");
}

void DeviceManager::onAdbCheckFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    // 如果已暂停监控，直接返回，避免与 fastboot 操作冲突
    if (m_isPaused) {
        m_isChecking = false;
        return;
    }

    QStringList adbSerials;

    if (exitStatus == QProcess::NormalExit && exitCode == 0) {
        const QString adbOutput = QString::fromLocal8Bit(m_adbCheckProcess->readAllStandardOutput());
        const QStringList lines = adbOutput.split(QRegularExpression("[\\r\\n]+"), Qt::SkipEmptyParts);
        for (const QString &rawLine : lines) {
            const QStringList fields = rawLine.trimmed().split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
            // ADB 输出的第二列必须是精确的 device，避免把序列号或其他文本中的
            // "device" 当成在线设备。
            if (fields.size() >= 2 && fields.at(1) == "device") {
                adbSerials.append(fields.at(0));
            }
        }
    }

    const bool adbConnected = !adbSerials.isEmpty();
    const QString adbSerial = adbSerials.contains(m_deviceSerial) ? m_deviceSerial : adbSerials.value(0);

    // 如果没有 ADB 连接，且当前不是 ADB-only 模式，才检查 Fastboot
    if (!adbConnected && !m_adbOnly) {
        QString fastbootPath = ResourceExtractor::getFastbootPath();
        m_fastbootCheckProcess->setWorkingDirectory(ResourceExtractor::getResourcePath());

        // 断开之前的连接
        disconnect(m_fastbootCheckProcess, nullptr, this, nullptr);

        // 连接完成信号
        connect(m_fastbootCheckProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, &DeviceManager::onFastbootCheckFinished);
        connect(m_fastbootCheckProcess, &QProcess::errorOccurred,
                this, &DeviceManager::onFastbootCheckError);

        m_fastbootCheckProcess->start(fastbootPath, QStringList() << "devices");
    } else {
        // 有 ADB 连接或处于 ADB-only 模式
        setDetectedDevice(adbConnected ? ADB : None, adbSerial);
        if (m_currentMode == ADB) updateDeviceInfo();
        m_isChecking = false;
    }
}

void DeviceManager::onAdbCheckError(QProcess::ProcessError error)
{
    if (error != QProcess::FailedToStart) {
        return;
    }

    if (m_isPaused) {
        m_isChecking = false;
        return;
    }

    // ADB 启动失败时不能依赖 finished 信号恢复状态；全模式仍继续探测 Fastboot。
    if (!m_adbOnly) {
        const QString fastbootPath = ResourceExtractor::getFastbootPath();
        m_fastbootCheckProcess->setWorkingDirectory(ResourceExtractor::getResourcePath());
        disconnect(m_fastbootCheckProcess, nullptr, this, nullptr);
        connect(m_fastbootCheckProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, &DeviceManager::onFastbootCheckFinished);
        connect(m_fastbootCheckProcess, &QProcess::errorOccurred,
                this, &DeviceManager::onFastbootCheckError);
        m_fastbootCheckProcess->start(fastbootPath, QStringList() << "devices");
        return;
    }

    setDetectedDevice(None, QString());
    m_isChecking = false;
}
void DeviceManager::onFastbootCheckFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    // 如果已暂停监控，直接返回，避免与 fastboot 操作冲突
    if (m_isPaused) {
        m_isChecking = false;
        return;
    }

    QStringList fastbootSerials;

    if (exitStatus == QProcess::NormalExit && exitCode == 0) {
        const QString fastbootOutput = QString::fromLocal8Bit(m_fastbootCheckProcess->readAllStandardOutput());
        const QStringList lines = fastbootOutput.split(QRegularExpression("[\\r\\n]+"), Qt::SkipEmptyParts);
        for (const QString &rawLine : lines) {
            const QStringList fields = rawLine.trimmed().split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
            // fastboot devices 的第二列必须精确为 fastboot。
            if (fields.size() >= 2 && fields.at(1) == "fastboot") {
                fastbootSerials.append(fields.at(0));
            }
        }
    }

    const QString serial = fastbootSerials.contains(m_deviceSerial) ? m_deviceSerial : fastbootSerials.value(0);
    setDetectedDevice(fastbootSerials.isEmpty() ? None : Fastboot, serial);
    if (m_currentMode != None) updateDeviceInfo();

    m_isChecking = false;
}

void DeviceManager::onFastbootCheckError(QProcess::ProcessError error)
{
    if (error != QProcess::FailedToStart) {
        return;
    }

    if (m_isPaused) {
        m_isChecking = false;
        return;
    }

    setDetectedDevice(None, QString());
    m_isChecking = false;
}
void DeviceManager::setDetectedDevice(DeviceMode mode, const QString &serial)
{
    if (mode == m_currentMode && serial == m_deviceSerial) return;
    cancelInfoQuery();
    const bool modeChanged = mode != m_currentMode;
    m_currentMode = mode;
    m_deviceSerial = serial;
    m_deviceInfo.clear();
    // Invalidate cached UI data even when one ADB device replaces another.
    emit deviceInfoUpdated(QString());
    if (modeChanged) emit deviceModeChanged(mode);
}

void DeviceManager::cancelInfoQuery()
{
    ++m_infoGeneration;
    m_infoQueryActive = false;
    m_infoTimer->stop();
    disconnect(m_infoTimer, nullptr, this, nullptr);
    disconnect(m_infoProcess, nullptr, this, nullptr);
    if (m_infoProcess->state() != QProcess::NotRunning) {
        m_infoProcess->kill();
        m_infoProcess->waitForFinished(1000);
    }
}

bool DeviceManager::isCurrentInfoQuery(quint64 generation, QProcess *process, int step) const
{
    return m_infoQueryActive && !m_isPaused && generation == m_infoGeneration &&
           process == m_infoProcess && step == m_infoStep &&
           m_currentMode == m_infoMode && m_deviceSerial == m_infoSerial;
}

void DeviceManager::updateDeviceInfo()
{
    if (m_isPaused || m_currentMode == None) return;
    // Polling checks connectivity, but must not interrupt a slow property query.
    if (m_infoQueryActive && m_infoMode == m_currentMode && m_infoSerial == m_deviceSerial) return;
    cancelInfoQuery();
    m_infoQueryActive = true;
    m_infoMode = m_currentMode;
    m_infoSerial = m_deviceSerial;
    m_pendingDevice = m_pendingSlot = m_pendingUnlock = QStringLiteral("未知");
    startInfoStep(0);
}

void DeviceManager::startInfoStep(int step)
{
    m_infoStep = step;
    disconnect(m_infoProcess, nullptr, this, nullptr);
    m_infoProcess->deleteLater();
    QProcess *process = ProcessManager::createProcess(this);
    m_infoProcess = process;
    process->setWorkingDirectory(ResourceExtractor::getResourcePath());
    const quint64 generation = m_infoGeneration;
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, process, generation, step](int code, QProcess::ExitStatus status) {
        if (!isCurrentInfoQuery(generation, process, step)) return;
        const bool success = status == QProcess::NormalExit && code == 0;
        const QString output = QString::fromLocal8Bit(m_infoMode == ADB ?
            process->readAllStandardOutput() : process->readAllStandardError() + process->readAllStandardOutput());
        finishInfoStep(success, output);
    });
    connect(process, &QProcess::errorOccurred, this,
            [this, process, generation, step](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && isCurrentInfoQuery(generation, process, step))
            finishInfoStep(false, QString());
    });
    disconnect(m_infoTimer, nullptr, this, nullptr);
    connect(m_infoTimer, &QTimer::timeout, this, [this, process, generation, step]() {
        if (isCurrentInfoQuery(generation, process, step)) finishInfoStep(false, QString());
    });
    QStringList arguments;
    if (!m_infoSerial.isEmpty()) arguments << "-s" << m_infoSerial;
    const QStringList adbProperties = {"ro.product.device", "ro.boot.slot_suffix", "ro.boot.verifiedbootstate"};
    const QStringList fastbootVariables = {"product", "current-slot", "unlocked"};
    if (m_infoMode == ADB) arguments << "shell" << "getprop" << adbProperties.at(step);
    else arguments << "getvar" << fastbootVariables.at(step);
    m_infoTimer->start();
    process->start(m_infoMode == ADB ? ResourceExtractor::getAdbPath() : ResourceExtractor::getFastbootPath(), arguments);
}

void DeviceManager::finishInfoStep(bool success, const QString &output)
{
    m_infoTimer->stop();
    disconnect(m_infoProcess, nullptr, this, nullptr);
    if (m_infoProcess->state() != QProcess::NotRunning) {
        m_infoProcess->kill();
        m_infoProcess->waitForFinished(1000);
    }
    QString value;
    bool found = success;
    if (success && m_infoMode == ADB) {
        value = output.trimmed();
        if (value.contains('\n') || value.contains('\r')) found = false;
    } else if (success) {
        const QStringList keys = {"product", "current-slot", "unlocked"};
        const QRegularExpression field("^(?:\\(bootloader\\)\\s*)?" + keys.at(m_infoStep) + ":\\s*(.*)$");
        found = false;
        for (const QString &line : output.split('\n')) {
            const auto match = field.match(line.trimmed());
            if (match.hasMatch()) { value = match.captured(1).trimmed(); found = true; break; }
        }
    }
    if (found) {
        if (m_infoStep == 0 && !value.isEmpty()) m_pendingDevice = value;
        else if (m_infoStep == 1) {
            if (value == "a" || value == "_a") m_pendingSlot = "a";
            else if (value == "b" || value == "_b") m_pendingSlot = "b";
            // Only a successful empty ADB property means no reported slot.
            else if (m_infoMode == ADB && value.isEmpty()) m_pendingSlot = "无";
        } else if (m_infoStep == 2) {
            if ((m_infoMode == ADB && value == "orange") || (m_infoMode == Fastboot && value == "yes"))
                m_pendingUnlock = "已解锁";
            else if ((m_infoMode == ADB && value == "green") || (m_infoMode == Fastboot && value == "no"))
                m_pendingUnlock = "未解锁";
        }
    }
    if (m_infoStep < 2) { startInfoStep(m_infoStep + 1); return; }
    m_infoQueryActive = false;
    const QString info = QString("代号:%1\n分区:%2\n解锁:%3").arg(m_pendingDevice, m_pendingSlot, m_pendingUnlock);
    if (info != m_deviceInfo) {
        m_deviceInfo = info;
        emit deviceInfoUpdated(info);
    }
}
