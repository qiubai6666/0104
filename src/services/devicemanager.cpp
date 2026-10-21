#include "deviceoperationlease.h"
#include "processmanager.h"
#include "devicemanager.h"
#include "resourceextractor.h"
#include "version.h"
#include <QDebug>
#include <QRegularExpression>
#include <memory>

DeviceManager* DeviceManager::m_instance = nullptr;

namespace {
// 设备检测每秒轮询一次，探测超时与解析正则必须复用，避免反复构造。
constexpr int kProbeTimeoutMs = 5000;
const QRegularExpression &lineSplitter()
{
    static const QRegularExpression re(QStringLiteral("[\\r\\n]+"));
    return re;
}
const QRegularExpression &fieldSplitter()
{
    static const QRegularExpression re(QStringLiteral("\\s+"));
    return re;
}
const QStringList &adbStates()
{
    static const QStringList states = {
        QStringLiteral("device"), QStringLiteral("unauthorized"), QStringLiteral("offline"),
        QStringLiteral("authorizing"), QStringLiteral("connecting"), QStringLiteral("recovery"),
        QStringLiteral("sideload"), QStringLiteral("bootloader"), QStringLiteral("rescue")};
    return states;
}
QVector<DeviceManager::DeviceEntry> parseAdbEntries(const QString &output)
{
    QVector<DeviceManager::DeviceEntry> entries;
    for (const QString &rawLine : output.split(lineSplitter(), Qt::SkipEmptyParts)) {
        const QStringList fields = rawLine.trimmed().split(fieldSplitter(), Qt::SkipEmptyParts);
        if (fields.size() < 2 || !adbStates().contains(fields.at(1))) continue;
        DeviceManager::DeviceEntry entry;
        entry.serial = fields.constFirst();
        entry.state = fields.at(1);
        for (int index = 2; index < fields.size(); ++index) {
            if (!fields.at(index).startsWith(QStringLiteral("model:"))) continue;
            entry.model = fields.at(index).mid(6);
            entry.model.replace('_', ' ');
            break;
        }
        if (!entry.serial.isEmpty() && entries.size() < 32) entries.append(entry);
    }
    return entries;
}
QVector<DeviceManager::DeviceEntry> parseFastbootEntries(const QString &output)
{
    QVector<DeviceManager::DeviceEntry> entries;
    for (const QString &rawLine : output.split(lineSplitter(), Qt::SkipEmptyParts)) {
        const QStringList fields = rawLine.trimmed().split(fieldSplitter(), Qt::SkipEmptyParts);
        if (fields.size() >= 2 && fields.at(1) == QStringLiteral("fastboot")
            && !fields.constFirst().isEmpty() && entries.size() < 32) {
            entries.append({fields.constFirst(), QString(), QStringLiteral("fastboot")});
        }
    }
    return entries;
}
QStringList operableSerials(const QVector<DeviceManager::DeviceEntry> &entries)
{
    QStringList serials;
    for (const auto &entry : entries)
        if (entry.isOperable() && !serials.contains(entry.serial)) serials.append(entry.serial);
    return serials;
}
} // namespace

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

    // 探测看门狗：adb server 卡死（驱动异常、端口冲突）时 adb devices 永远不返回，
    // 若不复位 m_isChecking，监控将永久停摆；超时后强制结束并恢复轮询。
    m_probeWatchdog = new QTimer(this);
    m_probeWatchdog->setSingleShot(true);
    connect(m_probeWatchdog, &QTimer::timeout, this, &DeviceManager::onProbeWatchdog);
    qDebug() << "DeviceManager 单例已创建";
}

DeviceManager::~DeviceManager()
{
    // stopMonitoring 负责断开并终止全部探测/信息进程；进程对象由父子关系清理。
    stopMonitoring();
    qDebug() << "DeviceManager 已销毁";
}

