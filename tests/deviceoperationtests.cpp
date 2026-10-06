#include <QtTest>
#include <QApplication>
#include <QFile>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageBox>
#include <QPointer>
#include <QTemporaryDir>
#include <memory>
#include <cstdio>
#include "devicecheckwindow.h"
#include "menuwidget.h"
#include "repairwindow.h"
#include "devicemanager.h"
#include "processmanager.h"
#include "resourceextractor.h"
#include "shellcommand.h"
#include "ougaflashwindow.h"
#include "xiaomiflashwindow.h"
#include "deviceoperationlease.h"
#include <QSettings>
#include <QUuid>

namespace {
QString fixturePath;
QString fastbootOverride;
QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}
bool putFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}
}

// 测试专用链接替身：所有业务进程只能启动隔离目录内的本测试程序副本。
QString ResourceExtractor::getResourcePath() { return fixturePath; }
QString ResourceExtractor::getAdbPath() { return fixturePath + "/adb.exe"; }
QString ResourceExtractor::getFastbootPath()
{
    return fastbootOverride.isEmpty() ? fixturePath + "/fastboot.exe" : fastbootOverride;
}
QString ResourceExtractor::getNeilImagePath() { return fixturePath + "/Neil.jpg"; }

class DeviceOperationTests : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void init();
    void ougaSingleWindowAndLease();
    void xiaomiMenuAndSingleWindow();
    void cleanup();
    void repeatedActionsKeepRunningFlash();
    void windowCloseCannotInterruptFlash();
    void menuToggleAndExitCannotInterruptFlash();
    void repairMenuCanHideAndResume_data();
    void repairMenuCanHideAndResume();
    void destructionRestoresMonitoring();
    void successfulFlashAndRebootRestoreMonitoring();
    void failedStartRestoresMonitoring();
    void waitTimeoutReleasesOperation();
    void fastbootDelayRemainsExclusive();
    void legacyWaitWorksWithApplicationLease();
    void menuStartsMonitoringWithoutOtherWindows();
    void repairStartsMonitoringWithoutOtherWindows();
    void rootQuoting_data();
    void rootQuoting();
    void apkCommandsQuoteInstallAndCleanup();
    void moduleCommandsQuoteInstallAndCleanup_data();
    void moduleCommandsQuoteInstallAndCleanup();
private:
    void awaitFastboot(DeviceCheckWindow &window);
    QString audit() const {
        QString result;
        for (const QFileInfo &file : QDir(fixturePath).entryInfoList({"command-*.json"}, QDir::Files))
            result += QString::fromUtf8(readFile(file.absoluteFilePath())) + '\n';
        return result;
    }
    std::unique_ptr<QTemporaryDir> fixture;
    QTimer messageCloser;
    int messages = 0;
};

void DeviceOperationTests::initTestCase()
{
    QCoreApplication::setOrganizationName("OrangeToolsTests");
    QCoreApplication::setApplicationName("DeviceOperationTests");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, QDir::tempPath()+"/settings");
    DeviceOperationLease::setIdleCheck([] { return !ProcessManager::hasActiveDeviceProcesses(); });
    connect(DeviceOperationLease::instance(), &DeviceOperationLease::changed, this, [](bool held) {
        if (held) DeviceManager::instance()->pauseMonitoring();
        else DeviceManager::instance()->resumeMonitoring();
    });
}

void DeviceOperationTests::ougaSingleWindowAndLease()
{
    RepairWindow repair;
    repair.buttons[RepairWindow::OugaFlash]->click();
    auto first = repair.ougaWindow;
    QVERIFY(first);
    repair.buttons[RepairWindow::OugaFlash]->click();
    QCOMPARE(repair.ougaWindow, first);
    // The flasher has no native owner so Windows can show its taskbar button.
    QVERIFY(!first->parentWidget());
    int flashWindows = 0;
    for (QWidget *widget : QApplication::topLevelWidgets())
        if (qobject_cast<OugaFlashWindow *>(widget)) ++flashWindows;
    QCOMPARE(flashWindows, 1);
    QObject operation(first);
    QVERIFY(!repair.hasActiveOugaTask());
    QVERIFY(DeviceOperationLease::acquire(&operation));
    QVERIFY(repair.hasActiveOugaTask());
    repair.show();
    QVERIFY(repair.close());
    QVERIFY(!repair.isVisible());
    QCOMPARE(DeviceOperationLease::owner(), &operation);
    QVERIFY(!first->close());
    first->showMinimized();
    QVERIFY(DeviceOperationLease::busyFor(&repair));
    DeviceOperationLease::release(&operation);
    QVERIFY(!repair.hasActiveOugaTask());
    QVERIFY(first->close());
}

