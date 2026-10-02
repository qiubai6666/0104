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

    qDebug() << "DeviceManager 单例已创建";
}

DeviceManager::~DeviceManager()
{
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
    if (m_checkTimer->isActive()) {
        m_checkTimer->stop();
        qDebug() << "DeviceManager 停止监控设备状态";
    }

    // 终止正在运行的进程
    if (m_adbCheckProcess->state() == QProcess::Running) {
        m_adbCheckProcess->kill();
    }
    if (m_fastbootCheckProcess->state() == QProcess::Running) {
        m_fastbootCheckProcess->kill();
    }
    if (m_infoProcess->state() == QProcess::Running) {
        m_infoProcess->kill();
    }

    m_isChecking = false;
}

void DeviceManager::pauseMonitoring()
{
    if (!m_isPaused) {
        m_isPaused = true;
        qDebug() << "DeviceManager 暂停监控（用于fastboot操作）";

        // Queries only; never kill a user's flash or shared ADB server.
        for (QProcess *process : {m_adbCheckProcess, m_fastbootCheckProcess, m_infoProcess}) {
            if (process->state() != QProcess::NotRunning) {
                process->kill();
                process->waitForFinished(1000);
            }
        }
        m_isChecking = false;
    }
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

    bool adbConnected = false;

    if (exitStatus == QProcess::NormalExit && exitCode == 0) {
        const QString adbOutput = QString::fromLocal8Bit(m_adbCheckProcess->readAllStandardOutput());
        const QStringList lines = adbOutput.split(QRegularExpression("[\\r\\n]+"), Qt::SkipEmptyParts);
        for (const QString &rawLine : lines) {
            const QStringList fields = rawLine.trimmed().split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
            // ADB 输出的第二列必须是精确的 device，避免把序列号或其他文本中的
            // "device" 当成在线设备。
            if (fields.size() >= 2 && fields.at(1) == "device") {
                adbConnected = true;
                break;
            }
        }
    }

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
        DeviceMode newMode = adbConnected ? ADB : None;
        if (newMode != m_currentMode) {
            m_currentMode = newMode;
            emit deviceModeChanged(m_currentMode);
            if (m_currentMode == ADB) {
                qDebug() << "设备状态变更: ADB 模式";
            } else {
                qDebug() << "设备状态变更: 未连接";
            }
        }

        // 获取设备信息（仅在 ADB 模式下）
        if (m_currentMode == ADB) {
            updateDeviceInfo();
        } else {
            m_deviceInfo.clear();
        }
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

    if (m_currentMode != None) {
        m_currentMode = None;
        emit deviceModeChanged(m_currentMode);
    }
    m_deviceInfo.clear();
    m_isChecking = false;
}
void DeviceManager::onFastbootCheckFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    // 如果已暂停监控，直接返回，避免与 fastboot 操作冲突
    if (m_isPaused) {
        m_isChecking = false;
        return;
    }

    bool fastbootConnected = false;

    if (exitStatus == QProcess::NormalExit && exitCode == 0) {
        const QString fastbootOutput = QString::fromLocal8Bit(m_fastbootCheckProcess->readAllStandardOutput());
        const QStringList lines = fastbootOutput.split(QRegularExpression("[\\r\\n]+"), Qt::SkipEmptyParts);
        for (const QString &rawLine : lines) {
            const QStringList fields = rawLine.trimmed().split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
            // fastboot devices 的第二列必须精确为 fastboot。
            if (fields.size() >= 2 && fields.at(1) == "fastboot") {
                fastbootConnected = true;
                break;
            }
        }
    }

    // 更新状态
    DeviceMode newMode = fastbootConnected ? Fastboot : None;
    if (newMode != m_currentMode) {
        m_currentMode = newMode;
        emit deviceModeChanged(m_currentMode);

        if (m_currentMode == Fastboot) {
            qDebug() << "设备状态变更: Fastboot 模式";
        } else {
            qDebug() << "设备状态变更: 未连接";
        }
    }

    // 获取设备信息
    if (m_currentMode != None) {
        updateDeviceInfo();
    } else {
        m_deviceInfo.clear();
    }

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

    if (m_currentMode != None) {
        m_currentMode = None;
        emit deviceModeChanged(m_currentMode);
    }
    m_deviceInfo.clear();
    m_isChecking = false;
}
void DeviceManager::updateDeviceInfo()
{
    if (m_isPaused) return;
    // 如果进程正在运行，先终止它
    if (m_infoProcess->state() == QProcess::Running) {
        m_infoProcess->kill();
        m_infoProcess->waitForFinished(100);
    }

    // 异步获取设备信息 - 第一步：获取设备代号
    QString adbPath = ResourceExtractor::getAdbPath();
    QString fastbootPath = ResourceExtractor::getFastbootPath();

    disconnect(m_infoProcess, nullptr, this, nullptr);

    if (m_currentMode == ADB) {
        connect(m_infoProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, &DeviceManager::onDeviceInfoStep1Finished);
        m_infoProcess->start(adbPath, QStringList() << "shell" << "getprop" << "ro.product.device");
    } else if (m_currentMode == Fastboot) {
        connect(m_infoProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, &DeviceManager::onDeviceInfoStep1Finished);
        m_infoProcess->start(fastbootPath, QStringList() << "getvar" << "product");
    }
}

