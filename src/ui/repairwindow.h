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
    enum RepairAction { UsbFix, TmpFix, InstallApk, InstallModule };
    enum class ApkInstallStep { Push, InstallWithSuC, InstallWithSuS, DeleteTemporaryFile };
    enum class ModuleInstallStep { Push, Install, DeleteTemporaryFile };
    enum class RootManager { Magisk, APatch, KernelSU };

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
    ApkInstallStep apkInstallStep;

    // 模块安装相关
    QStringList moduleFilesToInstall;
    int currentModuleIndex;
    int moduleSuccessCount;
    int moduleFailCount;
    ModuleInstallStep moduleInstallStep;
    RootManager rootManagerType;
};

#endif // REPAIRWINDOW_H
