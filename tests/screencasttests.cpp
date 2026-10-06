#include <QtTest>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QUuid>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include "menuwidget.h"
#include "screencastcontroller.h"
#include "devicemanager.h"
#include "deviceoperationlease.h"
#include "processmanager.h"
#include "resourceextractor.h"

namespace {
QString fixturePath;
QByteArray readFile(const QString &path) {
    QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
bool writeFile(const QString &path, const QByteArray &value) {
    QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(value) == value.size();
}
}
QString ResourceExtractor::getResourcePath() { return fixturePath; }
QString ResourceExtractor::getAdbPath() { return fixturePath + "/adb.exe"; }
QString ResourceExtractor::getFastbootPath() { return fixturePath + "/fastboot.exe"; }
QString ResourceExtractor::getNeilImagePath() { return fixturePath + "/Neil.jpg"; }

class ScreenCastTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("OrangeToolsTests");
        QCoreApplication::setApplicationName("ScreenCastTests");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, QDir::tempPath() + "/settings");
        DeviceOperationLease::setIdleCheck([] { return !ProcessManager::hasActiveDeviceProcesses(); });
    }
    void init() {
        fixture.reset(new QTemporaryDir(QDir::tempPath() + "/ScreenCastTests-XXXXXX"));
        QVERIFY(fixture->isValid());
        fixture->setAutoRemove(false);
        fixturePath = fixture->path();
        for (const QString name : {"adb", "fastboot", "scrcpy"})
            QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), fixturePath + "/" + name + ".exe"));
        QVERIFY(writeFile(fixturePath + "/devices.txt", ""));
        auto *manager = DeviceManager::instance();
        manager->stopMonitoring();
        manager->setDetectedDevice(DeviceManager::None, {});
    }
    void cleanup() {
        QVERIFY(!DeviceOperationLease::owner());
        DeviceManager::instance()->stopMonitoring();
        ProcessManager::stopAllProcesses();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCOMPARE(DeviceManager::instance()->m_adbOnlyRefCount, 0);
        QCOMPARE(DeviceManager::instance()->m_fullModeRefCount, 0);
        fixture.reset();
    }
    void waitingSpinnerAndCancel() {
        MenuWidget menu;
        const auto topLevels = QApplication::topLevelWidgets().size();
        auto *button = menu.buttons[MenuWidget::ScreenCast];
        button->click();
        QCOMPARE(menu.screenCastController->state(), ScreenCastController::WaitingForDevice);
        QVERIFY(button->text().isEmpty());
        QVERIFY(!button->icon().isNull());
        QVERIFY(button->isEnabled());
        QVERIFY(menu.screenCastSpinnerTimer->isActive());
        const auto firstFrame = button->icon().pixmap(24, 24).toImage();
        QTest::qWait(90);
        QVERIFY(firstFrame != button->icon().pixmap(24, 24).toImage());
        QCOMPARE(QApplication::topLevelWidgets().size(), topLevels);
        QVERIFY(scrcpyCommands().isEmpty());
        button->click();
        QCOMPARE(menu.screenCastController->state(), ScreenCastController::Idle);
        QCOMPARE(button->text(), QStringLiteral("投屏"));
        QVERIFY(button->icon().isNull());
        QVERIFY(!menu.screenCastSpinnerTimer->isActive());
        QCOMPARE(button->accessibleName(), QStringLiteral("投屏"));
    }
    void authorizedAdbStartsAndSecondClickStops() {
        QVERIFY(writeFile(fixturePath + "/devices.txt", "PHONE_A\tdevice\n"));
        MenuWidget menu;
        auto *button = menu.buttons[MenuWidget::ScreenCast];
        button->click();
        QTRY_COMPARE(menu.screenCastController->state(), ScreenCastController::Streaming);
        QTRY_COMPARE(scrcpyCommands().size(), 1);
        QCOMPARE(scrcpyCommands().first(), QJsonArray({"scrcpy", "--max-size", "1024", "--video-bit-rate", "4M", "--serial", "PHONE_A"}));
        QCOMPARE(button->text(), QStringLiteral("投屏"));
        QVERIFY(button->icon().isNull());
        QVERIFY(!menu.screenCastSpinnerTimer->isActive());
        QVERIFY(!ProcessManager::hasActiveDeviceProcesses());
        button->click();
        QTRY_COMPARE(menu.screenCastController->state(), ScreenCastController::Idle);
        QTest::qWait(250);
        QCOMPARE(scrcpyCommands().size(), 1); // Detection updates cannot restart a cancelled session.
    }
    void delayedAuthorization() {
        QVERIFY(writeFile(fixturePath + "/devices.txt", "PHONE_A\tunauthorized\nPHONE_B\toffline\n"));
        MenuWidget menu;
        menu.buttons[MenuWidget::ScreenCast]->click();
        QTest::qWait(1200);
        QCOMPARE(menu.screenCastController->state(), ScreenCastController::WaitingForDevice);
        QVERIFY(scrcpyCommands().isEmpty());
        QVERIFY(writeFile(fixturePath + "/devices.txt", "PHONE_A\tdevice\n"));
        QTRY_COMPARE_WITH_TIMEOUT(menu.screenCastController->state(), ScreenCastController::Streaming, 4000);
    }
    void fastbootDoesNotStart() {
        ScreenCastController controller;
        DeviceManager::instance()->setDetectedDevice(DeviceManager::Fastboot, "PHONE_A");
        controller.toggle();
        QTest::qWait(80);
        QCOMPARE(controller.state(), ScreenCastController::WaitingForDevice);
        QVERIFY(scrcpyCommands().isEmpty());
        DeviceManager::instance()->setDetectedDevice(DeviceManager::ADB, "PHONE_A");
        QTRY_COMPARE(controller.state(), ScreenCastController::Streaming);
    }
    void canceledWaitCannotStartLater() {
        ScreenCastController controller;
        controller.toggle(); controller.toggle();
        DeviceManager::instance()->setDetectedDevice(DeviceManager::ADB, "PHONE_A");
        QTest::qWait(100);
        QCOMPARE(controller.state(), ScreenCastController::Idle);
        QVERIFY(scrcpyCommands().isEmpty());
    }
    void failedStartRestoresButtonAndAllowsRetry() {
        QVERIFY(QFile::rename(fixturePath + "/scrcpy.exe", fixturePath + "/missing-scrcpy.exe"));
        MenuWidget menu;
        freeze(menu);
        DeviceManager::instance()->setDetectedDevice(DeviceManager::ADB, "PHONE_A");
        auto *button = menu.buttons[MenuWidget::ScreenCast];
        button->click();
        QTRY_COMPARE(menu.screenCastController->state(), ScreenCastController::Idle);
        QCOMPARE(button->text(), QStringLiteral("投屏"));
        QVERIFY(button->toolTip().contains(QStringLiteral("无法启动")));
        QVERIFY(QFile::rename(fixturePath + "/missing-scrcpy.exe", fixturePath + "/scrcpy.exe"));
        button->click();
        QTRY_COMPARE(menu.screenCastController->state(), ScreenCastController::Streaming);
    }
    void processExitRestoresButton_data() {
        QTest::addColumn<QByteArray>("behavior");
        QTest::newRow("window-closed") << QByteArray("close");
        QTest::newRow("start-failed") << QByteArray("fail");
        QTest::newRow("crashed") << QByteArray("crash");
    }
    void processExitRestoresButton() {
        QFETCH(QByteArray, behavior);
        QVERIFY(writeFile(fixturePath + "/behavior.txt", behavior));
        MenuWidget menu;
        freeze(menu);
        DeviceManager::instance()->setDetectedDevice(DeviceManager::ADB, "PHONE_A");
        menu.buttons[MenuWidget::ScreenCast]->click();
        QTRY_COMPARE(menu.screenCastController->state(), ScreenCastController::Streaming);
        QTRY_COMPARE(menu.screenCastController->state(), ScreenCastController::Idle);
        QCOMPARE(menu.buttons[MenuWidget::ScreenCast]->text(), QStringLiteral("投屏"));
        QVERIFY(!menu.screenCastSpinnerTimer->isActive());
        QCOMPARE(scrcpyCommands().size(), 1);
    }
    void disconnectAndReplacementStopOnlyCurrentSession_data() {
        QTest::addColumn<int>("mode"); QTest::addColumn<QString>("serial");
        QTest::newRow("disconnect") << int(DeviceManager::None) << QString();
        QTest::newRow("replacement") << int(DeviceManager::ADB) << QString("PHONE_B");
        QTest::newRow("mode-change") << int(DeviceManager::Fastboot) << QString("PHONE_A");
    }
    void disconnectAndReplacementStopOnlyCurrentSession() {
        QFETCH(int, mode); QFETCH(QString, serial);
        ScreenCastController controller;
        DeviceManager::instance()->setDetectedDevice(DeviceManager::ADB, "PHONE_A");
        controller.toggle();
        QTRY_COMPARE(controller.state(), ScreenCastController::Streaming);
        QTRY_COMPARE(scrcpyCommands().size(), 1);
        DeviceManager::instance()->setDetectedDevice(static_cast<DeviceManager::DeviceMode>(mode), serial);
        QTRY_COMPARE(controller.state(), ScreenCastController::Idle);
        QTest::qWait(80);
        QCOMPARE(scrcpyCommands().size(), 1);
    }
    void leaseDefersStartAndDoesNotBlockStop() {
        ScreenCastController controller;
        QObject operation;
        QVERIFY(DeviceOperationLease::acquire(&operation));
        DeviceManager::instance()->setDetectedDevice(DeviceManager::ADB, "PHONE_A");
        controller.toggle();
        QTest::qWait(80);
        QCOMPARE(controller.state(), ScreenCastController::WaitingForDevice);
        QVERIFY(scrcpyCommands().isEmpty());
        DeviceOperationLease::release(&operation);
        QTRY_COMPARE(controller.state(), ScreenCastController::Streaming);
        QVERIFY(DeviceOperationLease::acquire(&operation));
        controller.toggle();
        QTRY_COMPARE(controller.state(), ScreenCastController::Idle);
        QCOMPARE(DeviceOperationLease::owner(), &operation);
        DeviceOperationLease::release(&operation);
    }
    void cancelDuringProcessStart() {
        ScreenCastController controller;
        DeviceManager::instance()->setDetectedDevice(DeviceManager::ADB, "PHONE_A");
        // Windows may emit started synchronously from start(). Cancel when the
        // controller announces Starting, including the pre-launch interval.
        bool canceled = false;
        connect(&controller, &ScreenCastController::stateChanged, this, [&] {
            if (controller.state() == ScreenCastController::Starting) {
                canceled = true; controller.toggle();
            }
        });
        controller.toggle(); controller.tryStart();
        QVERIFY(canceled);
        QTRY_COMPARE(controller.state(), ScreenCastController::Idle);
        QVERIFY(!controller.m_process);
        QVERIFY(scrcpyCommands().isEmpty());
    }
    void startupTimeoutRestoresButton() {
        ScreenCastController controller;
        DeviceManager::instance()->setDetectedDevice(DeviceManager::ADB, "PHONE_A");
        // Suppress only the fake process's start notification to emulate an
        // uncompleted OS launch; the real timeout path must kill this process.
        connect(&controller, &ScreenCastController::stateChanged, this, [&] {
            if (controller.state() == ScreenCastController::Starting)
                QObject::disconnect(controller.m_process, &QProcess::started, &controller, nullptr);
        });
        controller.m_startTimer.setInterval(40);
        controller.toggle(); controller.tryStart();
        QCOMPARE(controller.state(), ScreenCastController::Starting);
        QTRY_COMPARE(controller.state(), ScreenCastController::Idle);
        QVERIFY(controller.statusText().contains(QStringLiteral("超时")));
    }
    void destroyingMenuStopsItsSessionOnly() {
        auto menu = std::unique_ptr<MenuWidget>(new MenuWidget);
        freeze(*menu);
        DeviceManager::instance()->setDetectedDevice(DeviceManager::ADB, "PHONE_A");
        menu->buttons[MenuWidget::ScreenCast]->click();
        QTRY_COMPARE(menu->screenCastController->state(), ScreenCastController::Streaming);
        QPointer<QProcess> owned = menu->screenCastController->m_process;
        QProcess unrelated;
        unrelated.start(fixturePath + "/scrcpy.exe", {"--serial", "OTHER"});
        QVERIFY(unrelated.waitForStarted());
        menu.reset();
        QVERIFY(owned.isNull());
        QCOMPARE(unrelated.state(), QProcess::Running);
        unrelated.kill(); QVERIFY(unrelated.waitForFinished());
    }
    void uiSnapshots() {
        const QString output = qEnvironmentVariable("ORANGE_UI_SNAPSHOT_DIR");
        if (output.isEmpty()) QSKIP("No snapshot directory requested");
        MenuWidget menu;
        freeze(menu);
        menu.show(); QTest::qWait(80);
        QVERIFY(menu.grab().save(output + "/idle.png"));
        menu.buttons[MenuWidget::ScreenCast]->click(); QTest::qWait(80);
        QVERIFY(menu.grab().save(output + "/waiting.png"));
        DeviceManager::instance()->setDetectedDevice(DeviceManager::ADB, "PHONE_A");
        QTRY_COMPARE(menu.screenCastController->state(), ScreenCastController::Streaming);
        QVERIFY(menu.grab().save(output + "/streaming.png"));
    }