void DeviceOperationTests::xiaomiMenuAndSingleWindow()
{
    RepairWindow repair;
    QCOMPARE(repair.buttons.size(), 6);
    QCOMPARE(repair.buttons[RepairWindow::OugaFlash]->text(), QString("欧加线刷"));
    QCOMPARE(repair.buttons[RepairWindow::XiaomiFlash]->text(), QString("小米线刷"));
    QCOMPARE(int(RepairWindow::XiaomiFlash), int(RepairWindow::OugaFlash) + 1);
    repair.buttons[RepairWindow::XiaomiFlash]->click();
    auto first = repair.xiaomiWindow;
    QVERIFY(first);
    QVERIFY(!first->parentWidget());
    QVERIFY(first->isVisible());
    repair.buttons[RepairWindow::XiaomiFlash]->click();
    QCOMPARE(repair.xiaomiWindow, first);
    QObject operation(first);
    QVERIFY(DeviceOperationLease::acquire(&operation));
    QVERIFY(repair.hasActiveOugaTask());
    repair.show();
    QVERIFY(repair.close());
    QVERIFY(!repair.isVisible());
    QCOMPARE(DeviceOperationLease::owner(), &operation);
    QVERIFY(!first->close());
    DeviceOperationLease::release(&operation);
    QVERIFY(!repair.hasActiveOugaTask());
    QVERIFY(first->close());
    repair.buttons[RepairWindow::XiaomiFlash]->click();
    QCOMPARE(repair.xiaomiWindow, first);
    QVERIFY(first->isVisible());
}
void DeviceOperationTests::init()
{
    fixture.reset(new QTemporaryDir(QDir::tempPath() + "/OrangeOperationTests-XXXXXX"));
    QVERIFY(fixture->isValid());
    fixture->setAutoRemove(false); // Keep test evidence for manual cleanup.
    fixturePath = fixture->path();
    fastbootOverride.clear();
    QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), ResourceExtractor::getAdbPath()));
    QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), ResourceExtractor::getFastbootPath()));
    QVERIFY(putFile(fixturePath + "/mode.txt", "fastboot"));
    QVERIFY(putFile(fixturePath + "/behavior.txt", "hang"));
    messages = 0;
    connect(&messageCloser, &QTimer::timeout, this, [this]() {
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            if (auto *message = qobject_cast<QMessageBox *>(widget)) {
                if (message->isVisible()) { ++messages; message->accept(); }
            }
        }
    });
    messageCloser.start(10);
}

void DeviceOperationTests::cleanup()
{
    messageCloser.stop();
    disconnect(&messageCloser, nullptr, this, nullptr);
    auto *manager = DeviceManager::instance();
    manager->stopMonitoring();
    ProcessManager::stopAllProcesses();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCOMPARE(manager->m_fullModeRefCount, 0);
    QCOMPARE(manager->m_adbOnlyRefCount, 0);
    manager->m_isPaused = false;
    manager->m_isChecking = false;
    manager->m_currentMode = DeviceManager::None;
    manager->m_deviceInfo.clear();
    fixture.reset();
}

