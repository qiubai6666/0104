#include "devicemanager.h"
#include "deviceinfowindow.h"
#include "devicecheckwindow.h"
#include "deviceoperationlease.h"
#include "processmanager.h"
#include "resourceextractor.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QScrollArea>
#include <QFontDatabase>
#include <QTextEdit>
#include <QPushButton>
#include <QClipboard>
#include <QUuid>
#include <QtTest>
#include <cstdio>
#include <memory>

namespace {
QString fixturePath;
bool unavailable = false;
QByteArray load(const QString &name) {
    QFile file(name); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
bool save(const QString &name, const QByteArray &bytes) {
    QFile file(name); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QJsonObject config() { return QJsonDocument::fromJson(load(fixturePath + "/config.json")).object(); }
void setting(const QString &key, const QJsonValue &value) {
    auto object = config(); object.insert(key, value);
    QVERIFY(save(fixturePath + "/config.json", QJsonDocument(object).toJson()));
}
QString snapshot(const QString &device = "alpha", const QString &slot = "a", const QString &unlock = QStringLiteral("已解锁")) {
    return QStringLiteral("代号:%1\n分区:%2\n解锁:%3").arg(device, slot, unlock);
}
QString details(const QString &model = "Model A", const QString &device = "alpha", const QString &version = "Android 16",
                const QString &slot = "a", const QString &unlock = QStringLiteral("已解锁")) {
    Q_UNUSED(model); Q_UNUSED(version);
    return QStringLiteral("设备代号：%1\n活动分区：%2\n解锁状态：%3").arg(device, slot, unlock);
}
QStringList audit() {
    QStringList records;
    for (const QString &file : QDir(fixturePath).entryList({"command-*.json"})) records << QString::fromUtf8(load(fixturePath + "/" + file));
    return records;
}
int queryCount(const QString &property) {
    int count = 0; for (const QString &record : audit()) if (record.contains(property)) ++count; return count;
}
}
QString ResourceExtractor::getResourcePath() { return fixturePath; }
QString ResourceExtractor::getAdbPath() { return fixturePath + (unavailable ? "/missing-adb.exe" : "/adb.exe"); }
QString ResourceExtractor::getFastbootPath() { return fixturePath + (unavailable ? "/missing-fastboot.exe" : "/fastboot.exe"); }
QString ResourceExtractor::getNeilImagePath() { return fixturePath + "/Neil.jpg"; }

class DeviceInformationTests : public QObject {
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> fixture;
    DeviceManager *manager = nullptr;
    void select(DeviceManager::DeviceMode mode = DeviceManager::ADB, const QString &serial = "first") {
        manager->setDetectedDevice(mode, serial);
        manager->updateDeviceInfo();
    }
    void freezePolling() {
        manager->m_checkTimer->stop();
        manager->pauseMonitoring();
        manager->resumeMonitoring();
    }
private slots:
    void init() {
        fixture.reset(new QTemporaryDir(QDir::tempPath() + "/device-info-XXXXXX"));
        QVERIFY(fixture->isValid());
        // Retain all fixtures for inspection; project cleanup is recommendation-only.
        fixture->setAutoRemove(false);
        fixturePath = fixture->path(); unavailable = false;
        for (const QString &name : QStringList{"adb", "fastboot", "scrcpy"})
            QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), fixturePath + "/" + name + ".exe"));
        QVERIFY(save(fixturePath + "/config.json", "{}"));
        manager = DeviceManager::instance();
        manager->stopMonitoring(); manager->m_isPaused = false;
        manager->setDetectedDevice(DeviceManager::None, QString());
        manager->m_infoTimer->setInterval(5000);
        DeviceOperationLease::setIdleCheck({});
    }
    void cleanup() {
        QVERIFY(!DeviceOperationLease::owner());
        manager->stopMonitoring();
        manager->setDetectedDevice(DeviceManager::None, QString());
        ProcessManager::stopAllProcesses();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCOMPARE(manager->m_fullModeRefCount, 0);
        QCOMPARE(manager->m_adbOnlyRefCount, 0);
        fixture.reset();
    }
    void slowAdbSlotIsNotRestartedByPolling() {
        setting("ro.boot.slot_suffix.delay", 1300);
        DeviceCheckWindow window;
        QSignalSpy spy(manager, &DeviceManager::deviceInfoUpdated);
        QTRY_COMPARE_WITH_TIMEOUT(manager->getDeviceInfo(), snapshot(), 6000);
        QTRY_COMPARE(window.infoLabel->text(), details());
        QCOMPARE(manager->deviceSerial(), QString("first"));
        QCOMPARE(queryCount("ro.product.device"), 1);
        for (const auto &args : spy) QVERIFY(!args.at(0).toString().contains("代号:orange"));
        for (const QString &record : audit()) if (record.contains("getprop")) {
            const auto args = QJsonDocument::fromJson(record.toUtf8()).array();
            QCOMPARE(args.at(1).toString(), QString("-s")); QCOMPARE(args.at(2).toString(), QString("first"));
        }
    }
    void slowFastbootSlotIsNotRestartedByPolling() {
        setting("mode", "fastboot"); setting("current-slot.delay", 1300);
        DeviceCheckWindow window;
        QTRY_COMPARE_WITH_TIMEOUT(manager->getDeviceInfo(), snapshot(), 6000);
        QTRY_COMPARE(window.infoLabel->text(), details(QStringLiteral("未知"), "alpha", QStringLiteral("未知")));
        QCOMPARE(queryCount("product"), 1);
    }
    void slotFailuresAndEmptyValues_data() {
        QTest::addColumn<QString>("value"); QTest::addColumn<int>("exitCode"); QTest::addColumn<QString>("expected");
        QTest::newRow("failed-empty") << "" << 1 << QStringLiteral("未知");
        QTest::newRow("failed-output") << "_a" << 1 << QStringLiteral("未知");
        QTest::newRow("successful-empty") << "" << 0 << QStringLiteral("无");
        QTest::newRow("valid-b") << "_b" << 0 << "b";
        QTest::newRow("invalid") << "orange" << 0 << QStringLiteral("未知");
        QTest::newRow("multiline") << "_a\n_b" << 0 << QStringLiteral("未知");
    }
    void slotFailuresAndEmptyValues() {
        QFETCH(QString, value); QFETCH(int, exitCode); QFETCH(QString, expected);
        setting("ro.boot.slot_suffix.value", value); setting("ro.boot.slot_suffix.code", exitCode);
        select(); QTRY_COMPARE(manager->getDeviceInfo(), snapshot("alpha", expected));
    }
    void fastbootOnlyParsesExactFields() {
        setting("product.value", "(bootloader) product: beta\nunrelated: yes\nFinished. Total time: 0.1s");
        setting("current-slot.value", "(bootloader) current-slot: b");
        setting("unlocked.value", "other-unlocked: yes\n(bootloader) unlocked: no");
        setting("unlocked.stdout", true);
        select(DeviceManager::Fastboot);
        QTRY_COMPARE(manager->getDeviceInfo(), snapshot("beta", "b", QStringLiteral("未解锁")));
        setting("unlocked.value", "other: yes"); manager->updateDeviceInfo();
        QTRY_COMPARE(manager->getDeviceInfo(), snapshot("beta", "b", QStringLiteral("未知")));
        setting("unlocked.value", "unlocked: yes"); setting("unlocked.code", 1); manager->updateDeviceInfo();
        QTRY_VERIFY(!manager->m_infoQueryActive);
        QCOMPARE(manager->getDeviceInfo(), snapshot("beta", "b", QStringLiteral("未知")));
    }
    void adbUnlockValues_data() {
        QTest::addColumn<QString>("value"); QTest::addColumn<int>("code"); QTest::addColumn<QString>("expected");
        QTest::newRow("orange") << "orange" << 0 << QStringLiteral("已解锁");
        QTest::newRow("green") << "green" << 0 << QStringLiteral("未解锁");
        QTest::newRow("empty") << "" << 0 << QStringLiteral("未知");
        QTest::newRow("unrecognized") << "yellow" << 0 << QStringLiteral("未知");
        QTest::newRow("failed") << "orange" << 1 << QStringLiteral("未知");
    }
    void adbUnlockValues() {
        QFETCH(QString, value); QFETCH(int, code); QFETCH(QString, expected);
        setting("ro.boot.verifiedbootstate.value", value); setting("ro.boot.verifiedbootstate.code", code);
        select(); QTRY_COMPARE(manager->getDeviceInfo(), snapshot("alpha", "a", expected));
    }
    void serialSelectionIsStableAndRejectsOfflineDevices() {
        manager->m_adbOnly = true;
        manager->setDetectedDevice(DeviceManager::ADB, "second");
        setting("adb.devices", "List of devices attached\ndevice-offline\toffline\nfirst\tdevice\nsecond\tdevice\n");
        manager->checkDeviceStatus(); QTRY_VERIFY(!manager->m_isChecking);
        QCOMPARE(manager->deviceSerial(), QString("second"));
        QTRY_COMPARE(manager->getDeviceInfo(), snapshot("beta"));
        setting("adb.devices", "List of devices attached\nfirst\tdevice\nsecond\tunauthorized\n");
        manager->checkDeviceStatus(); QTRY_VERIFY(!manager->m_isChecking);
        QCOMPARE(manager->deviceSerial(), QString("first")); QTRY_COMPARE(manager->getDeviceInfo(), snapshot());
        setting("adb.devices", "List of devices attached\ndevice-offline\toffline\nsecond\tunauthorized\n");
        manager->checkDeviceStatus(); QTRY_VERIFY(!manager->m_isChecking);
        QCOMPARE(manager->currentMode(), DeviceManager::None); QVERIFY(manager->getDeviceInfo().isEmpty());
    }
    void timeoutAndFailedStartPublishUnknownThenRecover() {
        setting("ro.boot.slot_suffix.delay", 5000); manager->m_infoTimer->setInterval(300);
        select(); QTRY_COMPARE(manager->getDeviceInfo(), snapshot("alpha", QStringLiteral("未知")));
        QTRY_VERIFY(!manager->m_infoQueryActive);
        setting("ro.boot.slot_suffix.delay", 0); manager->updateDeviceInfo();
        QTRY_COMPARE(manager->getDeviceInfo(), snapshot());
        QTRY_VERIFY(!manager->m_infoQueryActive);
        unavailable = true; manager->updateDeviceInfo();
        QTRY_COMPARE(manager->getDeviceInfo(), snapshot(QStringLiteral("未知"), QStringLiteral("未知"), QStringLiteral("未知")));
        QTRY_VERIFY(!manager->m_infoQueryActive);
        unavailable = false; manager->updateDeviceInfo(); QTRY_COMPARE(manager->getDeviceInfo(), snapshot());
    }
    void canceledGenerationCannotPublishForReplacement() {
        setting("ro.product.device.delay", 1200); select();
        QTRY_COMPARE(manager->m_infoProcess->state(), QProcess::Running);
        manager->setDetectedDevice(DeviceManager::None, QString());
        QTest::qWait(1400); QVERIFY(manager->getDeviceInfo().isEmpty());
        setting("ro.product.device.delay", 0); select(DeviceManager::ADB, "second");
        QTRY_COMPARE(manager->getDeviceInfo(), snapshot("beta"));
        QTRY_VERIFY(!manager->m_infoQueryActive);
        setting("ro.boot.slot_suffix.delay", 1200); manager->updateDeviceInfo();
        QTRY_COMPARE(manager->m_infoStep, 1);
        select(DeviceManager::Fastboot, "third"); QTRY_COMPARE(manager->getDeviceInfo(), snapshot("beta"));
        QTest::qWait(1400); QCOMPARE(manager->getDeviceInfo(), snapshot("beta"));
    }
    void pauseStopAndResumeDiscardIncompleteSnapshots() {
        setting("ro.boot.verifiedbootstate.delay", 1200); select(); QTRY_COMPARE(manager->m_infoStep, 2);
        QSignalSpy spy(manager, &DeviceManager::deviceInfoUpdated);
        manager->pauseMonitoring(); QTest::qWait(1400); QCOMPARE(spy.count(), 0);
        manager->resumeMonitoring(); setting("ro.boot.verifiedbootstate.delay", 0); manager->updateDeviceInfo();
        QTRY_COMPARE(manager->getDeviceInfo(), snapshot());
        QTRY_VERIFY(!manager->m_infoQueryActive);
        setting("ro.product.device.delay", 1200); manager->updateDeviceInfo();
        manager->stopMonitoring(); QTest::qWait(1400); QCOMPARE(manager->getDeviceInfo(), snapshot());
    }
    void slowModelAndVersionIgnoreRepeatedRefresh_data() {
        QTest::addColumn<QString>("slowProperty");
        QTest::newRow("model") << "ro.product.model";
        QTest::newRow("version") << "ro.build.version.release";
    }
    void slowModelAndVersionIgnoreRepeatedRefresh() {
        QFETCH(QString, slowProperty); setting(slowProperty + ".delay", 1300);
        DeviceInfoWindow window;
        QTRY_VERIFY(window.queryActive && window.queryStep == (slowProperty == "ro.product.model" ? 0 : 1));
        const auto generation = window.queryGeneration;
        for (int i = 0; i < 3; ++i) window.onDeviceInfoUpdated(snapshot());
        QCOMPARE(window.queryGeneration, generation);
        QTRY_COMPARE_WITH_TIMEOUT(window.versionLabel->text(), QStringLiteral("系统版本: Android 16"), 5000);
        QCOMPARE(window.modelLabel->text(), QStringLiteral("手机型号: Model A"));
        QCOMPARE(window.codenameLabel->text(), QStringLiteral("手机代号: alpha"));
        QCOMPARE(window.slotLabel->text(), QStringLiteral("活动分区: a"));
        QCOMPARE(window.unlockLabel->text(), QStringLiteral("解锁状态: 已解锁"));
        QTRY_VERIFY(!manager->m_infoQueryActive);
        QCOMPARE(queryCount(slowProperty), 1);
    }
    void projectionReplacementAndDisconnectRejectStaleResults() {
        setting("ro.build.version.release.delay", 1300);
        DeviceInfoWindow window; QTRY_VERIFY(window.queryActive && window.queryStep == 1);
        freezePolling(); setting("ro.build.version.release.delay", 0);
        select(DeviceManager::ADB, "second");
        QTRY_COMPARE(window.versionLabel->text(), QStringLiteral("系统版本: Android 17"));
        QCOMPARE(window.modelLabel->text(), QStringLiteral("手机型号: Model B"));
        QCOMPARE(window.codenameLabel->text(), QStringLiteral("手机代号: beta"));
        QTest::qWait(1500); QCOMPARE(window.versionLabel->text(), QStringLiteral("系统版本: Android 17"));
        manager->setDetectedDevice(DeviceManager::None, QString());
        QVERIFY(!window.queryActive); QVERIFY(window.queryProcess == nullptr);
        QTest::qWait(300); QVERIFY(window.modelLabel->text().contains(QStringLiteral("未检测")));
    }
    void projectionFailedStartTimeoutAndRetry() {
        DeviceInfoWindow window; QTRY_COMPARE(window.versionLabel->text(), QStringLiteral("系统版本: Android 16"));
        freezePolling(); unavailable = true; window.onDeviceInfoUpdated(snapshot());
        QTRY_COMPARE(window.versionLabel->text(), QStringLiteral("系统版本: Android 未知"));
        unavailable = false;
        QTRY_COMPARE_WITH_TIMEOUT(window.versionLabel->text(), QStringLiteral("系统版本: Android 16"), 4000);
        window.queryTimer->setInterval(300); setting("ro.product.model.delay", 5000); window.onDeviceInfoUpdated(snapshot());
        QTRY_COMPARE(window.modelLabel->text(), QStringLiteral("手机型号: 未知"));
        setting("ro.product.model.delay", 0);
        QTRY_COMPARE_WITH_TIMEOUT(window.modelLabel->text(), QStringLiteral("手机型号: Model A"), 4000);
        setting("ro.build.version.release.code", 1); setting("ro.build.version.release.value", "99");
        window.onDeviceInfoUpdated(snapshot());
        QTRY_COMPARE(window.versionLabel->text(), QStringLiteral("系统版本: Android 未知"));
    }
    void projectionVersionTimeoutAndMultilineAreUnknown() {
        DeviceInfoWindow window; QTRY_COMPARE(window.versionLabel->text(), QStringLiteral("系统版本: Android 16"));
        freezePolling(); window.queryTimer->setInterval(300);
        setting("ro.build.version.release.delay", 5000); window.onDeviceInfoUpdated(snapshot());
        QTRY_COMPARE(window.versionLabel->text(), QStringLiteral("系统版本: Android 未知"));
        setting("ro.build.version.release.delay", 0); setting("ro.build.version.release.value", "16\nerror");
        window.onDeviceInfoUpdated(snapshot()); QTRY_VERIFY(!window.queryActive);
        QCOMPARE(window.versionLabel->text(), QStringLiteral("系统版本: Android 未知"));
        setting("ro.build.version.release.value", "16");
        QTRY_COMPARE_WITH_TIMEOUT(window.versionLabel->text(), QStringLiteral("系统版本: Android 16"), 4000);
    }
    void projectionLeaseCancelsAndResumesQuery() {
        setting("ro.product.model.delay", 1300);
        DeviceInfoWindow window; QTRY_VERIFY(window.queryActive); freezePolling();
        QObject owner; QVERIFY(DeviceOperationLease::acquire(&owner));
        QVERIFY(!window.queryActive); QVERIFY(window.queryProcess == nullptr);
        setting("ro.product.model.delay", 0); DeviceOperationLease::release(&owner);
        QTRY_COMPARE(window.versionLabel->text(), QStringLiteral("系统版本: Android 16"));
    }
    void detectionPanelLayout_data() {
        QTest::addColumn<int>("menuHeight");
        QTest::addColumn<int>("mode");
        for (int height : {360, 400, 480}) {
            for (int mode : {int(DeviceManager::None), int(DeviceManager::ADB), int(DeviceManager::Fastboot)}) {
                const QByteArray name = QByteArray::number(height) + '-' + QByteArray::number(mode);
                QTest::newRow(name.constData()) << height << mode;
            }
        }
    }
    void detectionPanelLayout() {
        QFETCH(int, menuHeight);
        QFETCH(int, mode);
        // Offscreen Qt does not discover Windows fonts; use an installed CJK font for layout checks.
        if (QGuiApplication::platformName() == "offscreen") {
            QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");
            QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyhbd.ttc");
        }
        manager->pauseMonitoring();
        manager->m_currentMode = DeviceManager::DeviceMode(mode);
        DeviceCheckWindow window;
        window.setPosition(500, 100, menuHeight);
        window.onDeviceInfoUpdated(details());
        window.show();
        QCoreApplication::processEvents();
        QCOMPARE(window.size(), QSize(252, menuHeight));
        QCOMPARE(window.pos(), QPoint(248, 100));
        QCOMPARE(window.infoLabel->font().pixelSize(), 10);
        QCOMPARE(window.infoLabel->palette().color(QPalette::WindowText), QColor("#555"));
        QCOMPARE(window.statusLabel->font().pixelSize(), 13);
        QCOMPARE(window.statusLabel->palette().color(QPalette::WindowText), QColor("#2c3e50"));
        QCOMPARE(window.infoLabel->alignment(), Qt::AlignLeft | Qt::AlignTop);
        QVERIFY(window.findChildren<QScrollArea *>().isEmpty());
        QVERIFY2(window.infoLabel->height() >= window.infoLabel->heightForWidth(window.infoLabel->width()),
                 qPrintable(QString("information height %1, required %2").arg(window.infoLabel->height()).arg(window.infoLabel->heightForWidth(window.infoLabel->width()))));
        QVERIFY(window.statusLabel->mapTo(&window, QPoint()).y() < window.infoLabel->mapTo(&window, QPoint()).y());
        QVERIFY(window.infoLabel->mapTo(&window, window.infoLabel->rect().bottomRight()).y() < window.rebootComboBox->mapTo(&window, QPoint()).y());
        QVector<QPushButton *> buttons{window.executeButton, window.cmdButton, window.bootButton, window.initBootButton};
        const QStringList texts{"执行重启", "打开CMD", "刷入Boot", "刷入Init_Boot"};
        for (int i = 0; i < buttons.size(); ++i) {
            QCOMPARE(buttons[i]->text(), texts[i]);
            QCOMPARE(buttons[i]->height(), 34);
            QCOMPARE(buttons[i]->font().pixelSize(), 13);
            QCOMPARE(buttons[i]->palette().color(QPalette::Active, QPalette::ButtonText), QColor(Qt::white));
            QVERIFY(window.rect().contains(QRect(buttons[i]->mapTo(&window, QPoint()), buttons[i]->size())));
        }
        const int footerY = window.bootButton->mapTo(&window, QPoint()).y();
        QCOMPARE(footerY, window.initBootButton->mapTo(&window, QPoint()).y());
        QCOMPARE(menuHeight - (footerY + window.bootButton->height()), 12);
        QVERIFY(window.executeButton->mapTo(&window, QPoint()).y() > window.rebootComboBox->mapTo(&window, window.rebootComboBox->rect().bottomRight()).y());
        QVERIFY(window.cmdButton->mapTo(&window, QPoint()).y() > window.executeButton->mapTo(&window, window.executeButton->rect().bottomRight()).y());
        QVERIFY(footerY > window.cmdButton->mapTo(&window, window.cmdButton->rect().bottomRight()).y());
        const bool connected = mode != DeviceManager::None;
        QCOMPARE(window.executeButton->isEnabled(), connected);
        QCOMPARE(window.rebootComboBox->isEnabled(), connected);
        QCOMPARE(window.bootButton->isEnabled(), connected);
        QCOMPARE(window.initBootButton->isEnabled(), connected);
        QVERIFY(window.cmdButton->isEnabled());
        if (!connected) QCOMPARE(window.infoLabel->text(), QStringLiteral("等待设备"));
        window.operationInProgress = true;
        window.updateUIForMode(DeviceManager::DeviceMode(mode));
        for (auto button : buttons) QVERIFY(!button->isEnabled());
        window.operationInProgress = false;
    }
    void detectionDetailsContainOnlyRequestedFields() {
        DeviceCheckWindow window;
        QTRY_COMPARE(window.infoLabel->text(), details());
        QCOMPARE(window.infoLabel->text().split('\n').size(), 3);
        QVERIFY(!window.infoLabel->text().contains(QStringLiteral("设备型号")));
        QVERIFY(!window.infoLabel->text().contains(QStringLiteral("系统版本")));
        QCOMPARE(queryCount("ro.product.model"), 0);
        QCOMPARE(queryCount("ro.build.version.release"), 0);
    }
    void detectionPopupShowsExtendedDetailsAndTogglesClosed() {
        DeviceCheckWindow window;
        QTRY_COMPARE(window.infoLabel->text(), details());
        window.show();
        QTest::mouseDClick(window.infoLabel, Qt::LeftButton);
        QTRY_VERIFY(!window.findChildren<QDialog *>().isEmpty());
        auto *dialog = window.findChildren<QDialog *>().constLast();
        QVERIFY(dialog->isVisible());
        QCOMPARE(dialog->size(), window.size());
        const QRect screen = QGuiApplication::primaryScreen()->availableGeometry();
        QVERIFY((dialog->frameGeometry().center() - screen.center()).manhattanLength() <= 2);
        auto *editor = dialog->findChild<QTextEdit *>(QStringLiteral("deviceDetailsText"));
        QVERIFY(editor);
        QTRY_VERIFY_WITH_TIMEOUT(editor->toPlainText().contains(QStringLiteral("设备序列号：")), 6000);
        QVERIFY(editor->toPlainText().contains(QStringLiteral("CPU 代号：")));\n        QVERIFY(!editor->toPlainText().contains(QStringLiteral("CPU 厂商：")));\n        QVERIFY(!editor->toPlainText().contains(QStringLiteral("CPU 名称：")));
        QVERIFY(!dialog->findChild<QPushButton *>(QStringLiteral("copyAllButton")));
        QVERIFY(!dialog->findChild<QPushButton *>(QStringLiteral("closeButton")));

        QTest::mouseClick(window.infoLabel, Qt::LeftButton);
        QTRY_VERIFY(!dialog->isVisible());
    }
    void detectionReplacementShowsFetchingNotOldDevice() {
        DeviceCheckWindow window; QTRY_COMPARE(window.infoLabel->text(), details()); freezePolling();
        setting("ro.product.device.delay", 500); select(DeviceManager::ADB, "second");
        QCOMPARE(window.infoLabel->text(), QStringLiteral("获取中..."));
        QTRY_COMPARE(window.infoLabel->text(), details("Model B", "beta", "Android 17"));
        manager->setDetectedDevice(DeviceManager::None, QString());
        QCOMPARE(window.infoLabel->text(), QStringLiteral("等待设备"));
    }
};

