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
#include <cstdlib>
#include "devicecheckwindow.h"
#include "menuwidget.h"
#include "repairwindow.h"
#include "payloadwindow.h"
#include "configwindow.h"
#include <QLineEdit>
#include <QDragMoveEvent>
#include <QCheckBox>
#include <QLabel>
#include "devicemanager.h"
#include "processmanager.h"
#include "resourceextractor.h"
#include "shellcommand.h"
#include "ougaflashwindow.h"
#include "xiaomiflashwindow.h"
#include "openlistwindow.h"
#include "openlistinstaller.h"
#include "deviceoperationlease.h"
#include <QSettings>
#include <QUuid>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QNetworkProxy>
#include <QPushButton>

namespace {
QString fixturePath;
QString fastbootOverride;
QString adbOverride;
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
QString ResourceExtractor::getAdbPath()
{
    return adbOverride.isEmpty() ? fixturePath + "/adb.exe" : adbOverride;
}
QString ResourceExtractor::getFastbootPath()
{
    return fastbootOverride.isEmpty() ? fixturePath + "/fastboot.exe" : fastbootOverride;
}
QString ResourceExtractor::getNeilImagePath() { return fixturePath + "/Neil.jpg"; }

class InterruptedReply : public QNetworkReply
{
public:
    explicit InterruptedReply(QObject *parent = nullptr) : QNetworkReply(parent)
    {
        setUrl(QUrl("https://example.test/tool"));
        open(QIODevice::ReadOnly);
    }
    void abort() override
    {
        setError(OperationCanceledError, "cancelled fixture");
        setFinished(true);
        emit finished();
    }
protected:
    qint64 readData(char *, qint64) override { return -1; }
};

class DeviceOperationTests : public QObject
{
    Q_OBJECT
public:
    static int exitHelper(QApplication &application, const QStringList &arguments);
private slots:
    void uniqueTargetReboot_data();
    void uniqueTargetReboot();
    void multipleDevicesRejectOperations_data();
    void multipleDevicesRejectOperations();
    void physicalTargetChangeStopsOperation_data();
    void physicalTargetChangeStopsOperation();
    void physicalDisconnectStopsFileBatch();
    void configRejectsUnsafeInputs();
    void configTimeoutRetryRestoresButton();
    void configExistingExeWithoutHashCanLaunch();
    void configConflictingDestinationRejected();
    void initTestCase();
    void init();
    void payloadShortcutHasNoBootFilters();
    void ougaSingleWindowAndLease();
    void xiaomiMenuAndSingleWindow();
    void openListMenuAndSingleWindow();
    void openListApplicationExit_data();
    void openListApplicationExit();
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
    void droppedFilesReachSharedStorage();
    void allSubmenusAcceptFiles_data();
    void allSubmenusAcceptFiles();
    void submenuDropOverEditorCopiesFile();
    void submenuTransferSurvivesSwitchAndRejectsSecondDrop();
    void submenuDropRechecksModeAndLease_data();
    void submenuDropRechecksModeAndLease();
    void unrelatedWindowsAndFoldersDoNotTransfer();
    void invalidDropsAreRejected_data();
    void invalidDropsAreRejected();
    void dropRechecksModeAndLease();
    void transferFailures_data();
    void transferFailures();
    void failedTransferStartReleasesOperation();
    void transferHideAndRepeatedDrop();
    void transferDestructionDoesNotStartNextFile();
    void transferDeviceChangeStopsBatch_data();
    void transferDeviceChangeStopsBatch();
    void rootQuoting_data();
    void rootQuoting();
    void apkCommandsQuoteInstallAndCleanup();
    void moduleCommandsQuoteInstallAndCleanup_data();
    void moduleCommandsQuoteInstallAndCleanup();
private:
    void awaitFastboot(DeviceCheckWindow &window);
    void awaitAdb(DeviceCheckWindow &window);
    bool dropFiles(QWidget &window, const QList<QUrl> &urls);
    QList<QJsonArray> pushes() const;
    QString lastMessage;
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

void DeviceOperationTests::uniqueTargetReboot_data()
{
    QTest::addColumn<bool>("adb");
    QTest::newRow("adb") << true;
    QTest::newRow("fastboot") << false;
}

void DeviceOperationTests::uniqueTargetReboot()
{
    QFETCH(bool, adb);
    DeviceCheckWindow window;
    if (adb) awaitAdb(window); else awaitFastboot(window);
    window.onRebootButtonClicked();
    QTRY_VERIFY(!window.isOperationInProgress());
    const QJsonArray command{adb ? "adb" : "fastboot", "-s", "mock-serial", "reboot"};
    QVERIFY(audit().contains(QString::fromUtf8(QJsonDocument(command).toJson(QJsonDocument::Compact))));
    QVERIFY(!DeviceOperationLease::owner());
}

void DeviceOperationTests::multipleDevicesRejectOperations_data()
{
    uniqueTargetReboot_data();
}

void DeviceOperationTests::multipleDevicesRejectOperations()
{
    QFETCH(bool, adb);
    QVERIFY(putFile(fixturePath + "/mode.txt", adb ? "adb" : "fastboot"));
    QVERIFY(putFile(fixturePath + "/multiple.txt", "yes"));
    DeviceCheckWindow window;
    QTRY_COMPARE(DeviceManager::instance()->deviceSerials().size(), 2);
    QVERIFY(!DeviceManager::instance()->hasUniqueOperableDevice());
    window.onRebootButtonClicked();
    QVERIFY(!window.isOperationInProgress());
    QVERIFY(lastMessage.contains("断开其他设备"));
    window.flashPartition("boot");
    QVERIFY(!window.isOperationInProgress());
    QVERIFY(!window.transferFiles({fixturePath + "/dummy.txt"}));
    if (adb) {
        RepairWindow repair;
        QVERIFY(!repair.beginAdbOperation());
        QVERIFY(repair.operationSerial.isEmpty());
    }
    QVERIFY(!DeviceOperationLease::owner());
    QVERIFY(!audit().contains("\"reboot\""));
    QVERIFY(!audit().contains("\"flash\""));
    QVERIFY(!audit().contains("\"push\""));
}

void DeviceOperationTests::physicalTargetChangeStopsOperation_data()
{
    QTest::addColumn<bool>("adb");
    QTest::addColumn<QString>("change");
    for (bool adb : {true, false}) {
        for (const QString &change : {QString("serial"), QString("none"), QString("multiple")})
            QTest::newRow(qPrintable((adb ? QString("adb-") : QString("fastboot-")) + change)) << adb << change;
    }
}

void DeviceOperationTests::physicalTargetChangeStopsOperation()
{
    QFETCH(bool, adb);
    QFETCH(QString, change);
    DeviceCheckWindow window;
    if (adb) awaitAdb(window); else awaitFastboot(window);
    DeviceManager::instance()->pauseMonitoring();
    if (change == "serial") QVERIFY(putFile(fixturePath + "/serial.txt", "another-device"));
    else if (change == "none") QVERIFY(putFile(fixturePath + "/mode.txt", "none"));
    else QVERIFY(putFile(fixturePath + "/multiple.txt", "yes"));
    // Cached UI data intentionally still points to the original device.
    QCOMPARE(DeviceManager::instance()->uniqueDeviceSerial(), QString("mock-serial"));
    window.onRebootButtonClicked();
    QTRY_VERIFY(!window.isOperationInProgress());
    QVERIFY(!audit().contains("\"reboot\""));
    QVERIFY(!DeviceOperationLease::owner());
    QVERIFY(!lastMessage.isEmpty());
    if (adb) {
        DeviceManager::instance()->setDetectedDevice(DeviceManager::ADB, "mock-serial");
        DeviceManager::instance()->pauseMonitoring();
        RepairWindow repair;
        QVERIFY(repair.beginAdbOperation());
        repair.repairProcess = ProcessManager::createProcess(&repair);
        repair.startAdbCommand({"reboot"});
        QTRY_VERIFY(repair.operationSerial.isEmpty());
        QVERIFY(repair.buttons[RepairWindow::UsbFix]->isEnabled());
        QVERIFY(!DeviceOperationLease::owner());
        QVERIFY(!audit().contains("\"reboot\""));
    }
}

void DeviceOperationTests::physicalDisconnectStopsFileBatch()
{
    DeviceCheckWindow window;
    awaitAdb(window);
    const QString first = fixturePath + "/first.txt";
    const QString second = fixturePath + "/second.txt";
    QVERIFY(putFile(first, "first"));
    QVERIFY(putFile(second, "second"));
    QVERIFY(putFile(fixturePath + "/push-behavior.txt", "disconnect-after-first"));
    QVERIFY(window.transferFiles({first, second}));
    QTRY_VERIFY(!window.isOperationInProgress());
    QCOMPARE(pushes().size(), 1);
    QVERIFY(!DeviceOperationLease::owner());
}

void DeviceOperationTests::configRejectsUnsafeInputs()
{
    QVERIFY(putFile(fixturePath + "/download_config.txt",
        "id|type|file|url\n"
        "1|exe|safe.exe|https://example.test/file\n"
        "2|zip|tool.exe|https://example.test/archive\n"
        "3|exe|bad.exe|http://example.test/file\n"
        "4|exe|../bad.exe|https://example.test/file\n"
        "5|bat|bad.bat|https://example.test/file\n"
        "6|exe|bad.exe|https:///file\n"));
    ConfigWindow window;
    QCOMPARE(window.configData.size(), 2);
    for (const QString &name : {QString("CON.exe"), QString("LPT1.exe"), QString("bad?.exe"),
                                QString("bad.exe."), QString("bad.exe "), QString("../tool.exe")})
        QVERIFY(!ConfigWindow::isSafeFileName(name));
    QVERIFY(ConfigWindow::isSafeFileName("中文 tool.exe"));
    QVERIFY(putFile(fixturePath + "/download_config.txt", "header\n6|exe|new.exe|https://example.test/new\n"));
    window.loadConfigFile();
    QCOMPARE(window.configData.size(), 1);
    QVERIFY(window.configData.contains(6));
}

void DeviceOperationTests::configTimeoutRetryRestoresButton()
{
    QVERIFY(putFile(fixturePath + "/download_config.txt", "header\n1|exe|tool.exe|https://example.test/tool\n"));
    ConfigWindow window;
    auto *reply = new InterruptedReply(&window);
    auto *writer = new DownloadFileWriter(fixturePath + "/tool.exe");
    writer->attach(reply, 20);
    window.downloadingFiles[reply] = 1;
    window.downloadFiles[reply] = writer;
    window.downloadPaths[reply] = fixturePath + "/tool.exe";
    window.activeDownloadPaths[1] = fixturePath + "/tool.exe";
    window.buttons[0]->setEnabled(false);
    connect(reply, &QNetworkReply::finished, &window, [&window, reply] { window.onFileDownloadFinished(reply); });
    QTRY_COMPARE_WITH_TIMEOUT(window.retryCount.value(1), 1, 500);
    QVERIFY(!window.buttons[0]->isEnabled());
    QVERIFY(window.downloadFiles.isEmpty());
    // The retry uses the offline proxy configured by initTestCase and fails.
    QTRY_VERIFY_WITH_TIMEOUT(window.buttons[0]->isEnabled(), 5000);
    QVERIFY(window.activeDownloadPaths.isEmpty());
    QVERIFY(window.retryCount.isEmpty());
    QCOMPARE(window.buttons[0]->text(), QString("tool"));
    QVERIFY(!QFile::exists(fixturePath + "/tool.exe"));
}

void DeviceOperationTests::configExistingExeWithoutHashCanLaunch()
{
    QVERIFY(putFile(fixturePath + "/download_config.txt", "header\n1|exe|adb.exe|https://example.test/tool\n"));
    ConfigWindow window;
    window.downloadFile(1);
    QTRY_VERIFY(audit().contains("[\"adb\"]"));
    QVERIFY(window.buttons[0]->isEnabled());
    QVERIFY(window.activeDownloadPaths.isEmpty());
}

void DeviceOperationTests::configConflictingDestinationRejected()
{
    QVERIFY(putFile(fixturePath + "/download_config.txt",
        "header\n1|zip|tool.exe|https://example.test/one\n2|zip|TOOL.exe|https://example.test/two\n"));
    ConfigWindow window;
    window.activeDownloadPaths[1] = fixturePath + "/tool.zip";
    window.downloadFile(2);
    QVERIFY(window.downloadingFiles.isEmpty());
    QVERIFY(window.activeDownloadPaths.contains(1));
    QVERIFY(window.buttons[1]->isEnabled());
    QVERIFY(lastMessage.contains("正在下载"));
}

void DeviceOperationTests::initTestCase()
{
    // All UI tests are offline, including lazily-opened Openlist pages.
    QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::HttpProxy, QStringLiteral("127.0.0.1"), 1));
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

void DeviceOperationTests::payloadShortcutHasNoBootFilters()
{
    PayloadWindow window;
    // 2026-10 需求：boot / init_boot 复选（支持双选）+ 自定义分区名，默认不勾选。
    auto *boot = window.findChild<QCheckBox *>(QStringLiteral("PayloadBootCheck"));
    auto *initBoot = window.findChild<QCheckBox *>(QStringLiteral("PayloadInitBootCheck"));
    QVERIFY(boot);
    QVERIFY(initBoot);
    QVERIFY(!boot->isChecked());
    QVERIFY(!initBoot->isChecked());
    QVERIFY(window.findChild<QLineEdit *>(QStringLiteral("PayloadCustomPartitionsEdit")));
    bool describesOutput = false;
    for (auto label : window.findChildren<QLabel *>())
        describesOutput |= label->text().contains(QStringLiteral("输出路径"));
    QVERIFY(describesOutput);
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
    QCOMPARE(repair.buttons.size(), 7);
    QCOMPARE(repair.buttons[RepairWindow::OpenList]->text(), QString("Openlist"));
    QCOMPARE(int(RepairWindow::OpenList), int(RepairWindow::XiaomiFlash) + 1);
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
void DeviceOperationTests::openListMenuAndSingleWindow()
{
    RepairWindow repair;
    repair.buttons[RepairWindow::OpenList]->click();
    auto *first=repair.openListWindow; QVERIFY(first); QVERIFY(first->isVisible());
    QVERIFY(!first->parentWidget());
    repair.buttons[RepairWindow::OpenList]->click(); QCOMPARE(repair.openListWindow,first);
    QCloseEvent close; QApplication::sendEvent(first,&close); QVERIFY(close.isAccepted());
    QVERIFY(first->close()); QVERIFY(!first->isVisible());
    repair.buttons[RepairWindow::OpenList]->click(); QCOMPARE(repair.openListWindow,first); QVERIFY(first->isVisible());
    QObject operation(first); QVERIFY(DeviceOperationLease::acquire(&operation));
    QVERIFY(!repair.hasActiveOugaTask());
    QVERIFY(!first->close());
    QVERIFY(DeviceOperationLease::busyFor(&repair));
    DeviceOperationLease::release(&operation);
    QVERIFY(first->close());
}

void DeviceOperationTests::openListApplicationExit_data()
{
    QTest::addColumn<QString>("state");
    for (const QString &state : {QString("visible"), QString("minimized"), QString("hidden"), QString("busy"), QString("download"), QString("install"), QString("transfer")})
        QTest::newRow(qPrintable(state)) << state;
}

void DeviceOperationTests::openListApplicationExit()
{
    QFETCH(QString, state);
    QProcess child;
    child.setWorkingDirectory(fixturePath);
    child.start(QCoreApplication::applicationFilePath(), {"--exit-helper", state, fixturePath});
    QVERIFY(child.waitForFinished(10000));
    const QByteArray output = child.readAllStandardOutput() + child.readAllStandardError();
    QVERIFY2(child.exitStatus() == QProcess::NormalExit, output.constData());
    QVERIFY2(child.exitCode() == 0, output.constData());
    QCOMPARE(readFile(fixturePath + "/exit-result.txt"), QByteArray("all windows closed"));
    if (state == "busy") QCOMPARE(readFile(fixturePath + "/busy-result.txt"), QByteArray("exit blocked"));
}

int DeviceOperationTests::exitHelper(QApplication &application, const QStringList &arguments)
{
    const QString state = arguments.value(2), evidence = arguments.value(3);
    // cleanupAndExit may remove only this child's disposable resource directory.
    fixturePath = evidence + "/exit-resources";
    QDir().mkpath(fixturePath);
    adbOverride = evidence + "/adb.exe"; fastbootOverride = evidence + "/fastboot.exe";
    application.setQuitOnLastWindowClosed(false);
    // Refuse network traffic locally; never contact the cloud in exit regressions.
    QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::HttpProxy, QStringLiteral("127.0.0.1"), 1));
    MenuWidget menu; menu.show();
    menu.buttons[MenuWidget::RepairTools]->click();
    auto *repair = menu.repairWindow;
    repair->buttons[RepairWindow::OpenList]->click();
    auto *openlist = repair->openListWindow;
    if (state == "minimized") openlist->showMinimized();
    if (state == "hidden") openlist->hide();
    QObject operation(openlist);
    if (state == "busy" && !DeviceOperationLease::acquire(&operation)) return 90;
    QTimer messageCloser;
    QObject::connect(&messageCloser, &QTimer::timeout, &menu, [] {
        for (QWidget *widget : QApplication::topLevelWidgets())
            if (auto *message = qobject_cast<QMessageBox *>(widget)) message->accept();
    });
    messageCloser.start(10);
    QObject::connect(&application, &QCoreApplication::aboutToQuit, &menu, [&] {
        if (!menu.isVisible() && !repair->isVisible() && !openlist->isVisible())
            putFile(evidence + "/exit-result.txt", "all windows closed");
    });
    QTimer::singleShot(50, &menu, [&] {
        if (state == "download" || state == "install" || state == "transfer") {
            auto *button = openlist->findChild<QPushButton *>("openListInstallMt");
            if (!button) { application.exit(93); return; }
            button->click(); // Active download, before the proxy failure is delivered.
            if (!openlist->isBusy() || repair->hasActiveOugaTask()) { application.exit(94); return; }
            if (state != "download") {
                auto *installer = openlist->findChild<OpenListInstaller *>();
                if (!installer || !DeviceOperationLease::acquire(installer)) { application.exit(95); return; }
                if (DeviceOperationLease::blocksApplicationExit()) { application.exit(96); return; }
            }
        }
        menu.buttons[MenuWidget::Exit]->click(); // Exactly one click for Openlist, including active tasks.
        if (state != "busy") return;
        if (!menu.isVisible() || !repair->isVisible() || !openlist->isVisible() ||
            DeviceOperationLease::owner() != &operation) { application.exit(92); return; }
        putFile(evidence + "/busy-result.txt", "exit blocked");
        DeviceOperationLease::release(&operation);
        menu.buttons[MenuWidget::Exit]->click(); // Now idle: exit must succeed.
    });
    QTimer::singleShot(4000, &application, [&] { application.exit(91); });
    const int result = application.exec();
    return result;
}

