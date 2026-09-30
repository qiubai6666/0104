#ifndef REPAIRWINDOW_H
#define REPAIRWINDOW_H

#include <QWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <QVector>
#include <QProcess>
#include <QFile>
#include <QTimer>
#include <QStringList>

class RepairWindow : public QWidget
{
    Q_OBJECT

public:
    explicit RepairWindow(QWidget *parent = nullptr);
    ~RepairWindow();
    
    void setPosition(int mainMenuX, int mainMenuY, int mainMenuHeight);

private slots:
    void onButtonClicked();
    void onUsbFixStep1Finished(int exitCode, QProcess::ExitStatus exitStatus);
    void onUsbFixStep2Finished(int exitCode, QProcess::ExitStatus exitStatus);
    void onUsbFixStep3Finished(int exitCode, QProcess::ExitStatus exitStatus);
    void onTmpFixStepFinished();
    void onApkInstallStepFinished();
    void onModuleInstallStepFinished();
    void onRootDetectFinished();

private:
    void setupUI();
    void executeUsbFix();
    void fixTmpFolder();
    void installApk();
    void installModule();
    void startModuleInstall();
    void setButtonsEnabled(bool enabled);
    
    QVBoxLayout *mainLayout;
    QVector<QPushButton*> buttons;
    QProcess *repairProcess;
    int tmpFixStep;
    
    // APK安装相关
    QStringList apkFilesToInstall;
    int currentApkIndex;
    int successCount;
    int failCount;
    int apkInstallStep;  // 0=push, 1=install(-c), 2=install(-s), 3=delete
    
    // 模块安装相关
    QStringList moduleFilesToInstall;
    int currentModuleIndex;
    int moduleSuccessCount;
    int moduleFailCount;
    int moduleInstallStep;  // 0=push, 1=install, 2=delete
    int rootManagerType;    // 0=Magisk, 1=APatch, 2=KernelSU
};

#endif // REPAIRWINDOW_H
