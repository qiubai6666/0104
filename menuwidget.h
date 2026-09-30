#ifndef MENUWIDGET_H
#define MENUWIDGET_H

#include <QWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <QVector>
#include <QProcess>
#include <QTimer>
#include "deviceinfowindow.h"
#include "repairwindow.h"
#include "payloadwindow.h"
#include "devicecheckwindow.h"
#include "configwindow.h"

class MenuWidget : public QWidget
{
    Q_OBJECT

public:
    explicit MenuWidget(QWidget *parent = nullptr);
    ~MenuWidget();

private slots:
    void onButtonClicked();
    void onLsProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onPullProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onCleanupProcessFinished();

private:
    void setupUI();
    void updatePosition();
    void extractImg();
    void cleanupAndExit();
    QString getLatestImgFile();
    
    QVBoxLayout *mainLayout;
    QVector<QPushButton*> buttons;
    DeviceInfoWindow *deviceInfoWindow;
    RepairWindow *repairWindow;
    PayloadWindow *payloadWindow;
    DeviceCheckWindow *deviceCheckWindow;
    ConfigWindow *configWindow;
    
    QProcess *imgProcess;
    QString latestImgFile;
    QString imgOutputPath;
    int cleanupStep;
};

#endif // MENUWIDGET_H
