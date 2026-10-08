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
#include "repairwindow.h"
#include "payloadwindow.h"
#include "configwindow.h"
#include "uihelper.h"
#include <QLayout>
#include <QLabel>
#include <QPainter>
#include "devicecheckwindow.h"
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
        connect(DeviceOperationLease::instance(), &DeviceOperationLease::changed, this, [](bool held) {
            if (held) DeviceManager::instance()->pauseMonitoring();
            else DeviceManager::instance()->resumeMonitoring();
        });
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
    void compactMenuAndUniformDrawers() {
        // A local empty config prevents the constructor from downloading anything.
        QVERIFY(writeFile(fixturePath + "/download_config.txt", ""));
        MenuWidget menu;
        menu.setAttribute(Qt::WA_DontShowOnScreen);
        menu.show();
        menu.layout()->activate();
        QCOMPARE(menu.width(), int(UIHelper::MainMenuWidth));
        QVERIFY(menu.height() < 390);
        for (auto *button : menu.buttons) {
            QCOMPARE(button->size(), QSize(38, 38));
            QCOMPARE(button->iconSize(), QSize(24, 24));
            QVERIFY(!button->icon().isNull());
            QVERIFY(button->text().isEmpty());
            const QString bounds = QString("menu=%1x%2 button=%3,%4 %5x%6")
                .arg(menu.width()).arg(menu.height()).arg(button->x()).arg(button->y())
                .arg(button->width()).arg(button->height());
            QVERIFY2(menu.rect().contains(button->geometry()), qPrintable(bounds));
        }
        RepairWindow repair;
        PayloadWindow payload;
        DeviceCheckWindow device;
        ConfigWindow config;
        const QList<QWidget *> drawers = { &repair, &payload, &device, &config };
        for (auto *drawer : drawers) {
            drawer->setAttribute(Qt::WA_DontShowOnScreen);
            drawer->show();
            drawer->layout()->activate();
            UIHelper::positionDrawer(drawer, menu.geometry());
            QCOMPARE(drawer->size(), QSize(296, 336));
            QVERIFY(drawer->height() < menu.height());
            QVERIFY(drawer->y() > menu.y());
            QVERIFY(drawer->geometry().bottom() < menu.geometry().bottom());
            QVERIFY2(drawer->layout()->minimumSize().height() <= drawer->height(),
                     qPrintable(drawer->windowTitle() + " layout overflows"));
            QCOMPARE(drawer->property("surfaceRadius").toInt(), 28);
            for (auto *button : drawer->findChildren<QPushButton *>()) {
                const QRect bounds(button->mapTo(drawer, QPoint()), button->size());
                QVERIFY2(drawer->rect().contains(bounds), qPrintable(button->text()));
            }
        }
        DeviceManager::instance()->stopMonitoring();
        DeviceManager::instance()->setDetectedDevice(DeviceManager::ADB, "TEST1234");
        const QString deviceInfo = QStringLiteral(
            "设备名称：测试设备\n设备代号：TEST\n安卓版本：16\nA/B 分区：a\n解锁状态：已解锁");
        QVERIFY(QMetaObject::invokeMethod(&device, "onDeviceInfoUpdated", Qt::DirectConnection,
            Q_ARG(QString, deviceInfo)));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
        device.layout()->activate();
        QCOMPARE(device.size(), QSize(296, 336));
        bool deviceInfoShown = false;
        for (auto *label : device.findChildren<QLabel *>()) {
            if (label->text() != deviceInfo) continue;
            deviceInfoShown = true;
            QVERIFY2(label->heightForWidth(label->width()) <= label->height(),
                     qPrintable(QString("Device information needs %1px but has %2px; drawer minimum %3px")
                         .arg(label->heightForWidth(label->width())).arg(label->height())
                         .arg(device.layout()->minimumSize().height())));
        }
        QVERIFY(deviceInfoShown);
        // Toggling a submenu reuses the same dimensions and still hides it.
        menu.repairWindow = new RepairWindow;
        menu.repairWindow->setAttribute(Qt::WA_DontShowOnScreen);
        menu.buttons[MenuWidget::RepairTools]->click();
        QVERIFY(menu.repairWindow->isVisible());
        QCOMPARE(menu.repairWindow->size(), QSize(296, 336));
        menu.buttons[MenuWidget::RepairTools]->click();
        QVERIFY(!menu.repairWindow->isVisible());
        const QString output = qEnvironmentVariable("COMPACT_MENU_PREVIEW");
        if (!output.isEmpty()) {
            const int menuY = 48;
            const int drawerY = menuY + (menu.height() - UIHelper::DrawerHeight) / 2;
            const int drawerStep = UIHelper::DrawerWidth + 18;
            QImage sheet(86 + drawers.size() * drawerStep, menuY + menu.height() + 20,
                         QImage::Format_ARGB32_Premultiplied);
            sheet.fill(QColor("#dce8ed"));
            QPainter painter(&sheet);
            painter.setPen(QColor("#385565"));
            painter.setFont(QFont("Microsoft YaHei", 10));
            painter.drawText(16, 26, QStringLiteral("主菜单 48 × 372px / 次菜单统一 296 × 336px"));
            menu.render(&painter, QPoint(16, menuY));
            for (int i = 0; i < drawers.size(); ++i)
                drawers[i]->render(&painter, QPoint(86 + i * drawerStep, drawerY));
            painter.end();
            QVERIFY(sheet.save(output));
        }
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
        QVERIFY(button->text().isEmpty());
        QVERIFY(!button->icon().isNull());
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
        QVERIFY(button->text().isEmpty());
        QVERIFY(!button->icon().isNull());
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
    void failedStartKeepsAutoCastAndRetries() {
        QVERIFY(QFile::rename(fixturePath + "/scrcpy.exe", fixturePath + "/missing-scrcpy.exe"));
        MenuWidget menu;
        freeze(menu);
        DeviceManager::instance()->setDetectedDevice(DeviceManager::ADB, "PHONE_A");
        auto *button = menu.buttons[MenuWidget::ScreenCast];
        button->click();
        QTRY_VERIFY(menu.screenCastController->m_retryTimer.isActive());
        QCOMPARE(menu.screenCastController->state(), ScreenCastController::WaitingForDevice);
        QVERIFY(menu.screenCastController->m_autoCastEnabled);
        QVERIFY(button->text().isEmpty());
        QVERIFY(button->isEnabled());
        QVERIFY(menu.screenCastSpinnerTimer->isActive());
        QVERIFY(button->toolTip().contains(QStringLiteral("无法启动")));
        QVERIFY(QFile::rename(fixturePath + "/missing-scrcpy.exe", fixturePath + "/scrcpy.exe"));
        QTRY_COMPARE(menu.screenCastController->state(), ScreenCastController::Streaming);
    }
    void processExitAutomaticallyRestarts_data() {
        QTest::addColumn<QByteArray>("behavior");
        QTest::newRow("window-closed") << QByteArray("close");
        QTest::newRow("start-failed") << QByteArray("fail");
        QTest::newRow("crashed") << QByteArray("crash");
    }
    void processExitAutomaticallyRestarts() {
        QFETCH(QByteArray, behavior);
        QVERIFY(writeFile(fixturePath + "/behavior.txt", behavior));
        MenuWidget menu;
        freeze(menu);
        DeviceManager::instance()->setDetectedDevice(DeviceManager::ADB, "PHONE_A");
        menu.buttons[MenuWidget::ScreenCast]->click();
        QTRY_COMPARE(menu.screenCastController->state(), ScreenCastController::Streaming);
        QTRY_VERIFY(menu.screenCastController->m_retryTimer.isActive());
        QCOMPARE(menu.screenCastController->state(), ScreenCastController::WaitingForDevice);
        QVERIFY(menu.screenCastController->m_autoCastEnabled);
        QVERIFY(menu.buttons[MenuWidget::ScreenCast]->text().isEmpty());
        QVERIFY(menu.screenCastSpinnerTimer->isActive());
        QCOMPARE(scrcpyCommands().size(), 1);
        QVERIFY(writeFile(fixturePath + "/behavior.txt", ""));
        // Information refresh must not bypass the backoff or launch duplicates.
        for (int i = 0; i < 5; ++i) menu.screenCastController->checkDevice();
        QTest::qWait(100);
        QCOMPARE(scrcpyCommands().size(), 1);
        QTRY_COMPARE(menu.screenCastController->state(), ScreenCastController::Streaming);
        QTRY_COMPARE(scrcpyCommands().size(), 2);
        menu.buttons[MenuWidget::ScreenCast]->click();
        QTRY_COMPARE(menu.screenCastController->state(), ScreenCastController::Idle);
        QVERIFY(!menu.screenCastController->m_autoCastEnabled);
        QVERIFY(!menu.screenCastController->m_retryTimer.isActive());
    }
    void disconnectAndReplacementReconnectCurrentSession_data() {
        QTest::addColumn<int>("mode"); QTest::addColumn<QString>("serial");
        QTest::newRow("disconnect") << int(DeviceManager::None) << QString();
        QTest::newRow("replacement") << int(DeviceManager::ADB) << QString("PHONE_B");
        QTest::newRow("mode-change") << int(DeviceManager::Fastboot) << QString("PHONE_A");
    }
    void disconnectAndReplacementReconnectCurrentSession() {
        QFETCH(int, mode); QFETCH(QString, serial);
        ScreenCastController controller;
        DeviceManager::instance()->setDetectedDevice(DeviceManager::ADB, "PHONE_A");
        controller.toggle();
        QTRY_COMPARE(controller.state(), ScreenCastController::Streaming);
        QTRY_COMPARE(scrcpyCommands().size(), 1);
        DeviceManager::instance()->setDetectedDevice(static_cast<DeviceManager::DeviceMode>(mode), serial);
        QTRY_COMPARE(controller.state(), ScreenCastController::WaitingForDevice);
        QVERIFY(controller.m_autoCastEnabled);
        QVERIFY(!controller.m_process);
        QTest::qWait(80);
        QCOMPARE(scrcpyCommands().size(), 1);
        const QString returningSerial = mode == int(DeviceManager::ADB) ? serial : QString("PHONE_A");
        DeviceManager::instance()->setDetectedDevice(DeviceManager::ADB, returningSerial);
        QTRY_COMPARE(controller.state(), ScreenCastController::Streaming);
        QTRY_COMPARE(scrcpyCommands().size(), 2);
        QVERIFY(scrcpyCommands().contains(QJsonArray{"scrcpy", "--max-size", "1024",
            "--video-bit-rate", "4M", "--serial", returningSerial}));
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
    void startupTimeoutAutomaticallyRetries() {
        ScreenCastController controller;
        DeviceManager::instance()->setDetectedDevice(DeviceManager::ADB, "PHONE_A");
        // Suppress only the fake process's start notification to emulate an
        // uncompleted OS launch; the real timeout path must kill this process.
        bool suppressFirstStart = true;
        connect(&controller, &ScreenCastController::stateChanged, this, [&] {
            if (suppressFirstStart && controller.state() == ScreenCastController::Starting) {
                suppressFirstStart = false;
                QObject::disconnect(controller.m_process, &QProcess::started, &controller, nullptr);
            }
        });
        controller.m_startTimer.setInterval(40);
        controller.toggle(); controller.tryStart();
        QCOMPARE(controller.state(), ScreenCastController::Starting);
        QTRY_COMPARE(controller.state(), ScreenCastController::WaitingForDevice);
        QVERIFY(controller.m_autoCastEnabled);
        QVERIFY(controller.statusText().contains(QStringLiteral("超时")));
        QTRY_COMPARE(controller.state(), ScreenCastController::Streaming);
    }
    void cancelRetryCannotRestartLater() {
        QVERIFY(QFile::rename(fixturePath + "/scrcpy.exe", fixturePath + "/missing-scrcpy.exe"));
        MenuWidget menu;
        freeze(menu);
        DeviceManager::instance()->setDetectedDevice(DeviceManager::ADB, "PHONE_A");
        auto *controller = menu.screenCastController;
        controller->m_retryTimer.setInterval(120);
        menu.buttons[MenuWidget::ScreenCast]->click();
        QTRY_VERIFY(controller->m_retryTimer.isActive());
        menu.buttons[MenuWidget::ScreenCast]->click();
        QCOMPARE(controller->state(), ScreenCastController::Idle);
        QVERIFY(!controller->m_autoCastEnabled);
        QVERIFY(!controller->m_retryTimer.isActive());
        QVERIFY(QFile::rename(fixturePath + "/missing-scrcpy.exe", fixturePath + "/scrcpy.exe"));
        DeviceManager::instance()->setDetectedDevice(DeviceManager::None, {});
        DeviceManager::instance()->setDetectedDevice(DeviceManager::ADB, "PHONE_A");
        QTest::qWait(300);
        QVERIFY(scrcpyCommands().isEmpty());
        QCOMPARE(controller->state(), ScreenCastController::Idle);
    }
    void manualCancelWhileDisconnectIsStopping() {
        MenuWidget menu;
        freeze(menu);
        DeviceManager::instance()->setDetectedDevice(DeviceManager::ADB, "PHONE_A");
        auto *controller = menu.screenCastController;
        controller->m_retryTimer.setInterval(80);
        menu.buttons[MenuWidget::ScreenCast]->click();
        QTRY_COMPARE(controller->state(), ScreenCastController::Streaming);
        QTRY_COMPARE(scrcpyCommands().size(), 1);
        bool canceled = false;
        connect(controller, &ScreenCastController::stateChanged, this, [&] {
            if (!canceled && controller->state() == ScreenCastController::Stopping) {
                canceled = true;
                QVERIFY(menu.buttons[MenuWidget::ScreenCast]->isEnabled());
                menu.buttons[MenuWidget::ScreenCast]->click();
            }
        });
        DeviceManager::instance()->setDetectedDevice(DeviceManager::None, {});
        QTRY_COMPARE(controller->state(), ScreenCastController::Idle);
        QVERIFY(canceled);
        QVERIFY(!controller->m_autoCastEnabled);
        DeviceManager::instance()->setDetectedDevice(DeviceManager::ADB, "PHONE_A");
        QTest::qWait(200);
        QCOMPARE(scrcpyCommands().size(), 1);
        QCOMPARE(controller->state(), ScreenCastController::Idle);
    }
    void repeatedRebootsReconnectUntilExplicitCancel() {
        ScreenCastController controller;
        controller.m_retryTimer.setInterval(80);
        auto *manager = DeviceManager::instance();
        manager->setDetectedDevice(DeviceManager::ADB, "PHONE_A");
        controller.toggle();
        QTRY_COMPARE(controller.state(), ScreenCastController::Streaming);
        QTRY_COMPARE(scrcpyCommands().size(), 1);
        for (int reboot = 0; reboot < 3; ++reboot) {
            manager->setDetectedDevice(DeviceManager::None, {});
            QTRY_COMPARE(controller.state(), ScreenCastController::WaitingForDevice);
            QVERIFY(controller.m_autoCastEnabled);
            manager->setDetectedDevice(DeviceManager::ADB, "PHONE_A");
            QTRY_COMPARE(controller.state(), ScreenCastController::Streaming);
            QTRY_COMPARE(scrcpyCommands().size(), reboot + 2);
        }
        controller.toggle();
        QTRY_COMPARE(controller.state(), ScreenCastController::Idle);
        manager->setDetectedDevice(DeviceManager::None, {});
        manager->setDetectedDevice(DeviceManager::ADB, "PHONE_A");
        QTest::qWait(250);
        QCOMPARE(scrcpyCommands().size(), 4);
        QCOMPARE(controller.state(), ScreenCastController::Idle);
    }
    void retryRespectsLeaseAndCancellation() {
        QVERIFY(writeFile(fixturePath + "/behavior.txt", "close"));
        ScreenCastController controller;
        controller.m_retryTimer.setInterval(80);
        DeviceManager::instance()->setDetectedDevice(DeviceManager::ADB, "PHONE_A");
        controller.toggle();
        QTRY_COMPARE(controller.state(), ScreenCastController::Streaming);
        QTRY_VERIFY(controller.m_retryTimer.isActive());
        QObject operation;
        QVERIFY(DeviceOperationLease::acquire(&operation));
        QVERIFY(writeFile(fixturePath + "/behavior.txt", ""));
        QTest::qWait(180);
        QCOMPARE(controller.state(), ScreenCastController::WaitingForDevice);
        QCOMPARE(scrcpyCommands().size(), 1);
        DeviceOperationLease::release(&operation);
        QTRY_COMPARE(controller.state(), ScreenCastController::Streaming);
        QTRY_COMPARE(scrcpyCommands().size(), 2);
        controller.toggle();
        QTRY_COMPARE(controller.state(), ScreenCastController::Idle);
        QVERIFY(DeviceOperationLease::acquire(&operation));
        controller.toggle();
        QCOMPARE(controller.state(), ScreenCastController::WaitingForDevice);
        controller.toggle();
        QCOMPARE(controller.state(), ScreenCastController::Idle);
        DeviceOperationLease::release(&operation);
        QTest::qWait(180);
        QCOMPARE(scrcpyCommands().size(), 2);
    }
    void rebootFromDeviceCheckReconnectsWithRealPolling() {
        QVERIFY(writeFile(fixturePath + "/devices.txt", "PHONE_A\tdevice\n"));
        MenuWidget menu;
        auto *manager = DeviceManager::instance();
        manager->m_checkTimer->setInterval(80);
        auto *controller = menu.screenCastController;
        controller->m_retryTimer.setInterval(120);
        menu.buttons[MenuWidget::ScreenCast]->click();
        QTRY_COMPARE(controller->state(), ScreenCastController::Streaming);
        DeviceCheckWindow deviceWindow;
        // Drain the current query without accelerated polling immediately restarting it.
        // Resume real polling after reboot releases its device-operation lease.
        manager->m_checkTimer->stop();
        QTRY_VERIFY_WITH_TIMEOUT(!manager->m_isChecking && !manager->m_infoQueryActive, 10000);
        QVERIFY(QMetaObject::invokeMethod(&deviceWindow, "onRebootButtonClicked", Qt::DirectConnection));
        QTRY_VERIFY(QFile::exists(fixturePath + "/reboot.txt"));
        QTRY_VERIFY(!deviceWindow.isOperationInProgress());
        manager->m_checkTimer->start(80);
        QTRY_COMPARE(manager->currentMode(), DeviceManager::None);
        QTRY_COMPARE(controller->state(), ScreenCastController::WaitingForDevice);
        QVERIFY(controller->m_autoCastEnabled);
        QVERIFY(menu.screenCastSpinnerTimer->isActive());
        QVERIFY(writeFile(fixturePath + "/devices.txt", "PHONE_A\tunauthorized\n"));
        QTest::qWait(200);
        QCOMPARE(controller->state(), ScreenCastController::WaitingForDevice);
        QCOMPARE(scrcpyCommands().size(), 1);
        QVERIFY(writeFile(fixturePath + "/devices.txt", "PHONE_A\tdevice\n"));
        QTRY_COMPARE(controller->state(), ScreenCastController::Streaming);
        QTRY_COMPARE(scrcpyCommands().size(), 2);
        menu.buttons[MenuWidget::ScreenCast]->click();
        QTRY_COMPARE(controller->state(), ScreenCastController::Idle);
        QVERIFY(writeFile(fixturePath + "/devices.txt", ""));
        QTRY_COMPARE(manager->currentMode(), DeviceManager::None);
        QVERIFY(writeFile(fixturePath + "/devices.txt", "PHONE_A\tdevice\n"));
        QTRY_COMPARE(manager->currentMode(), DeviceManager::ADB);
        QTest::qWait(250);
        QCOMPARE(scrcpyCommands().size(), 2);
    }
    void rapidReconnectDuringStopStartsOnlyOneReplacement() {
        ScreenCastController controller;
        controller.m_retryTimer.setInterval(100);
        auto *manager = DeviceManager::instance();
        manager->setDetectedDevice(DeviceManager::ADB, "PHONE_A");
        controller.toggle();
        QTRY_COMPARE(controller.state(), ScreenCastController::Streaming);
        QTRY_COMPARE(scrcpyCommands().size(), 1);
        // Both events can arrive before the old process reports finished.
        manager->setDetectedDevice(DeviceManager::None, {});
        manager->setDetectedDevice(DeviceManager::ADB, "PHONE_A");
        QTRY_COMPARE(controller.state(), ScreenCastController::Streaming);
        QTRY_COMPARE(scrcpyCommands().size(), 2);
        QTest::qWait(250);
        QCOMPARE(scrcpyCommands().size(), 2);
    }
    void destroyingPendingRetryCannotStartLater() {
        QVERIFY(QFile::rename(fixturePath + "/scrcpy.exe", fixturePath + "/missing-scrcpy.exe"));
        auto controller = std::unique_ptr<ScreenCastController>(new ScreenCastController);
        controller->m_retryTimer.setInterval(150);
        DeviceManager::instance()->setDetectedDevice(DeviceManager::ADB, "PHONE_A");
        controller->toggle();
        QTRY_VERIFY(controller->m_retryTimer.isActive());
        controller.reset();
        QVERIFY(QFile::rename(fixturePath + "/missing-scrcpy.exe", fixturePath + "/scrcpy.exe"));
        QTest::qWait(350);
        QVERIFY(scrcpyCommands().isEmpty());
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
        if (base == "fastboot") return 0;
        std::fputs("List of devices attached\n", stdout);
        const auto devices = readFile(directory + "/devices.txt");
        std::fwrite(devices.constData(), 1, devices.size(), stdout); return 0;
    }
    if (args.value(0) == "-s") { args.removeFirst(); args.removeFirst(); }
    if (args.value(0) == "reboot") {
        writeFile(directory + "/devices.txt", "");
        writeFile(directory + "/reboot.txt", "yes");
        return 0;
    }
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