void DeviceManager::ensureMonitoring()
{
    // 增加全模式引用计数
    m_fullModeRefCount++;
    if (m_fullModeRefCount == 1) m_infoRefreshTimer.invalidate();
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
        checkDeviceStatus();  // 立即检测一次（m_isChecking 与启动细节由其内部处理）
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
    m_infoRefreshTimer.invalidate();
    cancelInfoQuery();
    m_pendingAdbEntries.clear();
    m_probeWatchdog->stop();
    // 只断开并终止进程，不阻塞等待：kill 后的退出是毫秒级异步操作，
    // 阻塞等待会让 GUI 线程在停止监控时冻结最长 1 秒。
    for (QProcess *process : {m_adbCheckProcess, m_fastbootCheckProcess}) {
        disconnect(process, nullptr, this, nullptr);
        if (process->state() != QProcess::NotRunning) {
            process->kill();
        }
    }
    m_isChecking = false;
}

void DeviceManager::pauseMonitoring()
{
    if (m_isPaused) return;
    m_isPaused = true;
    m_infoRefreshTimer.invalidate();
    cancelInfoQuery();
    m_pendingAdbEntries.clear();
    m_probeWatchdog->stop();
    for (QProcess *process : {m_adbCheckProcess, m_fastbootCheckProcess}) {
        disconnect(process, nullptr, this, nullptr);
        if (process->state() != QProcess::NotRunning) {
            process->kill();
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
    // 上一次被看门狗强杀的进程可能仍在退出（ClosingState），
    // 此时再次 start() 会被静默忽略且其信号已被 startProbe 断开，
    // m_isChecking 将永久卡住；任一探测进程未退出前等待下一个定时周期。
    if (m_adbCheckProcess->state() != QProcess::NotRunning ||
        m_fastbootCheckProcess->state() != QProcess::NotRunning) {
        return;
    }

    m_isChecking = true;
    startProbe(m_adbCheckProcess, ResourceExtractor::getAdbPath(),
               &DeviceManager::onAdbCheckFinished, &DeviceManager::onAdbCheckError, true);
}

void DeviceManager::startProbe(QProcess *process, const QString &tool,
                               void (DeviceManager::*onFinished)(int, QProcess::ExitStatus),
                               void (DeviceManager::*onError)(QProcess::ProcessError),
                               bool longListing)
{
    disconnect(process, nullptr, this, nullptr);
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, onFinished);
    connect(process, &QProcess::errorOccurred, this, onError);
    process->setWorkingDirectory(ResourceExtractor::getResourcePath());
    m_probeWatchdog->start(kProbeTimeoutMs);
    QStringList arguments{QStringLiteral("devices")};
    // -l 额外给出 model，用于多设备选择器；Fastboot 保持原有 devices 输出。
    if (longListing) arguments << QStringLiteral("-l");
    process->start(tool, arguments);
}

void DeviceManager::onProbeWatchdog()
{
    bool killed = false;
    for (QProcess *process : {m_adbCheckProcess, m_fastbootCheckProcess}) {
        disconnect(process, nullptr, this, nullptr);
        if (process->state() != QProcess::NotRunning) {
            process->kill();
            killed = true;
        }
    }
    // 无条件复位 m_isChecking：探测进程可能已自然退出但 finished 信号被
    // startProbe 断开（例如对收尾中的旧进程静默 start 失败），此时没有
    // 进程可 kill，若不复位，监控将从此永久停摆。
    m_pendingAdbEntries.clear();
    m_isChecking = false;
    if (killed) {
        qWarning() << "DeviceManager 设备检测进程超时，已强制结束并恢复监控";
    }
}

void DeviceManager::onAdbCheckFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    // 如果已暂停监控，直接返回，避免与 fastboot 操作冲突
    if (m_isPaused) {
        m_isChecking = false;
        return;
    }

    QVector<DeviceEntry> adbEntries;
    if (exitStatus == QProcess::NormalExit && exitCode == 0) {
        const QString adbOutput = QString::fromLocal8Bit(m_adbCheckProcess->readAllStandardOutput());
        adbEntries = parseAdbEntries(adbOutput);
    }
    const QStringList adbSerials = operableSerials(adbEntries);
    const bool adbConnected = !adbSerials.isEmpty();
    // 目标设备可能在重启期间短暂离开 ADB，而其他手机仍在线。此时额外查一次
    // Fastboot，避免目标已进入 bootloader 却被“另一台 ADB 设备”挡住。
    const bool targetMissing = adbConnected && !m_deviceSerial.isEmpty()
        && !adbSerials.contains(m_deviceSerial);
    const bool probeFastboot = !m_adbOnly && (!adbConnected || targetMissing);

    if (probeFastboot) {
        m_pendingAdbEntries = adbEntries;
        startProbe(m_fastbootCheckProcess, ResourceExtractor::getFastbootPath(),
                   &DeviceManager::onFastbootCheckFinished, &DeviceManager::onFastbootCheckError);
        return;
    }

    // 有目标 ADB 连接或处于 ADB-only 模式：本轮探测结束，停掉看门狗。
    m_probeWatchdog->stop();
    m_pendingAdbEntries.clear();
    setDetectedEntries(adbConnected ? ADB : None, adbEntries);
    if (m_currentMode == ADB) updatePolledDeviceInfo();
    m_isChecking = false;
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
        m_pendingAdbEntries.clear();
        startProbe(m_fastbootCheckProcess, ResourceExtractor::getFastbootPath(),
                   &DeviceManager::onFastbootCheckFinished, &DeviceManager::onFastbootCheckError);
        return;
    }

    m_probeWatchdog->stop();
    m_pendingAdbEntries.clear();
    setDetectedEntries(None, {});
    m_isChecking = false;
}
void DeviceManager::onFastbootCheckFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    // 如果已暂停监控，直接返回，避免与 fastboot 操作冲突
    if (m_isPaused) {
        m_isChecking = false;
        return;
    }

    QVector<DeviceEntry> fastbootEntries;
    if (exitStatus == QProcess::NormalExit && exitCode == 0) {
        const QString fastbootOutput = QString::fromLocal8Bit(m_fastbootCheckProcess->readAllStandardOutput());
        fastbootEntries = parseFastbootEntries(fastbootOutput);
    }
    const QStringList fastbootSerials = operableSerials(fastbootEntries);
    const bool targetInFastboot = !m_deviceSerial.isEmpty()
        && fastbootSerials.contains(m_deviceSerial);
    const QVector<DeviceEntry> pendingAdb = m_pendingAdbEntries;
    const QStringList pendingSerials = operableSerials(pendingAdb);
    m_pendingAdbEntries.clear();

    // fastboot 探测是本轮检测的最后一步，结束后停掉看门狗。
    m_probeWatchdog->stop();
    if (targetInFastboot || (!fastbootSerials.isEmpty() && pendingSerials.isEmpty())) {
        setDetectedEntries(Fastboot, fastbootEntries);
    } else if (!pendingAdb.isEmpty()) {
        // 目标已离开 ADB 但尚未出现在 Fastboot：保留其余 ADB 设备等待下一轮，
        // 绝不把刷新结果当作可刷写的目标。
        setDetectedEntries(pendingSerials.isEmpty() ? None : ADB, pendingAdb);
    } else {
        setDetectedEntries(None, {});
    }
    if (m_currentMode != None) updatePolledDeviceInfo();

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

    m_probeWatchdog->stop();
    const QVector<DeviceEntry> pendingAdb = m_pendingAdbEntries;
    m_pendingAdbEntries.clear();
    const QStringList pendingSerials = operableSerials(pendingAdb);
    setDetectedEntries(pendingAdb.isEmpty() ? None : (pendingSerials.isEmpty() ? None : ADB),
                       pendingAdb);
    m_isChecking = false;
}