void DeviceOperationTests::init()
{
    fixture.reset(new QTemporaryDir(QDir::tempPath() + "/OrangeOperationTests-XXXXXX"));
    QVERIFY(fixture->isValid());
    fixture->setAutoRemove(false); // Keep test evidence for manual cleanup.
    fixturePath = fixture->path();
    fastbootOverride.clear();
    adbOverride.clear();
    QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), ResourceExtractor::getAdbPath()));
    QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), ResourceExtractor::getFastbootPath()));
    QVERIFY(putFile(fixturePath + "/mode.txt", "fastboot"));
    QVERIFY(putFile(fixturePath + "/behavior.txt", "hang"));
    messages = 0;
    lastMessage.clear();
    connect(&messageCloser, &QTimer::timeout, this, [this]() {
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            if (auto *message = qobject_cast<QMessageBox *>(widget)) {
                if (message->isVisible()) { ++messages; lastMessage = message->text(); message->accept(); }
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
    // Native tool windows are released after their retained launcher is deleted.
    QTRY_COMPARE(manager->m_adbOnlyRefCount, 0);
    manager->m_isPaused = false;
    manager->m_isChecking = false;
    manager->setDetectedDevice(DeviceManager::None, {});
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

void DeviceOperationTests::awaitAdb(DeviceCheckWindow &window)
{
    QVERIFY(putFile(fixturePath + "/mode.txt", "adb"));
    QTRY_COMPARE_WITH_TIMEOUT(DeviceManager::instance()->currentMode(), DeviceManager::ADB, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!DeviceManager::instance()->m_isChecking, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!DeviceManager::instance()->m_infoQueryActive, 5000);
    DeviceManager::instance()->m_checkTimer->stop();
    window.updateUIForMode(DeviceManager::ADB);
}

bool DeviceOperationTests::dropFiles(QWidget &window, const QList<QUrl> &urls)
{
    QMimeData mime;
    mime.setUrls(urls);
    QDragEnterEvent enter(QPoint(30, 80), Qt::CopyAction | Qt::MoveAction, &mime,
                          Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&window, &enter);
    if (!enter.isAccepted()) return false;
    if (enter.dropAction() != Qt::CopyAction) return false;
    QDropEvent drop(QPointF(30, 80), Qt::CopyAction | Qt::MoveAction, &mime,
                    Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&window, &drop);
    return drop.isAccepted() && drop.dropAction() == Qt::CopyAction;
}

QList<QJsonArray> DeviceOperationTests::pushes() const
{
    QList<QJsonArray> result;
    for (const QFileInfo &file : QDir(fixturePath).entryInfoList({"command-*.json"}, QDir::Files)) {
        const QJsonArray command = QJsonDocument::fromJson(readFile(file.absoluteFilePath())).array();
        if (command.contains("push")) result.append(command);
    }
    return result;
}

void DeviceOperationTests::allSubmenusAcceptFiles_data()
{
    QTest::addColumn<int>("submenu");
    QTest::newRow("screen-cast") << -1;
    QTest::newRow("repair") << int(MenuWidget::RepairTools);
    QTest::newRow("payload") << int(MenuWidget::Payload);
    QTest::newRow("device-check") << int(MenuWidget::DeviceCheck);
    QTest::newRow("configuration") << int(MenuWidget::Configuration);
    QTest::newRow("ouga-flash") << 100;
    QTest::newRow("xiaomi-flash") << 101;
    QTest::newRow("openlist") << 102;
}

void DeviceOperationTests::allSubmenusAcceptFiles()
{
    QFETCH(int, submenu);
    MenuWidget menu;
    menu.show();
    QWidget *target = nullptr;
    if (submenu == -1) {
        // 主页（面板本体）同样是合法拖放目标。
        target = &menu;
    } else if (submenu >= 100) {
        menu.buttons[MenuWidget::RepairTools]->click();
        const int action = submenu == 100 ? RepairWindow::OugaFlash :
                           submenu == 101 ? RepairWindow::XiaomiFlash : RepairWindow::OpenList;
        menu.repairWindow->buttons[action]->click();
        target = submenu == 100 ? static_cast<QWidget *>(menu.repairWindow->ougaWindow) :
                 submenu == 101 ? static_cast<QWidget *>(menu.repairWindow->xiaomiWindow) :
                                  static_cast<QWidget *>(menu.repairWindow->openListWindow);
    } else {
        menu.buttons[submenu]->click();
        target = submenu == MenuWidget::RepairTools ? static_cast<QWidget *>(menu.repairWindow) :
                 submenu == MenuWidget::Payload ? static_cast<QWidget *>(menu.payloadWindow) :
                 submenu == MenuWidget::DeviceCheck ? static_cast<QWidget *>(menu.deviceCheckWindow) :
                                                      static_cast<QWidget *>(menu.configWindow);
    }
    QVERIFY(target);
    QVERIFY(target->acceptDrops());
    QVERIFY(putFile(fixturePath + "/mode.txt", "adb"));
    QTRY_COMPARE_WITH_TIMEOUT(DeviceManager::instance()->currentMode(), DeviceManager::ADB, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!DeviceManager::instance()->m_infoQueryActive, 5000);
    DeviceManager::instance()->m_checkTimer->stop();
    QVERIFY(putFile(fixturePath + "/push-behavior.txt", "success"));
    const QString path = fixturePath + QStringLiteral("/中文 文件's;&.txt");
    QVERIFY(putFile(path, "original contents"));
    const bool hadDeviceWindow = menu.deviceCheckWindow;
    QVERIFY(dropFiles(*target, {QUrl::fromLocalFile(path)}));
    QVERIFY(menu.deviceCheckWindow);
    QVERIFY(menu.deviceCheckWindow->operationInProgress);
    QCOMPARE(menu.deviceCheckWindow->fileTransferSource.data(), target);
    QCOMPARE(DeviceOperationLease::owner(), menu.deviceCheckWindow);
    if (!hadDeviceWindow) QVERIFY(!menu.deviceCheckWindow->isVisible());
    QTRY_VERIFY_WITH_TIMEOUT(!menu.deviceCheckWindow->operationInProgress, 5000);
    QTRY_COMPARE(messages, 1);
    QCOMPARE(pushes().size(), 1);
    QVERIFY(pushes().contains(QJsonArray{"adb", "-s", "mock-serial", "push", path,
                                       "/sdcard/" + QFileInfo(path).fileName()}));
    QCOMPARE(readFile(path), QByteArray("original contents"));
    const QString snapshots = qEnvironmentVariable("ORANGE_UI_SNAPSHOT_DIR");
    if (!snapshots.isEmpty()) {
        QVERIFY(QDir().mkpath(snapshots));
        QVERIFY(target->grab().save(snapshots + '/' + QString::fromLatin1(QTest::currentDataTag()) + ".png"));
    }
}

void DeviceOperationTests::submenuDropOverEditorCopiesFile()
{
    MenuWidget menu;
    menu.show();
    menu.buttons[MenuWidget::Payload]->click();
    auto *editor = menu.payloadWindow->findChild<QLineEdit *>();
    QVERIFY(editor);
    // Native drop targets must not insert the local path or consume the drop.
    editor->setAcceptDrops(true);
    editor->setText("keep editor text");
    QVERIFY(putFile(fixturePath + "/mode.txt", "adb"));
    QTRY_COMPARE_WITH_TIMEOUT(DeviceManager::instance()->currentMode(), DeviceManager::ADB, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!DeviceManager::instance()->m_infoQueryActive, 5000);
    DeviceManager::instance()->m_checkTimer->stop();
    QVERIFY(putFile(fixturePath + "/push-behavior.txt", "success"));
    const QString path = fixturePath + "/editor-drop.txt";
    QVERIFY(putFile(path, "contents"));
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(path)});
    QDragEnterEvent enter(QPoint(5, 5), Qt::CopyAction | Qt::MoveAction, &mime,
                         Qt::LeftButton, Qt::ShiftModifier);
    QApplication::sendEvent(editor, &enter);
    QVERIFY(enter.isAccepted());
    QCOMPARE(enter.dropAction(), Qt::CopyAction);
    QDragMoveEvent move(QPoint(5, 5), Qt::CopyAction | Qt::MoveAction, &mime,
                       Qt::LeftButton, Qt::ShiftModifier);
    QApplication::sendEvent(editor, &move);
    QVERIFY(move.isAccepted());
    QCOMPARE(move.dropAction(), Qt::CopyAction);
    QDropEvent drop(QPointF(5, 5), Qt::CopyAction | Qt::MoveAction, &mime,
                    Qt::LeftButton, Qt::ShiftModifier);
    QApplication::sendEvent(editor, &drop);
    QVERIFY(drop.isAccepted());
    QCOMPARE(drop.dropAction(), Qt::CopyAction);
    QTRY_VERIFY_WITH_TIMEOUT(!menu.deviceCheckWindow->operationInProgress, 5000);
    QTRY_COMPARE(messages, 1);
    QCOMPARE(editor->text(), QString("keep editor text"));
    QCOMPARE(pushes().size(), 1);
    QVERIFY(QFile::exists(path));
}

void DeviceOperationTests::submenuTransferSurvivesSwitchAndRejectsSecondDrop()
{
    MenuWidget menu;
    menu.show();
    menu.buttons[MenuWidget::DeviceCheck]->click();
    awaitAdb(*menu.deviceCheckWindow);
    menu.buttons[MenuWidget::RepairTools]->click();
    QVERIFY(putFile(fixturePath + "/push-behavior.txt", "hang"));
    const QString path = fixturePath + "/retained-drop.txt";
    QVERIFY(putFile(path, "contents"));
    QVERIFY(dropFiles(*menu.repairWindow, {QUrl::fromLocalFile(path)}));
    QTRY_COMPARE(menu.deviceCheckWindow->currentProcess->state(), QProcess::Running);
    menu.buttons[MenuWidget::Configuration]->click();
    QVERIFY(!menu.repairWindow->isVisible());
    QVERIFY(!dropFiles(*menu.configWindow, {QUrl::fromLocalFile(path)}));
    QCOMPARE(menu.deviceCheckWindow->currentProcess->state(), QProcess::Running);
    QCOMPARE(DeviceOperationLease::owner(), menu.deviceCheckWindow);
    QTRY_COMPARE(pushes().size(), 1);
    menu.deviceCheckWindow->currentProcess->kill();
    QTRY_VERIFY_WITH_TIMEOUT(!menu.deviceCheckWindow->operationInProgress, 5000);
    QTRY_COMPARE(messages, 1);
    QVERIFY(!DeviceOperationLease::owner());
}

void DeviceOperationTests::submenuDropRechecksModeAndLease_data()
{
    QTest::addColumn<int>("change");
    QTest::newRow("disconnected") << 0;
    QTest::newRow("fastboot") << 1;
    QTest::newRow("lease-held") << 2;
    QTest::newRow("move-only") << 3;
}

void DeviceOperationTests::submenuDropRechecksModeAndLease()
{
    QFETCH(int, change);
    MenuWidget menu;
    menu.show();
    menu.buttons[MenuWidget::DeviceCheck]->click();
    awaitAdb(*menu.deviceCheckWindow);
    menu.buttons[MenuWidget::Configuration]->click();
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(ResourceExtractor::getAdbPath())});
    QDragEnterEvent enter(QPoint(30, 80), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(menu.configWindow, &enter);
    QVERIFY(enter.isAccepted());
    QObject other;
    if (change == 0) DeviceManager::instance()->setDetectedDevice(DeviceManager::None, {});
    if (change == 1) DeviceManager::instance()->setDetectedDevice(DeviceManager::Fastboot, "mock-serial");
    if (change == 2) QVERIFY(DeviceOperationLease::acquire(&other));
    QDropEvent drop(QPointF(30, 80), change == 3 ? Qt::MoveAction : Qt::CopyAction,
                    &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(menu.configWindow, &drop);
    QVERIFY(!drop.isAccepted());
    QCOMPARE(pushes().size(), 0);
    QVERIFY(!menu.deviceCheckWindow->operationInProgress);
    if (change == 2) DeviceOperationLease::release(&other);
}

void DeviceOperationTests::unrelatedWindowsAndFoldersDoNotTransfer()
{
    MenuWidget menu;
    menu.show();
    menu.buttons[MenuWidget::RepairTools]->click();
    menu.repairWindow->buttons[RepairWindow::OugaFlash]->click();
    QVERIFY(putFile(fixturePath + "/mode.txt", "adb"));
    QTRY_COMPARE_WITH_TIMEOUT(DeviceManager::instance()->currentMode(), DeviceManager::ADB, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!DeviceManager::instance()->m_infoQueryActive, 5000);
    DeviceManager::instance()->m_checkTimer->stop();
    QWidget unrelated;
    unrelated.setAcceptDrops(true);
    QVERIFY(!menu.fileDropSurface(&unrelated));
    QVERIFY(!dropFiles(unrelated, {QUrl::fromLocalFile(ResourceExtractor::getAdbPath())}));
    QMessageBox dialog(menu.repairWindow);
    QVERIFY(!menu.fileDropSurface(&dialog));
    QMimeData folder;
    folder.setUrls({QUrl::fromLocalFile(fixturePath)});
    QVERIFY(DeviceCheckWindow::droppedFiles(&folder).isEmpty());
    QDragEnterEvent enter(QPoint(30, 80), Qt::CopyAction, &folder, Qt::LeftButton, Qt::NoModifier);
    // The existing after-sales flash-folder handler still sees the folder drag.
    auto *sales = menu.repairWindow->ougaWindow->findChild<QCheckBox *>("AfterSalesPackageModeCheckBox");
    QVERIFY(sales);
    sales->setChecked(true);
    QApplication::sendEvent(menu.repairWindow->ougaWindow, &enter);
    QVERIFY(enter.isAccepted());
    QVERIFY(!menu.deviceCheckWindow);
    QCOMPARE(pushes().size(), 0);
}

void DeviceOperationTests::droppedFilesReachSharedStorage()
{
    DeviceCheckWindow window;
    awaitAdb(window);
    QVERIFY(putFile(fixturePath + "/push-behavior.txt", "success"));
    const QStringList names = {QStringLiteral("中文 文件's;&.txt"), "error-failed.txt"};
    QList<QUrl> urls;
    for (const QString &name : names) {
        const QString path = fixturePath + '/' + name;
        QVERIFY(putFile(path, name.toUtf8()));
        urls.append(QUrl::fromLocalFile(path));
    }
    // Duplicate URLs are copied only once; local files are never removed.
    urls.append(urls.first());
    QVERIFY(dropFiles(window, urls));
    QVERIFY(window.operationInProgress);
    QCOMPARE(DeviceOperationLease::owner(), &window);
    QVERIFY(!window.executeButton->isEnabled());
    QVERIFY(window.statusLabel->text().contains("1/2"));
    QTRY_VERIFY_WITH_TIMEOUT(!window.operationInProgress, 5000);
    QTRY_COMPARE(messages, 1);
    QVERIFY(lastMessage.contains(QStringLiteral("成功：2，失败：0，未传输：0")));
    QCOMPARE(pushes().size(), 2);
    for (const QString &name : names) {
        const QString path = fixturePath + '/' + name;
        QVERIFY(pushes().contains(QJsonArray{"adb", "-s", "mock-serial", "push", path, "/sdcard/" + name}));
        QCOMPARE(readFile(fixturePath + "/device/" + name), name.toUtf8());
        QVERIFY(QFile::exists(path));
    }
    QVERIFY(window.currentProcess == nullptr);
    QVERIFY(DeviceOperationLease::owner() == nullptr);
    QVERIFY(!DeviceManager::instance()->m_isPaused);
    QVERIFY(window.executeButton->isEnabled());
}

void DeviceOperationTests::invalidDropsAreRejected_data()
{
    QTest::addColumn<QString>("reason");
    for (const QString reason : {"none", "fastboot", "lease", "serial", "directory", "remote", "missing", "mixed", "text", "move"})
        QTest::newRow(qPrintable(reason)) << reason;
}

void DeviceOperationTests::invalidDropsAreRejected()
{
    QFETCH(QString, reason);
    DeviceCheckWindow window;
    awaitAdb(window);
    const QString path = fixturePath + "/valid.txt";
    QVERIFY(putFile(path, "original"));
    QList<QUrl> urls{QUrl::fromLocalFile(path)};
    auto *manager = DeviceManager::instance();
    QObject other;
    if (reason == "none") manager->m_currentMode = DeviceManager::None;
    if (reason == "fastboot") manager->m_currentMode = DeviceManager::Fastboot;
    if (reason == "serial") manager->setDetectedDevice(DeviceManager::ADB, {});
    if (reason == "lease") QVERIFY(DeviceOperationLease::acquire(&other));
    if (reason == "directory") urls = {QUrl::fromLocalFile(fixturePath)};
    if (reason == "remote") urls = {QUrl("https://example.invalid/file.txt")};
    if (reason == "missing") urls = {QUrl::fromLocalFile(fixturePath + "/missing.txt")};
    if (reason == "mixed") urls.append(QUrl::fromLocalFile(fixturePath));
    if (reason == "text" || reason == "move") {
        QMimeData mime;
        if (reason == "text") mime.setText(path);
        else mime.setUrls(urls);
        QDragEnterEvent enter(QPoint(20, 80), reason == "move" ? Qt::MoveAction : Qt::CopyAction,
                              &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&window, &enter);
        QVERIFY(!enter.isAccepted());
    } else {
        QVERIFY(!dropFiles(window, urls));
    }
    QVERIFY(!window.operationInProgress);
    QVERIFY(window.currentProcess == nullptr);
    QCOMPARE(pushes().size(), 0);
    DeviceOperationLease::release(&other);
}

void DeviceOperationTests::dropRechecksModeAndLease()
{
    DeviceCheckWindow window;
    awaitAdb(window);
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(ResourceExtractor::getAdbPath())});
    for (bool changeMode : {true, false}) {
        QDragEnterEvent enter(QPoint(30, 80), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&window, &enter);
        QVERIFY(enter.isAccepted());
        QObject other;
        if (changeMode) DeviceManager::instance()->m_currentMode = DeviceManager::Fastboot;
        else QVERIFY(DeviceOperationLease::acquire(&other));
        QDropEvent drop(QPointF(30, 80), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&window, &drop);
        QVERIFY(!drop.isAccepted());
        QVERIFY(!window.operationInProgress);
        DeviceOperationLease::release(&other);
        DeviceManager::instance()->m_currentMode = DeviceManager::ADB;
    }
    QCOMPARE(pushes().size(), 0);
}

