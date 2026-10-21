#ifndef DEVICEPICKER_H
#define DEVICEPICKER_H

#include "devicemanager.h"

#include <QFrame>
#include <QVector>

class QLabel;
class QPaintEvent;
class QPushButton;
class QScrollArea;
class QTimer;
class QVBoxLayout;

// 主页目标设备浮层：展示全部已发现设备（含未授权/离线），
// 只允许把可操作设备设为投屏与设备检测的共同目标。
class DevicePicker : public QFrame
{
    Q_OBJECT

public:
    explicit DevicePicker(QWidget *parent = nullptr);

    void setDevices(const QVector<DeviceManager::DeviceEntry> &devices,
                    const QString &selectedSerial, const QString &selectedDisplayName);
    void setScanning(bool scanning);
    bool isScanning() const { return m_scanning; }
    void showHint(const QString &text, bool warning = false);
    void resetHint();
    int onlineCount() const;

signals:
    void deviceSelected(const QString &serial);
    void refreshRequested();
    void closeRequested();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void rebuildList();
    void updateCount();
    void updateRefreshIcon();
    void updateHintStyle();
    bool isDark() const;

    QVector<DeviceManager::DeviceEntry> m_devices;
    QString m_selectedSerial;
    QString m_selectedDisplayName;
    QString m_hintText;
    bool m_hintWarning = false;
    bool m_scanning = false;
    int m_spinAngle = 0;
    QLabel *m_countLabel = nullptr;
    QLabel *m_hintIcon = nullptr;
    QLabel *m_hintLabel = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QPushButton *m_closeButton = nullptr;
    QScrollArea *m_scroll = nullptr;
    QWidget *m_listContent = nullptr;
    QVBoxLayout *m_listLayout = nullptr;
    QTimer *m_spinTimer = nullptr;
};

#endif // DEVICEPICKER_H