// Fake helpers read only their own TEMP fixture. No USB, shell, network or flashing.
int helperMain(QCoreApplication &app) {
    fixturePath = app.applicationDirPath();
    const QString base = QFileInfo(app.applicationFilePath()).baseName();
    QStringList args = app.arguments().mid(1);
    QJsonArray record; record.append(base); for (const auto &arg : args) record.append(arg);
    if (!save(fixturePath + "/command-" + QUuid::createUuid().toString(QUuid::Id128) + ".json", QJsonDocument(record).toJson(QJsonDocument::Compact))) return 90;
    if (base == "scrcpy") return 0;
    QString serial = "first";
    if (args.value(0) == "-s") { serial = args.value(1); args = args.mid(2); }
    const auto object = config();
    if (args.value(0) == "devices") {
        if (object.contains(base + ".devices")) {
            std::printf("%s", object.value(base + ".devices").toString().toLocal8Bit().constData()); return 0;
        }
        const QString mode = object.value("mode").toString("adb");
        if (base == "adb") std::printf("List of devices attached\n%s", mode == "adb" ? "first\tdevice\n" : "");
        else if (mode == "fastboot") std::puts("first\tfastboot");
        return 0;
    }
    if (base == "adb" && args.value(0) == "shell" && args.value(1) == "getprop" && args.size() == 2) {
        const bool second = serial != "first";
        const QString model = second ? "Model B" : "Model A";
        const QString device = second ? "beta" : "alpha";
        const QString version = second ? "17" : "16";
        std::printf("[ro.serialno]: [%s]\n[ro.product.model]: [%s]\n[ro.product.device]: [%s]\n[ro.build.version.release]: [%s]\n[ro.boot.verifiedbootstate]: [orange]\n[ro.build.display.id]: [OrangeOS-%s]\n[ro.build.date]: [Mon Oct  6 10:00:00 CST 2026]\n[ro.boot.slot_suffix]: [_a]\n[ro.soc.manufacturer]: [Qualcomm]\n[ro.board.platform]: [sm8650]\n[ro.product.board]: [kalama]\n[ro.boot.selinux]: [Enforcing]\n", serial.toLocal8Bit().constData(), model.toLocal8Bit().constData(), device.toLocal8Bit().constData(), version.toLocal8Bit().constData(), version.toLocal8Bit().constData());
        return 0;
    }
    const QString key = base == "adb" ? args.value(2) : args.value(1);
    const bool second = serial != "first";
    QString value;
    if (key == "ro.product.device" || key == "product") value = second ? "beta" : "alpha";
    else if (key == "ro.product.model") value = second ? "Model B" : "Model A";
    else if (key == "ro.build.version.release") value = second ? "17" : "16";
    else if (key == "ro.boot.slot_suffix") value = "_a";
    else if (key == "current-slot") value = "a";
    else if (key == "ro.boot.verifiedbootstate") value = "orange";
    else if (key == "unlocked") value = "yes";
    if (base == "fastboot") value = "(bootloader) " + key + ": " + value;
    value = object.value(key + ".value").toString(value);
    const int delay = object.value(key + ".delay").toInt();
    const int code = object.value(key + ".code").toInt();
    const bool useStderr = base == "fastboot" && !object.value(key + ".stdout").toBool();
    QTimer::singleShot(delay, &app, [&app, useStderr, value, code]() {
        const QByteArray bytes = value.toLocal8Bit();
        std::fprintf(useStderr ? stderr : stdout, "%s\n", bytes.constData());
        app.exit(code);
    });
    return app.exec();
}
int main(int argc, char **argv) {
    const QString base = QFileInfo(QString::fromLocal8Bit(argv[0])).baseName();
    if (base == "adb" || base == "fastboot" || base == "scrcpy") {
        QCoreApplication app(argc, argv); return helperMain(app);
    }
    QApplication app(argc, argv); app.setQuitOnLastWindowClosed(false);
    DeviceInformationTests tests; return QTest::qExec(&tests, argc, argv);
}
#include "deviceinformationtests.moc"
