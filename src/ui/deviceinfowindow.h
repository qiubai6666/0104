#ifndef DEVICEINFOWINDOW_H
#define DEVICEINFOWINDOW_H

#include <QWidget>
#include <QVBoxLayout>
#include <QProcess>
#include <QPushButton>
#include <QMouseEvent>
#include <QLabel>
#include <QTimer>
#include "devicemanager.h"

class DeviceInfoWindow : public QWidget
{
    Q_OBJECT

public:
    explicit DeviceInfoWindow(QWidget *parent = nullptr);
    ~DeviceInfoWindow();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private slots:
    void onScrcpyFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onDeviceModeChanged(DeviceManager::DeviceMode mode);
    void onDeviceInfoUpdated(const QString &info);
    void onTransferFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void restoreOpacity();

private:
    friend class DeviceInformationTests;
    void cancelDeviceQuery();
    void startDeviceQueryStep(int step);
    void finishDeviceQueryStep(bool success, const QString &output);
    bool isCurrentDeviceQuery(quint64 generation, QProcess *process, int step) const;
    QTimer *queryTimer = nullptr;
    quint64 queryGeneration = 0;
    bool queryActive = false;
    QString querySerial;
    QString scrcpySerial;
    int queryStep = 0;
    QLabel *modelLabel;
    QLabel *codenameLabel;
    QLabel *versionLabel;
    QLabel *slotLabel;
    QLabel *unlockLabel;
    QWidget *cardContainer;
    QVBoxLayout *mainLayout;
    QProcess *scrcpyProcess;
    
    // 拖拽相关（顺序与初始化列表一致）
    bool isDragging;
    QPoint dragPosition;
    QTimer *opacityTimer;
    
    // 异步查询进程
    QProcess *queryProcess;
    QProcess *transferProcess;
    
    // 用于异步查询的临时变量
    QString pendingCodename;
    QString pendingSlot;
    QString pendingUnlock;
    QString pendingModel;
    QString pendingVersion;
    QStringList pendingTransferFiles;
    int transferIndex;
    int transferSuccessCount;
    int transferFailCount;
    QStringList transferFailedFiles;
    
    void setupUI();
    void applyPresentation(bool connected);
    void startScrcpy();
    void showNoDeviceMessage();
    void showDeviceInfo();
    void updateDeviceLabels(const QString &model, const QString &codename, 
                          const QString &version, const QString &slot, const QString &unlock);
    void transferFiles(const QStringList &filePaths);
    void transferNextFile();
};

#endif // DEVICEINFOWINDOW_H
