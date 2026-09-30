#ifndef DEVICECHECKWINDOW_H
#define DEVICECHECKWINDOW_H

#include <QWidget>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QProcess>
#include <QTimer>
#include <QMessageBox>
#include <QCoreApplication>
#include <QVector>
#include <QComboBox>
#include <QMouseEvent>
#include "devicemanager.h"

class DeviceCheckWindow : public QWidget
{
    Q_OBJECT

public:
    explicit DeviceCheckWindow(QWidget *parent = nullptr);
    ~DeviceCheckWindow();
    
    void setPosition(int mainMenuX, int mainMenuY);

private slots:
    void onRebootButtonClicked();
    void onOpenCmdClicked();
    void onFlashBootClicked();
    void onFlashInitBootClicked();
    void onDeviceModeChanged(DeviceManager::DeviceMode mode);
    void onDeviceInfoUpdated(const QString &info);
    void restoreOpacity();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void moveEvent(QMoveEvent *event) override;

private:
    void setupUI();
    void updateUIForMode(DeviceManager::DeviceMode mode);
    void flashPartition(const QString &partition);
    void performFlash(const QString &partition, const QString &imagePath);
    void waitForFastbootMode();
    
    QVBoxLayout *mainLayout;
    QLabel *statusLabel;
    QLabel *infoLabel;
    QComboBox *rebootComboBox;
    QPushButton *executeButton;
    
    QProcess *currentProcess;
    QTimer *waitTimer;
    QString pendingFlashPartition;
    QString pendingFlashImage;
    int waitCounter;
    
    // 拖动相关
    bool isDragging;
    QPoint dragStartPosition;
    QTimer *opacityTimer;
};

#endif // DEVICECHECKWINDOW_H
