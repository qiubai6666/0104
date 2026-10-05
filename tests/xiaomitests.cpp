#include <QtTest>
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QScreen>
#include <QTextStream>
#include <QUuid>
#include "xiaomiflashservice.h"
#include "xiaomiflashwindow.h"
#include "deviceoperationlease.h"
#include "resourceextractor.h"

namespace {
QString toolPath;
QString put(const QString &path, const QByteArray &data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) return {};
    return path;
}
QString artifactDir() {
    const QString root = qEnvironmentVariable("ORANGE_TEST_ARTIFACTS");
    if (root.isEmpty() || !QFileInfo(root).isDir())
        qFatal("ORANGE_TEST_ARTIFACTS must be an existing isolated TEMP directory");
    const QString path = root + '/' + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QDir().mkpath(path); return path;
}
QByteArray batch() {
    return "@echo off\r\nfastboot %* getvar product\r\nfastboot %* flash boot \"%~dp0images\\boot.img\"\r\nfastboot %* reboot\r\n";
}
QString makePackage(const QString &name = "rom") {
    const QString path = artifactDir() + '/' + name;
    put(path + "/images/boot.img", "test-image");
    for (const auto mode : {Xiaomi::Mode::Wipe, Xiaomi::Mode::KeepData, Xiaomi::Mode::WipeAndLock})
        put(path + '/' + Xiaomi::scriptName(mode), batch());
    return path;
}
int fakeFastboot(const QStringList &args) {
    const QString behavior = qEnvironmentVariable("XIAOMI_TEST_BEHAVIOR");
    QFile audit(qEnvironmentVariable("XIAOMI_TEST_AUDIT"));
    if (audit.open(QIODevice::Append)) audit.write((args.mid(1).join('|') + '\n').toUtf8());
    QTextStream out(stdout);
    if (args.contains("devices")) {
        if (behavior == "detect-timeout") QThread::msleep(3000);
        if (behavior == "detect-error") return 2;
        if (behavior == "no-device") return 0;
        if (behavior == "multiple-devices") out << "MOCK1\tfastboot\nMOCK2\tfastboot\n";
        else out << "MOCK1\tfastboot\n";
        out.flush(); return 0;
    }
    if (args.contains("getvar") && args.contains("product")) {
        if (behavior == "product-timeout") QThread::msleep(3000);
        if (behavior == "product-error") { out << "FAILED (remote: no product)\n"; out.flush(); return 1; }
        out << "product: fuxi\nFinished. Total time: 0.001s\n"; out.flush(); return 0;
    }
    const int flashIndex = args.indexOf("flash");
    if (flashIndex >= 0) {
        const QString partition = args.value(flashIndex + 1);
        if (behavior == "slow") QThread::msleep(700);
        if (behavior == "failed-zero") out << "FAILED (remote: denied)\n";
        else if (behavior == "failed-tail") out << "FAILED (remote: denied)";
        else if (behavior == "error-zero") out << "Error checking device product\n";
        else if (behavior == "mismatch") out << "Missmatching image and device\n";
        else if (behavior == "split-write") out << "Sending '" << partition << "' (1 KB) OKAY [0.001s]\nWriting '" << partition << "'\nOKAY [0.001s]\nFinished. Total time: 0.002s\n";
        else if (behavior == "no-writing") out << "Finished. Total time: 0.002s\n";
        else out << "Sending '" << partition << "' (1 KB) OKAY [0.001s]\nWriting '" << partition << "' OKAY [0.001s]\nFinished. Total time: 0.002s\n";
    }
    if (behavior == "nonzero" && args.contains("reboot")) return 3;
    out.flush(); return 0;
}
}
QString ResourceExtractor::getFastbootPath() { return toolPath; }

