#include <QtTest>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
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
#include <QFocusEvent>
#include "repairwindow.h"
#include "payloadwindow.h"
#include "configwindow.h"
#include "uihelper.h"
#include <QLayout>
#include <QLabel>
#include <QPainter>
#include <QProxyStyle>
#include <QStyleFactory>
#include "devicecheckwindow.h"
#include "devicepicker.h"
#include "elidedlabel.h"
#include "screencastcontroller.h"
#include "screencastnavbar.h"
#include "devicemanager.h"
#include "deviceoperationlease.h"
#include "processmanager.h"
#include "resourceextractor.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <dwmapi.h>
#endif

namespace {
QString fixturePath;
QByteArray readFile(const QString &path) {
    QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
bool writeFile(const QString &path, const QByteArray &value) {
    QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(value) == value.size();
}
class PolishCounter : public QProxyStyle {
public:
    PolishCounter() : QProxyStyle(QStyleFactory::create("Fusion")) {}
    void polish(QWidget *widget) override { ++polishes; QProxyStyle::polish(widget); }
    void unpolish(QWidget *widget) override { ++unpolishes; QProxyStyle::unpolish(widget); }
    int polishes = 0;
    int unpolishes = 0;
};
class UpdateCounter : public QObject {
public:
    int updates = 0;
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::UpdateRequest) ++updates;
        return false;
    }
};
#ifdef Q_OS_WIN
HWND windowByTitle(DWORD pid, const wchar_t *wanted)
{
    struct Context { DWORD pid; const wchar_t *wanted; HWND found; } context{pid, wanted, nullptr};
    EnumWindows([](HWND hwnd, LPARAM lparam) -> BOOL {
        auto *context = reinterpret_cast<Context *>(lparam);
        DWORD windowPid = 0;
        GetWindowThreadProcessId(hwnd, &windowPid);
        if (windowPid != context->pid || !IsWindowVisible(hwnd)) return TRUE;
        wchar_t title[256]{};
        GetWindowTextW(hwnd, title, 255);
        if (wcscmp(title, context->wanted) == 0) {
            context->found = hwnd;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&context));
    return context.found;
}
QRect visibleFrame(HWND hwnd)
{
    RECT rc{};
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rc, sizeof(rc))))
        return QRect(rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top);
    if (GetWindowRect(hwnd, &rc))
        return QRect(rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top);
    return QRect();
}
#endif
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
    void unrelatedPropertiesDoNotRepolishMenu() {
        QVERIFY(writeFile(fixturePath + "/download_config.txt", ""));
        MenuWidget menu;
        freeze(menu);
        PolishCounter style;
        QPushButton child(&menu);
        child.setStyle(&style);
        child.setStyleSheet("QPushButton { color: #22364a; font-size: 13px; }");
        child.ensurePolished();
        style.polishes = style.unpolishes = 0;
        // Metadata and Qt's internal stylesheet properties are not UI state.
        child.setProperty("operationProgress", 1);
        child.setProperty("operationProgress", 2);
        child.setProperty("_q_testStyleMetadata", true);
        QCOMPARE(style.polishes, 0);
        QCOMPARE(style.unpolishes, 0);
    }
    void statusPillRefreshesWithoutRecursivePolish() {
        QVERIFY(writeFile(fixturePath + "/download_config.txt", ""));
        MenuWidget menu;
        freeze(menu);
        PolishCounter style;
        QLabel pill(&menu);
        pill.setStyle(&style);
        pill.setObjectName("statusPill");
        pill.setProperty("state", "off");
        pill.setStyleSheet("QLabel { font-size: 13px; }"
                          "QLabel[state=\"off\"] { color: #9aa8b5; }"
                          "QLabel[state=\"ok\"] { color: #22c55e; }");
        pill.ensurePolished();
        style.polishes = style.unpolishes = 0;
        pill.setProperty("state", "ok");
        QCOMPARE(pill.palette().color(QPalette::WindowText), QColor("#22c55e"));
        QCOMPARE(style.polishes, 1);
        QCOMPARE(style.unpolishes, 1);
        pill.setProperty("state", "off");
        QCOMPARE(pill.palette().color(QPalette::WindowText), QColor("#9aa8b5"));
        QCOMPARE(style.polishes, 2);
        QCOMPARE(style.unpolishes, 2);
    }
    void idleMenuStopsRepainting() {
        QVERIFY(writeFile(fixturePath + "/download_config.txt", ""));
        MenuWidget menu;
        freeze(menu);
        menu.setAttribute(Qt::WA_DontShowOnScreen);
        menu.show();
        QTest::qWait(200);
        UpdateCounter counter;
        menu.installEventFilter(&counter);
        QTest::qWait(300);
        qInfo() << "Idle panel update requests in 300 ms:" << counter.updates;
        QVERIFY2(counter.updates <= 2, "Idle panel continuously requests repaints");
        QVERIFY(!menu.screenCastSpinnerTimer->isActive());
    }
    void deviceTargetPickerSelectsAndCloses() {
        QVERIFY(writeFile(fixturePath + "/download_config.txt", ""));
        MenuWidget menu;
        freeze(menu);
        menu.setAttribute(Qt::WA_DontShowOnScreen);
        menu.show();
        menu.layout()->activate();
        auto *manager = DeviceManager::instance();
        manager->setDetectedEntries(DeviceManager::ADB, {
            {QStringLiteral("serial-a"), QStringLiteral("Phone A"), QStringLiteral("device")},
            {QStringLiteral("serial-b"), QStringLiteral("Phone B"), QStringLiteral("device")}});
        QCOMPARE(manager->deviceSerial(), QStringLiteral("serial-a"));
        QCOMPARE(menu.deviceSerialLabel->text(), QStringLiteral("serial-a"));
        QCOMPARE(menu.deviceNameLabel->text(), QStringLiteral("Phone A"));
        QCOMPARE(menu.devicePill->text(), QStringLiteral("2 台在线"));
        QVERIFY(menu.devicePicker);
        QCOMPARE(menu.devicePicker->onlineCount(), 2);
        QCOMPARE(menu.deviceRowButton->height(), 48);
        QVERIFY(menu.rect().contains(menu.deviceRowButton->geometry()));
        QVERIFY(!menu.devicePicker->isVisible());

        menu.deviceRowButton->click();
        QVERIFY(menu.devicePicker->isVisible());
        QCOMPARE(menu.devicePicker->x(), menu.deviceRowButton->x());
        QVERIFY(menu.devicePicker->y() >= menu.deviceRowButton->geometry().bottom());
        QVERIFY(menu.devicePicker->geometry().right() < menu.width());
        QVERIFY(menu.devicePicker->geometry().bottom() < menu.height());

        auto item = [&menu](const QString &serial) -> QPushButton * {
            for (auto *button : menu.devicePicker->findChildren<QPushButton *>(
                     QStringLiteral("devicePickerItem")))
                if (button->property("deviceSerial").toString() == serial) return button;
            return nullptr;
        };
        auto *second = item(QStringLiteral("serial-b"));
        QVERIFY(second);
        QCOMPARE(second->accessibleDescription(), QStringLiteral("serial-b，在线"));
        QTest::mouseClick(second, Qt::LeftButton);
        QTRY_COMPARE(manager->deviceSerial(), QStringLiteral("serial-b"));
        QVERIFY(!menu.devicePicker->isVisible());
        QCOMPARE(menu.deviceSerialLabel->text(), QStringLiteral("serial-b"));
        QCOMPARE(menu.deviceNameLabel->text(), QStringLiteral("Phone B"));
        QVERIFY(menu.screenCastSubtitle->text().contains(QStringLiteral("Phone B")));
        QVERIFY(menu.deviceToast->isVisible());
        QVERIFY(menu.deviceToast->text().contains(QStringLiteral("Phone B")));

        // 操作通道被占用时只提示，不改变目标，也不关闭浮层。
        manager->stopMonitoring();
        QTRY_VERIFY(!ProcessManager::hasActiveDeviceProcesses());
        QObject owner;
        QString leaseError;
        QVERIFY2(DeviceOperationLease::acquire(&owner, &leaseError), qPrintable(leaseError));
        menu.deviceRowButton->click();
        QVERIFY(menu.devicePicker->isVisible());
        auto *first = item(QStringLiteral("serial-a"));
        QVERIFY(first);
        QTest::mouseClick(first, Qt::LeftButton);
        QCOMPARE(manager->deviceSerial(), QStringLiteral("serial-b"));
        QVERIFY(menu.devicePicker->isVisible());
        auto *hint = menu.devicePicker->findChild<QLabel *>(QStringLiteral("devicePickerHint"));
        QVERIFY(hint);
        QVERIFY(hint->text().contains(QStringLiteral("操作进行中")));
        DeviceOperationLease::release(&owner);
        QTest::keyClick(&menu, Qt::Key_Escape);
        QVERIFY(!menu.devicePicker->isVisible());
        QVERIFY(menu.isVisible());
    }
    void castTargetsSelectedDeviceAmongMany() {
        QVERIFY(writeFile(fixturePath + "/devices.txt",
                          "PHONE_A\tdevice\nPHONE_B\tdevice\n"));
        MenuWidget menu;
        auto *manager = DeviceManager::instance();
        QTRY_COMPARE(manager->deviceSerials().size(), 2);
        QVERIFY(manager->selectDevice(QStringLiteral("PHONE_B")));
        QCOMPARE(menu.deviceSerialLabel->text(), QStringLiteral("PHONE_B"));

        menu.buttons[MenuWidget::ScreenCast]->click();
        QTRY_COMPARE(menu.screenCastController->state(), ScreenCastController::Streaming);
        QTRY_COMPARE(scrcpyCommands().size(), 1);
        QCOMPARE(scrcpyCommands().first(), QJsonArray({"scrcpy", "--max-size", "1024",
            "--video-bit-rate", "4M", "--serial", "PHONE_B"}));

        menu.buttons[MenuWidget::ScreenCast]->click();
        QTRY_COMPARE(menu.screenCastController->state(), ScreenCastController::Idle);
        QTest::qWait(250);
        QCOMPARE(scrcpyCommands().size(), 1);
    }
    void compactMenuAndUniformDrawers() {
        // A local empty config prevents the constructor from downloading anything.
        QVERIFY(writeFile(fixturePath + "/download_config.txt", ""));
        MenuWidget menu;
        menu.setAttribute(Qt::WA_DontShowOnScreen);
        menu.show();
        menu.layout()->activate();
        QCOMPARE(menu.width(), int(UIHelper::ControlCenterWidth));
        QCOMPARE(menu.height(), int(UIHelper::ControlCenterHeight));
        for (auto *button : menu.buttons) {
            QVERIFY(!button->icon().isNull());
            QVERIFY2(!button->accessibleName().isEmpty(),
                     qPrintable(QString("missing accessible name at %1,%2").arg(button->x()).arg(button->y())));
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
            const int menuX = 16;
            const int drawerY = menuY + (menu.height() - UIHelper::DrawerHeight) / 2;
            const int drawerStep = UIHelper::DrawerWidth + 18;
            const int drawerX = menuX + menu.width() + 18;
            QImage sheet(drawerX + drawers.size() * 2 * drawerStep, menuY + menu.height() + 20,
                         QImage::Format_ARGB32_Premultiplied);
            sheet.fill(QColor("#dce8ed"));
            QPainter painter(&sheet);
            painter.setPen(QColor("#385565"));
            painter.setFont(QFont("Microsoft YaHei", 10));
            painter.drawText(16, 26, QStringLiteral("控制中心 296 × 452px / 嵌入子页（真实样式）+ 独立抽屉（测试替身）"));
            menu.render(&painter, QPoint(menuX, menuY));
            for (int i = 0; i < drawers.size(); ++i)
                drawers[i]->render(&painter, QPoint(drawerX + i * drawerStep, drawerY));
            // 嵌入态子页预览：带 parent 构造即真实应用中的面板内翻页样式。
            RepairWindow embeddedRepair(&menu);
            PayloadWindow embeddedPayload(&menu);
            DeviceCheckWindow embeddedDevice(&menu);
            ConfigWindow embeddedConfig(&menu);
            const QList<QWidget *> embedded = {
                &embeddedRepair, &embeddedPayload, &embeddedDevice, &embeddedConfig};
            const int embeddedX = drawerX + drawers.size() * drawerStep;
            for (int i = 0; i < embedded.size(); ++i) {
                embedded[i]->resize(UIHelper::DrawerWidth, UIHelper::ControlCenterHeight);
                embedded[i]->layout()->activate();
                embedded[i]->render(&painter, QPoint(embeddedX + i * drawerStep, menuY));
            }
            painter.end();
            QVERIFY(sheet.save(output));
        }
    }
    void closingDrawersWithMouseFocusRestoresButton()
    {
        QVERIFY(writeFile(fixturePath + "/download_config.txt", ""));
        MenuWidget menu;
        menu.setAttribute(Qt::WA_DontShowOnScreen);
        menu.show();
        menu.layout()->activate();
        menu.repairWindow = new RepairWindow;
        menu.payloadWindow = new PayloadWindow;
        menu.deviceCheckWindow = new DeviceCheckWindow;
        menu.configWindow = new ConfigWindow;
        const QList<QPair<int, QWidget *>> drawers = {
            {MenuWidget::RepairTools, menu.repairWindow}, {MenuWidget::Payload, menu.payloadWindow},
            {MenuWidget::DeviceCheck, menu.deviceCheckWindow}, {MenuWidget::Configuration, menu.configWindow}};
        for (const auto &entry : drawers) {
            entry.second->setAttribute(Qt::WA_DontShowOnScreen);
            auto *button = menu.buttons[entry.first];
            QEvent leave(QEvent::Leave);
            QCoreApplication::sendEvent(button, &leave);
            QTest::qWait(200);
            const auto resting = button->grab().toImage();
            QFocusEvent mouseFocus(QEvent::FocusIn, Qt::MouseFocusReason);
            QCoreApplication::sendEvent(button, &mouseFocus);
            button->click();
            QVERIFY(entry.second->isVisible());
            QVERIFY(button->property("menuSelected").toBool());
            button->click();
            QVERIFY(!entry.second->isVisible());
            QVERIFY(!button->property("menuSelected").toBool());
            QCoreApplication::sendEvent(button, &leave);
            QTest::qWait(200);
            QCOMPARE(button->grab().toImage(), resting);
        }
    }
    void castingSelectionSurvivesDrawerChanges()
    {
        QVERIFY(writeFile(fixturePath + "/download_config.txt", ""));
        MenuWidget menu;
        menu.setAttribute(Qt::WA_DontShowOnScreen);
        menu.show();
        menu.repairWindow = new RepairWindow;
        menu.repairWindow->setAttribute(Qt::WA_DontShowOnScreen);
        auto *castButton = menu.buttons[MenuWidget::ScreenCast];
        auto *drawerButton = menu.buttons[MenuWidget::RepairTools];
        castButton->click();
        QCOMPARE(menu.screenCastController->state(), ScreenCastController::WaitingForDevice);
        QVERIFY(castButton->property("menuSelected").toBool());
        drawerButton->click();
        QVERIFY(menu.repairWindow->isVisible());
        QVERIFY(drawerButton->property("menuSelected").toBool());
        QVERIFY(castButton->property("menuSelected").toBool());
        drawerButton->click();
        QVERIFY(!menu.repairWindow->isVisible());
        QVERIFY(!drawerButton->property("menuSelected").toBool());
        QVERIFY(castButton->property("menuSelected").toBool());
        castButton->click();
        QCOMPARE(menu.screenCastController->state(), ScreenCastController::Idle);
        QVERIFY(!castButton->property("menuSelected").toBool());
    }
    void waitingSpinnerAndCancel() {
        MenuWidget menu;
        const auto topLevels = QApplication::topLevelWidgets().size();
        auto *button = menu.buttons[MenuWidget::ScreenCast];
        button->click();
        QCOMPARE(menu.screenCastController->state(), ScreenCastController::WaitingForDevice);
        QVERIFY(button->text().isEmpty());
        QVERIFY(!button->icon().isNull());
        QVERIFY(button->property("menuSelected").toBool());
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
        QVERIFY(!button->property("menuSelected").toBool());
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
    void navigationKeyCodesMatchAndroidConstants() {
        QCOMPARE(ScreenCastNavBar::androidKeyCode(ScreenCastNavBar::NavAction::Back), 4);
        QCOMPARE(ScreenCastNavBar::androidKeyCode(ScreenCastNavBar::NavAction::Home), 3);
        QCOMPARE(ScreenCastNavBar::androidKeyCode(ScreenCastNavBar::NavAction::Recents), 187);
    }
    void navigationBarPlacementMath() {
        const QSize nav = ScreenCastNavBar::preferredSize();
        QCOMPARE(nav, QSize(48, 128));
        // 右侧有空间：贴右侧、相对投屏窗口垂直居中。
        QCOMPARE(ScreenCastNavBar::computePlacement(QRect(100, 100, 400, 300), QRect(0, 0, 800, 600), nav),
                 QRect(508, 186, 48, 128));
        // 右侧越界、左侧有空间：回退到左侧。
        QCOMPARE(ScreenCastNavBar::computePlacement(QRect(500, 100, 280, 400), QRect(0, 0, 800, 600), nav),
                 QRect(444, 236, 48, 128));
        // 两侧都放不下（全屏）：贴工作区右缘。
        QCOMPARE(ScreenCastNavBar::computePlacement(QRect(0, 0, 800, 600), QRect(0, 0, 800, 600), nav),
                 QRect(752, 236, 48, 128));
        // 垂直越出工作区下方：钳制在工作区内。
        QCOMPARE(ScreenCastNavBar::computePlacement(QRect(100, 400, 400, 500), QRect(0, 0, 800, 600), nav),
                 QRect(508, 472, 48, 128));
        // 垂直越出工作区上方：钳制在工作区内。
        QCOMPARE(ScreenCastNavBar::computePlacement(QRect(100, -300, 400, 300), QRect(0, 0, 800, 600), nav),
                 QRect(508, 0, 48, 128));
    }
    void navigationBarWindowCandidateFilter() {
        const QRect regular(100, 100, 400, 700);
        QVERIFY(ScreenCastNavBar::isCastWindowCandidate(true, false, false, regular, "SDL_app"));
        QVERIFY(!ScreenCastNavBar::isCastWindowCandidate(false, false, false, regular, "SDL_app"));
        QVERIFY(!ScreenCastNavBar::isCastWindowCandidate(true, true, false, regular, "SDL_app"));
        QVERIFY(!ScreenCastNavBar::isCastWindowCandidate(true, false, true, regular, "SDL_app"));
        QVERIFY(!ScreenCastNavBar::isCastWindowCandidate(true, false, false, QRect(0, 0, 0, 0), "SDL_app"));
        QVERIFY(!ScreenCastNavBar::isCastWindowCandidate(true, false, false,
                                                         QRect(-32000, -32000, 199, 34), "ConsoleWindowClass"));
        QVERIFY(!ScreenCastNavBar::isCastWindowCandidate(true, false, false, regular, "ConsoleWindowClass"));
        QVERIFY(!ScreenCastNavBar::isCastWindowCandidate(true, false, false, regular, "PseudoConsoleWindow"));
    }
    void navigationBarFollowsForeignCastWindow() {
#ifdef Q_OS_WIN
        QProcess helper;
        helper.setProgram(QCoreApplication::applicationFilePath());
        helper.setArguments({QStringLiteral("--fake-cast-window")});
        helper.start();
        QVERIFY(helper.waitForStarted(5000));

        ScreenCastNavBar navbar;
        navbar.attachToSession(helper.processId());
        QTRY_VERIFY_WITH_TIMEOUT(navbar.hasCastWindow(), 5000);
        QVERIFY(navbar.isTracking());

        HWND cast = windowByTitle(static_cast<DWORD>(helper.processId()), L"FakeCastWindow");
        QVERIFY(cast);
        HWND nav = reinterpret_cast<HWND>(navbar.winId());

        // 导航条应贴在画面窗口右侧，并在画面移动后继续跟随，而不是停在原处。
        QVERIFY(SetWindowPos(cast, nullptr, 120, 120, 420, 640, SWP_NOACTIVATE | SWP_NOZORDER));
        QTRY_VERIFY_WITH_TIMEOUT(
            qAbs(visibleFrame(nav).left() - (visibleFrame(cast).right() + 1 + 8)) <= 2, 2000);
        const int firstLeft = visibleFrame(nav).left();
        QVERIFY(SetWindowPos(cast, nullptr, 520, 160, 420, 640, SWP_NOACTIVATE | SWP_NOZORDER));
        QTRY_VERIFY_WITH_TIMEOUT(
            visibleFrame(nav).left() != firstLeft &&
            qAbs(visibleFrame(nav).left() - (visibleFrame(cast).right() + 1 + 8)) <= 2, 2000);

        // 导航条必须位于画面窗口正上方：画面盖住按钮原位置时按钮仍然可点。
        QTRY_COMPARE_WITH_TIMEOUT(
            reinterpret_cast<quintptr>(GetWindow(cast, GW_HWNDPREV)),
            reinterpret_cast<quintptr>(nav), 2000);

        navbar.detach();
        QVERIFY(!navbar.hasCastWindow());
        helper.kill();
        QVERIFY(helper.waitForFinished(3000));
#else
        QSKIP("Windows-only native window tracking");
#endif
    }
    void navigationKeysFollowSessionLifecycle() {
        QVERIFY(writeFile(fixturePath + "/devices.txt", "PHONE_A\tdevice\n"));
        MenuWidget menu;
        auto *controller = menu.screenCastController;
        auto *navbar = menu.screenCastNavBar;
        navbar->setAttribute(Qt::WA_DontShowOnScreen);
        menu.buttons[MenuWidget::ScreenCast]->click();
        QTRY_COMPARE(controller->state(), ScreenCastController::Streaming);
        QTRY_COMPARE(scrcpyCommands().size(), 1);
        // 会话开始即进入跟踪；替身 scrcpy 无窗口，导航条等待而不显示。
        QVERIFY(navbar->isTracking());
        QVERIFY(!navbar->hasCastWindow());
        QVERIFY(!navbar->isVisible());
        // 点击导航条按钮：通过 adb input keyevent 发送对应按键。
        QPushButton *back = nullptr; QPushButton *home = nullptr; QPushButton *recents = nullptr;
        for (QPushButton *button : navbar->findChildren<QPushButton *>()) {
            if (button->accessibleName() == QStringLiteral("返回")) back = button;
            if (button->accessibleName() == QStringLiteral("主页")) home = button;
            if (button->accessibleName() == QStringLiteral("多任务")) recents = button;
        }
        QVERIFY(back && home && recents);
        back->click();
        QTRY_COMPARE(adbKeyCommands().size(), 1);
        QCOMPARE(adbKeyCommands().first(), QJsonArray({"adb", "-s", "PHONE_A",
            "shell", "input", "keyevent", "4"}));
        home->click();
        recents->click();
        QTRY_COMPARE(adbKeyCommands().size(), 3);
        QVERIFY(adbKeyCommands().contains(QJsonArray({"adb", "-s", "PHONE_A",
            "shell", "input", "keyevent", "3"})));
        QVERIFY(adbKeyCommands().contains(QJsonArray({"adb", "-s", "PHONE_A",
            "shell", "input", "keyevent", "187"})));
        // 会话结束：停止跟踪，按键不再发送。
        menu.buttons[MenuWidget::ScreenCast]->click();
        QTRY_COMPARE(controller->state(), ScreenCastController::Idle);
        QVERIFY(!navbar->isTracking());
        QVERIFY(!navbar->hasCastWindow());
        back->click();
        QTest::qWait(200);
        QCOMPARE(adbKeyCommands().size(), 3);
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
    void normalWindowCloseRestartsWithoutBackoff() {
        ScreenCastController controller;
        // An exaggerated failure delay proves normal closes do not use it.
        controller.m_retryTimer.setInterval(10000);
        DeviceManager::instance()->setDetectedDevice(DeviceManager::ADB, "PHONE_A");
        controller.toggle();
        QTRY_COMPARE(controller.state(), ScreenCastController::Streaming);
        QTRY_COMPARE(scrcpyCommands().size(), 1);
        QElapsedTimer reopening;
        bool usedBackoff = false;
        connect(&controller, &ScreenCastController::stateChanged, this, [&] {
            if (controller.state() == ScreenCastController::WaitingForDevice) {
                reopening.start();
                usedBackoff |= controller.m_retryTimer.isActive();
            }
        });
        for (int close = 0; close < 3; ++close) {
            reopening.invalidate();
            QVERIFY(writeFile(fixturePath + "/close-request.txt", "close"));
            QTRY_COMPARE_WITH_TIMEOUT(scrcpyCommands().size(), close + 2, 1500);
            QTRY_COMPARE(controller.state(), ScreenCastController::Streaming);
            QVERIFY(reopening.isValid());
            QVERIFY(!usedBackoff);
            QVERIFY2(reopening.elapsed() < 1500, "Normal window close was delayed by failure backoff");
            qInfo() << "Normal close-to-restart:" << reopening.elapsed() << "ms";
            // Device-info notifications must not launch another replacement.
            for (int update = 0; update < 5; ++update) controller.checkDevice();
            QTest::qWait(100);
            QCOMPARE(scrcpyCommands().size(), close + 2);
        }
        QCOMPARE(controller.m_retryTimer.interval(), 10000);
        controller.toggle();
        QTRY_COMPARE(controller.state(), ScreenCastController::Idle);
        QTest::qWait(150);
        QCOMPARE(scrcpyCommands().size(), 4);
    }
    void manualCancelDuringImmediateRestartRemainsStopped() {
        MenuWidget menu;
        freeze(menu);
        auto *controller = menu.screenCastController;
        DeviceManager::instance()->setDetectedDevice(DeviceManager::ADB, "PHONE_A");
        auto *button = menu.buttons[MenuWidget::ScreenCast];
        button->click();
        QTRY_COMPARE(controller->state(), ScreenCastController::Streaming);
        QTRY_COMPARE(scrcpyCommands().size(), 1);
        bool canceled = false;
        connect(controller, &ScreenCastController::stateChanged, this, [&] {
            if (controller->state() == ScreenCastController::WaitingForDevice) {
                canceled = true;
                button->click();
            }
        });
        QVERIFY(writeFile(fixturePath + "/close-request.txt", "close"));
        QTRY_COMPARE(controller->state(), ScreenCastController::Idle);
        QVERIFY(canceled);
        QVERIFY(!controller->m_autoCastEnabled);
        QVERIFY(!controller->m_retryTimer.isActive());
        QTest::qWait(250);
        QCOMPARE(scrcpyCommands().size(), 1);
    }
    void normalCloseDuringOperationWaitsForLease() {
        ScreenCastController controller;
        controller.m_retryTimer.setInterval(10000);
        DeviceManager::instance()->setDetectedDevice(DeviceManager::ADB, "PHONE_A");
        controller.toggle();
        QTRY_COMPARE(controller.state(), ScreenCastController::Streaming);
        QTRY_COMPARE(scrcpyCommands().size(), 1);
        QObject operation;
        QVERIFY(DeviceOperationLease::acquire(&operation));
        QVERIFY(writeFile(fixturePath + "/close-request.txt", "close"));
        QTRY_COMPARE(controller.state(), ScreenCastController::WaitingForDevice);
        QVERIFY(controller.m_retryTimer.isActive());
        QTest::qWait(150);
        QCOMPARE(scrcpyCommands().size(), 1);
        DeviceOperationLease::release(&operation);
        QTRY_COMPARE_WITH_TIMEOUT(scrcpyCommands().size(), 2, 1500);
        QTRY_COMPARE(controller.state(), ScreenCastController::Streaming);
    }
    void leaseReleaseWithoutSignalStillRestarts() {
        ScreenCastController controller;
        controller.m_retryTimer.setInterval(80);
        DeviceManager::instance()->setDetectedDevice(DeviceManager::ADB, "PHONE_A");
        controller.toggle();
        QTRY_COMPARE(controller.state(), ScreenCastController::Streaming);
        QTRY_COMPARE(scrcpyCommands().size(), 1);
        QObject operation;
        QVERIFY(DeviceOperationLease::acquire(&operation));
        QVERIFY(writeFile(fixturePath + "/close-request.txt", "close"));
        QTRY_COMPARE(controller.state(), ScreenCastController::WaitingForDevice);
        QVERIFY(controller.m_retryTimer.isActive());
        // Simulate a missed/dropped lease release notification: the periodic
        // fallback must still recover the cast instead of waiting forever.
        {
            QSignalBlocker blocker(DeviceOperationLease::instance());
            DeviceOperationLease::release(&operation);
        }
        QTRY_COMPARE_WITH_TIMEOUT(scrcpyCommands().size(), 2, 1500);
        QTRY_COMPARE(controller.state(), ScreenCastController::Streaming);
        controller.toggle();
        QTRY_COMPARE(controller.state(), ScreenCastController::Idle);
    }
    void failedRestartAfterNormalCloseStillBacksOff() {
        ScreenCastController controller;
        controller.m_retryTimer.setInterval(600);
        DeviceManager::instance()->setDetectedDevice(DeviceManager::ADB, "PHONE_A");
        controller.toggle();
        QTRY_COMPARE(controller.state(), ScreenCastController::Streaming);
        QTRY_COMPARE(scrcpyCommands().size(), 1);
        QVERIFY(writeFile(fixturePath + "/behavior.txt", "fail"));
        QVERIFY(writeFile(fixturePath + "/close-request.txt", "close"));
        QTRY_COMPARE_WITH_TIMEOUT(scrcpyCommands().size(), 2, 1500);
        QTRY_VERIFY(controller.m_retryTimer.isActive());
        QCOMPARE(controller.state(), ScreenCastController::WaitingForDevice);
        QCOMPARE(controller.m_retryTimer.interval(), 600);
        QVERIFY(writeFile(fixturePath + "/behavior.txt", ""));
        for (int update = 0; update < 5; ++update) controller.checkDevice();
        QTest::qWait(100);
        QCOMPARE(scrcpyCommands().size(), 2);
        QTRY_COMPARE(controller.state(), ScreenCastController::Streaming);
        QTRY_COMPARE(scrcpyCommands().size(), 3);
    }
    void fastRestartRechecksDeviceBeforeLaunch() {
        ScreenCastController controller;
        controller.m_retryTimer.setInterval(10000);
        auto *manager = DeviceManager::instance();
        manager->setDetectedDevice(DeviceManager::ADB, "PHONE_A");
        controller.toggle();
        QTRY_COMPARE(controller.state(), ScreenCastController::Streaming);
        QTRY_COMPARE(scrcpyCommands().size(), 1);
        connect(&controller, &ScreenCastController::stateChanged, this, [&] {
            if (controller.state() == ScreenCastController::WaitingForDevice)
                manager->setDetectedDevice(DeviceManager::None, {});
        });
        QVERIFY(writeFile(fixturePath + "/close-request.txt", "close"));
        QTRY_COMPARE(controller.state(), ScreenCastController::WaitingForDevice);
        QVERIFY(!controller.m_retryTimer.isActive());
        QTest::qWait(150);
        QCOMPARE(scrcpyCommands().size(), 1);
        manager->setDetectedDevice(DeviceManager::ADB, "PHONE_B");
        QTRY_COMPARE_WITH_TIMEOUT(scrcpyCommands().size(), 2, 1500);
        QTRY_COMPARE(controller.state(), ScreenCastController::Streaming);
        QVERIFY(scrcpyCommands().contains(QJsonArray{"scrcpy", "--max-size", "1024",
            "--video-bit-rate", "4M", "--serial", "PHONE_B"}));
    }
    void processExitAutomaticallyRestarts_data() {
        QTest::addColumn<QByteArray>("behavior");
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
        QVERIFY(writeFile(fixturePath + "/behavior.txt", "fail"));
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
    QList<QJsonArray> adbKeyCommands() const {
        QList<QJsonArray> commands;
        for (const QFileInfo &file : QDir(fixturePath).entryInfoList({"command-*.json"}, QDir::Files)) {
            auto array = QJsonDocument::fromJson(readFile(file.absoluteFilePath())).array();
            if (!array.isEmpty() && array.at(0).toString() == "adb" &&
                array.contains(QStringLiteral("keyevent"))) commands.append(array);
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
        QTimer closeRequest;
        closeRequest.setInterval(10);
        QObject::connect(&closeRequest, &QTimer::timeout, &application, [&] {
            if (QFile::remove(directory + "/close-request.txt")) application.exit(0);
        });
        closeRequest.start();
        const auto behavior = readFile(directory + "/behavior.txt").trimmed();
        if (behavior == "fail") {
            QTimer::singleShot(300, &application, [&application] { application.exit(1); });
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
#ifdef Q_OS_WIN
    // 回归测试替身：供 navigationBarFollowsForeignCastWindow 在一个独立
    // 进程里创建“投屏画面”窗口，测试 PID 跟随和 Z 序，不连接真实设备。
    if (application.arguments().contains(QStringLiteral("--fake-cast-window"))) {
        QWidget cast;
        cast.setWindowTitle(QStringLiteral("FakeCastWindow"));
        cast.setGeometry(200, 120, 420, 640);
        cast.show();
        return application.exec();
    }
#endif
    ScreenCastTests tests; return QTest::qExec(&tests, argc, argv);
}
#include "screencasttests.moc"
