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

class OugaFlashWindow;
class XiaomiFlashWindow;

class RepairWindow : public QWidget
{
    Q_OBJECT

public:
    explicit RepairWindow(QWidget *parent = nullptr);
    ~RepairWindow();

    void setPosition(int mainMenuX, int mainMenuY, int mainMenuHeight);
    bool hasActiveOugaTask() const;
protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onButtonClicked();
    void onUsbFixStep1Finished(int exitCode, QProcess::ExitStatus exitStatus);
    void onUsbFixStep2Finished(int exitCode, QProcess::ExitStatus exitStatus);
    void onUsbFixStep3Finished(int exitCode, QProcess::ExitStatus exitStatus);
    void onUsbFixProcessError(QProcess::ProcessError error);
    void onTmpFixStepFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onTmpFixProcessError(QProcess::ProcessError error);
    void onApkInstallStepFinished();
    void onModuleInstallStepFinished();
    void onRootDetectFinished();

private:
    friend class DeviceOperationTests;
    enum RepairAction { UsbFix, TmpFix, InstallApk, InstallModule, OugaFlash, XiaomiFlash };
    enum class ApkInstallStep { Push, InstallWithSuC, InstallWithSuS, DeleteTemporaryFile };
    enum class ModuleInstallStep { Push, Install, DeleteTemporaryFile };
    enum class UsbFixStep { Push, Execute, Cleanup };
    enum class TmpFixStep {
        CreateDirectory,
        CreateDirectoryFallback,
        SetContext,
        SetContextFallback,
        SetPermissions,
        SetPermissionsFallback
    };
    enum class RootManager { Magisk, APatch, KernelSU };

    void setupUI();
    void executeUsbFix();
    void fixTmpFolder();
    void installApk();
    void installModule();
    void startModuleInstall();
    bool setButtonsEnabled(bool enabled);
    void finishUsbFix(bool success, const QString &detail);
    void startUsbFixCleanup();
    void finishTmpFix(bool success, const QString &detail);
    void startTmpFixCommand(const QStringList &arguments, TmpFixStep step);

    OugaFlashWindow *ougaWindow = nullptr;
    XiaomiFlashWindow *xiaomiWindow = nullptr;
    QVBoxLayout *mainLayout;
    QVector<QPushButton*> buttons;
    QProcess *repairProcess;
    UsbFixStep usbFixStep;
    TmpFixStep tmpFixStep;
    bool usbFixExecutionFailed;
    QString usbFixError;

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
