#include "deviceoperationlease.h"
#include "ougacommandrunner.h"
#include "ougapackage.h"
#include "ougapreparation.h"
#include "resourceextractor.h"
#include <QCoreApplication>
#include <QDir>
#include <QCryptographicHash>
#include <QtEndian>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QUuid>
#include <QtTest>
#include <cstdio>

namespace {
bool put(const QString &path, const QByteArray &bytes) {
  if (!QDir().mkpath(QFileInfo(path).absolutePath()))
    return false;
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray read(const QString &path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
QByteArray helpText(const QString &kind) {
  QByteArray text =
      "lpmake - command-line tool for creating Android Logical "
      "Partition images.\nUsage:\n--metadata-size --metadata-slots "
      "--block-size --super-name --device --sparse --output "
      "--group --partition --image\n";
  if (kind == "no-banner")
    text = "Usage: --metadata-size --metadata-slots --device --output\n";
  if (kind == "no-device")
    text.replace("--device ", "");
  if (kind == "failed")
    text += "FAILED (injected help error)\n";
  return text;
}
bool sourcePackage(const QString &directory) {
  const QJsonObject definition{
      {"block_devices",
       QJsonArray{QJsonObject{
           {"name", "super"}, {"size", "8388608"}, {"alignment", "4096"}}}},
      {"groups", QJsonArray{QJsonObject{{"name", "test_group"},
                                        {"maximum_size", "4194304"}}}},
      {"partitions", QJsonArray{QJsonObject{{"name", "system_a"},
                                            {"size", "4096"},
                                            {"group_name", "test_group"},
                                            {"path", "system.img"}},
                                QJsonObject{{"name", "vendor_a"},
                                            {"size", "4096"},
                                            {"group_name", "test_group"},
                                            {"path", "vendor.img"}}}}};
  return put(directory + "/system.img", QByteArray(4096, 's')) &&
         put(directory + "/vendor.img", QByteArray(4096, 'v')) &&
         put(directory + "/super_def.json", QJsonDocument(definition).toJson());
}
int localCommand(const QString &tool, const QStringList &args,
                 const QString &directory, QByteArray *output) {
  QProcess process;
  process.setWorkingDirectory(directory);
  process.start(tool, args);
  if (!process.waitForStarted(5000) || !process.waitForFinished(15000)) {
    *output = process.errorString().toUtf8();
    return -1;
  }
  *output = process.readAllStandardOutput() + process.readAllStandardError();
  return process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
}
} // namespace

class OugaDependencyTests : public QObject {
  Q_OBJECT
  QString root, directory, resources, neil, realTools;

private slots:
  void initTestCase() {
    root = QDir::fromNativeSeparators(qEnvironmentVariable("ORANGE_TEST_ARTIFACTS"));
    QVERIFY2(
        !root.isEmpty() && QFileInfo(root).isAbsolute(),
        "Set ORANGE_TEST_ARTIFACTS to a new project-external TEMP directory");
    // Keep offline fixtures within the bundled legacy tool's Win32 path limit.
    root += "/deps-" + QUuid::createUuid().toString(QUuid::Id128);
    QVERIFY(QDir().mkpath(root));
    resources = root + "/extracted/qiubai";
    neil = root + "/extracted/Neil.jpg";
    // Never call the default overload: it manages the user's real AppData.
    QVERIFY(!QFileInfo::exists(resources));
    QVERIFY(ResourceExtractor::extractResources(resources, neil));
#ifdef ORANGE_TEST_REAL_RESOURCES
    realTools = resources;
#else
    realTools = qEnvironmentVariable("ORANGE_BUNDLED_TOOL_ROOT");
#endif
    QVERIFY(realTools.isEmpty() || QFileInfo(realTools).isAbsolute());
  }
  void init() {
    directory = root + "/" + QTest::currentTestFunction() + "-" +
                QUuid::createUuid().toString(QUuid::Id128);
    QVERIFY(QDir().mkpath(directory));
  }
  void cleanup() {
    for (const char *key :
         {"ORANGE_DEPENDENCY_MOCK", "ORANGE_LPMAKE_HELP_KIND",
          "ORANGE_LPMAKE_HELP_STATUS", "ORANGE_LPMAKE_MARKER",
          "ORANGE_LPMAKE_GENERATE_STATUS", "ORANGE_LPMAKE_GENERATE_OUTPUT"})
      qunsetenv(key);
    QVERIFY(!DeviceOperationLease::owner());
    // Test artifacts are deliberately preserved for manual cleanup.
  }
  void resourceHierarchyAndContent() {
    const QDir resourceRoot(":/qiubai/qiubai");
    QDirIterator iterator(resourceRoot.path(), QDirIterator::Subdirectories);
    int count = 0;
    while (iterator.hasNext()) {
      const QString source = iterator.next();
      if (!QFileInfo(source).isFile())
        continue;
      const QString relative = resourceRoot.relativeFilePath(source);
      const QString target =
          relative == "Neil.jpg" ? neil : QDir(resources).filePath(relative);
      QVERIFY2(QFileInfo::exists(target), qPrintable(target));
      QCOMPARE(read(target), read(source));
      ++count;
    }
    QVERIFY(count >= 12);
    // Device tools are embedded/extracted once, never as a second bin copy.
    for (const QString name :
         {"adb.exe", "fastboot.exe", "AdbWinApi.dll", "AdbWinUsbApi.dll"}) {
      QVERIFY(!read(resources + "/" + name).isEmpty());
      QVERIFY(!QFileInfo::exists(resources + "/bin/platform-tools/" + name));
      QVERIFY(!QFileInfo::exists(":/qiubai/qiubai/bin/platform-tools/" + name));
    }
    QVERIFY(!QFileInfo::exists(resources + "/7z.exe"));
  }
  void defaultPaths_data() {
    QTest::addColumn<QString>("key");
    QTest::addColumn<QString>("relative");
    QTest::newRow("7zip") << "7z" << "bin/7zip/7z.exe";
    QTest::newRow("lpmake") << "lpmake" << "bin/lpmake/lpmake.exe";
    QTest::newRow("fastboot") << "fastboot" << "fastboot.exe";
    QTest::newRow("adb") << "adb" << "adb.exe";
    QTest::newRow("payload") << "payload" << "payload.exe";
    QTest::newRow("unknown-key") << "rom" << "";
    QTest::newRow("path-not-a-tool-key") << "../fastboot" << "";
  }
  void defaultPaths() {
    QFETCH(QString, key);
    QFETCH(QString, relative);
    const QString fallback = directory + "/user-selected.exe";
    QCOMPARE(OugaProcessRunner::bundledToolPath(resources, key, fallback),
             relative.isEmpty() ? fallback
                                : QDir(resources).absoluteFilePath(relative));
    QCOMPARE(OugaProcessRunner::bundledToolPath({}, key, fallback), fallback);
  }
  void resolvedToolsFollowRuntimeLayout_data() {
    QTest::addColumn<QString>("key");
    QTest::addColumn<QString>("layout");
    QTest::addColumn<bool>("staleConfiguration");
    for (const QString key : {"7z", "lpmake", "payload", "fastboot", "adb"})
      for (const QString layout : {"extracted", "app-qiubai", "beside-exe",
                                   "sibling-qiubai"})
        for (const bool stale : {false, true}) {
          const QString row = key + "-" + layout + (stale ? "-stale" : "-empty");
          QTest::newRow(qPrintable(row)) << key << layout << stale;
        }
  }
  void resolvedToolsFollowRuntimeLayout() {
    QFETCH(QString, key);
    QFETCH(QString, layout);
    QFETCH(bool, staleConfiguration);
    const QString extracted = directory + "/extracted";
    const QString application = directory + "/发布 中文/OrangeToolsApp";
    const QString toolRoot = layout == "extracted" ? extracted
                           : layout == "app-qiubai" ? application + "/qiubai"
                           : layout == "beside-exe" ? application
                           : directory + "/发布 中文/qiubai";
    const QString relative = key == "7z" ? "bin/7zip/7z.exe"
                           : key == "lpmake" ? "bin/lpmake/lpmake.exe"
                           : key + ".exe";
    const QString expected = toolRoot + "/" + relative;
    QVERIFY(put(expected, "inert tool fixture"));
    if (key == "7z")
      QVERIFY(put(toolRoot + "/bin/7zip/7z.dll", "inert library fixture"));
    const QString configured = staleConfiguration
                                   ? directory + "/removed/selected.exe"
                                   : QString();
    QCOMPARE(OugaProcessRunner::resolveToolPath(extracted, application, key,
                                                configured), expected);
  }
  void invalidConfiguredPathUsesBundle_data() {
    QTest::addColumn<int>("kind");
    QTest::newRow("empty-setting") << 0;
    QTest::newRow("whitespace-setting") << 1;
    QTest::newRow("missing-file") << 2;
    QTest::newRow("empty-file") << 3;
    QTest::newRow("directory-not-executable") << 4;
    QTest::newRow("relative-path") << 5;
  }
  void invalidConfiguredPathUsesBundle() {
    QFETCH(int, kind);
    const QString extracted = directory + "/extracted";
    const QString expected = extracted + "/bin/7zip/7z.exe";
    QVERIFY(put(expected, "inert executable"));
    QVERIFY(put(extracted + "/bin/7zip/7z.dll", "inert library"));
    QString configured;
    if (kind == 1)
      configured = "   ";
    else if (kind >= 2)
      configured = directory + "/configured.exe";
    if (kind == 3)
      QVERIFY(put(configured, {}));
    if (kind == 4)
      QVERIFY(QDir().mkpath(configured));
    if (kind == 5)
      configured = "configured.exe";
    QCOMPARE(OugaProcessRunner::resolveToolPath(extracted, {}, "7z", configured),
             expected);
  }
  void explicitConfigurationHasPriority() {
    const QString custom = directory + "/custom/7za.exe";
    const QString extracted = directory + "/extracted";
    const QString application = directory + "/app";
    QVERIFY(put(custom, "explicit standalone tool"));
    for (const QString root : {extracted, application + "/qiubai", application,
                               directory + "/qiubai"}) {
      QVERIFY(put(root + "/bin/7zip/7z.exe", "bundled executable"));
      QVERIFY(put(root + "/bin/7zip/7z.dll", "bundled library"));
    }
    QCOMPARE(OugaProcessRunner::resolveToolPath(extracted, application, "7z",
                                                "  " + custom + "  "), custom);
    QCOMPARE(OugaProcessRunner::resolveToolPath(extracted, application, "7z"),
             extracted + "/bin/7zip/7z.exe");
    QCOMPARE(OugaProcessRunner::resolveToolPath(directory + "/missing",
                                                application, "7z"),
             application + "/qiubai/bin/7zip/7z.exe");
  }
  void incompleteBundledSevenzipUsesCompleteCopy() {
    const QString extracted = directory + "/extracted";
    const QString application = directory + "/app";
    const QString first = extracted + "/bin/7zip/7z.exe";
    const QString second = application + "/qiubai/bin/7zip/7z.exe";
    QVERIFY(put(first, "partial installation"));
    QVERIFY(put(second, "complete installation"));
    QVERIFY(put(application + "/qiubai/bin/7zip/7z.dll", "format library"));
    for (const bool emptyLibrary : {false, true}) {
      if (emptyLibrary)
        QVERIFY(put(extracted + "/bin/7zip/7z.dll", {}));
      QCOMPARE(OugaProcessRunner::bundledToolPath(extracted, "7z", "fallback"),
               QString("fallback"));
      QCOMPARE(OugaProcessRunner::resolveToolPath(extracted, application, "7z"),
               second);
    }
    QVERIFY(put(extracted + "/bin/7zip/7z.dll", "format library"));
    QCOMPARE(OugaProcessRunner::resolveToolPath(extracted, application, "7z"),
             first);
  }
  void resolverDoesNotSearchRomWorkingDirectory() {
    const QString previous = QDir::currentPath();
    const auto restore = qScopeGuard([&] { QDir::setCurrent(previous); });
    const QString rom = root + "/cwd-" + QUuid::createUuid().toString(QUuid::Id128);
    QVERIFY(put(rom + "/bin/7zip/7z.exe", "untrusted ROM executable"));
    QVERIFY(put(rom + "/bin/7zip/7z.dll", "untrusted ROM library"));
    QVERIFY(put(rom + "/payload.exe", "untrusted ROM executable"));
    QVERIFY(QDir::setCurrent(rom));
    for (const QString key : {"7z", "lpmake", "payload", "fastboot", "adb"})
      QVERIFY(OugaProcessRunner::resolveToolPath(
                  directory + "/missing", directory + "/app", key).isEmpty());
    QVERIFY(OugaProcessRunner::resolveToolPath({}, {}, "7z", "bin/7zip/7z.exe")
                .isEmpty());
    const QString explicitFallback = directory + "/fallback.exe";
    QVERIFY(put(explicitFallback, "explicit absolute fallback"));
    QCOMPARE(OugaProcessRunner::resolveToolPath({}, {}, "7z", {},
                                                explicitFallback),
             explicitFallback);
    QVERIFY(OugaProcessRunner::resolveToolPath({}, {}, "7z", {},
                                               directory + "/absent.exe")
                .isEmpty());
  }
  void unavailableToolFallsBack_data() {
    QTest::addColumn<int>("kind");
    QTest::newRow("missing") << 0;
    QTest::newRow("empty") << 1;
    QTest::newRow("directory-not-file") << 2;
  }
  void unavailableToolFallsBack() {
    QFETCH(int, kind);
    const QString path = directory + "/bin/7zip/7z.exe";
    if (kind == 1)
      QVERIFY(put(path, {}));
    if (kind == 2)
      QVERIFY(QDir().mkpath(path));
    QCOMPARE(OugaProcessRunner::bundledToolPath(directory, "7z", "fallback"),
             QString("fallback"));
  }
  void legacyBinDoesNotOverrideSharedTools() {
    for (const QString key : {"adb", "fastboot"}) {
      const QString shared = QDir(directory).absoluteFilePath(key + ".exe");
      QVERIFY(
          put(directory + "/bin/platform-tools/" + key + ".exe", "old-copy"));
      QCOMPARE(OugaProcessRunner::bundledToolPath(directory, key, "fallback"),
               QString("fallback"));
      QVERIFY(put(shared, "shared-tool"));
      QCOMPARE(OugaProcessRunner::bundledToolPath(directory, key, "fallback"),
               shared);
    }
  }
  void formatterUsesMatchingDirectory() {
    const QString fastboot =
        OugaProcessRunner::bundledToolPath(resources, "fastboot");
    QVERIFY(OugaProcessRunner::formatToolsError(fastboot).isEmpty());
    QCOMPARE(fastboot, QDir(resources).absoluteFilePath("fastboot.exe"));
    // An explicitly configured external tool must bring its own formatters.
    QVERIFY(put(directory + "/fastboot.exe", "fake"));
    QVERIFY(put(directory + "/mke2fs.exe", "fake"));
    QVERIFY(put(directory + "/make_f2fs.exe", "fake"));
    QVERIFY(OugaProcessRunner::formatToolsError(directory + "/fastboot.exe")
                .contains("mke2fs.conf"));
  }
  void lpmakeHelpPolicy_data() {
    QTest::addColumn<int>("code");
    QTest::addColumn<QString>("kind");
    QTest::addColumn<bool>("attemptGeneration");
    QTest::newRow("normal-help-zero") << 0 << "valid" << true;
    QTest::newRow("aosp-help-one") << 1 << "valid" << true;
    QTest::newRow("help-error-two") << 2 << "valid" << false;
    QTest::newRow("usage-without-banner") << 1 << "no-banner" << false;
    QTest::newRow("zero-with-failed-output") << 0 << "failed" << false;
    QTest::newRow("one-with-failed-output") << 1 << "failed" << false;
    QTest::newRow("missing-required-flag") << 1 << "no-device" << false;
  }
  void lpmakeHelpPolicy() {
    QFETCH(int, code);
    QFETCH(QString, kind);
    QFETCH(bool, attemptGeneration);
    const QString source = directory + "/source";
    QVERIFY(sourcePackage(source));
    const QByteArray original = read(source + "/super_def.json");
    qputenv("ORANGE_DEPENDENCY_MOCK", "1");
    qputenv("ORANGE_LPMAKE_HELP_KIND", kind.toUtf8());
    qputenv("ORANGE_LPMAKE_HELP_STATUS", QByteArray::number(code));
    qputenv("ORANGE_LPMAKE_MARKER", (directory + "/commands.txt").toUtf8());
    OugaPreparation preparation;
    QSignalSpy prepared(&preparation, &OugaPreparation::prepared);
    QSignalSpy done(&preparation, &OugaPreparation::finished);
    preparation.makeSuper(QCoreApplication::applicationFilePath(), source,
                          directory + "/output");
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 5000);
    QVERIFY(!done[0][0].toBool()); // The mock generation always fails.
    QCOMPARE(prepared.count(), 0);
    QCOMPARE(read(directory + "/commands.txt"), attemptGeneration
                                                    ? QByteArray("help\nmake\n")
                                                    : QByteArray("help\n"));
    QCOMPARE(read(source + "/super_def.json"), original);
    if (attemptGeneration)
      QVERIFY(done[0][1].toString().contains("Super 生成失败"));
    QVERIFY(!QFileInfo::exists(directory + "/output/super.img"));
  }
  void generationRemainsStrict_data() {
    QTest::addColumn<int>("code");
    QTest::addColumn<QString>("outputKind");
    QTest::newRow("nonzero") << 9 << "plain";
    QTest::newRow("zero-failed") << 0 << "failed";
    QTest::newRow("help-banner-does-not-excuse-generation") << 1 << "help";
  }
  void generationRemainsStrict() {
    QFETCH(int, code);
    QFETCH(QString, outputKind);
    const QString source = directory + "/source";
    QVERIFY(sourcePackage(source));
    qputenv("ORANGE_DEPENDENCY_MOCK", "1");
    qputenv("ORANGE_LPMAKE_HELP_KIND", "valid");
    qputenv("ORANGE_LPMAKE_HELP_STATUS", "1");
    qputenv("ORANGE_LPMAKE_GENERATE_STATUS", QByteArray::number(code));
    qputenv("ORANGE_LPMAKE_GENERATE_OUTPUT", outputKind.toUtf8());
    qputenv("ORANGE_LPMAKE_MARKER", (directory + "/commands.txt").toUtf8());
    OugaPreparation preparation;
    QSignalSpy done(&preparation, &OugaPreparation::finished);
    preparation.makeSuper(QCoreApplication::applicationFilePath(), source,
                          directory + "/output");
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 5000);
    QVERIFY(!done[0][0].toBool());
    QVERIFY(done[0][1].toString().contains("Super 生成失败"));
    QCOMPARE(read(directory + "/commands.txt"), QByteArray("help\nmake\n"));
  }
  void realBundleManifest() {
    if (realTools.isEmpty())
      QSKIP("Enable bundled_dependency_resources or set "
            "ORANGE_BUNDLED_TOOL_ROOT");
    const QString bin = realTools + "/bin";
    QJsonParseError error;
    const auto document =
        QJsonDocument::fromJson(read(bin + "/manifest.json"), &error);
    QCOMPARE(error.error, QJsonParseError::NoError);
    QCOMPARE(document.object()["schema"].toInt(), 2);
    QCOMPARE(document.object()["pathBase"].toString(), QString("qiubai"));
    const auto files = document.object()["files"].toArray();
    QVERIFY(files.size() >= 18);
    for (const auto &value : files) {
      const auto entry = value.toObject();
      const QString path = QDir(realTools).filePath(entry["path"].toString());
      QVERIFY2(OugaPackage::inside(realTools, path), qPrintable(path));
      QCOMPARE(QFileInfo(path).size(), qint64(entry["bytes"].toDouble()));
      QCOMPARE(QString::fromLatin1(OugaPackage::digest(path, nullptr).toHex())
                   .toUpper(),
               entry["sha256"].toString());
    }
  }
  void realToolVersions_data() {
    QTest::addColumn<QString>("key");
    QTest::addColumn<QStringList>("args");
    QTest::addColumn<int>("code");
    QTest::addColumn<QByteArray>("expected");
    QTest::newRow("fastboot-version-only")
        << "fastboot" << QStringList{"--version"} << 0 << QByteArray("37.0.1");
    QTest::newRow("adb-version-no-server")
        << "adb" << QStringList{"version"} << 0 << QByteArray("37.0.1");
    QTest::newRow("7zip-format-library")
        << "7z" << QStringList{"i"} << 0 << QByteArray("26.03");
    QTest::newRow("lpmake-help-only")
        << "lpmake" << QStringList{"--help"} << 1
        << QByteArray("command-line tool for creating Android Logical "
                      "Partition images.");
  }
  void realToolVersions() {
    if (realTools.isEmpty())
      QSKIP("Real tools are never discovered from PATH");
    QFETCH(QString, key);
    QFETCH(QStringList, args);
    QFETCH(int, code);
    QFETCH(QByteArray, expected);
    const QString tool = OugaProcessRunner::bundledToolPath(realTools, key);
    QVERIFY(!tool.isEmpty());
    QByteArray output;
    QCOMPARE(localCommand(tool, args, directory, &output), code);
    QVERIFY2(output.contains(expected), output.constData());
    QVERIFY(put(directory + "/tool-output.txt", output));
    if (key == "7z") {
      QVERIFY(output.contains("7z.dll"));
      QVERIFY(output.contains("Rar"));
    }
    if (key == "fastboot")
      QVERIFY(OugaProcessRunner::formatToolsError(tool).isEmpty());
  }
  void realArchiveExtraction() {
    if (realTools.isEmpty())
      QSKIP("Real local archive tool not explicitly supplied");
    const QString source = directory + "/压缩 中文 空格";
    const QByteArray bytes(4096, 'b');
    QVERIFY(put(source + "/boot.img", bytes));
    const QString archive = source + "/刷机包 中文.zip";
    const QString tool = OugaProcessRunner::bundledToolPath(realTools, "7z");
    QByteArray output;
    QCOMPARE(localCommand(tool,
                          {"a", "-tzip", "-y", "-mx=0", "-sccUTF-8", archive,
                           "boot.img"},
                          source, &output),
             0);
    QVERIFY(put(directory + "/archive-create.txt", output));
    OugaPreparation preparation;
    QSignalSpy progress(&preparation, &OugaPreparation::archiveProgress);
    QSignalSpy logs(&preparation, &OugaPreparation::log);
    QSignalSpy done(&preparation, &OugaPreparation::finished);
    preparation.extractArchive(tool, archive, directory + "/解压 输出");
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 15000);
    QVERIFY2(done[0][0].toBool(), qPrintable(done[0][1].toString()));
    QCOMPARE(read(directory + "/解压 输出/boot.img"), bytes);
    QCOMPARE(read(source + "/boot.img"), bytes);
    QVERIFY(QFileInfo::exists(archive));
    QVERIFY(progress.count() >= 2);
    QCOMPARE(progress.first()[0].toInt(), 0);
    QCOMPARE(progress.last()[0].toInt(), 100);
    int previous = -1;
    for (const auto &args : progress) {
      const int percent = args[0].toInt();
      QVERIFY(percent > previous && percent <= 100);
      previous = percent;
    }
    QString text;
    for (const auto &args : logs)
      text += args[0].toString();
    QVERIFY(!text.contains("Everything is Ok"));
    QVERIFY(!text.contains("Path = "));
    QVERIFY(text.contains("正在解压售后包"));
  }
  void realPayloadProgress_data() {
    QTest::addColumn<bool>("parallel");
    QTest::newRow("single-partition") << false;
    QTest::newRow("parallel-partitions") << true;
  }
  void realPayloadProgress() {
    QFETCH(bool, parallel);
    if (realTools.isEmpty()) QSKIP("Real payload tool must be supplied explicitly");
    // 256 actual REPLACE_BZ operations; no image-size-derived progress. The
    // embedded BZip2 stream expands to one MiB of 'p'. Entire fixture is synthetic.
    const QByteArray packed = QByteArray::fromHex("425a683931415926535953c3a50500080a4080800440000008200030cc0549ea71060140601e2ee48a70a120a7874a0a");
    const int operations = 256, blockBytes = 1024 * 1024;
    const auto varint = [](quint64 value) {
      QByteArray result;
      do { result += char((value & 127) | (value > 127 ? 128 : 0)); value >>= 7; } while (value);
      return result;
    };
    const auto number = [&](int field, quint64 value) { return varint(field * 8) + varint(value); };
    const auto message = [&](int field, const QByteArray &value) {
      return varint(field * 8 + 2) + varint(value.size()) + value;
    };
    const QByteArray data(blockBytes, 'p');
    int offset = 0;
    const auto partition = [&](const QByteArray &name, int count) {
      QCryptographicHash hash(QCryptographicHash::Sha256);
      for (int i = 0; i < count; ++i) hash.addData(data);
      QByteArray result = message(1, name) + message(7,
          number(1, quint64(count) * blockBytes) + message(2, hash.result()));
      for (int i = 0; i < count; ++i)
        result += message(8, number(1, 1) + number(2, offset++ * packed.size()) +
            number(3, packed.size()) + message(6, number(1, i * 256) + number(2, 256)) +
            number(7, blockBytes));
      return message(13, result);
    };
    const int bootOperations = parallel ? 192 : operations;
    QByteArray manifest = number(3, 4096) + partition("boot", bootOperations);
    if (parallel) manifest += partition("vendor", operations - bootOperations);
    QByteArray header(24, 0);
    header.replace(0, 4, "CrAU");
    qToBigEndian<quint64>(2, reinterpret_cast<uchar *>(header.data() + 4));
    qToBigEndian<quint64>(manifest.size(), reinterpret_cast<uchar *>(header.data() + 12));
    const QString source = directory + "/输入 中文/payload.bin";
    QVERIFY(put(source, header + manifest + packed.repeated(operations)));
    const QString archive = directory + "/输入 中文/全量 包.zip";
    QVERIFY(put(directory + "/输入 中文/unrelated.img", QByteArray(4096, 'x')));
    QByteArray zipOutput;
    const QString sevenZip = OugaProcessRunner::bundledToolPath(realTools, "7z");
    QCOMPARE(localCommand(sevenZip, {"a", "-tzip", "-y", "-sccUTF-8", archive,
                                    "payload.bin", "unrelated.img"}, QFileInfo(source).absolutePath(), &zipOutput), 0);
    OugaPreparation prep;
    QSignalSpy done(&prep, &OugaPreparation::finished);
    QSignalSpy progress(&prep, &OugaPreparation::payloadProgress);
    QSignalSpy rows(&prep, &OugaPreparation::payloadPartitionFinished);
    QSignalSpy logs(&prep, &OugaPreparation::log);
    const QString unpacked = directory + "/unpacked";
    prep.extractArchive(sevenZip, archive, unpacked, false);
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 15000);
    QVERIFY2(done[0][0].toBool(), qPrintable(done[0][1].toString()));
    QCOMPARE(read(unpacked + "/payload.bin"), read(source));
    QVERIFY(!QFileInfo::exists(unpacked + "/unrelated.img"));
    const QString tool = OugaProcessRunner::bundledToolPath(realTools, "payload");
    prep.payload(tool, unpacked + "/payload.bin", directory + "/镜像 输出");
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 2, 60000);
    QVERIFY2(done[1][0].toBool(), qPrintable(done[1][1].toString()));
    QCOMPARE(progress.first()[0].toInt(), 0);
    QCOMPARE(progress.last()[0].toInt(), 100);
    bool intermediate = false;
    QStringList percentages;
    for (const auto &row : progress) {
      percentages << row[0].toString();
      intermediate |= row[0].toInt() > 0 && row[0].toInt() < 99;
    }
    QVERIFY2(intermediate, qPrintable("No real intermediate operation counters: " + percentages.join(",")));
    QCOMPARE(rows.count(), parallel ? 2 : 1);
    QSet<QString> finished;
    for (const auto &row : rows) {
      QVERIFY(row[1].toBool());
      finished.insert(row[0].toString());
    }
    QVERIFY(finished.contains("boot"));
    QCOMPARE(QFileInfo(directory + "/镜像 输出/boot.img").size(), qint64(bootOperations) * blockBytes);
    if (parallel) {
      QVERIFY(finished.contains("vendor"));
      QCOMPARE(QFileInfo(directory + "/镜像 输出/vendor.img").size(),
               qint64(operations - bootOperations) * blockBytes);
    }
    QString text;
    for (const auto &row : logs) text += row[0].toString();
    QVERIFY(!text.contains("Everything is Ok"));
    QVERIFY(!text.contains("Path = "));
    QVERIFY(text.contains("payload.bin 解压完成"));
    qInfo() << "Bundled tool operation progress:" << percentages;
  }
  void realSuperGeneration() {
    if (realTools.isEmpty())
      QSKIP("Real local lpmake not explicitly supplied");
    const QString source = directory + "/售后 包 中文";
    const QString output = directory + "/Super 输出 中文";
    QVERIFY(sourcePackage(source));
    const auto systemHash =
        OugaPackage::digest(source + "/system.img", nullptr);
    const auto vendorHash =
        OugaPackage::digest(source + "/vendor.img", nullptr);
    const QByteArray definition = read(source + "/super_def.json");
    OugaPreparation preparation;
    QSignalSpy done(&preparation, &OugaPreparation::finished);
    QSignalSpy prepared(&preparation, &OugaPreparation::prepared);
    preparation.makeSuper(
        OugaProcessRunner::bundledToolPath(realTools, "lpmake"), source,
        output);
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 15000);
    QVERIFY2(done[0][0].toBool(), qPrintable(done[0][1].toString()));
    QCOMPARE(prepared.count(), 1);
    Ouga::Partition image;
    QString error;
    QVERIFY2(
        OugaPackage::inspect("super", output + "/super.img", &image, &error),
        qPrintable(error));
    QCOMPARE(image.expandedBytes, qint64(8388608));
    QCOMPARE(image.merged, (QSet<QString>{"system", "vendor"}));
    QCOMPARE(OugaPackage::digest(source + "/system.img", nullptr), systemHash);
    QCOMPARE(OugaPackage::digest(source + "/vendor.img", nullptr), vendorHash);
    QCOMPARE(read(source + "/super_def.json"), definition);
  }
};

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  const auto args = app.arguments();
  if (qEnvironmentVariable("ORANGE_DEPENDENCY_MOCK") == "1" &&
      args.size() > 1) {
    const bool help = args.mid(1) == QStringList{"--help"};
    QFile marker(qEnvironmentVariable("ORANGE_LPMAKE_MARKER"));
    if (!marker.open(QIODevice::WriteOnly | QIODevice::Append))
      return 99;
    marker.write(help ? "help\n" : "make\n");
    marker.close();
    if (help) {
      const auto text =
          helpText(qEnvironmentVariable("ORANGE_LPMAKE_HELP_KIND"));
      std::fwrite(text.constData(), 1, size_t(text.size()), stdout);
      return qEnvironmentVariableIntValue("ORANGE_LPMAKE_HELP_STATUS");
    }
    const QString kind = qEnvironmentVariable("ORANGE_LPMAKE_GENERATE_OUTPUT");
    const auto text = kind == "help" ? helpText("valid")
                      : kind == "failed"
                          ? QByteArray("FAILED (mock generation)\n")
                          : QByteArray("injected generation failure\n");
    std::fwrite(text.constData(), 1, size_t(text.size()), stdout);
    return qEnvironmentVariableIsSet("ORANGE_LPMAKE_GENERATE_STATUS")
               ? qEnvironmentVariableIntValue("ORANGE_LPMAKE_GENERATE_STATUS")
               : 42;
  }
  qRegisterMetaType<QVector<Ouga::Partition>>();
  OugaDependencyTests tests;
  return QTest::qExec(&tests, argc, argv);
}
#include "ougadependencytests.moc"
