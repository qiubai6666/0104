#include "deviceoperationlease.h"
#include "ougacommandrunner.h"
#include "ougapackage.h"
#include "ougapreparation.h"
#include "resourceextractor.h"
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSignalSpy>
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
    root = qEnvironmentVariable("ORANGE_TEST_ARTIFACTS");
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
    QTest::newRow("legacy-payload") << "payload" << "";
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
    QSignalSpy done(&preparation, &OugaPreparation::finished);
    preparation.extractArchive(tool, archive, directory + "/解压 输出");
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 15000);
    QVERIFY2(done[0][0].toBool(), qPrintable(done[0][1].toString()));
    QCOMPARE(read(directory + "/解压 输出/boot.img"), bytes);
    QCOMPARE(read(source + "/boot.img"), bytes);
    QVERIFY(QFileInfo::exists(archive));
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