void DeviceManager::onDeviceInfoStep1Finished()
{
    if (m_isPaused) return;
    disconnect(m_infoProcess, nullptr, this, nullptr);

    // 检查模式是否已经变化
    if (m_currentMode == None) {
        return;
    }

    QString adbPath = ResourceExtractor::getAdbPath();
    QString fastbootPath = ResourceExtractor::getFastbootPath();

    if (m_currentMode == ADB) {
        m_pendingDevice = QString::fromLocal8Bit(m_infoProcess->readAllStandardOutput()).trimmed();
        if (m_pendingDevice.isEmpty()) m_pendingDevice = "未知";

        // 第二步：获取活动分区
        connect(m_infoProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, &DeviceManager::onDeviceInfoStep2Finished);
        m_infoProcess->start(adbPath, QStringList() << "shell" << "getprop" << "ro.boot.slot_suffix");
    } else if (m_currentMode == Fastboot) {
        QString output = QString::fromLocal8Bit(m_infoProcess->readAllStandardError());
        m_pendingDevice = "未知";
        if (output.contains("product:")) {
            int start = output.indexOf("product:") + 8;
            int end = output.indexOf('\n', start);
            m_pendingDevice = output.mid(start, end - start).trimmed();
        }

        // 第二步：获取活动分区
        connect(m_infoProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, &DeviceManager::onDeviceInfoStep2Finished);
        m_infoProcess->start(fastbootPath, QStringList() << "getvar" << "current-slot");
    }
}

void DeviceManager::onDeviceInfoStep2Finished()
{
    if (m_isPaused) return;
    disconnect(m_infoProcess, nullptr, this, nullptr);

    // 检查模式是否已经变化
    if (m_currentMode == None) {
        return;
    }

    QString adbPath = ResourceExtractor::getAdbPath();
    QString fastbootPath = ResourceExtractor::getFastbootPath();

    if (m_currentMode == ADB) {
        m_pendingSlot = QString::fromLocal8Bit(m_infoProcess->readAllStandardOutput()).trimmed();
        if (m_pendingSlot.isEmpty()) m_pendingSlot = "无";
        else m_pendingSlot = m_pendingSlot.replace("_", "");

        // 第三步：获取解锁状态
        connect(m_infoProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, &DeviceManager::onDeviceInfoStep3Finished);
        m_infoProcess->start(adbPath, QStringList() << "shell" << "getprop" << "ro.boot.verifiedbootstate");
    } else if (m_currentMode == Fastboot) {
        QString output = QString::fromLocal8Bit(m_infoProcess->readAllStandardError());
        m_pendingSlot = "无";
        if (output.contains("current-slot:")) {
            int start = output.indexOf("current-slot:") + 13;
            int end = output.indexOf('\n', start);
            QString slotValue = output.mid(start, end - start).trimmed();
            if (!slotValue.isEmpty() && slotValue != "not found") {
                m_pendingSlot = slotValue;
            }
        }

        // 第三步：获取解锁状态
        connect(m_infoProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, &DeviceManager::onDeviceInfoStep3Finished);
        m_infoProcess->start(fastbootPath, QStringList() << "getvar" << "unlocked");
    }
}

void DeviceManager::onDeviceInfoStep3Finished()
{
    if (m_isPaused) return;
    disconnect(m_infoProcess, nullptr, this, nullptr);

    // 检查模式是否已经变化
    if (m_currentMode == None) {
        return;
    }

    if (m_currentMode == ADB) {
        QString unlocked = QString::fromLocal8Bit(m_infoProcess->readAllStandardOutput()).trimmed();
        if (unlocked == "orange") m_pendingUnlock = "已解锁";
        else if (unlocked == "green") m_pendingUnlock = "未解锁";
        else m_pendingUnlock = "未知";
    } else if (m_currentMode == Fastboot) {
        QString output = QString::fromLocal8Bit(m_infoProcess->readAllStandardError());
        m_pendingUnlock = "未知";
        if (output.contains("unlocked:")) {
            m_pendingUnlock = output.contains("yes") ? "已解锁" : "未解锁";
        }
    }

    // 构建并发送设备信息
    QString info = QString("代号:%1\n分区:%2\n解锁:%3").arg(m_pendingDevice, m_pendingSlot, m_pendingUnlock);

    if (info != m_deviceInfo) {
        m_deviceInfo = info;
        emit deviceInfoUpdated(m_deviceInfo);
    }
}