private:
    void freeze(MenuWidget &) { DeviceManager::instance()->stopMonitoring(); }
    QList<QJsonArray> scrcpyCommands() const {
        QList<QJsonArray> commands;
        for (const QFileInfo &file : QDir(fixturePath).entryInfoList({"command-*.json"}, QDir::Files)) {
            auto array = QJsonDocument::fromJson(readFile(file.absoluteFilePath())).array();
            if (!array.isEmpty() && array.at(0).toString() == "scrcpy") commands.append(array);
        }
        return commands;
    }
    std::unique_ptr<QTemporaryDir> fixture;
};

// Fake tool executables never access a phone, network or shared ADB server.
int helperMain(QCoreApplication &application) {
    const QString base = QFileInfo(application.applicationFilePath()).baseName();
    const QString directory = application.applicationDirPath();
    QJsonArray command; command.append(base);
    auto args = application.arguments().mid(1);
    for (const auto &arg : args) command.append(arg);
    QFile audit(directory + "/command-" + QUuid::createUuid().toString(QUuid::Id128) + ".json");
    if (!audit.open(QIODevice::WriteOnly)) return 90;
    audit.write(QJsonDocument(command).toJson(QJsonDocument::Compact)); audit.close();
    if (base == "scrcpy") {
        const auto behavior = readFile(directory + "/behavior.txt").trimmed();
        if (behavior == "close" || behavior == "fail") {
            QTimer::singleShot(300, &application, [&application, behavior] { application.exit(behavior == "fail" ? 1 : 0); });
        } else if (behavior == "crash") QTimer::singleShot(300, &application, [] { std::abort(); });
        return application.exec();
    }
    if (args.value(0) == "devices") {
        std::fputs("List of devices attached\n", stdout);
        const auto devices = readFile(directory + "/devices.txt");
        std::fwrite(devices.constData(), 1, devices.size(), stdout); return 0;
    }
    if (args.value(0) == "-s") { args.removeFirst(); args.removeFirst(); }
    if (args.value(0) == "shell") {
        const QString property = args.last();
        std::puts(property.contains("slot") ? "_a" : property.contains("bootstate") ? "orange" : "mock");
    }
    return 0;
}
int main(int argc, char **argv) {
    const QString base = QFileInfo(QString::fromLocal8Bit(argv[0])).baseName();
    if (base == "adb" || base == "fastboot" || base == "scrcpy") {
        QCoreApplication application(argc, argv); return helperMain(application);
    }
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    ScreenCastTests tests; return QTest::qExec(&tests, argc, argv);
}
#include "screencasttests.moc"
