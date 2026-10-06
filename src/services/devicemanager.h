#ifndef DEVICEMANAGER_H
#define DEVICEMANAGER_H

#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QString>

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

    // 获取当前设备模式
    DeviceMode currentMode() const { return m_currentMode; }

    // 获取设备信息
    QString getDeviceInfo() const;
    // Five-row detection snapshot; keep the legacy snapshot for existing consumers.
    QString getDeviceDetails() const { return m_deviceDetails; }
    QString getExtendedDeviceDetails() const { return m_extendedDeviceDetails; }
    QString deviceSerial() const { return m_deviceSerial; }

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

private slots:
    void checkDeviceStatus();
    void onAdbCheckFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onAdbCheckError(QProcess::ProcessError error);
    void onFastbootCheckFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onFastbootCheckError(QProcess::ProcessError error);
    void updateDeviceInfo();

private:
    friend class DeviceOperationTests;
    friend class DeviceInformationTests;
    void setDetectedDevice(DeviceMode mode, const QString &serial);
    void cancelInfoQuery();
    void startInfoStep(int step);
    void finishInfoStep(bool success, const QString &output);
    void startExtendedInfoQuery();
    void finishExtendedInfoQuery(bool success, const QString &output);
    bool isCurrentInfoQuery(quint64 generation, QProcess *process, int step) const;
    explicit DeviceManager(QObject *parent = nullptr);
    ~DeviceManager();

    // 禁止拷贝
    DeviceManager(const DeviceManager&) = delete;
    DeviceManager& operator=(const DeviceManager&) = delete;

    static DeviceManager* m_instance;

    QTimer *m_checkTimer;
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
    QTimer *m_infoTimer;
    quint64 m_infoGeneration = 0;
    bool m_infoQueryActive = false;
    DeviceMode m_infoMode = None;
    QString m_infoSerial;
    int m_infoStep = 0;

    // 异步设备信息查询中间变量
    QString m_pendingDevice;
    QString m_pendingSlot;
    QString m_pendingUnlock;
};

#endif // DEVICEMANAGER_H