class XiaomiTests : public QObject {
    Q_OBJECT
private slots:
    void init() {
        QVERIFY(!DeviceOperationLease::owner());
        qputenv("XIAOMI_TEST_BEHAVIOR", "success");
        qputenv("XIAOMI_TEST_AUDIT", (artifactDir() + "/audit.txt").toUtf8());
    }
    void modeMapping() {
        QCOMPARE(Xiaomi::scriptName(Xiaomi::Mode::Wipe), QString("flash_all.bat"));
        QCOMPARE(Xiaomi::scriptName(Xiaomi::Mode::KeepData), QString("flash_all_except_storage.bat"));
        QCOMPARE(Xiaomi::scriptName(Xiaomi::Mode::WipeAndLock), QString("flash_all_lock.bat"));
    }
    void validPackage() {
        Xiaomi::Package p; QString error;
        QVERIFY2(Xiaomi::inspectPackage(makePackage(), Xiaomi::Mode::Wipe, &p, &error), qPrintable(error));
        QVERIFY(p.progressPlanValid);
        QCOMPARE(p.images.size(), 1); QCOMPARE(p.images[0].partition, QString("boot"));
        QCOMPARE(p.totalBytes, qint64(10));
    }
    void progressPlanIsOptional_data() {
        QTest::addColumn<QByteArray>("script"); QTest::addColumn<bool>("hasPlan");
        QTest::newRow("no-fastboot") << QByteArray("@echo off\r\necho vendor script\r\n") << false;
        QTest::newRow("no-percent-forwarding") << QByteArray("fastboot flash boot \"%~dp0images\\boot.img\"\r\n") << true;
        QTest::newRow("missing-image") << QByteArray("fastboot %* flash boot images\\missing.img\r\n") << false;
        QTest::newRow("commented-command") << QByteArray("@echo off\r\nrem fastboot flash boot missing.img\r\necho no-op\r\n") << false;
        QTest::newRow("variable-image") << QByteArray("set ROM=%~dp0images\r\nfastboot flash boot \"%ROM%\\boot.img\"\r\n") << false;
    }
    void progressPlanIsOptional() {
        QFETCH(QByteArray, script); QFETCH(bool, hasPlan);
        const QString path = makePackage(); put(path + "/flash_all.bat", script);
        Xiaomi::Package p; QString error;
        QVERIFY2(Xiaomi::inspectPackage(path, Xiaomi::Mode::Wipe, &p, &error), qPrintable(error));
        QCOMPARE(p.progressPlanValid, hasPlan);
        XiaomiFlashService service; service.configure(toolPath);
        QSignalSpy finished(&service, &XiaomiFlashService::finished);
        QSignalSpy progress(&service, &XiaomiFlashService::progress);
        QVERIFY(service.start(p, &error));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 15000);
        QVERIFY2(finished[0][0].toBool(), qPrintable(finished[0][1].toString()));
        if (!hasPlan) QCOMPARE(progress.first()[0].toInt(), -1);
    }
    void wrongDirectoryAndMissingMode() {
        Xiaomi::Package p; QString error;
        QVERIFY(!Xiaomi::inspectPackage(artifactDir() + "/absent", Xiaomi::Mode::Wipe, &p, &error));
        const QString root = artifactDir(); put(root + "/flash_all.bat", batch());
        QVERIFY(!Xiaomi::inspectPackage(root, Xiaomi::Mode::KeepData, &p, &error));
        QVERIFY(error.contains("flash_all_except_storage.bat"));
    }
    void runOutcomes_data() {
        QTest::addColumn<QByteArray>("behavior"); QTest::addColumn<bool>("success");
        QTest::newRow("success-special-path") << QByteArray("success") << true;
        QTest::newRow("failed-with-zero-exit") << QByteArray("failed-zero") << false;
        QTest::newRow("failed-without-final-newline") << QByteArray("failed-tail") << false;
        QTest::newRow("error-without-colon") << QByteArray("error-zero") << false;
        QTest::newRow("mismatching") << QByteArray("mismatch") << false;
        QTest::newRow("nonzero-exit") << QByteArray("nonzero") << false;
        QTest::newRow("split-writing-okay") << QByteArray("split-write") << true;
        QTest::newRow("no-writing-log") << QByteArray("no-writing") << true;
    }
    void runOutcomes() {
        QFETCH(QByteArray, behavior); QFETCH(bool, success);
        qputenv("XIAOMI_TEST_BEHAVIOR", behavior);
        Xiaomi::Package p; QString error;
        const QString path = makePackage(QString::fromUtf8("ROM 中文 & 50% ! (test)"));
        QVERIFY2(Xiaomi::inspectPackage(path, Xiaomi::Mode::Wipe, &p, &error), qPrintable(error));
        XiaomiFlashService service; service.configure(toolPath);
        QSignalSpy finished(&service, &XiaomiFlashService::finished);
        QSignalSpy progress(&service, &XiaomiFlashService::progress);
        QVERIFY2(service.start(p, &error), qPrintable(error));
        QVERIFY(service.isBusy()); QCOMPARE(DeviceOperationLease::owner(), &service);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 15000);
        QCOMPARE(finished[0][0].toBool(), success);
        QVERIFY(!service.isBusy()); QVERIFY(!DeviceOperationLease::owner());
        QFile audit(qEnvironmentVariable("XIAOMI_TEST_AUDIT")); QVERIFY(audit.open(QIODevice::ReadOnly));
        const auto trace = audit.readAll();
        QVERIFY(trace.contains("devices\n"));
        QVERIFY(trace.contains("-s|MOCK1|getvar|product"));
        // Only the read-only product probe is serial-selected; the BAT is unchanged.
        QVERIFY(!trace.contains("-s|MOCK1|flash"));
        QVERIFY2(trace.contains("flash|boot|"), trace.constData());
        QVERIFY2(trace.contains("ROM"), trace.constData());
        if (success) QCOMPARE(progress.last()[0].toInt(), 100);
        else for (const auto &value : progress) QVERIFY(value[0].toInt() < 100);
    }
    void allModesExecute_data() {
        QTest::addColumn<int>("mode");
        QTest::newRow("wipe") << int(Xiaomi::Mode::Wipe);
        QTest::newRow("keep-data") << int(Xiaomi::Mode::KeepData);
        QTest::newRow("wipe-and-lock") << int(Xiaomi::Mode::WipeAndLock);
    }
    void allModesExecute() {
        QFETCH(int, mode);
        Xiaomi::Package p; QString error;
        const auto selected = Xiaomi::Mode(mode);
        const QString path = makePackage();
        const QString marker = artifactDir() + "/selected-mode.txt";
        QByteArray script = batch();
        script += "echo " + Xiaomi::scriptName(selected).toUtf8() + " > \"" + QDir::toNativeSeparators(marker).toUtf8() + "\"\r\n";
        put(path + '/' + Xiaomi::scriptName(selected), script);
        QVERIFY2(Xiaomi::inspectPackage(path, selected, &p, &error), qPrintable(error));
        XiaomiFlashService service; service.configure(toolPath);
        QSignalSpy finished(&service, &XiaomiFlashService::finished);
        QVERIFY(service.start(p, &error));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 15000);
        QVERIFY2(finished[0][0].toBool(), qPrintable(finished[0][1].toString()));
        QFile result(marker); QVERIFY(result.open(QIODevice::ReadOnly));
        QVERIFY(result.readAll().contains(Xiaomi::scriptName(selected).toUtf8()));
    }
    void changedScriptBeforeStart() {
        const QString path = makePackage();
        Xiaomi::Package p; QString error;
        QVERIFY(Xiaomi::inspectPackage(path, Xiaomi::Mode::Wipe, &p, &error));
        put(path + "/flash_all.bat", batch() + "echo changed\r\n");
        XiaomiFlashService service; service.configure(toolPath);
        QSignalSpy finished(&service, &XiaomiFlashService::finished);
        QVERIFY(!service.start(p, &error));
        QCOMPARE(finished.size(), 0);
        QVERIFY(error.contains("变化"));
        QVERIFY(!QFileInfo::exists(qEnvironmentVariable("XIAOMI_TEST_AUDIT")));
        QVERIFY(!DeviceOperationLease::owner());
    }
    void preflightRejects_data() {
        QTest::addColumn<QByteArray>("behavior");
        QTest::newRow("none") << QByteArray("no-device");
        QTest::newRow("multiple") << QByteArray("multiple-devices");
        QTest::newRow("failed") << QByteArray("detect-error");
        QTest::newRow("devices-timeout") << QByteArray("detect-timeout");
        QTest::newRow("product-timeout") << QByteArray("product-timeout");
        QTest::newRow("product-error") << QByteArray("product-error");
    }
    void preflightRejects() {
        QFETCH(QByteArray, behavior);
        qputenv("XIAOMI_TEST_BEHAVIOR", behavior);
        Xiaomi::Package p; QString error;
        QVERIFY(Xiaomi::inspectPackage(makePackage(), Xiaomi::Mode::Wipe, &p, &error));
        XiaomiFlashService service; service.configure(toolPath);
        service.m_probeTimer.setInterval(500);
        QSignalSpy finished(&service, &XiaomiFlashService::finished);
        QSignalSpy progress(&service, &XiaomiFlashService::progress);
        QVERIFY(service.start(p, &error)); QVERIFY(service.isChecking());
        QVERIFY(!service.hasStartedScript()); QVERIFY(progress.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
        QVERIFY(!finished[0][0].toBool()); QVERIFY(!service.isBusy());
        QVERIFY(!service.hasStartedScript()); QVERIFY(progress.isEmpty());
        QVERIFY(!DeviceOperationLease::owner());
        QFile audit(qEnvironmentVariable("XIAOMI_TEST_AUDIT"));
        if (audit.open(QIODevice::ReadOnly)) QVERIFY(!audit.readAll().contains("flash|"));
        // The same service can retry immediately; no stale cancelled probe may finish it.
        qputenv("XIAOMI_TEST_BEHAVIOR", "success");
        service.m_probeTimer.setInterval(5000);
        QVERIFY(service.start(p, &error));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 10000);
        QVERIFY(finished[1][0].toBool());
    }
    void cancelReadOnlyProbe() {
        for (const QByteArray &behavior : {QByteArray("detect-timeout"), QByteArray("product-timeout")}) {
            qputenv("XIAOMI_TEST_BEHAVIOR", behavior);
            Xiaomi::Package p; QString error;
            QVERIFY(Xiaomi::inspectPackage(makePackage(), Xiaomi::Mode::Wipe, &p, &error));
            XiaomiFlashService service; service.configure(toolPath);
            QSignalSpy finished(&service, &XiaomiFlashService::finished);
            QSignalSpy progress(&service, &XiaomiFlashService::progress);
            QVERIFY(service.start(p, &error));
            if (behavior == "product-timeout") QTRY_VERIFY(service.m_checkProduct);
            service.cancelCheck();
            QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 2000);
            QVERIFY(!finished[0][0].toBool()); QVERIFY(finished[0][1].toString().contains("取消"));
            QVERIFY(progress.isEmpty()); QVERIFY(!service.isBusy()); QVERIFY(!DeviceOperationLease::owner());
        }
    }
    void scriptChangedDuringProbe() {
        qputenv("XIAOMI_TEST_BEHAVIOR", "product-timeout");
        const QString rom = makePackage(); Xiaomi::Package p; QString error;
        QVERIFY(Xiaomi::inspectPackage(rom, Xiaomi::Mode::Wipe, &p, &error));
        XiaomiFlashService service; service.configure(toolPath);
        QSignalSpy finished(&service, &XiaomiFlashService::finished);
        QVERIFY(service.start(p, &error));
        QTRY_VERIFY(service.m_checkProduct);
        QVERIFY(!put(p.script, "echo changed\r\n").isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 6000);
        QVERIFY(!finished[0][0].toBool()); QVERIFY(finished[0][1].toString().contains("变化"));
        QVERIFY(!service.hasStartedScript()); QVERIFY(!DeviceOperationLease::owner());
    }
    void echoedVendorScript_data() {
        QTest::addColumn<bool>("echoOn");
        QTest::newRow("default-echo") << false;
        QTest::newRow("explicit-echo-on") << true;
    }
    void echoedVendorScript() {
        QFETCH(bool, echoOn);
        const QString rom = makePackage();
        QByteArray script = echoOn ? "@echo on\r\n" : "";
        script += "fastboot %* getvar product 2>&1 | findstr /r /c:\"^product: *fuxi\" || echo Missmatching image and device\r\n";
        script += "fastboot %* getvar product 2>&1 | findstr /r /c:\"^product: *fuxi\" || exit /B 1\r\n";
        script += "fastboot %* flash boot \"%~dp0images\\boot.img\" || @echo \"Flash boot error\" && exit /B 1\r\n";
        script += "fastboot %* reboot || @echo \"Reboot error\" && exit /B 1\r\n";
        QVERIFY(!put(rom + "/flash_all.bat", script).isEmpty());
        Xiaomi::Package p; QString error;
        QVERIFY(Xiaomi::inspectPackage(rom, Xiaomi::Mode::Wipe, &p, &error));
        XiaomiFlashService service; service.configure(toolPath);
        QSignalSpy finished(&service, &XiaomiFlashService::finished);
        QSignalSpy warnings(&service, &XiaomiFlashService::warning);
        QSignalSpy progress(&service, &XiaomiFlashService::progress);
        QVERIFY(service.start(p, &error));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QVERIFY2(finished[0][0].toBool(), qPrintable(finished[0][1].toString()));
        QVERIFY(warnings.isEmpty());
        bool advanced = false;
        for (const auto &entry : progress) if (entry[0].toInt() > 0 && entry[0].toInt() < 100) advanced = true;
        QVERIFY(advanced); QCOMPARE(progress.last()[0].toInt(), 100);
    }
    void commandEchoIsNotAResult() {
        XiaomiFlashService service; service.m_elapsed.start();
        service.m_package.progressPlanValid = true;
        service.m_package.images = {{"boot", 1024}};
        service.m_package.totalBytes = 1024;
        QSignalSpy warnings(&service, &XiaomiFlashService::warning);
        for (const QString &line : {
            QStringLiteral("C:\\Users\\Administrator\\qiubai>fastboot getvar product 2>&1 | findstr /r /c:\"^product: *fuxi\" || echo Missmatching image and device"),
            QStringLiteral("fastboot flash boot images\\boot.img || @echo \"Flash boot error\" && exit 1"),
            QStringLiteral("if 0 equ 0 (fastboot flash boot images\\boot.img || @echo error)"),
            QStringLiteral("echo \"Antirollback check error\" && exit /B 1")}) {
            service.handleLine(line);
        }
        QVERIFY(!service.m_failed); QVERIFY(warnings.isEmpty());
        QVERIFY(!service.m_mismatchTimer.isActive());
        QSignalSpy progress(&service, &XiaomiFlashService::progress);
        service.handleLine("Sending 'boot' (1 KB) OKAY [0.001s]");
        service.handleLine("Writing 'boot' OKAY [0.001s]");
        service.handleLine("Finished. Total time: 0.002s");
        QCOMPARE(progress.last()[0].toInt(), 99);
        service.handleLine("Flash boot error");
        QVERIFY(service.m_failed); QCOMPARE(warnings.size(), 1);
    }
    void sparseAndMultipleImagesProgress() {
        XiaomiFlashService service;
        service.m_package.progressPlanValid = true;
        service.m_package.images = {{"boot", 1024}, {"system", 3072}, {"system", 1024}};
        service.m_package.totalBytes = 5120;
        service.m_elapsed.start();
        QSignalSpy progress(&service, &XiaomiFlashService::progress);
        QSignalSpy info(&service, &XiaomiFlashService::progressInfo);
        service.handleLine("Sending 'boot' (1 KB)");
        service.handleLine("boot: 0.5 KB/1 KB (50.0%) [raw] 2.5 MB/s");
        QCOMPARE(progress.last()[0].toInt(), 10);
        QVERIFY(info.last()[0].toString().contains("2.5MB/s"));
        service.handleLine("Writing 'boot'"); service.handleLine("OKAY [0.001s]");
        service.handleLine("Finished. Total time: 0.002s");
        QCOMPARE(progress.last()[0].toInt(), 20);
        service.handleLine("Sending sparse 'system' 1/3 (1 KB)");
        service.handleLine("Writing 'system'");
        QCOMPARE(progress.last()[0].toInt(), 40);
        service.handleLine("Sending sparse 'system' 2/3 (1 KB)");
        service.handleLine("Writing 'system'");
        QCOMPARE(progress.last()[0].toInt(), 60);
        service.handleLine("Sending sparse 'system' 3/3 (1 KB)");
        service.handleLine("Writing 'system'"); service.handleLine("Finished. Total time: 0.1s");
        QCOMPARE(progress.last()[0].toInt(), 80);
        service.handleLine("Finished. Total time: 0.1s"); // getvar/erase is not another image
        QCOMPARE(service.m_completedBytes, qint64(4096));
        service.handleLine("Sending 'system' (1 KB)");
        service.handleLine("Writing 'system'"); service.handleLine("Finished. Total time: 0.1s");
        QCOMPARE(service.m_completedBytes, qint64(5120));
        QCOMPARE(progress.last()[0].toInt(), 99); // 100 only after process exit
        QCOMPARE(service.m_package.images[0].bytes, qint64(1024)); // immutable plan
    }
    void failureKeywordsStopProgress() {
        for (const QString &line : {QStringLiteral("FAILED (remote: denied)"),
                                   QStringLiteral("Error checking device product"),
                                   QStringLiteral("设备检查失败"), QStringLiteral("刷机错误"),
                                   QStringLiteral("Missmatching image and device")}) {
            XiaomiFlashService service; service.m_elapsed.start();
            QSignalSpy warnings(&service, &XiaomiFlashService::warning);
            QSignalSpy progress(&service, &XiaomiFlashService::progress);
            service.handleLine(line);
            QVERIFY2(service.m_failed, qPrintable(line));
            QCOMPARE(warnings.size(), 1);
            service.handleLine("system: 100 MB/100 MB (100.0%)");
            QVERIFY(progress.isEmpty());
            service.handleLine(line);
            QCOMPARE(warnings.size(), 1);
        }
    }
    void fallbackProgressAndReset() {
        XiaomiFlashService service; service.m_elapsed.start();
        QSignalSpy progress(&service, &XiaomiFlashService::progress);
        service.handleLine("target reported max download size");
        QCOMPARE(progress.last()[0].toInt(), 1);
        service.handleLine("Erasing userdata");
        QCOMPARE(progress.last()[0].toInt(), 3);
        service.handleLine("Sending system");
        QCOMPARE(progress.last()[0].toInt(), 5);
        service.handleLine("Rebooting");
        QCOMPARE(progress.last()[0].toInt(), 5);
        service.handleLine("system: 95 MB/100 MB (95.0%) [raw] 40 MB/s");
        QCOMPARE(progress.last()[0].toInt(), 95);
        service.handleLine("vendor: 10 MB/100 MB (10.0%) [raw] 40 MB/s");
        QCOMPARE(progress.last()[0].toInt(), 10);
        service.handleLine("boot: 1 KB/1 KB (100.0%)");
        QCOMPARE(progress.last()[0].toInt(), 10);
    }
    void mismatchTimerKeepsLeaseAndCancelsOnNewLine() {
        XiaomiFlashService service; service.m_busy = true; service.m_elapsed.start();
        QVERIFY(DeviceOperationLease::acquire(&service));
        service.m_mismatchTimer.setInterval(30);
        QSignalSpy warnings(&service, &XiaomiFlashService::warning);
        service.handleLine("Missmatching image and device");
        QVERIFY(service.m_mismatchTimer.isActive());
        service.handleLine("new output");
        QVERIFY(!service.m_mismatchTimer.isActive());
        QTest::qWait(60); QCOMPARE(warnings.size(), 1); // immediate error warning only
        service.handleLine("Missmatching image and device");
        QTRY_COMPARE_WITH_TIMEOUT(warnings.size(), 2, 1000);
        QVERIFY(service.isBusy()); QCOMPARE(DeviceOperationLease::owner(), &service);
        service.finish(false, "test ended"); QVERIFY(!DeviceOperationLease::owner());
    }
    void cancelledLockConfirmation() {
        XiaomiFlashWindow window; window.show();
        window.findChild<QLineEdit *>("XiaomiFlashScriptPathTextBox")->setText(makePackage());
        window.findChild<QRadioButton *>("WipeAndLockBLCheckBox")->click();
        QTimer timer; timer.setInterval(10); bool inspected = false;
        connect(&timer, &QTimer::timeout, &window, [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog || dialog->objectName() != "XiaomiFlashConfirmDialog") return;
            auto general = dialog->findChild<QCheckBox *>("XiaomiConfirmAgreement");
            auto lock = dialog->findChild<QCheckBox *>("XiaomiConfirmLockAgreement");
            QVERIFY(lock); auto ok = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
            general->setChecked(true); QVERIFY(!ok->isEnabled()); lock->setChecked(true); QVERIFY(ok->isEnabled());
            inspected = true; dialog->reject(); timer.stop();
        }); timer.start();
        window.findChild<QPushButton *>("StartXiaomiFlashButton")->click();
        QVERIFY(inspected); QVERIFY(!window.isBusy()); QVERIFY(!DeviceOperationLease::owner());
        QVERIFY(!QFileInfo::exists(qEnvironmentVariable("XIAOMI_TEST_AUDIT")));
    }
    void mutualExclusion() {
        QObject other; QVERIFY(DeviceOperationLease::acquire(&other));
        Xiaomi::Package p; QString error;
        QVERIFY(Xiaomi::inspectPackage(makePackage(), Xiaomi::Mode::Wipe, &p, &error));
        XiaomiFlashService service; service.configure(toolPath);
        QVERIFY(!service.start(p, &error)); QVERIFY(!service.isBusy());
        QCOMPARE(DeviceOperationLease::owner(), &other); DeviceOperationLease::release(&other);
    }
    void missingTools() {
        Xiaomi::Package p; QString error;
        QVERIFY(Xiaomi::inspectPackage(makePackage(), Xiaomi::Mode::Wipe, &p, &error));
        XiaomiFlashService service; service.configure(artifactDir() + "/absent.exe");
        QVERIFY(!service.start(p, &error)); QVERIFY(!DeviceOperationLease::owner());
    }
    void defaultGeometryAndCentering_data() {
        QTest::addColumn<bool>("withLauncher");
        QTest::newRow("launcher-screen") << true;
        QTest::newRow("standalone-screen") << false;
    }
    void defaultGeometryAndCentering() {
        QFETCH(bool, withLauncher);
        QWidget launcher;
        QScreen *targetScreen = QApplication::primaryScreen();
        QVERIFY(targetScreen);
        const QRect available = targetScreen->availableGeometry();
        launcher.resize(200, 100);
        launcher.move(available.topLeft() + QPoint(20, 30));
        if (withLauncher) launcher.show();

        XiaomiFlashWindow window(withLauncher ? &launcher : nullptr);
        QCOMPARE(window.minimumSize(), QSize(779, 656));
        window.show();
        QTRY_COMPARE(window.size(), QSize(779, 656));
        auto centered = [&] {
            QScreen *screen = withLauncher ? launcher.screen() : window.screen();
            if (!screen) return false;
            const QPoint delta = window.frameGeometry().center() - screen->availableGeometry().center();
            return qAbs(delta.x()) <= 1 && qAbs(delta.y()) <= 1;
        };
        QTRY_VERIFY(centered());

        window.hide();
        window.move(available.topLeft() + QPoint(30, 40));
        window.show();
        QTRY_VERIFY(centered());
    }
    void uiAndCloseProtection() {
        qputenv("XIAOMI_TEST_BEHAVIOR", "slow");
        XiaomiFlashWindow window;
        window.show();
        window.findChild<QLineEdit *>("XiaomiFlashScriptPathTextBox")->setText(makePackage());
        auto start = window.findChild<QPushButton *>("StartXiaomiFlashButton"); QVERIFY(start);
        auto wipe = window.findChild<QRadioButton *>("CompleteWipeCheckBox");
        auto keep = window.findChild<QRadioButton *>("KeepDataCheckBox");
        auto lock = window.findChild<QRadioButton *>("WipeAndLockBLCheckBox");
        QVERIFY(wipe->isChecked()); keep->click(); QVERIFY(keep->isChecked()); QVERIFY(!wipe->isChecked());
        lock->click(); QVERIFY(lock->isChecked()); QVERIFY(!keep->isChecked()); wipe->click();
        QTimer timer; timer.setInterval(10);
        connect(&timer, &QTimer::timeout, &window, [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog || dialog->objectName() != "XiaomiFlashConfirmDialog") return;
            auto agreement = dialog->findChild<QCheckBox *>("XiaomiConfirmAgreement");
            auto ok = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
            QVERIFY(!ok->isEnabled()); agreement->setChecked(true); QVERIFY(ok->isEnabled());
            ok->click(); timer.stop();
        }); timer.start(); start->click();
        QVERIFY(window.isBusy()); QVERIFY(!start->isEnabled()); QVERIFY(!window.close());
        auto cancel = window.findChild<QPushButton *>("CancelXiaomiCheckButton"); QVERIFY(cancel);
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<XiaomiFlashService *>()->hasStartedScript(), 5000);
        QVERIFY(cancel->isHidden());
        const auto log = window.findChild<QPlainTextEdit *>("XiaomiFlashLogTextBox");
        QVERIFY(log->height() >= 240);
        QVERIFY(log->height() >= window.height() / 3);
        QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 15000);
        QVERIFY(start->isEnabled());
        QVERIFY(window.findChild<QPlainTextEdit *>("XiaomiFlashLogTextBox")->toPlainText().contains("完成"));
        const QString image = qEnvironmentVariable("ORANGE_TEST_ARTIFACTS") + "/xiaomi-ui.png";
        QVERIFY(window.grab().save(image));
        QVERIFY(window.close());
    }
};
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    if (QFileInfo(app.applicationFilePath()).fileName().compare("fastboot.exe", Qt::CaseInsensitive) == 0)
        return fakeFastboot(app.arguments());
    toolPath = artifactDir() + "/fastboot.exe";
    if (!QFile::copy(app.applicationFilePath(), toolPath)) return 2;
    XiaomiTests tests; return QTest::qExec(&tests, argc, argv);
}
#include "xiaomitests.moc"