void DeviceOperationTests::transferFailures_data()
{
    QTest::addColumn<QByteArray>("behavior");
    QTest::newRow("nonzero") << QByteArray("fail-first");
    QTest::newRow("crash") << QByteArray("crash-first");
}

void DeviceOperationTests::transferFailures()
{
    QFETCH(QByteArray, behavior);
    DeviceCheckWindow window;
    awaitAdb(window);
    QVERIFY(putFile(fixturePath + "/push-behavior.txt", behavior));
    QList<QUrl> urls;
    for (const QString name : {"first.txt", "second.txt"}) {
        const QString path = fixturePath + '/' + name;
        QVERIFY(putFile(path, "data"));
        urls.append(QUrl::fromLocalFile(path));
    }
    QVERIFY(dropFiles(window, urls));
    QTRY_VERIFY_WITH_TIMEOUT(!window.operationInProgress, 5000);
    QTRY_COMPARE(messages, 1);
    QCOMPARE(pushes().size(), 2);
    QVERIFY(lastMessage.contains(QStringLiteral("成功：1，失败：1，未传输：0")));
    QVERIFY(lastMessage.contains("first.txt"));
    QVERIFY(QFile::exists(fixturePath + "/device/second.txt"));
    QVERIFY(DeviceOperationLease::owner() == nullptr);
}