void DeviceManager::setDetectedDevice(DeviceMode mode, const QString &serial)
{
    setDetectedDevices(mode, serial.isEmpty() ? QStringList() : QStringList{serial});
}

void DeviceManager::setDetectedDevices(DeviceMode mode, const QStringList &serials)
{
    QVector<DeviceEntry> entries;
    const QString state = mode == DeviceManager::Fastboot ? QStringLiteral("fastboot")
                                                          : QStringLiteral("device");
    for (const QString &serial : serials) {
        const QString trimmed = serial.trimmed();
        bool duplicate = false;
        for (const auto &entry : entries) duplicate |= entry.serial == trimmed;
        if (!trimmed.isEmpty() && !duplicate)
            entries.append({trimmed, QString(), state});
    }
    if (mode == None) entries.clear();
    setDetectedEntries(mode, entries);
}

void DeviceManager::setDetectedEntries(DeviceMode mode, const QVector<DeviceEntry> &entries)
{
    QVector<DeviceEntry> normalized;
    for (const DeviceEntry &raw : entries) {
        const QString serial = raw.serial.trimmed();
        if (serial.isEmpty()) continue;
        bool duplicate = false;
        for (const auto &entry : normalized) duplicate |= entry.serial == serial;
        if (duplicate) continue;
        DeviceEntry entry = raw;
        entry.serial = serial;
        normalized.append(entry);
    }
    const QStringList serials = operableSerials(normalized);
    // 设备操作期间目标可能在重启中短暂消失；保持该序列号，避免把界面临时
    // 切到另一台手机，也避免后续命令落到非用户选择的设备上。
    const bool holdSelection = DeviceOperationLease::owner() != nullptr && !m_deviceSerial.isEmpty();
    const QString preferredSerial = serials.contains(m_deviceSerial) || holdSelection
        ? m_deviceSerial : serials.value(0);
    const bool modeChanged = mode != m_currentMode;
    const bool targetChanged = preferredSerial != m_deviceSerial;
    const bool listChanged = normalized != m_deviceEntries;
    if (!modeChanged && !targetChanged && !listChanged) return;

    const bool dataChanged = modeChanged || targetChanged;
    m_currentMode = mode;
    m_deviceSerials = serials;
    m_deviceSerial = preferredSerial;
    m_deviceEntries = normalized;
    if (preferredSerial.isEmpty()) {
        m_deviceDisplayName.clear();
    } else if (targetChanged) {
        m_deviceDisplayName.clear();
        for (const DeviceEntry &entry : normalized) {
            if (entry.serial == preferredSerial && !entry.model.isEmpty()) {
                m_deviceDisplayName = entry.model;
                break;
            }
        }
    }
    if (dataChanged) {
        m_infoRefreshTimer.invalidate();
        cancelInfoQuery();
        m_deviceInfo.clear();
        m_deviceDetails.clear();
        m_extendedDeviceDetails.clear();
        // Invalidate cached UI data even when one device replaces another.
        emit deviceInfoUpdated(QString());
        emit deviceDetailsUpdated(QString());
        emit extendedDeviceDetailsUpdated(QString());
    }
    if (listChanged) emit deviceListChanged();
    if (modeChanged) emit deviceModeChanged(mode);
    if (targetChanged) emit deviceSelectionChanged(preferredSerial);
}

