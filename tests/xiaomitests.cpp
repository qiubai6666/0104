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
#ifdef Q_OS_WIN
#include <windows.h>
#endif
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
// Keep Unicode fixture paths as ASCII across the Windows ANSI environment.
QString testPathEnvironment(const char *name) {
    return QString::fromUtf8(QByteArray::fromBase64(qgetenv(name)));
}
int fakeFastboot(const QStringList &args) {
    const QString behavior = qEnvironmentVariable("XIAOMI_TEST_BEHAVIOR");
    QFile audit(testPathEnvironment("XIAOMI_TEST_AUDIT_BASE64"));
    if (audit.open(QIODevice::Append)) {
        audit.write((args.mid(1).join('|') + '\n').toUtf8());
        audit.close();
    }
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
    const int partitionTypeIndex = args.indexOf("getvar");
    if (partitionTypeIndex >= 0 && partitionTypeIndex + 1 < args.size() &&
        args[partitionTypeIndex + 1].startsWith("partition-type:", Qt::CaseInsensitive)) {
        const QString partition = args[partitionTypeIndex + 1].section(':', 1);
        if (behavior.startsWith("cota") && qEnvironmentVariable("XIAOMI_TEST_COTA").split(',').contains(partition)) out << "partition-type:" << partition << ": raw\n";
        out << "Finished. Total time: 0.001s\n"; out.flush(); return 0;
    }
    const int eraseIndex = args.indexOf("erase");
    if (eraseIndex >= 0 && eraseIndex + 1 < args.size()) {
        const QString partition = args[eraseIndex + 1];
        if (behavior.startsWith("cota") &&
            (!qEnvironmentVariable("XIAOMI_TEST_COTA").split(',').contains(partition) ||
             behavior == "cota-erase-failure")) {
            out << "FAILED (remote: denied)\n"; out.flush(); return 1;
        }
        out << "Erasing '" << partition << "' OKAY [0.001s]\n";
        out.flush(); return 0;
    }
    const int flashIndex = args.indexOf("flash");
    if (flashIndex >= 0) {
        const QString partition = args.value(flashIndex + 1);
        if (behavior.startsWith("cota")) {
            const QFileInfo actual(args.value(flashIndex + 2));
            const QFileInfo expected(testPathEnvironment("XIAOMI_TEST_IMAGE_BASE64"));
            if (!actual.isFile() || actual.canonicalFilePath() != expected.canonicalFilePath()) {
                out << "FAILED (image path mismatch)\n"; out.flush(); return 1;
            }
        }
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
        qputenv("XIAOMI_TEST_AUDIT_BASE64", (artifactDir() + "/audit.txt").toUtf8().toBase64());
    }
    void modeMapping() {
        QCOMPARE(Xiaomi::scriptName(Xiaomi::Mode::Wipe), QString("flash_all.bat"));
        QCOMPARE(Xiaomi::scriptName(Xiaomi::Mode::KeepData), QString("flash_all_except_storage.bat"));
        QCOMPARE(Xiaomi::scriptName(Xiaomi::Mode::WipeAndLock), QString("flash_all_lock.bat"));
    }
    void keepDataRepairsSwappedCotaTargets_data() {
        QTest::addColumn<QByteArray>("partitions");
        QTest::addColumn<bool>("eraseFails");
        QTest::newRow("both") << QByteArray("opcust,opconfig") << false;
        QTest::newRow("opcust-only") << QByteArray("opcust") << false;
        QTest::newRow("opconfig-only") << QByteArray("opconfig") << false;
        QTest::newRow("neither") << QByteArray() << false;
        QTest::newRow("real-erase-failure") << QByteArray("opcust,opconfig") << true;
    }
    void keepDataRepairsSwappedCotaTargets() {
        QFETCH(QByteArray, partitions); QFETCH(bool, eraseFails);
        qputenv("XIAOMI_TEST_COTA", partitions);
        qputenv("XIAOMI_TEST_BEHAVIOR", eraseFails ? "cota-erase-failure" : "cota");
        const QString path = makePackage("keep_data_cota");
        put(path + "/flash_all_except_storage.bat",
            "@echo off\r\n"
            "fastboot %* getvar partition-type:opcust 2>&1 | findstr /r /c:\"^partition-type:opcust: raw\"\r\n"
            "if %errorlevel% equ 0 (\r\n"
            "fastboot %* erase opconfig || exit /B 1\r\n"
            ")\r\n"
            "fastboot %* getvar partition-type:opconfig 2>&1 | findstr /r /c:\"^partition-type:opconfig: raw\"\r\n"
            "if %errorlevel% equ 0 (\r\n"
            "fastboot %* erase opcust || exit /B 1\r\n"
            ")\r\n"
            "fastboot %* flash boot \"%~dp0images\\boot.img\"\r\n"
            "fastboot %* reboot\r\n");
        QFile before(path + "/flash_all_except_storage.bat");
        QVERIFY(before.open(QIODevice::ReadOnly));
        const QByteArray originalBytes = before.readAll(); before.close();
        qputenv("XIAOMI_TEST_IMAGE_BASE64", (path + "/images/boot.img").toUtf8().toBase64());
        Xiaomi::Package p; QString error;
        QVERIFY2(Xiaomi::inspectPackage(path, Xiaomi::Mode::KeepData, &p, &error), qPrintable(error));
        XiaomiFlashService service; service.configure(toolPath);
        QSignalSpy logs(&service, &XiaomiFlashService::log);
        QSignalSpy finished(&service, &XiaomiFlashService::finished);
        QVERIFY(service.start(p, &error));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 15000);
#ifdef Q_OS_WIN
        const auto nativePath=QDir::toNativeSeparators(path).toStdWString();
        const DWORD length=GetShortPathNameW(nativePath.c_str(),nullptr,0);
        QString compatible=QString::fromStdWString(nativePath);
        if(length>0) {
            std::wstring buffer(length,L'\0');
            const DWORD count=GetShortPathNameW(nativePath.c_str(),buffer.data(),length);
            if(count>0 && count<length) compatible=QString::fromWCharArray(buffer.c_str(),int(count));
        }
        const QRegularExpression safePath(R"(^[A-Za-z]:[\\/][A-Za-z0-9_.~\\/\-]+$)");
        if(!safePath.match(compatible).hasMatch()) {
            QVERIFY(!finished[0][0].toBool());
            QVERIFY(finished[0][1].toString().contains(QStringLiteral("无法为修正版脚本生成安全的刷机包路径")));
            QFile audit(testPathEnvironment("XIAOMI_TEST_AUDIT_BASE64")); QVERIFY(audit.open(QIODevice::ReadOnly));
            const auto trace=audit.readAll(); QVERIFY(!trace.contains("erase|")); QVERIFY(!trace.contains("flash|"));
            QVERIFY(service.m_runtimeScript.isEmpty()); QVERIFY(!DeviceOperationLease::owner());
            QFile original(p.script); QVERIFY(original.open(QIODevice::ReadOnly)); QCOMPARE(original.readAll(),originalBytes);
            QSKIP("No safe ASCII/8.3 fixture path on this volume; verified rejection without any erase/flash");
        }
#endif
        QCOMPARE(finished[0][0].toBool(), !eraseFails);
        QFile audit(testPathEnvironment("XIAOMI_TEST_AUDIT_BASE64")); QVERIFY(audit.open(QIODevice::ReadOnly));
        const QByteArray trace = audit.readAll();
        QCOMPARE(trace.contains("erase|opcust\n"), partitions.split(',').contains("opcust"));
        QCOMPARE(trace.contains("erase|opconfig\n"), !eraseFails && partitions.split(',').contains("opconfig"));
        QVERIFY(!trace.contains("erase|userdata"));
        QVERIFY(!trace.contains("erase|metadata"));
        QCOMPARE(trace.contains("flash|boot|"), !eraseFails);
        bool repairedLog = false;
        for (const auto &entry : logs) repairedLog |= entry[0].toString().contains("修正脚本中的 COTA");
        QVERIFY(repairedLog);
        QVERIFY(QFileInfo::exists(p.script));
        QFile original(p.script); QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), originalBytes);
        QVERIFY(service.m_runtimeScript.isEmpty());
    }

    void transferRateFromFastbootTiming_data() {
        QTest::addColumn<QStringList>("lines");
        QTest::addColumn<bool>("hasPlan");
        for (const bool plan : {false, true}) {
            const QByteArray suffix = plan ? "-plan" : "-fallback";
            QTest::newRow(("same-line" + suffix).constData())
                << QStringList{"Sending 'super' (4 MB) OKAY [2.000s]"} << plan;
            QTest::newRow(("split-line" + suffix).constData())
                << QStringList{"Sending 'super' (4096 KB)", "OKAY [  2.000s]"} << plan;
            QTest::newRow(("sparse" + suffix).constData())
                << QStringList{"Sending sparse 'super' 1/5 (4096 KB) OKAY [ 2.000s]"} << plan;
        }
    }
    void transferRateFromFastbootTiming() {
        QFETCH(QStringList, lines); QFETCH(bool, hasPlan);
        XiaomiFlashService service; service.m_elapsed.start();
        service.m_package.progressPlanValid = hasPlan;
        service.m_package.images = {{"super", 4 * 1024 * 1024}};
        service.m_package.totalBytes = 4 * 1024 * 1024;
        QSignalSpy info(&service, &XiaomiFlashService::progressInfo);
        for (const auto &line : lines) service.handleLine(line);
        QVERIFY(!info.isEmpty());
        QCOMPARE(service.m_transferRate, QString("2MB/s"));
        service.handleLine("Sending 'super' (0 KB) OKAY [0.001s]");
        service.handleLine("Sending 'super' (4 MB) OKAY [0.000s]");
        service.handleLine("Writing 'super' OKAY [10.000s]");
        service.handleLine("Finished. Total time: 20.000s");
        QVERIFY(info.last()[0].toString().startsWith("2MB/s"));
        service.handleLine("Sending 'super' (4 MB)");
        service.handleLine("Writing 'super'");
        service.handleLine("OKAY [8.000s]");
        QCOMPARE(service.m_transferRate, QString("2MB/s"));
    }

    void cotaFailuresAreNotIgnored() {
        XiaomiFlashService service;
        service.handleLine("C:\\ROM>fastboot getvar partition-type:opcust 2>&1 | findstr raw");
        service.handleLine("FAILED (remote: erase denied)");
        QVERIFY(service.m_failed);
        service.handleLine("Sending 'super' (4 MB) OKAY [2.000s]");
        QCOMPARE(service.m_transferRate, QString("0MB/s"));
    }

    void cotaRepairPreservesBytesAndScope_data() {
        QTest::addColumn<int>("variant");
        QTest::newRow("swapped-bom-crlf") << 0;
        QTest::newRow("already-correct") << 1;
        QTest::newRow("comment-only") << 2;
        QTest::newRow("wipe-mode") << 3;
        QTest::newRow("different-guard") << 4;
        QTest::newRow("mismatched-filter") << 5;
        QTest::newRow("changed-after-inspection") << 6;
    }
    void cotaRepairPreservesBytesAndScope() {
        QFETCH(int, variant);
        const QString path = makePackage("cota_bytes");
        QByteArray bytes = QByteArray::fromHex("efbbbf") + "@echo off\r\nrem " +
            QByteArray::fromHex("b2e2cad4") + "\r\n";
        QByteArray block =
            "fastboot %* getvar partition-type:opcust 2>&1 | findstr /r /c:\"^partition-type:opcust: raw\"\r\n"
            "if %errorlevel% equ 0 (\r\n"
            "fastboot %* erase opconfig || @echo \"Erase opcust error\" && exit /B 1\r\n"
            ")\r\n";
        if (variant == 1) block.replace("erase opconfig", "erase opcust");
        if (variant == 2) block.replace("fastboot %* getvar", "rem fastboot %* getvar");
        if (variant == 4) block.replace("if %errorlevel% equ 0 (", "if %errorlevel% equ 1 (");
        if (variant == 5) block.replace("^partition-type:opcust", "^partition-type:opconfig");
        bytes += block;
        const auto mode = variant == 3 ? Xiaomi::Mode::Wipe : Xiaomi::Mode::KeepData;
        const QString script = path + '/' + Xiaomi::scriptName(mode);
        QVERIFY(!put(script, bytes).isEmpty());
        XiaomiFlashService service; QString error;
        QVERIFY(Xiaomi::inspectPackage(path, mode, &service.m_package, &error));
        if (variant == 6) QVERIFY(!put(script, bytes + "rem changed\r\n").isEmpty());
        QCOMPARE(service.prepareRuntimeScript(&error), variant != 6);
        if (variant == 0) {
            QVERIFY(!service.m_runtimeScript.isEmpty());
            QFile runtime(service.m_runtimeScript); QVERIFY(runtime.open(QIODevice::ReadOnly));
            QByteArray expected = bytes; expected.replace("erase opconfig", "erase opcust");
            QCOMPARE(runtime.readAll(), expected); runtime.close();
            service.cleanupRuntimeScript();
        } else QVERIFY(service.m_runtimeScript.isEmpty());
        QFile original(script); QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), variant == 6 ? bytes + "rem changed\r\n" : bytes);
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
        QFile audit(testPathEnvironment("XIAOMI_TEST_AUDIT_BASE64")); QVERIFY(audit.open(QIODevice::ReadOnly));
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
        const QString marker = path + "/selected-mode.txt";
        QByteArray script = batch();
        // Use cmd's Unicode script directory, not an embedded UTF-8 path in BAT.
        script += "echo " + Xiaomi::scriptName(selected).toUtf8() + " > \"%~dp0selected-mode.txt\"\r\n";
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
        QVERIFY(!QFileInfo::exists(testPathEnvironment("XIAOMI_TEST_AUDIT_BASE64")));
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
        QFile audit(testPathEnvironment("XIAOMI_TEST_AUDIT_BASE64"));
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
        QVERIFY(!QFileInfo::exists(testPathEnvironment("XIAOMI_TEST_AUDIT_BASE64")));
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
