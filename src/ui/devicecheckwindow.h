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
#include <QPointer>
#include <QStringList>
#include "devicemanager.h"

class QDialog;
class QMimeData;
class QDragEnterEvent;
class QDropEvent;

class DeviceCheckWindow : public QWidget
{
    Q_OBJECT

public:
    explicit DeviceCheckWindow(QWidget *parent = nullptr);
    ~DeviceCheckWindow();
    
    void setPosition(int mainMenuX, int mainMenuY, int mainMenuHeight);
    bool isOperationInProgress() const { return operationInProgress; }

private slots:
    void onRebootButtonClicked();
    void onOpenCmdClicked();
    void onFlashBootClicked();
    void onFlashInitBootClicked();
    void onDeviceModeChanged(DeviceManager::DeviceMode mode);
    void onDeviceInfoUpdated(const QString &info);
    void restoreOpacity();
    bool eventFilter(QObject *watched, QEvent *event) override;

protected:
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void moveEvent(QMoveEvent *event) override;

private:
    friend class DeviceOperationTests;
    friend class DeviceInformationTests;
    bool beginOperation();
    void finishOperation();
    QProcess *createOperationProcess();
    void releaseOperationProcess(QProcess *process);
    bool canAcceptFileTransfer() const;
    QStringList droppedFiles(const QMimeData *mime) const;
    bool transferFiles(const QStringList &files);
    void transferNextFile();
    void finishFileTransferProcess(QProcess *process, bool success, bool failedToStart = false);
    void finishFileTransfer(const QString &interruption = QString());
    void setupUI();
    void updateUIForMode(DeviceManager::DeviceMode mode);
    void showDeviceDetails();
    void flashPartition(const QString &partition);
    void performFlash(const QString &partition, const QString &imagePath);
    void waitForFastbootMode();
    
    QVBoxLayout *mainLayout;
    QLabel *statusLabel;
    QLabel *infoLabel;
    QComboBox *rebootComboBox;
    QPushButton *executeButton;
    QPushButton *cmdButton;
    QPushButton *bootButton;
    QPushButton *initBootButton;
    bool operationInProgress = false;
    bool monitoringPausedByOperation = false;
    
    QProcess *currentProcess;
    QTimer *waitTimer;
    QString pendingFlashPartition;
    QString pendingFlashImage;
    int waitCounter;
    
    struct FileTransfer {
        QStringList files;
        QString serial;
        int next = 0;
        int succeeded = 0;
        QStringList failures;
    } fileTransfer;

    // 拖动相关
    bool isDragging;
    QPoint dragStartPosition;
    QTimer *opacityTimer;
    QPointer<QDialog> detailsDialog;
};

#endif // DEVICECHECKWINDOW_H