QString DeviceManager::uniqueDeviceSerial(QString *error) const
{
    if (error) error->clear();
    if (m_currentMode == None || m_deviceSerials.isEmpty()) {
        if (error) *error = QStringLiteral("未检测到可操作设备，请检查设备连接与授权。");
        return QString();
    }
    if (m_deviceSerials.size() != 1) {
        if (error) {
            *error = QStringLiteral("检测到 %1 台设备，请断开其他设备后重试。")
                .arg(m_deviceSerials.size());
        }
        return QString();
    }
    return m_deviceSerials.constFirst();
}

QString DeviceManager::operableDeviceSerial(QString *error) const
{
    if (error) error->clear();
    if (m_currentMode == None || m_deviceSerial.isEmpty()) {
        if (m_deviceEntries.isEmpty()) {
            if (error) *error = QStringLiteral("未检测到可操作设备，请检查设备连接与授权。");
        } else if (error) {
            *error = QStringLiteral("设备尚未授权或不在线，请在手机上允许 USB 调试后重试。");
        }
        return QString();
    }
    if (!m_deviceSerials.contains(m_deviceSerial)) {
        if (error) *error = QStringLiteral("目标设备已断开，请刷新设备列表后重试。");
        return QString();
    }
    return m_deviceSerial;
}