void DeviceOperationTests::failedTransferStartReleasesOperation()
{
    DeviceCheckWindow window;
    awaitAdb(window);
    const QString path = fixturePath + "/first.txt";
    QVERIFY(putFile(path, "data"));
    adbOverride = fixturePath + "/missing-adb.exe";
    QVERIFY(dropFiles(window, {QUrl::fromLocalFile(path), QUrl::fromLocalFile(ResourceExtractor::getFastbootPath())}));
    QTRY_VERIFY(!window.operationInProgress);
    QTRY_COMPARE(messages, 1);
    // The connection preflight now fails before either push is attempted.
    QVERIFY(lastMessage.contains(QStringLiteral("成功：0，失败：0，未传输：2")));
    QVERIFY(lastMessage.contains(QStringLiteral("无法启动设备连接复核")));
    QVERIFY(pushes().isEmpty());
    QVERIFY(window.fileTransfer.files.isEmpty());
    QVERIFY(window.currentProcess == nullptr);
    QVERIFY(DeviceOperationLease::owner() == nullptr);
    QVERIFY(!DeviceManager::instance()->m_isPaused);
    adbOverride.clear();
}

void DeviceOperationTests::transferHideAndRepeatedDrop()
{
    DeviceCheckWindow window;
    awaitAdb(window);
    QVERIFY(putFile(fixturePath + "/push-behavior.txt", "success"));
    const QString path = fixturePath + "/first.txt";
    QVERIFY(putFile(path, "data"));
    window.show();
    QVERIFY(dropFiles(window, {QUrl::fromLocalFile(path)}));
    QTRY_COMPARE(window.currentProcess->state(), QProcess::Running);
    auto *process = window.currentProcess;
    QVERIFY(!dropFiles(window, {QUrl::fromLocalFile(path)}));
    window.onRebootButtonClicked();
    QCOMPARE(window.currentProcess, process);
    QVERIFY(window.close());
    QVERIFY(!window.isVisible());
    QTRY_VERIFY(!window.operationInProgress);
    QTRY_COMPARE(messages, 1);
    QCOMPARE(pushes().size(), 1);
    QVERIFY(QFile::exists(fixturePath + "/device/first.txt"));
}

