#ifndef DEVICEMANAGER_H
#define DEVICEMANAGER_H

#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QElapsedTimer>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

// 共享设备监控：轮询 ADB/Fastboot，缓存信息，并以信号通知窗口。
// 窗口必须成对申请/释放监控；刷写分区时暂停监控以避免命令冲突。
class DeviceManager : public QObject
{
    Q_OBJECT

public:
    // 获取单例实例
    static DeviceManager* instance();

    // 设备模式枚举
    enum DeviceMode {
        None,
        ADB,
        Fastboot
    };

    // 选择器展示项：在线设备可操作，未授权/离线设备仅展示并给出提示。
    struct DeviceEntry {
        QString serial;
        QString model;
        QString state;
        bool isOperable() const
        {
            return state == QStringLiteral("device") || state == QStringLiteral("fastboot");
        }
        bool operator==(const DeviceEntry &other) const
        {
            return serial == other.serial && model == other.model && state == other.state;
        }
    };

    // 获取当前设备模式
    DeviceMode currentMode() const { return m_currentMode; }

    // 获取设备信息
    QString getDeviceInfo() const;
    // Five-row detection snapshot; keep the legacy snapshot for existing consumers.
    QString getDeviceDetails() const { return m_deviceDetails; }
    QString getExtendedDeviceDetails() const { return m_extendedDeviceDetails; }
    QString deviceSerial() const { return m_deviceSerial; }
    QStringList deviceSerials() const { return m_deviceSerials; }
    QVector<DeviceEntry> deviceEntries() const { return m_deviceEntries; }
    // 目标设备的用户可见名称；优先取扩展信息，其次取 ADB 列表型号。
    QString deviceDisplayName() const { return m_deviceDisplayName; }
    QString uniqueDeviceSerial(QString *error = nullptr) const;
    bool hasUniqueOperableDevice() const { return !uniqueDeviceSerial().isEmpty(); }
    // 多设备场景下选择目标；仅接受当前列表中可操作的序列号。
    bool selectDevice(const QString &serial, QString *error = nullptr);
    // 返回当前选中的可操作设备，不要求全机唯一。
    QString operableDeviceSerial(QString *error = nullptr) const;
    // 立即重新枚举设备列表；暂停或未监控时为空操作。
    void refreshDevices();
    // Monitoring is paused while a lease is held. Re-enumerate before each command.
    void verifyOperationTarget(DeviceMode mode, const QString &serial, QObject *context,
                               std::function<void(const QString &)> completed);
    bool monitoringEnabled() const { return m_checkTimer->isActive(); }

    // 确保监控已启动（引用计数）
    // 启动全模式监控（ADB + Fastboot）- 调用时增加引用计数
    void ensureMonitoring();
    // 释放全模式监控 - 调用时减少引用计数
    void releaseFullModeMonitoring();
    // 启动仅 ADB 模式监控（不检测 Fastboot）- 调用时增加引用计数
    void ensureAdbOnlyMonitoring();
    // 释放 ADB-only 模式监控 - 调用时减少引用计数
    void releaseAdbOnlyMonitoring();

    // 停止监控
    void stopMonitoring();

    // 暂停/恢复监控（用于刷入镜像等操作，避免fastboot冲突）
    void pauseMonitoring();
    // Only an owning legacy mode-wait may temporarily permit read-only polling.
    void resumeMonitoring(QObject *waitingOwner = nullptr);

    // 读取缓存的连接状态；不会在此发起新的设备检测。
    bool isDeviceConnected() const;

signals:
    // 设备状态改变信号
    void deviceModeChanged(DeviceMode mode);

    // 设备信息更新信号
    void deviceInfoUpdated(const QString &info);
    void deviceDetailsUpdated(const QString &details);
    void extendedDeviceDetailsUpdated(const QString &details);
    // 设备列表（含未授权/离线项）或当前目标发生变化，供选择器刷新。
    void deviceListChanged();
    void deviceSelectionChanged(const QString &serial);

private slots:
    void checkDeviceStatus();
    void onAdbCheckFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onAdbCheckError(QProcess::ProcessError error);
    void onFastbootCheckFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onFastbootCheckError(QProcess::ProcessError error);
    // 探测看门狗：adb/fastboot 卡死时强制结束进程并复位 m_isChecking。
    void onProbeWatchdog();
    void updateDeviceInfo();

private:
    friend class DeviceOperationTests;
    friend class DeviceInformationTests;
    friend class ScreenCastTests;
    // 统一的探测启动入口：断开旧连接、重连信号、启动看门狗并执行 `xx devices`。
    void startProbe(QProcess *process, const QString &tool,
                    void (DeviceManager::*onFinished)(int, QProcess::ExitStatus),
                    void (DeviceManager::*onError)(QProcess::ProcessError),
                    bool longListing = false);
    void setDetectedDevice(DeviceMode mode, const QString &serial);
    void setDetectedDevices(DeviceMode mode, const QStringList &serials);
    void setDetectedEntries(DeviceMode mode, const QVector<DeviceEntry> &entries);
    void cancelInfoQuery();
    void updatePolledDeviceInfo();
    void startInfoStep(int step);
    void finishInfoStep(bool success, const QString &output);
    void finishExtendedInfoQuery(bool success, const QString &output);
    bool isCurrentInfoQuery(quint64 generation, QProcess *process, int step) const;
    explicit DeviceManager(QObject *parent = nullptr);
    ~DeviceManager();

    // 禁止拷贝
    DeviceManager(const DeviceManager&) = delete;
    DeviceManager& operator=(const DeviceManager&) = delete;

    static DeviceManager* m_instance;

    QTimer *m_checkTimer;
    QTimer *m_probeWatchdog = nullptr;
    QProcess *m_adbCheckProcess;
    QProcess *m_fastbootCheckProcess;
    QProcess *m_infoProcess;  // 用于异步获取设备信息
    DeviceMode m_currentMode;
    bool m_isChecking;
    bool m_adbOnly;
    int m_fullModeRefCount;   // 全模式监控的引用计数
    int m_adbOnlyRefCount;    // ADB-only 模式监控的引用计数
    bool m_isPaused;          // 监控是否被暂停
    QString m_deviceInfo;
    QString m_deviceDetails;
    QString m_extendedDeviceDetails;
    QString m_deviceSerial;
    QString m_deviceDisplayName;
    QStringList m_deviceSerials;
    QVector<DeviceEntry> m_deviceEntries;
    // ADB 目标暂时缺席时先查 Fastboot，再决定回退到其余 ADB 设备。
    QVector<DeviceEntry> m_pendingAdbEntries;
    QTimer *m_infoTimer;
    QElapsedTimer m_infoRefreshTimer;
    quint64 m_infoGeneration = 0;
    bool m_infoQueryActive = false;
    DeviceMode m_infoMode = None;
    QString m_infoSerial;
    int m_infoStep = 0;

    // 异步设备信息查询中间变量
    QString m_pendingDevice;
    QString m_pendingSlot;
    QString m_pendingUnlock;
    QString m_pendingKernelVersion;
    QString m_pendingSelinuxStatus;
    QString m_pendingBuildDate;
    QString m_pendingExtendedProps;
};

#endif // DEVICEMANAGER_H