bool DeviceManager::selectDevice(const QString &serial, QString *error)
{
    if (error) error->clear();
    const QString target = serial.trimmed();
    const DeviceEntry *selected = nullptr;
    for (const auto &entry : m_deviceEntries)
        if (entry.serial == target) selected = &entry;
    if (!selected) {
        if (error) *error = QStringLiteral("该设备已断开，请刷新设备列表后重试。");
        return false;
    }
    if (!selected->isOperable() || m_currentMode == None
        || !m_deviceSerials.contains(target)) {
        if (error) {
            *error = selected->state == QStringLiteral("unauthorized")
                ? QStringLiteral("该设备尚未授权，请在手机上允许 USB 调试后重试。")
                : QStringLiteral("该设备当前不可操作，请重新连接后重试。");
        }
        return false;
    }
    if (target == m_deviceSerial) return true;
    // 操作进行中切换目标会让已启动的命令与界面目标不一致，直接拒绝。
    if (DeviceOperationLease::owner()) {
        if (error) *error = QStringLiteral("设备操作进行中，暂时无法切换目标设备。");
        return false;
    }
    m_infoRefreshTimer.invalidate();
    cancelInfoQuery();
    m_deviceSerial = target;
    // 列表中的型号可立即显示，不必等待完整信息查询。
    m_deviceDisplayName = selected->model;
    m_deviceInfo.clear();
    m_deviceDetails.clear();
    m_extendedDeviceDetails.clear();
    emit deviceInfoUpdated(QString());
    emit deviceDetailsUpdated(QString());
    emit extendedDeviceDetailsUpdated(QString());
    emit deviceSelectionChanged(target);
    if (!m_isPaused) updateDeviceInfo();
    return true;
}

void DeviceManager::refreshDevices()
{
    if (m_isPaused || !m_checkTimer->isActive()) return;
    m_infoRefreshTimer.invalidate();
    if (!m_isChecking) checkDeviceStatus();
}

void DeviceManager::verifyOperationTarget(DeviceMode mode, const QString &serial,
                                         QObject *context,
                                         std::function<void(const QString &)> completed)
{
    auto *process = ProcessManager::createProcess(context);
    auto *timer = new QTimer(process);
    timer->setSingleShot(true);
    auto done = std::make_shared<bool>(false);
    auto finish = [process, timer, done, completed](const QString &error) {
        if (*done) return;
        *done = true;
        timer->stop();
        if (process->state() != QProcess::NotRunning) process->kill();
        process->deleteLater();
        completed(error);
    };
    connect(timer, &QTimer::timeout, context, [finish] {
        finish(QStringLiteral("复核设备连接超时，未执行后续命令。"));
    });
    connect(process, &QProcess::errorOccurred, context,
            [finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish(QStringLiteral("无法启动设备连接复核，未执行后续命令。"));
    });
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), context,
            [process, mode, serial, finish](int code, QProcess::ExitStatus status) {
        if (code != 0 || status != QProcess::NormalExit) {
            finish(QStringLiteral("设备连接复核失败，未执行后续命令。"));
            return;
        }
        QStringList serials;
        const QString output = QString::fromLocal8Bit(process->readAllStandardOutput());
        for (const QString &line : output.split(lineSplitter(), Qt::SkipEmptyParts)) {
            const QStringList fields = line.trimmed().split(fieldSplitter(), Qt::SkipEmptyParts);
            if (fields.size() >= 2 && fields[1] == (mode == ADB ? "device" : "fastboot"))
                serials.append(fields[0]);
        }
        serials.removeDuplicates();
        // 多设备本身不是错误：只确认本次操作锁定的序列号仍在目标模式中，
        // 其他设备是否在线不影响 -s 指定的命令。
        if (!serials.contains(serial)) {
            finish(QStringLiteral("原设备已断开、切换模式或序列号发生变化，未执行后续命令。"));
        } else {
            finish(QString());
        }
    });
    process->setWorkingDirectory(ResourceExtractor::getResourcePath());
    timer->start(5000);
    process->start(mode == ADB ? ResourceExtractor::getAdbPath() : ResourceExtractor::getFastbootPath(),
                   {QStringLiteral("devices")});
}