void DeviceOperationTests::transferDestructionDoesNotStartNextFile()
{
    auto *window = new DeviceCheckWindow;
    awaitAdb(*window);
    QVERIFY(putFile(fixturePath + "/push-behavior.txt", "hang"));
    QVERIFY(dropFiles(*window, {QUrl::fromLocalFile(ResourceExtractor::getAdbPath()),
                              QUrl::fromLocalFile(ResourceExtractor::getFastbootPath())}));
    QTRY_COMPARE(window->currentProcess->state(), QProcess::Running);
    QTRY_COMPARE(pushes().size(), 1);
    delete window;
    QTest::qWait(200);
    QCOMPARE(pushes().size(), 1);
    QCOMPARE(messages, 0);
    QVERIFY(DeviceOperationLease::owner() == nullptr);
    QVERIFY(!DeviceManager::instance()->m_isPaused);
}

void DeviceOperationTests::transferDeviceChangeStopsBatch_data()
{
    QTest::addColumn<bool>("disconnected");
    QTest::newRow("disconnected") << true;
    QTest::newRow("different-serial") << false;
}

void DeviceOperationTests::transferDeviceChangeStopsBatch()
{
    QFETCH(bool, disconnected);
    DeviceCheckWindow window;
    awaitAdb(window);
    QVERIFY(putFile(fixturePath + "/push-behavior.txt", "success"));
    QVERIFY(dropFiles(window, {QUrl::fromLocalFile(ResourceExtractor::getAdbPath()),
                              QUrl::fromLocalFile(ResourceExtractor::getFastbootPath())}));
    QTRY_COMPARE(window.currentProcess->state(), QProcess::Running);
    if (disconnected) DeviceManager::instance()->m_currentMode = DeviceManager::None;
    else DeviceManager::instance()->setDetectedDevice(DeviceManager::ADB, "different-device");
    QTRY_VERIFY(!window.operationInProgress);
    QTRY_COMPARE(messages, 1);
    QCOMPARE(pushes().size(), 1);
    QVERIFY(lastMessage.contains(QStringLiteral("未传输：1")));
    QVERIFY(lastMessage.contains(QStringLiteral("原设备已断开或改变")));
    QVERIFY(DeviceOperationLease::owner() == nullptr);
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
    QVERIFY(audit().contains("[\"fastboot\",\"-s\",\"mock-serial\",\"flash\",\"boot\",\"boot.img\"]"));
    QVERIFY(audit().contains("[\"fastboot\",\"-s\",\"mock-serial\",\"reboot\"]"));
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
    QTRY_VERIFY(audit().contains("[\"fastboot\",\"-s\",\"mock-serial\",\"flash\",\"boot\",\"boot.img\"]"));
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
    QVERIFY(repair.beginAdbOperation());
    repair.startAdbCommand({"push", repair.apkFilesToInstall[0],
        "/data/local/tmp/My App's;test.apk"});
    QTRY_COMPARE_WITH_TIMEOUT(repair.currentApkIndex, 1, 5000);
    QCOMPARE(repair.successCount, 1);
    QCOMPARE(repair.failCount, 0);
    const QString remote = "/data/local/tmp/My App's;test.apk";
    for (const QString &cmd : {"pm install -r ", "rm -f "}) {
        const QString quoted = ShellCommand::asRoot(cmd + ShellCommand::quote(remote));
        const QByteArray json = QJsonDocument(QJsonArray{"adb", "-s", "mock-serial", "shell", quoted}).toJson(QJsonDocument::Compact);
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
    QVERIFY(repair.beginAdbOperation());
    repair.startModuleInstall();
    QTRY_COMPARE_WITH_TIMEOUT(repair.currentModuleIndex, 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(repair.buttons[RepairWindow::InstallModule]->text(), QString("安装模块"), 5000);
    QCOMPARE(repair.moduleSuccessCount, 1);
    QCOMPARE(repair.moduleFailCount, 0);
    const QString remote = "/data/local/tmp/My Module's;test.zip";
    for (const QString &cmd : {installPrefix, QString("rm -f ")}) {
        const QString quoted = ShellCommand::asRoot(cmd + ShellCommand::quote(remote));
        const QByteArray json = QJsonDocument(QJsonArray{"adb", "-s", "mock-serial", "shell", quoted}).toJson(QJsonDocument::Compact);
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
        const QByteArray serial = QFile::exists(directory + "/serial.txt")
            ? readFile(directory + "/serial.txt") : QByteArray("mock-serial");
        const bool multiple = QFile::exists(directory + "/multiple.txt");
        if (base == "adb") {
            std::puts("List of devices attached");
            if (mode == "adb") {
                std::printf("%s\tdevice\n", serial.constData());
                if (multiple) std::puts("other-device\tdevice");
            }
        } else if (mode == "fastboot") {
            std::printf("%s\tfastboot\n", serial.constData());
            if (multiple) std::puts("other-device\tfastboot");
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
    if (base == "adb" && args.value(0) == "push" && QFile::exists(directory + "/push-behavior.txt")) {
        const QByteArray behavior = readFile(directory + "/push-behavior.txt");
        if (behavior == "hang") return application.exec();
        QTimer::singleShot(200, &application, [&application, directory, args, behavior]() {
            if (QFileInfo(args.value(1)).fileName() == "first.txt") {
                if (behavior == "crash-first") std::abort();
                if (behavior == "fail-first") {
                    std::fprintf(stderr, "adb: error: simulated device transfer failure\n");
                    application.exit(1);
                    return;
                }
            }
            QDir().mkpath(directory + "/device");
            const QString target = directory + "/device/" + QFileInfo(args.value(2)).fileName();
            QFile::remove(target);
            if (!QFile::copy(args.value(1), target)) { application.exit(1); return; }
            if (behavior == "disconnect-after-first") putFile(directory + "/mode.txt", "none");
            std::puts("1 file pushed");
            application.exit(0);
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
    if (application.arguments().value(1) == "--exit-helper")
        return DeviceOperationTests::exitHelper(application, application.arguments());
    application.setQuitOnLastWindowClosed(false);
    DeviceOperationTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "deviceoperationtests.moc"