void DeviceOperationTests::awaitFastboot(DeviceCheckWindow &window)
{
    QTRY_COMPARE_WITH_TIMEOUT(DeviceManager::instance()->currentMode(), DeviceManager::Fastboot, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!DeviceManager::instance()->m_isChecking, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(DeviceManager::instance()->m_infoProcess->state(), QProcess::NotRunning, 5000);
    DeviceManager::instance()->m_checkTimer->stop();
    window.updateUIForMode(DeviceManager::Fastboot);
}

void DeviceOperationTests::repeatedActionsKeepRunningFlash()
{
    DeviceCheckWindow window;
    awaitFastboot(window);
    QVERIFY(window.beginOperation());
    window.performFlash("boot", fixturePath + "/boot.img");
    QTRY_COMPARE(window.currentProcess->state(), QProcess::Running);
    const QPointer<QProcess> original = window.currentProcess;
    const qint64 pid = original->processId();
    window.onRebootButtonClicked();
    window.onFlashBootClicked();
    window.onFlashInitBootClicked();
    window.performFlash("init_boot", "another.img");
    window.onDeviceModeChanged(DeviceManager::ADB);
    QCOMPARE(window.currentProcess, original.data());
    QCOMPARE(original->processId(), pid);
    QCOMPARE(original->state(), QProcess::Running);
    QVERIFY(window.isOperationInProgress());
    QVERIFY(!window.executeButton->isEnabled());
    QVERIFY(!window.bootButton->isEnabled());
    QVERIFY(!window.initBootButton->isEnabled());
    QVERIFY(!window.cmdButton->isEnabled());
}

void DeviceOperationTests::windowCloseCannotInterruptFlash()
{
    DeviceCheckWindow window;
    awaitFastboot(window);
    window.show();
    QVERIFY(window.beginOperation());
    window.performFlash("boot", "boot.img");
    QTRY_COMPARE(window.currentProcess->state(), QProcess::Running);
    QPointer<QProcess> running = window.currentProcess;
    QVERIFY(window.close());
    QVERIFY(!window.isVisible());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(running);
    QCOMPARE(DeviceOperationLease::owner(), &window);
    window.show();
    QCOMPARE(window.currentProcess, running.data());
    QCOMPARE(window.currentProcess->state(), QProcess::Running);
    QVERIFY(DeviceManager::instance()->m_isPaused);
    window.currentProcess->kill(); // 仅结束本测试的模拟进程，覆盖失败收尾。
    QTRY_VERIFY(!window.isOperationInProgress());
    QVERIFY(!DeviceManager::instance()->m_isPaused);
    QVERIFY(window.close());
    // Hiding emitted no warning; only the fake-process failure did.
    QVERIFY(messages >= 1);
}

void DeviceOperationTests::menuToggleAndExitCannotInterruptFlash()
{
    MenuWidget menu;
    menu.show();
    menu.buttons[MenuWidget::DeviceCheck]->click();
    QPointer<DeviceCheckWindow> window = menu.deviceCheckWindow;
    QVERIFY(window);
    awaitFastboot(*window);
    QVERIFY(window->beginOperation());
    window->performFlash("boot", "boot.img");
    QTRY_COMPARE(window->currentProcess->state(), QProcess::Running);
    QPointer<QProcess> running = window->currentProcess;
    menu.buttons[MenuWidget::DeviceCheck]->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCOMPARE(menu.deviceCheckWindow, window.data());
    QVERIFY(!window->isVisible());
    QCOMPARE(window->currentProcess, running.data());
    QCOMPARE(running->state(), QProcess::Running);
    QCOMPARE(DeviceOperationLease::owner(), window.data());
    QCOMPARE(messages, 0);
    menu.buttons[MenuWidget::DeviceCheck]->click();
    QVERIFY(window->isVisible());
    QCOMPARE(menu.deviceCheckWindow, window.data());
    menu.cleanupAndExit();
    QVERIFY(!menu.close());
    QVERIFY(menu.isVisible());
    QCOMPARE(running->state(), QProcess::Running);
    QVERIFY(messages >= 1); // Explicit application exit still warns/blocks.
    window->currentProcess->kill();
    QTRY_VERIFY(!window->isOperationInProgress());
    menu.buttons[MenuWidget::DeviceCheck]->click();
    QVERIFY(!window->isVisible());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(window);
    menu.buttons[MenuWidget::DeviceCheck]->click();
    QCOMPARE(menu.deviceCheckWindow, window.data());
    QVERIFY(window->isVisible());
}

void DeviceOperationTests::repairMenuCanHideAndResume_data()
{
    QTest::addColumn<int>("operationKind");
    QTest::newRow("xiaomi-flash") << 0;
    QTest::newRow("ouga-flash") << 1;
    QTest::newRow("repair-owned-operation") << 2;
}

void DeviceOperationTests::repairMenuCanHideAndResume()
{
    QFETCH(int, operationKind);
    MenuWidget menu;
    menu.show();
    menu.buttons[MenuWidget::RepairTools]->click();
    QPointer<RepairWindow> repair = menu.repairWindow;
    QVERIFY(repair);
    QWidget *operationWindow = repair;
    if (operationKind != 2) {
        repair->buttons[operationKind == 0 ? RepairWindow::XiaomiFlash : RepairWindow::OugaFlash]->click();
        operationWindow = operationKind == 0 ? static_cast<QWidget *>(repair->xiaomiWindow)
                                            : static_cast<QWidget *>(repair->ougaWindow);
    }
    QVERIFY(operationWindow);
    QPointer<QWidget> flashWindow = operationKind == 2 ? nullptr : operationWindow;
    QObject *owner = operationKind == 2 ? static_cast<QObject *>(repair.data()) : new QObject(operationWindow);
    QVERIFY(DeviceOperationLease::acquire(owner));
    QPointer<QProcess> process = ProcessManager::createProcess(operationWindow);
    process->start(ResourceExtractor::getFastbootPath(), {"flash", "boot", "test.img"});
    QTRY_COMPARE(process->state(), QProcess::Running);
    menu.buttons[MenuWidget::RepairTools]->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(repair); QVERIFY(process);
    QVERIFY(!repair->isVisible());
    QCOMPARE(menu.repairWindow, repair.data());
    QCOMPARE(process->state(), QProcess::Running);
    QCOMPARE(DeviceOperationLease::owner(), owner);
    QCOMPARE(messages, 0);
    if (flashWindow) QVERIFY(flashWindow->isVisible());
    menu.buttons[MenuWidget::RepairTools]->click();
    QCOMPARE(menu.repairWindow, repair.data());
    QVERIFY(repair->isVisible());
    QVERIFY(repair->close()); // Native Close also retains an active submenu.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(repair); QVERIFY(!repair->isVisible());
    QCOMPARE(process->state(), QProcess::Running);
    QCOMPARE(DeviceOperationLease::owner(), owner);
    if (flashWindow) {
        QVERIFY(!flashWindow->close()); // The actual flashing window stays protected.
        menu.buttons[MenuWidget::RepairTools]->click();
        repair->buttons[operationKind == 0 ? RepairWindow::XiaomiFlash : RepairWindow::OugaFlash]->click();
        QCOMPARE(operationKind == 0 ? static_cast<QWidget *>(repair->xiaomiWindow)
                                   : static_cast<QWidget *>(repair->ougaWindow), flashWindow.data());
    }
    QVERIFY(!menu.close());
    menu.cleanupAndExit();
    QCOMPARE(process->state(), QProcess::Running);
    process->kill(); // Only our isolated fake fastboot, never a real device.
    QVERIFY(process->waitForFinished(3000));
    DeviceOperationLease::release(owner);
    if (operationKind != 2) delete owner;
    if (flashWindow) {
        delete flashWindow.data();
        repair->xiaomiWindow = nullptr; repair->ougaWindow = nullptr;
    }
}

void DeviceOperationTests::destructionRestoresMonitoring()
{
    auto *window = new DeviceCheckWindow;
    awaitFastboot(*window);
    QVERIFY(window->beginOperation());
    window->performFlash("boot", "boot.img");
    QTRY_COMPARE(window->currentProcess->state(), QProcess::Running);
    QVERIFY(DeviceManager::instance()->m_isPaused);
    delete window; // 模拟强制销毁/退出，不能遗留暂停或运行结束回调。
    QVERIFY(!DeviceManager::instance()->m_isPaused);
    DeviceManager::instance()->m_currentMode = DeviceManager::None;
    DeviceCheckWindow reopened;
    QTRY_COMPARE_WITH_TIMEOUT(DeviceManager::instance()->currentMode(), DeviceManager::Fastboot, 5000);
}

void DeviceOperationTests::successfulFlashAndRebootRestoreMonitoring()
{
    QVERIFY(putFile(fixturePath + "/behavior.txt", "success"));
    DeviceCheckWindow window;
    awaitFastboot(window);
    QVERIFY(window.beginOperation());
    window.show();
    window.performFlash("boot", "boot.img");
    QVERIFY(window.close());
    QVERIFY(!window.isVisible());
    QTRY_VERIFY_WITH_TIMEOUT(!window.isOperationInProgress(), 5000);
    QVERIFY(!window.currentProcess);
    QVERIFY(!DeviceManager::instance()->m_isPaused);
    QVERIFY(audit().contains("[\"fastboot\",\"flash\",\"boot\",\"boot.img\"]"));
    QVERIFY(audit().contains("[\"fastboot\",\"reboot\"]"));
    QVERIFY(!window.isVisible());
    window.show();
    QVERIFY(window.bootButton->isEnabled());
}

void DeviceOperationTests::failedStartRestoresMonitoring()
{
    DeviceCheckWindow window;
    awaitFastboot(window);
    fastbootOverride = fixturePath + "/not-present.exe";
    QVERIFY(window.beginOperation());
    window.performFlash("boot", "boot.img");
    QTRY_VERIFY(!window.isOperationInProgress());
    QVERIFY(!window.currentProcess);
    QVERIFY(!DeviceManager::instance()->m_isPaused);
    QTRY_VERIFY(messages > 0);
}

void DeviceOperationTests::waitTimeoutReleasesOperation()
{
    DeviceCheckWindow window;
    awaitFastboot(window);
    DeviceManager::instance()->m_currentMode = DeviceManager::ADB;
    QVERIFY(window.beginOperation());
    window.pendingFlashPartition = "boot";
    window.pendingFlashImage = "boot.img";
    window.waitForFastbootMode();
    window.waitCounter = 39;
    QVERIFY(QMetaObject::invokeMethod(window.waitTimer, "timeout", Qt::DirectConnection));
    QVERIFY(!window.isOperationInProgress());
    QVERIFY(!window.waitTimer);
    QVERIFY(window.pendingFlashImage.isEmpty());
    QVERIFY(!DeviceManager::instance()->m_isPaused);
}

void DeviceOperationTests::fastbootDelayRemainsExclusive()
{
    DeviceCheckWindow window;
    awaitFastboot(window);
    QVERIFY(window.beginOperation());
    window.pendingFlashPartition = "boot";
    window.pendingFlashImage = "boot.img";
    window.waitForFastbootMode();
    QVERIFY(QMetaObject::invokeMethod(window.waitTimer, "timeout", Qt::DirectConnection));
    QVERIFY(!window.waitTimer);
    QVERIFY(window.isOperationInProgress());
    QVERIFY(!window.beginOperation());
    QVERIFY(window.close());
    QVERIFY(!window.isVisible());
    QVERIFY(!window.currentProcess);
    QTRY_VERIFY_WITH_TIMEOUT(window.currentProcess && window.currentProcess->state() == QProcess::Running, 4000);
    QVERIFY(DeviceManager::instance()->m_isPaused);
}

void DeviceOperationTests::legacyWaitWorksWithApplicationLease()
{
    QVERIFY(putFile(fixturePath + "/mode.txt", "adb"));
    DeviceCheckWindow window;
    QTRY_COMPARE_WITH_TIMEOUT(DeviceManager::instance()->currentMode(), DeviceManager::ADB, 5000);
    QVERIFY(window.beginOperation());
    QVERIFY(DeviceManager::instance()->m_isPaused);
    window.pendingFlashPartition = "boot";
    window.pendingFlashImage = "boot.img";
    // A completed fake ADB reboot makes the original fixture visible in Fastboot.
    QVERIFY(putFile(fixturePath + "/mode.txt", "fastboot"));
    window.waitForFastbootMode();
    QVERIFY(!DeviceManager::instance()->m_isPaused);
    QVERIFY(DeviceOperationLease::owner() == &window);
    QTRY_VERIFY_WITH_TIMEOUT(window.currentProcess && window.currentProcess->state() == QProcess::Running, 7000);
    QVERIFY(DeviceManager::instance()->m_isPaused);
    QTRY_VERIFY(audit().contains("[\"fastboot\",\"flash\",\"boot\",\"boot.img\"]"));
    window.currentProcess->kill(); // This is the isolated test executable, never a device command.
    QTRY_VERIFY(!window.isOperationInProgress());
    QVERIFY(!DeviceManager::instance()->m_isPaused);
}

void DeviceOperationTests::menuStartsMonitoringWithoutOtherWindows()
{
    QVERIFY(putFile(fixturePath + "/mode.txt", "adb"));
    MenuWidget menu;
    QVERIFY(!menu.deviceCheckWindow);
    QVERIFY(menu.screenCastController);
    QVERIFY(DeviceManager::instance()->m_checkTimer->isActive());
    QCOMPARE(DeviceManager::instance()->m_adbOnlyRefCount, 1);
    QTRY_COMPARE_WITH_TIMEOUT(DeviceManager::instance()->currentMode(), DeviceManager::ADB, 5000);
    QVERIFY(!audit().contains("\"fastboot\""));
}

void DeviceOperationTests::repairStartsMonitoringWithoutOtherWindows()
{
    QVERIFY(putFile(fixturePath + "/mode.txt", "adb"));
    RepairWindow repair;
    QVERIFY(DeviceManager::instance()->m_checkTimer->isActive());
    QCOMPARE(DeviceManager::instance()->m_adbOnlyRefCount, 1);
    QTRY_COMPARE_WITH_TIMEOUT(DeviceManager::instance()->currentMode(), DeviceManager::ADB, 5000);
}

void DeviceOperationTests::rootQuoting_data()
{
    QTest::addColumn<QString>("fileName");
    QTest::addColumn<QString>("option");
    const QStringList names = {"Normal.apk", "My App.apk", "单引号'空 格.apk",
                              "semi;printf INJECTED.apk", "$(printf INJECTED).zip",
                              "back\x60printf INJECTED\x60.zip", "dollar$HOME&test.zip", "percent%1%2.apk"};
    for (int i = 0; i < names.size(); ++i) {
        for (const QString &option : {QString("-c"), QString("-s")}) {
            QTest::newRow(qPrintable(QString::number(i) + option)) << names[i] << option;
        }
    }
}

void DeviceOperationTests::rootQuoting()
{
    QFETCH(QString, fileName);
    QFETCH(QString, option);
    const QString remote = "/data/local/tmp/" + fileName;
    const QString command = ShellCommand::asRoot("printf '%s' " + ShellCommand::quote(remote), option);
    // 仅用本地 POSIX shell 模拟两层解析；su 被无权限的函数替身覆盖。
    const QString bash = "C:/Program Files/Git/bin/bash.exe";
    QVERIFY2(QFile::exists(bash), "POSIX shell is required for quoting regression tests");
    QProcess process;
    process.start(bash, {"--noprofile", "--norc", "-c",
        "su() { /bin/sh -c \"$2\"; }; " + command});
    QVERIFY(process.waitForFinished(5000));
    QCOMPARE(process.exitCode(), 0);
    QCOMPARE(QString::fromUtf8(process.readAllStandardOutput()), remote);
    QVERIFY(process.readAllStandardError().isEmpty());
}

void DeviceOperationTests::apkCommandsQuoteInstallAndCleanup()
{
    QVERIFY(putFile(fixturePath + "/mode.txt", "adb"));
    RepairWindow repair;
    QTRY_COMPARE(DeviceManager::instance()->currentMode(), DeviceManager::ADB);
    repair.apkFilesToInstall = {fixturePath + "/My App's;test.apk"};
    repair.repairProcess = ProcessManager::createProcess(&repair);
    connect(repair.repairProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            &repair, &RepairWindow::onApkInstallStepFinished);
    repair.repairProcess->start(ResourceExtractor::getAdbPath(), {"push", repair.apkFilesToInstall[0],
        "/data/local/tmp/My App's;test.apk"});
    QTRY_COMPARE_WITH_TIMEOUT(repair.currentApkIndex, 1, 5000);
    QCOMPARE(repair.successCount, 1);
    QCOMPARE(repair.failCount, 0);
    const QString remote = "/data/local/tmp/My App's;test.apk";
    for (const QString &cmd : {"pm install -r ", "rm -f "}) {
        const QString quoted = ShellCommand::asRoot(cmd + ShellCommand::quote(remote));
        const QByteArray json = QJsonDocument(QJsonArray{"adb", "shell", quoted}).toJson(QJsonDocument::Compact);
        QVERIFY(audit().contains(QString::fromUtf8(json)));
    }
}

void DeviceOperationTests::moduleCommandsQuoteInstallAndCleanup_data()
{
    QTest::addColumn<int>("manager");
    QTest::addColumn<QString>("installPrefix");
    QTest::newRow("Magisk") << 0 << "magisk --install-module ";
    QTest::newRow("APatch") << 1 << "/data/adb/ap/bin/apd module install ";
    QTest::newRow("KernelSU") << 2 << "/data/adb/ksu/bin/ksud module install ";
}

void DeviceOperationTests::moduleCommandsQuoteInstallAndCleanup()
{
    QFETCH(int, manager);
    QFETCH(QString, installPrefix);
    QVERIFY(putFile(fixturePath + "/mode.txt", "adb"));
    RepairWindow repair;
    QTRY_COMPARE(DeviceManager::instance()->currentMode(), DeviceManager::ADB);
    repair.rootManagerType = static_cast<RepairWindow::RootManager>(manager);
    repair.moduleFilesToInstall = {fixturePath + "/My Module's;test.zip"};
    repair.repairProcess = ProcessManager::createProcess(&repair);
    repair.startModuleInstall();
    QTRY_COMPARE_WITH_TIMEOUT(repair.currentModuleIndex, 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(repair.buttons[RepairWindow::InstallModule]->text(), QString("安装模块"), 5000);
    QCOMPARE(repair.moduleSuccessCount, 1);
    QCOMPARE(repair.moduleFailCount, 0);
    const QString remote = "/data/local/tmp/My Module's;test.zip";
    for (const QString &cmd : {installPrefix, QString("rm -f ")}) {
        const QString quoted = ShellCommand::asRoot(cmd + ShellCommand::quote(remote));
        const QByteArray json = QJsonDocument(QJsonArray{"adb", "shell", quoted}).toJson(QJsonDocument::Compact);
        QVERIFY(audit().contains(QString::fromUtf8(json)));
    }
}

// 假 ADB/Fastboot 只读写临时夹具，不访问 USB、网络或运行任何刷机程序。
int helperMain(QCoreApplication &application)
{
    const QString base = QFileInfo(application.applicationFilePath()).baseName();
    const QString directory = application.applicationDirPath();
    QStringList args = application.arguments().mid(1);
    QJsonArray command;
    command.append(base);
    for (const QString &arg : args) command.append(arg);
    // One closed record per command: no shared append races or buffered records
    // hidden while the fake flash deliberately stays running.
    QFile auditFile(directory + "/command-" + QUuid::createUuid().toString(QUuid::Id128) + ".json");
    if (!auditFile.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return 90;
    const QByteArray record = QJsonDocument(command).toJson(QJsonDocument::Compact);
    if (auditFile.write(record) != record.size() || !auditFile.flush()) return 91;
    auditFile.close();
    if (args.value(0) == "-s") { args.removeFirst(); args.removeFirst(); }
    const QByteArray mode = readFile(directory + "/mode.txt");
    if (args.value(0) == "devices") {
        if (base == "adb") {
            std::puts(mode == "adb" ? "List of devices attached\nmock-serial\tdevice" : "List of devices attached");
        } else if (mode == "fastboot") {
            std::puts("mock-serial\tfastboot");
        }
        return 0;
    }
    if (base == "fastboot" && args.value(0) == "flash") {
        putFile(directory + "/flash-started.txt", "yes");
        if (readFile(directory + "/behavior.txt") == "hang") return application.exec();
        QTimer::singleShot(200, &application, [&application]() {
            std::puts("OKAY\nFinished. Total time: 0.2s");
            application.quit();
        });
        return application.exec();
    }
    if (args.value(0) == "reboot") {
        putFile(directory + "/mode.txt", args.contains("bootloader") ? "fastboot" : "adb");
        return 0;
    }
    if (args.value(0) == "getvar") {
        std::fprintf(stderr, "product: mock\ncurrent-slot: a\nunlocked: yes\n");
        return 0;
    }
    if (args.value(0) == "shell") std::puts("Success");
    return 0;
}

int main(int argc, char **argv)
{
    const QString base = QFileInfo(QString::fromLocal8Bit(argv[0])).baseName();
    if (base == "adb" || base == "fastboot") {
        QCoreApplication application(argc, argv);
        return helperMain(application);
    }
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    DeviceOperationTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "deviceoperationtests.moc"