void DeviceManager::cancelInfoQuery()
{
    ++m_infoGeneration;
    m_infoQueryActive = false;
    m_pendingExtendedProps.clear();
    m_infoTimer->stop();
    disconnect(m_infoTimer, nullptr, this, nullptr);
    disconnect(m_infoProcess, nullptr, this, nullptr);
    // 只终止不阻塞等待：迟到的输出由 generation + 进程指针双重校验拦截，
    // 旧进程对象在 startInfoStep 换新时通过 deleteLater 释放。
    if (m_infoProcess->state() != QProcess::NotRunning) {
        m_infoProcess->kill();
    }
}

bool DeviceManager::isCurrentInfoQuery(quint64 generation, QProcess *process, int step) const
{
    return m_infoQueryActive && !m_isPaused && generation == m_infoGeneration &&
           process == m_infoProcess && step == m_infoStep &&
           m_currentMode == m_infoMode && m_deviceSerial == m_infoSerial;
}

void DeviceManager::updatePolledDeviceInfo()
{
    // Connectivity still polls every second. Reuse a completed information
    // snapshot so an idle connected device does not spawn seven extra commands
    // on each tick. Explicit refreshes bypass this cache.
    if (m_infoRefreshTimer.isValid() &&
        m_infoRefreshTimer.elapsed() < DEVICE_INFO_REFRESH_INTERVAL) return;
    updateDeviceInfo();
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
    m_pendingKernelVersion = m_pendingSelinuxStatus = m_pendingBuildDate = QStringLiteral("未知");
    startInfoStep(0);
}

void DeviceManager::startInfoStep(int step)
{
    if (!m_infoQueryActive || m_isPaused || m_currentMode != m_infoMode || m_deviceSerial != m_infoSerial) return;
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
        if (step < 3) finishInfoStep(success, output);
        else finishExtendedInfoQuery(success, output);
    });
    connect(process, &QProcess::errorOccurred, this,
            [this, process, generation, step](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && isCurrentInfoQuery(generation, process, step)) {
            if (step < 3) finishInfoStep(false, QString());
            else finishExtendedInfoQuery(false, QString());
        }
    });
    disconnect(m_infoTimer, nullptr, this, nullptr);
    connect(m_infoTimer, &QTimer::timeout, this, [this, process, generation, step]() {
        if (!isCurrentInfoQuery(generation, process, step)) return;
        if (step < 3) finishInfoStep(false, QString());
        else finishExtendedInfoQuery(false, QString());
    });
    QStringList arguments;
    if (!m_infoSerial.isEmpty()) arguments << "-s" << m_infoSerial;
    const QStringList adbProperties = {"ro.product.device", "ro.boot.slot_suffix", "ro.boot.verifiedbootstate"};
    const QStringList fastbootVariables = {"product", "current-slot", "unlocked"};
    if (m_infoMode == ADB) {
        arguments << "shell";
        if (step < 3) arguments << "getprop" << adbProperties.at(step);
        else if (step == 3) arguments << "getprop";
        else if (step == 4) arguments << "uname" << "-r";
        else if (step == 5) arguments << "getenforce";
        else arguments << "cat" << "/proc/version";
    } else {
        arguments << "getvar" << fastbootVariables.at(step);
    }
    m_infoTimer->start();
    process->start(m_infoMode == ADB ? ResourceExtractor::getAdbPath() : ResourceExtractor::getFastbootPath(), arguments);
}

