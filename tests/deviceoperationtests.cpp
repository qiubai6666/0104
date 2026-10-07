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
#include "devicemanager.h"
#include "processmanager.h"
#include "resourceextractor.h"
#include "shellcommand.h"
#include "ougaflashwindow.h"
#include "xiaomiflashwindow.h"
#include "openlistwindow.h"
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

class DeviceOperationTests : public QObject
{
    Q_OBJECT
public:
    static int exitHelper(QApplication &application, const QStringList &arguments);
private slots:
    void initTestCase();
    void init();
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
    bool dropFiles(DeviceCheckWindow &window, const QList<QUrl> &urls);
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
    QVERIFY(repair.hasActiveOugaTask());
    QVERIFY(!first->close());
    QVERIFY(DeviceOperationLease::busyFor(&repair));
    DeviceOperationLease::release(&operation);
    QVERIFY(first->close());
}

void DeviceOperationTests::openListApplicationExit_data()
{
    QTest::addColumn<QString>("state");
    for (const QString &state : {QString("visible"), QString("minimized"), QString("hidden"), QString("busy")})
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
        menu.buttons[MenuWidget::Exit]->click(); // Exactly one click when idle.
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

void DeviceOperationTests::awaitAdb(DeviceCheckWindow &window)
{
    QVERIFY(putFile(fixturePath + "/mode.txt", "adb"));
    QTRY_COMPARE_WITH_TIMEOUT(DeviceManager::instance()->currentMode(), DeviceManager::ADB, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!DeviceManager::instance()->m_isChecking, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!DeviceManager::instance()->m_infoQueryActive, 5000);
    DeviceManager::instance()->m_checkTimer->stop();
    window.updateUIForMode(DeviceManager::ADB);
}

bool DeviceOperationTests::dropFiles(DeviceCheckWindow &window, const QList<QUrl> &urls)
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
    if (reason == "serial") manager->m_deviceSerial.clear();
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
    QVERIFY(lastMessage.contains(QStringLiteral("成功：0，失败：1，未传输：1")));
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
    else DeviceManager::instance()->m_deviceSerial = "different-device";
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