void DeviceManager::finishInfoStep(bool success, const QString &output)
{
    m_infoTimer->stop();
    disconnect(m_infoTimer, nullptr, this, nullptr);
    disconnect(m_infoProcess, nullptr, this, nullptr);
    // 只终止不等待（超时路径需要杀进程；正常完成路径进程已退出，kill 为空操作）。
    if (m_infoProcess->state() != QProcess::NotRunning) {
        m_infoProcess->kill();
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
    if (m_infoStep == 2) {
        // Keep the compact legacy snapshot for existing consumers while the
        // detection panel uses the dedicated three-field details signal.
        const QString info = QString("代号:%1\n分区:%2\n解锁:%3")
                                  .arg(m_pendingDevice, m_pendingSlot, m_pendingUnlock);
        if (info != m_deviceInfo) {
            m_deviceInfo = info;
            const quint64 generation = m_infoGeneration;
            emit deviceInfoUpdated(info);
            if (!m_infoQueryActive || generation != m_infoGeneration) return;
        }
    }
    if (m_infoStep < 2) { startInfoStep(m_infoStep + 1); return; }

    const QString details = QString("设备代号：%1\n活动分区：%2\n解锁状态：%3")
                                .arg(m_pendingDevice, m_pendingSlot, m_pendingUnlock);
    if (details != m_deviceDetails) {
        m_deviceDetails = details;
        const quint64 generation = m_infoGeneration;
        emit deviceDetailsUpdated(details);
        if (!m_infoQueryActive || generation != m_infoGeneration) return;
    }
    if (m_infoMode == ADB) {
        startInfoStep(3);
        return;
    }
    m_infoRefreshTimer.start();
    m_infoQueryActive = false;
}

void DeviceManager::finishExtendedInfoQuery(bool success, const QString &output)
{
    m_infoTimer->stop();
    disconnect(m_infoTimer, nullptr, this, nullptr);
    // Retire the old command before advancing, including on timeout. Each next
    // step owns a fresh process, so late output cannot become another field.
    disconnect(m_infoProcess, nullptr, this, nullptr);
    if (m_infoProcess->state() != QProcess::NotRunning) {
        m_infoProcess->kill();
    }
    const int step = m_infoStep;
    if (step == 3 && success) {
        m_pendingExtendedProps = output;
        const QString props = output;
        auto property = [&props](const QStringList &keys, const QString &fallback = QStringLiteral("未知")) {
            for (const QString &key : keys) {
                const QRegularExpression re(QStringLiteral("\\[%1\\]\\s*:\\s*\\[(.*?)\\]").arg(QRegularExpression::escape(key)));
                const auto match = re.match(props);
                if (match.hasMatch() && !match.captured(1).trimmed().isEmpty()) return match.captured(1).trimmed();
            }
            return fallback;
        };
        m_pendingKernelVersion = QStringLiteral("未知");
        m_pendingSelinuxStatus = property({"ro.boot.selinux", "ro.build.version.selinux"});
        m_pendingBuildDate = property({"ro.build.date", "ro.product.build.date", "ro.system.build.date"});
    } else if (step == 4) {
        const QString value = success ? output.trimmed() : QString();
        if (!value.isEmpty()) m_pendingKernelVersion = value;
    } else if (step == 5) {
        const QString value = success ? output.trimmed() : QString();
        if (!value.isEmpty()) m_pendingSelinuxStatus = value;
    } else if (step == 6 && success && !output.trimmed().isEmpty()) {
        const QString value = success ? output.trimmed() : QString();
        const QRegularExpression full(QStringLiteral("#\\d+[^\\n]*\\d{2}:\\d{2}:\\d{2}[^\\n]*\\d{4}"));
        const auto match = full.match(value);
        m_pendingBuildDate = match.hasMatch() ? match.captured(0).trimmed() : value;
    }
    if (step < 6) {
        startInfoStep(step + 1);
        return;
    }
    const QString props = m_pendingExtendedProps;
    auto property = [&props](const QStringList &keys, const QString &fallback = QStringLiteral("未知")) {
        for (const QString &key : keys) {
            const QRegularExpression re(QStringLiteral("\\[%1\\]\\s*:\\s*\\[(.*?)\\]").arg(QRegularExpression::escape(key)));
            const auto match = re.match(props);
            if (match.hasMatch() && !match.captured(1).trimmed().isEmpty()) return match.captured(1).trimmed();
        }
        return fallback;
    };
    const QString unlock = m_pendingUnlock == QStringLiteral("未知") ? property({"ro.boot.verifiedbootstate"}) : m_pendingUnlock;
    const QString manufacturer = property({"ro.product.manufacturer", "ro.product.brand", "ro.product.vendor.brand"}, QString());
    const QString model = property({"ro.product.marketname", "ro.config.marketing_name", "ro.product.model"});
    const QString miuiVersion = property({"ro.miui.ui.version.name", "ro.miui.ui.version.code"}, QString());
    const bool isXiaomi = manufacturer.contains(QStringLiteral("xiaomi"), Qt::CaseInsensitive)
        || manufacturer.contains(QStringLiteral("redmi"), Qt::CaseInsensitive)
        || model.contains(QStringLiteral("xiaomi"), Qt::CaseInsensitive)
        || model.contains(QStringLiteral("redmi"), Qt::CaseInsensitive)
        || miuiVersion.startsWith(QStringLiteral("V"), Qt::CaseInsensitive);
    // Xiaomi/MIUI exposes the user-facing ROM version in ro.build.version.incremental
    // (for example V14.0.29.0.TMCCNXM); ro.build.display.id is only the Android
    // branch/build tag (for example TKQ1.220905.001 test-keys).
    const QString versionInfo = isXiaomi
        ? property({"ro.build.version.incremental", "ro.build.version.ota", "ro.miui.build.version", "ro.build.display.id.show", "ro.build.display.id", "ro.build.version.miui"})
        : property({"ro.build.display.id.show", "ro.build.display.id", "ro.build.version.oplusrom", "ro.build.version.oplusrom.display", "ro.build.version.miui", "ro.build.version.incremental", "ro.build.version.ota"});
    const QString details = QStringLiteral(
        "设备序列号：%1\n设备名称：%2\n设备代号：%3\n安卓版本：%4\n解锁状态：%5\n版本信息：%6\n构建日期：%7\n内核版本：%8\nA/B 分区：%9\nCPU 代号：%10\n操作系统：Android %4\nSELinux：%11")
        .arg(property({"ro.serialno", "ro.boot.serialno"}, m_deviceSerial))
        .arg(model)
        .arg(property({"ro.product.device", "ro.product.odm.device"}, m_pendingDevice))
        .arg(property({"ro.build.version.release", "ro.product.build.version.release"}))
        .arg(unlock == QStringLiteral("orange") ? QStringLiteral("已解锁") : unlock == QStringLiteral("green") ? QStringLiteral("未解锁") : unlock)
        .arg(versionInfo)
        .arg(m_pendingBuildDate)
        .arg(m_pendingKernelVersion)
        .arg(m_pendingSlot)
        .arg(property({"ro.product.board", "ro.board.platform", "ro.hardware", "ro.product.cpu.abi"}))
        .arg(m_pendingSelinuxStatus == QStringLiteral("Enforcing") ? QStringLiteral("严格模式") :
             m_pendingSelinuxStatus == QStringLiteral("Permissive") ? QStringLiteral("宽容模式") :
             m_pendingSelinuxStatus == QStringLiteral("Disabled") ? QStringLiteral("已禁用") : m_pendingSelinuxStatus);
    // Finish the batch before notifying observers; a slot may start a new one.
    m_infoRefreshTimer.start();
    m_infoQueryActive = false;
    if (model != QStringLiteral("未知")) m_deviceDisplayName = model;
    if (details != m_extendedDeviceDetails) {
        m_extendedDeviceDetails = details;
        emit extendedDeviceDetailsUpdated(details);
    }
}
