#include "deviceoperationlease.h"
#include "ougaflashplanner.h"
#include "ougaflashservice.h"
#include "ougaflashwindow.h"
#include "ougapreparation.h"
#include "resourceextractor.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QGroupBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QScopeGuard>
#include <QSettings>
#include <QStandardPaths>
#include <QStyle>
#include <QTabWidget>
#include <QTableWidget>
#include <QUuid>
#include <QtEndian>
#include <QtTest>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <numeric>
#include <thread>
using namespace Ouga;
namespace {
QString testRoot, testResourceDirectory;
QByteArray read(const QString &p) {
  QFile f(p);
  if (!f.open(QIODevice::ReadOnly))
    return {};
  return f.readAll();
}
bool put(const QString &p, const QByteArray &b) {
  QFile f(p);
  return f.open(QIODevice::WriteOnly) && f.write(b) == b.size();
}
void le16(QByteArray &b, int at, quint16 v) {
  qToLittleEndian(v, reinterpret_cast<uchar *>(b.data() + at));
}
void le32(QByteArray &b, int at, quint32 v) {
  qToLittleEndian(v, reinterpret_cast<uchar *>(b.data() + at));
}
void le64(QByteArray &b, int at, quint64 v) {
  qToLittleEndian(v, reinterpret_cast<uchar *>(b.data() + at));
}
QByteArray sparse() {
  QByteArray b(28 + 12 + 512, 0);
  le32(b, 0, 0xed26ff3a);
  le16(b, 4, 1);
  le16(b, 8, 28);
  le16(b, 10, 12);
  le32(b, 12, 512);
  le32(b, 16, 1);
  le32(b, 20, 1);
  le16(b, 28, 0xcac1);
  le32(b, 32, 1);
  le32(b, 36, 524);
  return b;
}
QByteArray arb(quint32 value) {
  QByteArray b(232, 0);
  b.replace(0, 4,
            QByteArray("\x7f"
                       "ELF",
                       4));
  b[4] = 2;
  b[5] = 1;
  le64(b, 32, 64);
  le16(b, 54, 56);
  le16(b, 56, 1);
  le32(b, 64, 0);
  le64(b, 72, 128);
  le64(b, 96, 104);
  le32(b, 128, 1);
  le32(b, 140, 12);
  le32(b, 172, value);
  return b;
}
QByteArray vi(quint64 v) {
  QByteArray b;
  do {
    quint8 c = v & 127;
    v >>= 7;
    b += char(c | (v ? 128 : 0));
  } while (v);
  return b;
}
QByteArray pb(int n, const QByteArray &v) {
  return vi((n << 3) | 2) + vi(v.size()) + v;
}
QByteArray payloadBytes(bool delta = false) {
  QByteArray contents(512, 'p'),
      info =
          vi(8) + vi(contents.size()) +
          pb(2, QCryptographicHash::hash(contents, QCryptographicHash::Sha256));
  QByteArray part = pb(1, "boot") + pb(7, info);
  if (delta)
    part += pb(6, info);
  QByteArray manifest =
      pb(13, part) + (delta ? vi(12 << 3) + vi(1) : QByteArray());
  QByteArray h(24, 0);
  h.replace(0, 4, "CrAU");
  qToBigEndian<quint64>(2, reinterpret_cast<uchar *>(h.data() + 4));
  qToBigEndian<quint64>(manifest.size(),
                        reinterpret_cast<uchar *>(h.data() + 12));
  return h + manifest;
}
QByteArray superBytes() {
  QByteArray b(2 * 1024 * 1024, 0), g(52, 0);
  le32(g, 0, 0x616c4467);
  le32(g, 4, 52);
  le32(g, 40, 65536);
  le32(g, 44, 2);
  le32(g, 48, 4096);
  g.replace(8, 32, QCryptographicHash::hash(g, QCryptographicHash::Sha256));
  b.replace(4096, g.size(), g);
  b.replace(8192, g.size(), g);
  QByteArray tables(52 + 24 + 48 + 64, 0);
  tables.replace(0, 8, "system_a");
  le32(tables, 36, 1);
  le32(tables, 44, 1);
  le64(tables, 52, 8);
  le64(tables, 64, 2048);
  tables.replace(76, 7, "default");
  le64(tables, 116, 1048576);
  le64(tables, 124, 2048);
  le32(tables, 132, 1048576);
  le64(tables, 140, b.size());
  tables.replace(148, 5, "super");
  QByteArray h(128, 0);
  le32(h, 0, 0x414c5030);
  le16(h, 4, 10);
  le32(h, 8, 128);
  le32(h, 44, tables.size());
  h.replace(48, 32,
            QCryptographicHash::hash(tables, QCryptographicHash::Sha256));
  int offsets[] = {0, 52, 76, 124}, sizes[] = {52, 24, 48, 64};
  for (int i = 0; i < 4; ++i) {
    le32(h, 80 + i * 12, offsets[i]);
    le32(h, 84 + i * 12, 1);
    le32(h, 88 + i * 12, sizes[i]);
  }
  h.replace(12, 32, QCryptographicHash::hash(h, QCryptographicHash::Sha256));
  for (int i = 0; i < 4; ++i) {
    b.replace(12288 + i * 65536, h.size(), h);
    b.replace(12288 + i * 65536 + h.size(), tables.size(), tables);
  }
  return b;
}
Device fixtureDevice(Platform platform = Platform::Qualcomm,
                     const QString &slot = "a", bool userspace = true) {
  Device d;
  d.serial = "TEST-SERIAL";
  d.product = "test-product";
  d.slot = slot;
  d.platform = platform;
  d.userspace = userspace;
  d.modeKnown = d.unlockKnown = d.unlocked = true;
  QStringList names = {
      "boot",          "init_boot",
      "dtbo",          "vbmeta",
      "vendor_boot",   "vbmeta_system",
      "vbmeta_vendor", "modem",
      "recovery",      "system",
      "vendor",        "my_company",
      "my_preload",    platform == Platform::Qualcomm ? "xbl" : "lk"};
  for (const auto &n : names)
    for (const QString s : {"a", "b"}) {
      QString t = n + "_" + s;
      d.partitions.insert(t);
      d.sizes[t] = 1024 * 1024;
      if (logicalName(n))
        d.logical.insert(t);
    }
  for (const QString n :
       {"persist", "userdata", "metadata", "super", "system_a-cow"}) {
    d.partitions.insert(n);
    d.sizes[n] = n == "super" ? 64 * 1024 * 1024 : 1024 * 1024;
  }
  d.logical.insert("system_a-cow");
  return d;
}
QString dump(const Device &d) {
  QString out = "(bootloader) serialno: " + d.serial +
                "\n(bootloader) product: " + d.product +
                "\n(bootloader) current-slot: " + d.slot +
                "\n(bootloader) is-userspace: " + (d.userspace ? "yes" : "no") +
                "\n";
  if (d.unlockKnown)
    out +=
        "(bootloader) unlocked: " + QString(d.unlocked ? "yes" : "no") + "\n";
  for (auto i = d.sizes.cbegin(); i != d.sizes.cend(); ++i)
    out += "(bootloader) partition-size:" + i.key() + ": 0x" +
           QString::number(i.value(), 16) + "\n";
  for (const auto &n : d.logical)
    out += "(bootloader) is-logical:" + n + ": yes\n";
  for (auto i = d.variables.cbegin(); i != d.variables.cend(); ++i)
    if (i.key().startsWith("has-slot:"))
      out += "(bootloader) " + i.key() + ": " + i.value() + "\n";
  return out + "Finished. Total time: 0.001s\n";
}
QStringList commands(const Plan &p) {
  QStringList out;
  for (const auto &s : p.steps)
    if (!s.arguments.isEmpty())
      out << s.arguments.mid(0, s.arguments[0] == "flash" ? 2 : -1).join(' ');
  return out;
}
class FakeRunner final : public OugaCommandRunner {
public:
  Device device = fixtureDevice();
  QStringList trace;
  QString failAt, failOutput = "FAILED (remote: test failure)";
  int failCode = 1;
  bool normal = true, holdFlash = false, stuckMode = false,
       disconnected = false, multipleDevices = true;
  std::function<void(const QStringList &)> after;
  std::function<void(Device &, int)> beforeProbe;
  int probes = 0, outputStyle = 0;
  bool markedOutput = false;
  bool active = false;
  void run(const QString &, const QStringList &args, int) override {
    Q_ASSERT(!active);
    active = true;
    QStringList a = args.value(0) == "-s" ? args.mid(2) : args;
    trace << a.join(' ');
    if (args.value(0) == "-s" && args.value(1) != device.serial) {
      complete(-1, false, "ERROR: wrong device");
      return;
    }
    if (holdFlash && a.value(0) == "flash")
      return;
    QTimer::singleShot(0, this, [this, a] {
      QString line = a.join(' ');
      if (!failAt.isEmpty() && line.startsWith(failAt)) {
        complete(failCode, normal, failOutput);
        return;
      }
      if (a.value(0) == "getvar") {
        ++probes;
        if (beforeProbe)
          beforeProbe(device, probes);
        complete(disconnected ? 1 : 0, true,
                 disconnected ? "FAILED transport disconnected" : dump(device));
        return;
      }
      if (a.value(0) == "devices") {
        complete(0, true,
                 device.serial + "\tfastboot\n" +
                     (multipleDevices ? "SECOND\tfastboot\n" : ""));
        return;
      }
      if (a.value(0) == "reboot" && !stuckMode)
        device.userspace = a.value(1) == "fastboot";
      if (a.value(0) == "set_active")
        device.slot = a.value(1);
      if (a.value(0) == "delete-logical-partition") {
        device.partitions.remove(a.value(1));
        device.sizes.remove(a.value(1));
        device.logical.remove(a.value(1));
      }
      if (a.value(0) == "create-logical-partition") {
        device.partitions.insert(a.value(1));
        device.logical.insert(a.value(1));
        device.sizes[a.value(1)] = a.value(2).toULongLong();
      }
      if (after)
        after(a);
      complete(0, true, "OKAY\nFinished. Total time: 0.001s");
    });
  }
  void complete(int c, bool n, const QString &out) {
    active = false;
    const QString prefix = "OUTPUT-ONE\nOUTPUT-TWO\n";
    const QString text = markedOutput ? prefix + out + "\nOUTPUT-TAIL\n" : out;
    if (outputStyle == 1)
      emit output(text);
    else if (outputStyle == 2) {
      emit output("OUTPUT-ONE\n");
      emit output("OUTPUT-TWO\n");
    }
    emit completed(c, n, text);
  }
  bool running() const override { return active; }
};
} // namespace
QString ResourceExtractor::getResourcePath() {
  return testResourceDirectory;
}
QString ResourceExtractor::getAdbPath() {
  return getResourcePath() + "/adb.exe";
}
QString ResourceExtractor::getFastbootPath() {
  return getResourcePath() + "/fastboot.exe";
}
QString ResourceExtractor::getNeilImagePath() { return {}; }
class OugaTests : public QObject {
  Q_OBJECT
  QString dir;
  Partition image(const QString &name,
                  const QByteArray &bytes = QByteArray(512, 'p')) {
    QString f = dir + "/" + name + ".img";
    put(f, bytes);
    Partition p;
    QString e;
    if (!OugaPackage::inspect(name, f, &p, &e))
      qFatal("Fixture error: %s", qPrintable(e));
    return p;
  }
  QVector<Partition> images() {
    QVector<Partition> ps;
    for (const QString n :
         {"boot", "system", "my_company", "my_preload", "modem", "persist"})
      ps << image(n);
    return ps;
  }
  Options options(FlashMode mode = FlashMode::Normal,
                  Platform platform = Platform::Qualcomm) {
    Options o;
    o.mode = mode;
    o.packagePlatform = platform;
    o.clearData = false;
    o.autoReboot = false;
    return o;
  }
  void configure(OugaFlashService &s) {
    OugaFlashService::Timing t;
    t.pollMs = 1;
    // Leave room for Windows scheduler jitter while keeping waits injected.
    t.modeTimeoutMs = 1500;
    t.stableWindowMs = 1000;
    t.waitScale = 0;
    t.readTimeoutMs = 10;
    s.setTiming(t);
    s.configure(dir + "/fake-fastboot.exe", dir + "/logs");
  }
private slots:
  void initTestCase() {
    testRoot = qEnvironmentVariable("ORANGE_TEST_ARTIFACTS");
    QVERIFY2(!testRoot.isEmpty(),
             "Run only with an isolated TEMP artifact directory");
    testRoot = QDir::fromNativeSeparators(testRoot);
    QVERIFY(QDir().mkpath(testRoot));
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName("OrangeToolsTest");
    QCoreApplication::setApplicationName("OugaTests");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       testRoot + "/settings");
  }
  void init() {
    dir = testRoot + "/" + QString::fromLatin1(QTest::currentTestFunction()) +
          "-" + QUuid::createUuid().toString(QUuid::Id128);
    QVERIFY(QDir().mkpath(dir));
    testResourceDirectory = dir + "/nonexistent-tools";
    DeviceOperationLease::setIdleCheck({});
  }
  void cleanup() {
    QVERIFY2(!DeviceOperationLease::owner(), "Leaked device lease");
  }
  void sparse_data() {
    QTest::addColumn<int>("fault");
    for (int i = 0; i < 11; ++i)
      QTest::newRow(qPrintable(QString::number(i))) << i;
  }
  void sparse() {
    QFETCH(int, fault);
    auto b = ::sparse();
    switch (fault) {
    case 1:
      b.chop(1);
      break;
    case 2:
      le16(b, 4, 2);
      break;
    case 3:
      le16(b, 8, 27);
      break;
    case 4:
      le16(b, 10, 11);
      break;
    case 5:
      le32(b, 12, 0);
      break;
    case 6:
      le32(b, 16, 2);
      break;
    case 7:
      le16(b, 28, 0xbeef);
      break;
    case 8:
      le32(b, 36, 99);
      break;
    case 9:
      b += 'x';
      break;
    case 10:
      le32(b, 20, 10000001);
      break;
    }
    QString f = dir + "/稀疏 中文.img";
    QVERIFY(put(f, b));
    QString e;
    auto n = OugaPackage::expandedSize(f, &e);
    if (!fault)
      QCOMPARE(n, 512);
    else
      QVERIFY2(n < 0, qPrintable(e));
  }
  void imageCandidates_data() {
    QTest::addColumn<QString>("subdirectory");
    QTest::addColumn<QString>("file");
    QTest::addColumn<bool>("candidate");
    QTest::newRow("empty") << "" << "" << false;
    QTest::newRow("empty-images") << "images" << "" << false;
    QTest::newRow("payload-not-an-image") << "" << "payload.bin" << false;
    QTest::newRow("arbitrary-bin") << "" << "script.bin" << false;
    QTest::newRow("root-image") << "" << "boot.img" << true;
    QTest::newRow("images") << "images" << "boot.img" << true;
    QTest::newRow("IMAGES") << "IMAGES" << "boot.img" << true;
    QTest::newRow("RADIO") << "RADIO" << "modem.img" << true;
    QTest::newRow("rawprogram-needs-validation")
        << "RADIO" << "rawprogram0.xml" << true;
    QTest::newRow("empty-after-sales-needs-validation")
        << "IMAGES/my_company" << "" << true;
    QTest::newRow("after-sales-image")
        << "my_preload" << "candidate.img" << true;
    QTest::newRow("unsupported-nesting") << "unknown" << "boot.img" << false;
  }
  void imageCandidates() {
    QFETCH(QString, subdirectory);
    QFETCH(QString, file);
    QFETCH(bool, candidate);
    const QString directory = QDir(dir).filePath(subdirectory);
    QVERIFY(QDir().mkpath(directory));
    if (!file.isEmpty())
      QVERIFY(put(QDir(directory).filePath(file), "candidate, not validated"));
    QCOMPARE(OugaPackage::hasImageCandidates(dir), candidate);
    QVERIFY(!OugaPackage::hasImageCandidates(dir + "/missing"));
  }
  void scanBoundaries() {
    QString e;
    QVERIFY(put(dir + "/boot.img", QByteArray(512, 'b')));
    QVERIFY(put(dir + "/payload.bin", "not an image"));
    QVERIFY(put(dir + "/script.bin", "no"));
    QVERIFY(put(dir + "/misc.img", "no"));
    auto p = OugaPackage::scan(dir, &e);
    QCOMPARE(p.size(), 1);
    QCOMPARE(p[0].name, "boot");
    QVERIFY(put(dir + "/boot_b.img", "duplicate"));
    QVERIFY(OugaPackage::scan(dir, &e).isEmpty());
    QVERIFY(e.contains("多个"));
  }
  void rawprogram_data() {
    QTest::addColumn<QString>("extra");
    QTest::addColumn<bool>("valid");
    QTest::newRow("whole") << "file_sector_offset=\"0\"" << true;
    QTest::newRow("offset") << "file_sector_offset=\"1\"" << false;
    QTest::newRow("garbage-offset") << "file_sector_offset=\"oops\"" << false;
    QTest::newRow("partial")
        << "num_partition_sectors=\"2\" SECTOR_SIZE_IN_BYTES=\"512\"" << false;
  }
  void rawprogram() {
    QFETCH(QString, extra);
    QFETCH(bool, valid);
    QVERIFY(put(dir + "/known.bin", QByteArray(512, 0)));
    QString xml = "<data><program label=\"boot\" filename=\"known.bin\" " +
                  extra + "/></data>";
    QVERIFY(put(dir + "/rawprogram0.xml", xml.toUtf8()));
    QString e;
    auto ps = OugaPackage::scan(dir, &e);
    QCOMPARE(!ps.isEmpty(), valid);
  }
  void traversalAndAmbiguity() {
    QString e;
    QVERIFY(put(
        dir + "/rawprogram0.xml",
        "<data><program label=\"boot\" filename=\"../outside.bin\"/></data>"));
    QVERIFY(OugaPackage::scan(dir, &e).isEmpty());
    QString sub = dir + "/other";
    QVERIFY(QDir().mkpath(sub + "/my_company"));
    QVERIFY(put(sub + "/my_company/first.img", "1"));
    QVERIFY(put(sub + "/my_company/second.img", "2"));
    QVERIFY(OugaPackage::scan(sub, &e).isEmpty());
    QVERIFY(e.contains("多个"));
  }
  void archive_data() {
    QTest::addColumn<QString>("entry");
    QTest::addColumn<bool>("ok");
    QTest::newRow("safe") << "Path = IMAGES/boot.img\nAttributes = A\n" << true;
    for (const QString p : {"../escape", "C:/boot.img", "/root", "dir/NUL.img",
                            "boot.img:stream", "dir/file. ", "dir/../boot.img"})
      QTest::newRow(qPrintable(p)) << "Path = " + p + "\n" << false;
    QTest::newRow("symlink") << "Path = alias\nSymbolic Link = ../elsewhere\n"
                             << false;
    QTest::newRow("case-alias") << "Path = boot.img\n\nPath = BOOT.IMG\n"
                                << false;
    QTest::newRow("file-parent")
        << "Path = folder\nAttributes = A\n\nPath = folder/boot.img\n"
        << false;
    QTest::newRow("directory-parent")
        << "Path = folder\nFolder = +\n\nPath = folder/boot.img\n"
        << true;
    QTest::newRow("dot-alias") << "Path = folder/./boot.img\n" << false;
    QTest::newRow("control") << QString("Path = boot") + QChar(1) + ".img\n"
                             << false;
    QTest::newRow("unix-link") << "Path = alias\nMode = lrwxrwxrwx\n" << false;
  }
  void archive() {
    QFETCH(QString, entry);
    QFETCH(bool, ok);
    QString e;
    QCOMPARE(OugaPackage::safeArchiveListing(entry, &e), ok);
  }
  void independentSparseConversion() {
    QString source = dir + "/source.img", dest = dir + "/copy.img", e;
    auto b = ::sparse();
    QVERIFY(put(source, b));
    QVERIFY2(OugaPackage::toRaw(source, dest, 4096, &e), qPrintable(e));
    QCOMPARE(read(source), b);
    QCOMPARE(QFileInfo(dest).size(), 4096);
    QVERIFY(!OugaPackage::toRaw(source, dest, 4096, &e));
  }
  void payloadAndArb() {
    QString f = dir + "/payload.bin", e;
    QVERIFY(put(f, payloadBytes()));
    QVector<OugaPayloadEntry> ps;
    bool delta;
    QVERIFY(OugaPackage::payloadManifest(f, &ps, &delta, &e));
    QVERIFY(!delta);
    QCOMPARE(ps[0].name, "boot");
    QVERIFY(put(f, payloadBytes(true)));
    QVERIFY(OugaPackage::payloadManifest(f, &ps, &delta, &e));
    QVERIFY(delta);
    QCOMPARE(ps[0].oldSize, quint64(512));
    QVERIFY(put(f, arb(9)));
    quint32 v = 0;
    QVERIFY2(OugaPackage::readArb(f, &v, &e), qPrintable(e));
    QCOMPARE(v, quint32(9));
    QVERIFY(put(f, QByteArray(64, 0)));
    QVERIFY(!OugaPackage::readArb(f, &v, &e));
  }
  void superDefinition() {
    auto im = image("system");
    QJsonObject def{
        {"block_devices",
         QJsonArray{QJsonObject{
             {"name", "super"}, {"size", "8388608"}, {"alignment", "4096"}}}},
        {"groups",
         QJsonArray{QJsonObject{{"name", "g"}, {"maximum_size", "4194304"}}}},
        {"partitions", QJsonArray{QJsonObject{{"name", "system_a"},
                                              {"size", "4096"},
                                              {"group_name", "g"},
                                              {"path", "system.img"}}}}};
    QString file = dir + "/super_def.json";
    QVERIFY(put(file, QJsonDocument(def).toJson()));
    QStringList args;
    QSet<QString> merged;
    QString e;
    QVERIFY2(
        OugaPackage::lpmakeArguments(dir, dir + "/out.img", &args, &merged, &e),
        qPrintable(e));
    QVERIFY(args.contains("system_a:readonly:4096:g"));
    QVERIFY(merged.contains("system"));
    auto hash = OugaPackage::digest(im.path, &e);
    def["groups"] =
        QJsonArray{QJsonObject{{"name", "g"}, {"maximum_size", "512"}}};
    QVERIFY(put(file, QJsonDocument(def).toJson()));
    QVERIFY(!OugaPackage::lpmakeArguments(dir, dir + "/out.img", &args, &merged,
                                          &e));
    QCOMPARE(hash, OugaPackage::digest(im.path, &e));
  }
  void sequences_data() {
    QTest::addColumn<int>("mode");
    QTest::addColumn<int>("platform");
    QTest::addColumn<QString>("slot");
    QTest::addColumn<QString>("target");
    for (int mode = 0; mode <= 6; ++mode)
      for (int platform = 1; platform <= 2; ++platform)
        for (QString slot : {"a", "b"})
          for (QString target :
               (mode == 1 ? QStringList{"a", "b"} : QStringList{""})) {
            if (mode == 3 && platform == 2)
              continue;
            QString label = QString("mode%1-p%2-%3-to%4")
                                .arg(mode)
                                .arg(platform)
                                .arg(slot, target);
            QTest::newRow(qPrintable(label))
                << mode << platform << slot << target;
          }
  }
  void sequences() {
    QFETCH(int, mode);
    QFETCH(int, platform);
    QFETCH(QString, slot);
    QFETCH(QString, target);
    auto m = FlashMode(mode);
    auto pf = Platform(platform);
    Device d = fixtureDevice(pf, slot,
                             m != FlashMode::RepairFastbootd &&
                                 m != FlashMode::AfterSalesBootloader);
    Options o = options(m, pf);
    o.targetSlot = target;
    auto ps = images();
    if (m == FlashMode::RepairFastbootd ||
        m == FlashMode::AfterSalesBootloader) {
      ps.clear();
      for (const auto &n : criticalImages(pf))
        ps << image(n);
      if (m == FlashMode::AfterSalesBootloader) {
        ps << image("system");
        ps << image("super", superBytes());
      }
    }
    Plan p;
    QString e;
    QVERIFY2(OugaFlashPlanner::build(ps, d, o, &p, &e), qPrintable(e));
    auto cmds = commands(p);
    int writes = 0;
    QSet<QString> targets;
    for (const auto &s : p.steps) {
      if (!s.image.isEmpty()) {
        ++writes;
        QVERIFY(!targets.contains(s.target));
        targets.insert(s.target);
      }
      QVERIFY(!s.arguments.contains("frp"));
      QVERIFY(!s.arguments.contains("misc"));
      QVERIFY(!s.arguments.contains("flashing"));
    }
    QCOMPARE(p.flashCount, writes);
    QCOMPARE(
        p.totalBytes,
        std::accumulate(p.steps.cbegin(), p.steps.cend(), qint64(0),
                        [](qint64 v, const Step &s) { return v + s.bytes; }));
    if (m == FlashMode::RepairFastbootd) {
      QCOMPARE(writes, criticalImages(pf).size() * 2);
      QVERIFY(!p.options.clearData);
      QVERIFY(!p.options.autoReboot);
      QVERIFY(p.steps.last().kind == Step::ModeSwitch);
      return;
    }
    if (m == FlashMode::AfterSalesBootloader) {
      QCOMPARE(cmds.first(), "erase super");
      QCOMPARE(cmds[1], "flash super");
      QCOMPARE(p.steps[2].waitMs, 120000);
      QVERIFY(p.steps[3].kind == Step::Checkpoint);
      QVERIFY(!targets.contains("system_a"));
      return;
    }
    const QString finalSlot = m == FlashMode::Force       ? "a"
                              : m == FlashMode::BothSlots ? target
                              : m == FlashMode::OnlyFastbootd
                                  ? (slot == "a" ? "b" : "a")
                                  : slot;
    QCOMPARE(p.options.targetSlot, finalSlot);
    QVERIFY(targets.contains("system_" + finalSlot));
    QVERIFY(!targets.contains(
        "system_" + (finalSlot == "a" ? QString("b") : QString("a"))));
    QVERIFY(targets.contains("persist"));
    if (m == FlashMode::BothSlots) {
      QVERIFY(targets.contains("boot_a"));
      QVERIFY(targets.contains("boot_b"));
    }
    if (m == FlashMode::OnlyFastbootd) {
      for (const auto &s : p.steps)
        QVERIFY(s.userspace);
      QCOMPARE(p.steps[0].waitMs, 5000);
      QVERIFY(!cmds.contains("reboot bootloader"));
      QVERIFY(targets.contains("modem_" + finalSlot));
    } else if (pf == Platform::Qualcomm || m == FlashMode::BothSlots ||
               m == FlashMode::AfterSalesFastbootd) {
      QVERIFY(targets.contains("modem_a"));
      QVERIFY(targets.contains("modem_b"));
      if (pf == Platform::Qualcomm)
        QVERIFY(cmds.indexOf("reboot bootloader") <
                cmds.indexOf("flash modem_a"));
    }
    if (m == FlashMode::Force && slot == "b") {
      QVERIFY(cmds.indexOf("flash boot_a") < cmds.indexOf("set_active a"));
      QVERIFY(cmds.indexOf("set_active a") <
              cmds.indexOf("delete-logical-partition system_a"));
    }
    if (m == FlashMode::BothSlots && slot == target)
      for (const auto &c : cmds)
        QVERIFY(!c.startsWith("set_active") &&
                !c.startsWith("create-logical-partition"));
  }
  void targetRouting_data() {
    QTest::addColumn<QStringList>("partitions");
    QTest::addColumn<QString>("hasSlot");
    QTest::addColumn<QString>("source");
    QTest::addColumn<QString>("slot");
    QTest::addColumn<QString>("target");
    const QStringList dual = {"boot_a", "boot_b"};
    QTest::newRow("source-b-to-a")
        << dual << "yes" << "BOOT_B.IMG" << "a" << "boot_a";
    QTest::newRow("source-a-to-b")
        << dual << "" << "boot_a.bin" << "b" << "boot_b";
    QTest::newRow("slotless-once") << QStringList{"persist"} << "no"
                                   << "persist_b.img" << "b" << "persist";
    QTest::newRow("one-present-slot")
        << QStringList{"boot_a"} << "yes" << "boot" << "a" << "boot_a";
    QTest::newRow("missing-other-slot")
        << QStringList{"boot_a"} << "yes" << "boot" << "b" << "";
    QTest::newRow("mixed-table")
        << QStringList{"boot", "boot_a", "boot_b"} << "" << "boot" << "a" << "";
    QTest::newRow("slotless-with-yes")
        << QStringList{"boot"} << "yes" << "boot" << "a" << "";
    QTest::newRow("slotted-with-no") << dual << "no" << "boot" << "a" << "";
    QTest::newRow("unknown-has-slot")
        << dual << "unknown" << "boot" << "a" << "";
    QTest::newRow("missing-target")
        << QStringList{} << "" << "boot" << "a" << "";
    QTest::newRow("invalid-slot") << dual << "yes" << "boot" << "c" << "";
    QTest::newRow("unsafe-name") << dual << "yes" << "../boot.img" << "a" << "";
  }
  void targetRouting() {
    QFETCH(QStringList, partitions);
    QFETCH(QString, hasSlot);
    QFETCH(QString, source);
    QFETCH(QString, slot);
    QFETCH(QString, target);
    Device device;
    for (const QString &name : partitions)
      device.partitions.insert(name);
    if (!hasSlot.isEmpty())
      device.variables["has-slot:" + baseName(source)] = hasSlot;
    QString error = "stale";
    QCOMPARE(device.targetPartition(source, slot, &error), target);
    QCOMPARE(error.isEmpty(), !target.isEmpty());
  }
  void sharedRules() {
    QCOMPARE(
        criticalImages(Platform::Qualcomm),
        QStringList({"boot", "init_boot", "dtbo", "vbmeta", "vendor_boot",
                     "vbmeta_system", "vbmeta_vendor", "modem", "recovery"}));
    QCOMPARE(criticalImages(Platform::MediaTek),
             QStringList({"boot", "init_boot", "dtbo", "vbmeta", "vendor_boot",
                          "vbmeta_system", "vbmeta_vendor", "lk"}));
    QVERIFY(criticalImages(Platform::Unknown).isEmpty());
    QVERIFY(criticalImages(Platform(99)).isEmpty());
    for (int i = 0; i <= 6; ++i) {
      const FlashMode mode = FlashMode(i);
      QCOMPARE(needsAdditionalImages(mode), i == 1 || i == 2 || i == 3);
      QCOMPARE(startsInFastbootd(mode), i != 4 && i != 5);
    }
    for (const QString name :
         {"frp.img", "misc_b.img", "USERDATA_A", "metadata", "super_empty",
          "payload.bin", "gpt_backup", "prog_firehose"})
      QVERIFY2(blockedImageName(name), qPrintable(name));
    for (const QString name : {"boot", "modem", "modem_backup", "my_custom"})
      QVERIFY2(!blockedImageName(name), qPrintable(name));
  }
  void snapshotRules() {
    const Device original = fixtureDevice();
    Device changed = original;
    QVERIFY(original.sameLayout(changed));
    QVERIFY(original.sameSnapshot(changed));
    changed.variables["version-bootloader"] = "transient-version";
    QVERIFY(original.sameSnapshot(changed));
    changed.variables["has-slot:boot"] = "yes";
    QVERIFY(!original.sameLayout(changed));
    QVERIFY(!original.sameSnapshot(changed));
    changed = original;
    changed.sizes["boot_a"] += 512;
    QVERIFY(!original.sameLayout(changed));
    changed = original;
    changed.logical.insert("my_custom_a");
    QVERIFY(!original.sameLayout(changed));
    QVERIFY(changed.isLogical("my_custom"));
    QVERIFY(!original.isLogical("my_custom"));
    QVERIFY(original.isLogical("system_a.img"));
    changed = original;
    changed.partitions.remove("persist");
    QVERIFY(!original.sameLayout(changed));
    changed = original;
    changed.slot = "b";
    QVERIFY(original.sameLayout(changed));
    QVERIFY(!original.sameSnapshot(changed));
    changed = original;
    changed.variables["serialno"] = "OTHER";
    QVERIFY(!original.sameSnapshot(changed));
    changed = original;
    changed.unlockKnown = false;
    QVERIFY(!original.sameSnapshot(changed));
  }
  void deterministicOrderAndProgress() {
    QVector<Partition> ps = {image("boot", QByteArray(4096, 'b')),
                             image("system", QByteArray(1024, 's')),
                             image("persist", QByteArray(512, 'p')),
                             image("modem", QByteArray(256, 'm')),
                             image("vbmeta", QByteArray(128, 'v'))};
    Plan first, second;
    QString error;
    const auto device = fixtureDevice();
    QVERIFY2(OugaFlashPlanner::build(ps, device, options(), &first, &error),
             qPrintable(error));
    std::reverse(ps.begin(), ps.end());
    QVERIFY2(OugaFlashPlanner::build(ps, device, options(), &second, &error),
             qPrintable(error));
    QCOMPARE(planText(first), planText(second));
    QStringList writes;
    for (const QString &cmd : commands(first))
      if (cmd.startsWith("flash "))
        writes << cmd;
    QCOMPARE(writes,
             QStringList({"flash vbmeta_a", "flash persist", "flash system_a",
                          "flash boot_a", "flash modem_a", "flash modem_b"}));
    QCOMPARE(first.flashCount, 6);
    QCOMPARE(first.totalBytes, qint64(128 + 512 + 1024 + 4096 + 256 * 2));
    ps = {image("boot", QByteArray(1024, 'b')),
          image("system", QByteArray(1536, 's')),
          image("persist", QByteArray(512, 'p')),
          image("my_company"),
          image("my_preload"),
          image("modem", QByteArray(256, 'm'))};
    auto opts = options(FlashMode::BothSlots);
    opts.targetSlot = "a";
    QVERIFY2(OugaFlashPlanner::build(ps, device, opts, &second, &error),
             qPrintable(error));
    QCOMPARE(second.flashCount, 8);
    QCOMPARE(second.totalBytes, qint64(5632));
    QCOMPARE(commands(second).count("flash persist"), 1);
  }
  void forceMtkModemLast() {
    Plan plan;
    QString error;
    QVERIFY2(OugaFlashPlanner::build(
                 images(), fixtureDevice(Platform::MediaTek, "b"),
                 options(FlashMode::Force, Platform::MediaTek), &plan, &error),
             qPrintable(error));
    const QStringList cmds = commands(plan);
    QCOMPARE(cmds.count("flash boot_a"), 1);
    QCOMPARE(cmds.count("flash persist"), 1);
    QVERIFY(cmds.indexOf("flash boot_a") < cmds.indexOf("set_active a"));
    QVERIFY(cmds.indexOf("flash persist") < cmds.indexOf("set_active a"));
    QCOMPARE(cmds.last(), QString("flash modem_a"));
    QCOMPARE(cmds.count("flash modem_a"), 1);
    QVERIFY(!cmds.contains("flash modem_b"));
    for (const auto &step : plan.steps)
      QVERIFY(step.userspace);
  }
  void planOverflow_data() {
    QTest::addColumn<bool>("dual");
    QTest::newRow("single-slot-sum") << false;
    QTest::newRow("dual-slot-write-count") << true;
  }
  void planOverflow() {
    QFETCH(bool, dual);
    auto device = fixtureDevice();
    auto opts = options(dual ? FlashMode::BothSlots : FlashMode::Normal);
    opts.targetSlot = "a";
    auto boot = image("boot");
    boot.expandedBytes = std::numeric_limits<qint64>::max() / 2 + 1;
    device.sizes["boot_a"] = device.sizes["boot_b"] =
        quint64(std::numeric_limits<qint64>::max());
    QVector<Partition> ps{boot};
    if (dual)
      ps << image("my_company") << image("my_preload");
    else {
      auto persist = image("persist");
      persist.expandedBytes = boot.expandedBytes;
      device.sizes["persist"] = quint64(std::numeric_limits<qint64>::max());
      ps << persist;
    }
    Plan plan;
    QString error;
    QVERIFY(!OugaFlashPlanner::build(ps, device, opts, &plan, &error));
    QVERIFY2(error.contains("总大小溢出"), qPrintable(error));
    QVERIFY(plan.steps.isEmpty());
    QCOMPARE(plan.flashCount, 0);
  }
  void internalModeGuards() {
    auto opts = options(FlashMode(99));
    Plan plan;
    QString error;
    QVERIFY(!OugaFlashPlanner::build(images(), fixtureDevice(), opts, &plan,
                                     &error));
    QVERIFY(error.contains("未知刷写模式"));
    opts = options();
    opts.afterSuper = true;
    QVERIFY(!OugaFlashPlanner::build(images(), fixtureDevice(), opts, &plan,
                                     &error));
    QVERIFY(error.contains("Super 检查点"));
    opts = options(FlashMode::AfterSalesBootloader);
    opts.afterSuper = true;
    QVERIFY(!OugaFlashPlanner::build(images(), fixtureDevice(), opts, &plan,
                                     &error));
    QVERIFY(error.contains("普通 Fastboot"));
  }
  void guardrails() {
    auto d = fixtureDevice();
    auto ps = images();
    auto o = options();
    Plan p;
    QString e;
    auto reject = [&] { return !OugaFlashPlanner::build(ps, d, o, &p, &e); };
    d.unlockKnown = false;
    QVERIFY(reject());
    d = fixtureDevice();
    d.unlocked = false;
    QVERIFY(reject());
    d = fixtureDevice();
    o.packagePlatform = Platform::MediaTek;
    QVERIFY(reject());
    o = options();
    d.sizes["boot_a"] = 1;
    QVERIFY(reject());
    o.validateTable = false;
    QVERIFY(reject());
    d = fixtureDevice();
    ps << ps[0];
    QVERIFY(reject());
    ps = images();
    o = options(FlashMode::BothSlots);
    o.targetSlot = "b";
    ps.removeAt(2);
    QVERIFY(reject());
    ps = images();
    o = options();
    o.arbDowngrade = true;
    QVERIFY(reject());
    o = options();
    o.clearData = true;
    QVERIFY(reject());
    o.formatToolsReady = true;
    QVERIFY2(!reject(), qPrintable(e));
    QVERIFY(commands(p).contains("-w"));
  }
  void slotlessModem() {
    auto d = fixtureDevice();
    d.partitions.remove("modem_a");
    d.partitions.remove("modem_b");
    d.sizes.remove("modem_a");
    d.sizes.remove("modem_b");
    d.partitions.insert("modem");
    d.sizes["modem"] = 1024 * 1024;
    Plan p;
    QString e;
    QVERIFY2(OugaFlashPlanner::build(images(), d, options(), &p, &e),
             qPrintable(e));
    auto c = commands(p);
    QCOMPARE(c.count("flash modem"), 1);
    QVERIFY(!c.contains("reboot bootloader"));
  }
  void safeDeleteFailure_data() {
    QTest::addColumn<QString>("output");
    QTest::addColumn<bool>("missing");
    QTest::newRow("absent")
        << "FAILED (remote: partition does not exist)" << true;
    QTest::newRow("transport")
        << "FAILED transport read failed: partition does not exist" << false;
    QTest::newRow("denied") << "FAILED (remote: permission denied)" << false;
    QTest::newRow("timeout") << "ERROR timeout partition not found" << false;
  }
  void safeDeleteFailure() {
    QFETCH(QString, output);
    QFETCH(bool, missing);
    QCOMPARE(partitionMissing(output), missing);
    QVERIFY(!commandSucceeded(0, true, output));
  }
  void executionFailure_data() {
    QTest::addColumn<int>("kind");
    for (int i = 0; i < 6; ++i)
      QTest::newRow(qPrintable(QString::number(i))) << i;
  }
  void executionFailure() {
    QFETCH(int, kind);
    FakeRunner r;
    OugaFlashService s(&r);
    configure(s);
    auto o = options();
    o.autoReboot = true;
    Plan p;
    QString e;
    QVERIFY(OugaFlashPlanner::build(images(), r.device, o, &p, &e));
    QSignalSpy done(&s, &OugaFlashService::finished);
    QSignalSpy prog(&s, &OugaFlashService::progress);
    if (kind == 0) {
      r.failAt = "flash boot_a";
      r.failCode = 0;
    }
    if (kind == 1) {
      r.failAt = "flash boot_a";
      r.failOutput = "unknown";
      r.failCode = 2;
    }
    if (kind == 2) {
      r.failAt = "getvar";
      r.normal = false;
      r.failCode = -1;
    }
    if (kind == 3)
      r.device.product = "replacement";
    if (kind == 4)
      put(p.images[0].path, "changed");
    if (kind == 5)
      r.stuckMode = true;
    s.execute(p);
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 3000);
    QVERIFY(!done[0][0].toBool());
    QVERIFY(!r.trace.contains("reboot"));
    for (const auto &row : prog)
      QVERIFY(row[2].toString() != "全部步骤成功");
  }
  void formatDependencies_data() {
    QTest::addColumn<QString>("missing");
    QTest::addColumn<bool>("empty");
    QTest::newRow("complete") << QString() << false;
    for (const QString name : {"mke2fs.exe", "make_f2fs.exe", "mke2fs.conf"}) {
      QTest::newRow(qPrintable(name + "-missing")) << name << false;
      QTest::newRow(qPrintable(name + "-empty")) << name << true;
    }
  }
  void formatDependencies() {
    QFETCH(QString, missing);
    QFETCH(bool, empty);
    for (const QString name : {"mke2fs.exe", "make_f2fs.exe", "mke2fs.conf"}) {
      if (name == missing && !empty)
        continue;
      QVERIFY(put(dir + "/" + name,
                  name == missing ? QByteArray() : QByteArray("fixture-only")));
    }
    const QString error =
        OugaProcessRunner::formatToolsError(dir + "/fake-fastboot.exe");
    QCOMPARE(error.isEmpty(), missing.isEmpty());
    if (!missing.isEmpty())
      QVERIFY2(error.contains(missing), qPrintable(error));
    FakeRunner runner;
    OugaFlashService service(&runner);
    configure(service);
    auto opts = options();
    opts.clearData = true;
    // The IO boundary must not trust a previously prepared boolean flag.
    opts.formatToolsReady = true;
    Plan plan;
    QString buildError;
    QVERIFY2(OugaFlashPlanner::build(images(), runner.device, opts, &plan,
                                     &buildError),
             qPrintable(buildError));
    QSignalSpy done(&service, &OugaFlashService::finished);
    service.execute(plan);
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 3000);
    QCOMPARE(done[0][0].toBool(), missing.isEmpty());
    if (!missing.isEmpty()) {
      QVERIFY(runner.trace.isEmpty());
      QVERIFY(done[0][1].toString().contains(missing));
    } else
      QVERIFY(runner.trace.contains("-w"));
  }
  void streamedOutput_data() {
    QTest::addColumn<int>("style");
    QTest::newRow("completion-only") << 0;
    QTest::newRow("all-streamed") << 1;
    QTest::newRow("streamed-prefix-final-tail") << 2;
  }
  void streamedOutput() {
    QFETCH(int, style);
    FakeRunner runner;
    runner.outputStyle = style;
    runner.markedOutput = true;
    OugaFlashService service(&runner);
    configure(service);
    Plan plan;
    QString error;
    QVERIFY2(OugaFlashPlanner::build(images(), runner.device, options(), &plan,
                                     &error),
             qPrintable(error));
    QSignalSpy done(&service, &OugaFlashService::finished);
    service.execute(plan);
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 3000);
    QVERIFY2(done[0][0].toBool(), qPrintable(done[0][1].toString()));
    const QStringList sessions =
        QDir(dir + "/logs").entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    QCOMPARE(sessions.size(), 1);
    const QByteArray log =
        read(dir + "/logs/" + sessions[0] + "/execution.log");
    for (const QByteArray &marker :
         {QByteArray("OUTPUT-ONE"), QByteArray("OUTPUT-TWO"),
          QByteArray("OUTPUT-TAIL")})
      QCOMPARE(log.count(marker), runner.trace.size());
  }
  void stableProbeIncludesSlotProperties() {
    FakeRunner runner;
    runner.device.variables["has-slot:boot"] = "yes";
    runner.beforeProbe = [](Device &device, int count) {
      device.variables["has-slot:boot"] = count == 3 ? "no" : "yes";
    };
    OugaFlashService service(&runner);
    configure(service);
    QSignalSpy ready(&service, &OugaFlashService::deviceReady);
    QSignalSpy done(&service, &OugaFlashService::finished);
    service.probe(runner.device.serial);
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 3000);
    QVERIFY(done[0][0].toBool());
    QCOMPARE(ready.count(), 1);
    QCOMPARE(runner.probes,
             6); // Initial query, two changes, three stable samples.
    QCOMPARE(
        qvariant_cast<Device>(ready[0][0]).variables.value("has-slot:boot"),
        QString("yes"));
    for (const QString &cmd : runner.trace)
      QVERIFY(cmd.startsWith("getvar "));
  }
  void changedSlotPropertyStopsBeforeWrite() {
    FakeRunner runner;
    runner.device.variables["has-slot:boot"] = "yes";
    Plan plan;
    QString error;
    auto opts = options();
    opts.validateTable = false;
    QVERIFY2(
        OugaFlashPlanner::build(images(), runner.device, opts, &plan, &error),
        qPrintable(error));
    runner.device.variables["has-slot:boot"] = "no";
    OugaFlashService service(&runner);
    configure(service);
    QSignalSpy done(&service, &OugaFlashService::finished);
    service.execute(plan);
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 3000);
    QVERIFY(!done[0][0].toBool());
    QVERIFY2(done[0][1].toString().contains("设备表自预览后改变"),
             qPrintable(done[0][1].toString()));
    for (const QString &cmd : runner.trace)
      QVERIFY(cmd.startsWith("getvar "));
  }
  void stopDuringAsyncVerification() {
    FakeRunner runner;
    OugaFlashService service(&runner);
    configure(service);
    Plan plan;
    QString error;
    QVERIFY2(OugaFlashPlanner::build(images(), runner.device, options(), &plan,
                                     &error),
             qPrintable(error));
    QSignalSpy done(&service, &OugaFlashService::finished);
    service.execute(plan);
    service.requestStop();
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 3000);
    QVERIFY(!done[0][0].toBool());
    QVERIFY(runner.trace.isEmpty());
    QVERIFY(!service.busy());
  }
  void changedImageBetweenWrites() {
    FakeRunner runner;
    runner.device = fixtureDevice(Platform::MediaTek);
    auto ps = images();
    const QString changed = ps[2].path;
    bool mutated = false;
    runner.after = [&](const QStringList &args) {
      if (args.value(0) == "flash" && args.value(1) == "boot_a")
        mutated = put(changed, "changed after first write");
    };
    OugaFlashService service(&runner);
    configure(service);
    auto opts = options(FlashMode::Normal, Platform::MediaTek);
    opts.clearData = opts.autoReboot = true;
    Plan plan;
    QString error;
    QVERIFY2(OugaFlashPlanner::build(ps, runner.device, opts, &plan, &error),
             qPrintable(error));
    QSignalSpy done(&service, &OugaFlashService::finished);
    service.execute(plan);
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 3000);
    QVERIFY(mutated);
    QVERIFY(!done[0][0].toBool());
    QVERIFY2(done[0][1].toString().contains("镜像在执行期间改变"),
             qPrintable(done[0][1].toString()));
    QVERIFY(!runner.trace.join('\n').contains("flash my_company"));
    for (const QString &cmd : runner.trace)
      QVERIFY(!cmd.startsWith("erase ") && cmd != "-w" && cmd != "reboot");
  }
  void resultFailureNeverReportsComplete() {
    FakeRunner runner;
    bool blockedResult = false;
    runner.after = [&](const QStringList &args) {
      if (args.value(0) != "flash" || blockedResult)
        return;
      const QStringList sessions =
          QDir(dir + "/logs").entryList(QDir::Dirs | QDir::NoDotAndDotDot);
      if (sessions.size() == 1)
        blockedResult =
            QDir().mkpath(dir + "/logs/" + sessions[0] + "/result.json");
    };
    OugaFlashService service(&runner);
    configure(service);
    Plan plan;
    QString error;
    QVERIFY2(OugaFlashPlanner::build(images(), runner.device, options(), &plan,
                                     &error),
             qPrintable(error));
    QSignalSpy done(&service, &OugaFlashService::finished);
    QSignalSpy progress(&service, &OugaFlashService::progress);
    service.execute(plan);
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 3000);
    QVERIFY(blockedResult);
    QVERIFY(!done[0][0].toBool());
    QVERIFY2(done[0][1].toString().contains("最终结果"),
             qPrintable(done[0][1].toString()));
    for (const auto &sample : progress)
      QVERIFY(sample[2].toString() != "全部步骤成功");
  }
  void successfulExecutionAndLogs() {
    FakeRunner r;
    OugaFlashService s(&r);
    configure(s);
    Plan p;
    QString e;
    QVERIFY(OugaFlashPlanner::build(images(), r.device, options(), &p, &e));
    QSignalSpy done(&s, &OugaFlashService::finished);
    s.execute(p);
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 3000);
    QVERIFY2(done[0][0].toBool(), qPrintable(done[0][1].toString()));
    QStringList dirs =
        QDir(dir + "/logs").entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    QCOMPARE(dirs.size(), 1);
    QString session = dir + "/logs/" + dirs[0];
    QVERIFY(QFileInfo(session + "/plan-0.json").exists());
    auto result =
        QJsonDocument::fromJson(read(session + "/result.json")).object();
    QVERIFY(result["success"].toBool());
    QCOMPARE(result["completedWrites"].toInt(), p.flashCount);
    QVERIFY(read(session + "/execution.log").contains("TEST-SERIAL"));
  }
  void forcePrewriteFailure() {
    FakeRunner r;
    r.device.slot = "b";
    r.failAt = "flash boot_a";
    OugaFlashService s(&r);
    configure(s);
    Plan p;
    QString e;
    QVERIFY(OugaFlashPlanner::build(images(), r.device,
                                    options(FlashMode::Force), &p, &e));
    QSignalSpy done(&s, &OugaFlashService::finished);
    s.execute(p);
    QTRY_COMPARE(done.count(), 1);
    QVERIFY(!done[0][0].toBool());
    QVERIFY(!r.trace.contains("set_active a"));
  }
  void stopAndLease() {
    QWidget parent;
    FakeRunner r;
    r.holdFlash = true;
    OugaFlashService s(&r, &parent);
    configure(s);
    Plan p;
    QString e;
    QVERIFY(OugaFlashPlanner::build(images(), r.device, options(), &p, &e));
    QSignalSpy done(&s, &OugaFlashService::finished);
    parent.show();
    s.execute(p);
    QTRY_VERIFY(r.active && r.trace.last().startsWith("flash "));
    QObject other;
    QVERIFY(!DeviceOperationLease::acquire(&other));
    QVERIFY(!parent.close());
    parent.showMinimized();
    QVERIFY(s.busy());
    s.requestStop();
    QVERIFY(r.running());
    QCOMPARE(done.count(), 0);
    int count = r.trace.size();
    r.complete(0, true, "OKAY");
    QTRY_COMPARE(done.count(), 1);
    QCOMPARE(r.trace.size(), count);
    QVERIFY(!done[0][0].toBool());
    QVERIFY(parent.close());
  }
  void afterSuperCheckpoint() {
    FakeRunner r;
    r.device.userspace = false;
    OugaFlashService s(&r);
    configure(s);
    QVector<Partition> ps;
    for (const auto &n : criticalImages(Platform::Qualcomm))
      ps << image(n);
    ps << image("system") << image("super", superBytes());
    Plan p;
    QString e;
    auto o = options(FlashMode::AfterSalesBootloader);
    QVERIFY2(OugaFlashPlanner::build(ps, r.device, o, &p, &e), qPrintable(e));
    QSignalSpy done(&s, &OugaFlashService::finished);
    QSignalSpy checkpoint(&s, &OugaFlashService::checkpoint);
    s.execute(p);
    QTRY_COMPARE_WITH_TIMEOUT(checkpoint.count(), 1, 3000);
    QVERIFY(s.paused());
    QVERIFY(DeviceOperationLease::owner() == &s);
    QVERIFY(!r.trace.join('\n').contains("flash boot"));
    s.confirmCheckpoint(false);
    QCOMPARE(done.count(), 1);
    QVERIFY(!done[0][0].toBool());
  }
  void afterSalesTailUsesVerifiedTargets_data() {
    QTest::addColumn<int>("platform");
    QTest::newRow("qualcomm") << 1;
    QTest::newRow("mediatek") << 2;
  }
  void afterSalesTailUsesVerifiedTargets() {
    QFETCH(int, platform);
    const Platform pf = Platform(platform);
    auto device = fixtureDevice(pf, "a", false);
    device.sizes["system_b"] += 1024 * 1024;
    for (const QString slot : {"a", "b"}) {
      device.partitions.insert("modem_backup_" + slot);
      device.sizes["modem_backup_" + slot] = 1024 * 1024;
    }
    QVector<Partition> ps;
    for (const QString &name : criticalImages(pf))
      ps << image(name);
    if (pf == Platform::MediaTek)
      ps << image("modem");
    ps << image("system") << image("super", superBytes())
       << image("modem_backup");
    auto opts = options(FlashMode::AfterSalesBootloader, pf);
    opts.afterSuper = true;
    Plan plan;
    QString error;
    QVERIFY2(OugaFlashPlanner::build(ps, device, opts, &plan, &error),
             qPrintable(error));
    QCOMPARE(plan.options.targetSlot, QString("b"));
    const QStringList cmds = commands(plan);
    for (const QString &name : criticalImages(pf)) {
      QCOMPARE(cmds.count("flash " + name + "_a"), 1);
      QCOMPARE(cmds.count("flash " + name + "_b"), 1);
    }
    QVERIFY(!cmds.contains("flash system_a") &&
            !cmds.contains("flash system_b"));
    QCOMPARE(cmds.count("flash modem_backup_b"), 1);
    QVERIFY(!cmds.contains("flash modem_backup_a"));
    if (pf == Platform::MediaTek) {
      QCOMPARE(cmds.count("flash modem_b"), 1);
      QVERIFY(!cmds.contains("flash modem_a"));
    }
    QCOMPARE(cmds.count("set_active b"), 1);
    QVERIFY(cmds.indexOf("reboot fastboot") <
            cmds.indexOf("flash modem_backup_b"));
    for (const QString &key : device.sizes.keys())
      if (device.isLogical(key) && key.endsWith("_b"))
        device.sizes.remove(key);
    QVERIFY(!OugaFlashPlanner::build(ps, device, opts, &plan, &error));
    QVERIFY2(error.contains("双槽逻辑分区容量"), qPrintable(error));
  }
  void afterSuperReconfirmationAndResume() {
    FakeRunner runner;
    runner.device.userspace = false;
    runner.after = [&](const QStringList &args) {
      if (args.value(0) == "flash" && args.value(1) == "super")
        runner.device.sizes["system_b"] += 1024 * 1024;
    };
    QVector<Partition> ps;
    for (const QString &name : criticalImages(Platform::Qualcomm))
      ps << image(name);
    ps << image("system") << image("vendor") << image("super", superBytes());
    OugaFlashService service(&runner);
    configure(service);
    Plan preview;
    QString error;
    QVERIFY2(OugaFlashPlanner::build(ps, runner.device,
                                     options(FlashMode::AfterSalesBootloader),
                                     &preview, &error),
             qPrintable(error));
    QCOMPARE(preview.options.targetSlot, QString("a"));
    QSignalSpy checkpoint(&service, &OugaFlashService::checkpoint);
    QSignalSpy done(&service, &OugaFlashService::finished);
    service.execute(preview);
    QTRY_COMPARE_WITH_TIMEOUT(checkpoint.count(), 1, 3000);
    const Plan tail = qvariant_cast<Plan>(checkpoint[0][0]);
    QCOMPARE(tail.options.targetSlot, QString("b"));
    QVERIFY(service.paused());
    QVERIFY(!runner.trace.join('\n').contains("flash boot_"));
    service.confirmCheckpoint(true);
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 3000);
    QVERIFY2(done[0][0].toBool(), qPrintable(done[0][1].toString()));
    const QStringList sessions =
        QDir(dir + "/logs").entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    QCOMPARE(sessions.size(), 1);
    const QString session = dir + "/logs/" + sessions[0];
    QVERIFY(QFileInfo(session + "/plan-1.json").exists());
    const auto result =
        QJsonDocument::fromJson(read(session + "/result.json")).object();
    QVERIFY(result["success"].toBool());
    QCOMPARE(result["completedWrites"].toInt(), preview.flashCount);
    QCOMPARE(result["totalWrites"].toInt(), preview.flashCount);
    int writes = 0;
    for (const QString &cmd : runner.trace)
      writes += cmd.startsWith("flash ");
    QCOMPARE(writes, preview.flashCount);
    QVERIFY(!runner.trace.join('\n').contains("flash system_"));
    QVERIFY(runner.trace.join('\n').contains("flash vendor_b "));
  }
  void payloadFailureNeverPrepared() {
    QString f = dir + "/payload.bin", output = dir + "-out";
    QVERIFY(put(f, payloadBytes(true)));
    OugaPreparation prep;
    QSignalSpy done(&prep, &OugaPreparation::finished),
        ready(&prep, &OugaPreparation::prepared);
    prep.payload(dir + "/absent.exe", f, output);
    QTRY_COMPARE(done.count(), 1);
    QVERIFY(!done[0][0].toBool());
    QCOMPARE(ready.count(), 0);
  }
  void missingPreparationTools() {
    QString f = dir + "/package.zip";
    QVERIFY(put(f, "placeholder"));
    OugaPreparation prep;
    QSignalSpy done(&prep, &OugaPreparation::finished),
        ready(&prep, &OugaPreparation::prepared);
    prep.extractArchive(dir + "/nonexistent7z.exe", f, dir + "-out");
    QTRY_COMPARE(done.count(), 1);
    QVERIFY(!done[0][0].toBool());
    QCOMPARE(ready.count(), 0);
  }
  void superBoundaries_data() {
    QTest::addColumn<int>("kind");
    for (int i = 0; i < 7; ++i)
      QTest::newRow(qPrintable(QString::number(i))) << i;
  }
  void superBoundaries() {
    QFETCH(int, kind);
    QByteArray bytes = superBytes();
    auto tables = bytes.mid(12416, 188);
    if (kind == 1)
      le64(tables, 64, 5000); // outside physical device
    if (kind == 2)
      le32(tables, 44, 2); // invalid extent count
    if (kind == 3)
      le64(tables, 116, 512); // too-small group
    if (kind == 4)
      le32(tables, 72, 1); // second physical source
    if (kind == 5)
      le32(tables, 36, 2); // unsupported slot-suffixed layout
    bytes.replace(12416, tables.size(), tables);
    QByteArray header = bytes.mid(12288, 128);
    header.replace(
        48, 32, QCryptographicHash::hash(tables, QCryptographicHash::Sha256));
    header.replace(12, 32, QByteArray(32, 0));
    header.replace(
        12, 32, QCryptographicHash::hash(header, QCryptographicHash::Sha256));
    bytes.replace(12288, 128, header);
    if (kind == 6)
      bytes[4096 + 8] ^= 1;
    QString file = dir + "/super.img", error;
    QSet<QString> names;
    QVERIFY(put(file, bytes));
    QCOMPARE(OugaPackage::superContents(file, &names, &error), kind == 0);
    if (kind == 0)
      QVERIFY(names.contains("system"));
  }
  void readOnlyQueryDeadline() {
    OugaProcessRunner runner;
    QSignalSpy done(&runner, &OugaCommandRunner::completed);
    runner.run(QCoreApplication::applicationFilePath(),
               {"getvar", "orange-test-helper"}, 30);
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 2000);
    QVERIFY(!done[0][1].toBool());
    QVERIFY(done[0][2].toString().contains("timed out"));
    QVERIFY(!runner.running());
  }
  void writeIgnoresReadDeadline() {
    OugaProcessRunner runner;
    QSignalSpy done(&runner, &OugaCommandRunner::completed);
    runner.run(QCoreApplication::applicationFilePath(),
               {"flash", "orange-test-helper"}, 10);
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 3000);
    QCOMPARE(done[0][0].toInt(), 0);
    QVERIFY(done[0][1].toBool());
    QVERIFY(done[0][2].toString().contains("FAILED"));
    QVERIFY(!commandSucceeded(done[0][0].toInt(), done[0][1].toBool(),
                              done[0][2].toString()));
  }
  void noWipeAfterFailure() {
    FakeRunner runner;
    runner.device = fixtureDevice(Platform::MediaTek);
    runner.failAt = "flash boot_a";
    OugaFlashService service(&runner);
    configure(service);
    auto opts = options(FlashMode::Normal, Platform::MediaTek);
    opts.clearData = true;
    opts.autoReboot = true;
    Plan plan;
    QString error;
    QVERIFY2(
        OugaFlashPlanner::build(images(), runner.device, opts, &plan, &error),
        qPrintable(error));
    QSignalSpy done(&service, &OugaFlashService::finished);
    service.execute(plan);
    QTRY_COMPARE(done.count(), 1);
    QVERIFY(!done[0][0].toBool());
    for (const QString &cmd : runner.trace)
      QVERIFY(!cmd.startsWith("erase ") && cmd != "-w" && cmd != "reboot");
  }
  void leaseOwnerLifecycle() {
    auto owner = new QObject;
    QObject stranger;
    QSignalSpy changes(DeviceOperationLease::instance(),
                       &DeviceOperationLease::changed);
    for (int i = 0; i < 3; ++i) {
      QVERIFY(DeviceOperationLease::acquire(owner));
      QVERIFY(DeviceOperationLease::acquire(owner));
      DeviceOperationLease::release(&stranger);
      QVERIFY(DeviceOperationLease::owner() == owner);
      DeviceOperationLease::release(owner);
    }
    QCOMPARE(changes.count(), 6);
    QVERIFY(DeviceOperationLease::acquire(owner));
    delete owner;
    QCOMPARE(changes.count(), 8);
    QVERIFY(!DeviceOperationLease::owner());
  }
  void widgetDefaultGeometryAndCentering_data() {
    QTest::addColumn<bool>("withParent");
    QTest::newRow("launcher-screen") << true;
    QTest::newRow("standalone-screen") << false;
  }
  void widgetDefaultGeometryAndCentering() {
    QFETCH(bool, withParent);
    FakeRunner runner;
    QWidget launcher;
    auto targetScreen = QApplication::primaryScreen();
    QVERIFY(targetScreen);
    const QRect available = targetScreen->availableGeometry();
    launcher.resize(200, 100);
    launcher.move(available.topLeft() + QPoint(20, 30));
    if (withParent)
      launcher.show();
    OugaFlashWindow window(withParent ? &launcher : nullptr, &runner,
                           dir + "/logs");
    QCOMPARE(window.minimumSize(), QSize(779, 656));
    window.show();
    QTRY_COMPARE(window.size(), QSize(779, 656));
    auto centered = [&] {
      const QPoint delta = window.frameGeometry().center() -
                           (withParent ? launcher.screen() : window.screen())
                               ->availableGeometry().center();
      return qAbs(delta.x()) <= 1 && qAbs(delta.y()) <= 1;
    };
    QTRY_VERIFY(centered());
    window.hide();
    window.move(available.topLeft() + QPoint(30, 40));
    window.show();
    QTRY_VERIFY(centered());
    QVERIFY(runner.trace.isEmpty());
    QVERIFY(!window.isBusy());
  }
  void widgetCompactLayout_data() {
    QTest::addColumn<bool>("afterSales");
    QTest::addColumn<QSize>("windowSize");
    QTest::newRow("compact-full") << false << QSize(779, 656);
    QTest::newRow("compact-sales") << true << QSize(779, 656);
    QTest::newRow("expanded-full") << false << QSize(866, 729);
    QTest::newRow("expanded-sales") << true << QSize(866, 729);
  }
  void widgetCompactLayout() {
    QFETCH(bool, afterSales);
    QFETCH(QSize, windowSize);
    FakeRunner runner;
    OugaFlashWindow window(nullptr, &runner, dir + "/logs");
    window.resize(windowSize);
    window.show();
    window.findChild<QCheckBox *>("AfterSalesPackageModeCheckBox")
        ->setChecked(afterSales);
    QCoreApplication::processEvents();
    QCOMPARE(window.size(), windowSize);
    QVERIFY(window.grab().save(dir + "/layout.png"));
    for (auto control : window.findChildren<QWidget *>()) {
      if (!control->isVisible() ||
          !(qobject_cast<QPushButton *>(control) ||
            qobject_cast<QLineEdit *>(control) ||
            qobject_cast<QComboBox *>(control) ||
            qobject_cast<QCheckBox *>(control) ||
            qobject_cast<QProgressBar *>(control) ||
            control->inherits("QGroupBox")))
        continue;
      auto parent = control->parentWidget();
      const QRect geometry = control->geometry();
      const QString message =
          QString("%1 extends beyond %2: child=(%3,%4 %5x%6), parent=%7x%8")
              .arg(control->objectName(),
                   parent ? parent->objectName() : QString())
              .arg(geometry.x()).arg(geometry.y())
              .arg(geometry.width()).arg(geometry.height())
              .arg(parent ? parent->width() : 0)
              .arg(parent ? parent->height() : 0);
      QVERIFY2(parent && parent->rect().contains(control->geometry()),
               qPrintable(message));
    }
    QVERIFY(runner.trace.isEmpty());
    QVERIFY(!window.isBusy());
  }
  void widgetIsInert() {
    auto applicationStyle = QApplication::style();
    FakeRunner runner;
    OugaFlashWindow window(nullptr, &runner, dir + "/logs");
    window.show();
    QTest::qWait(30);
    QVERIFY(!window.isBusy());
    QVERIFY(!DeviceOperationLease::owner());
    QVERIFY(runner.trace.isEmpty());
    auto table = window.findChild<QTableWidget *>("OugaPartitionTableDataGrid");
    QVERIFY(table);
    QCOMPARE(table->rowCount(), 0);
    QCOMPARE(table->columnCount(), 4);
    QVERIFY(!table->wordWrap());
    QCOMPARE(QApplication::style(), applicationStyle);
    auto referenceStyle = window.findChild<QStyle *>(
        "OugaReferenceCheckboxStyle", Qt::FindDirectChildrenOnly);
    QVERIFY(referenceStyle);
    QCOMPARE(window.font().family(), QString("Microsoft YaHei UI"));
    for (auto card : window.findChildren<QGroupBox *>())
      QCOMPARE(card->font().family(), QString("Microsoft YaHei UI"));
    for (auto box : window.findChildren<QCheckBox *>()) {
      QCOMPARE(box->font().family(), QString("Microsoft YaHei UI"));
      QCOMPARE(box->style()->pixelMetric(QStyle::PM_IndicatorWidth),
               referenceStyle->pixelMetric(QStyle::PM_IndicatorWidth));
    }
    QCOMPARE(table->horizontalHeaderItem(1)->text(), QString("名称"));
    QCOMPARE(table->horizontalHeaderItem(2)->text(), QString("大小"));
    QCOMPARE(table->horizontalHeaderItem(3)->text(), QString("文件路径"));
    QVERIFY(window.findChildren<QTabWidget *>().isEmpty());
    auto check = [&](const char *name) {
      return window.findChild<QCheckBox *>(name);
    };
    QVERIFY(check("FullPackageModeCheckBox")->isChecked());
    QVERIFY(!check("AfterSalesPackageModeCheckBox")->isChecked());
    QVERIFY(check("PartitionTableValidationEnabledCheckBox")->isChecked());
    QVERIFY(check("ClearDataCheckBox")->isChecked());
    QVERIFY(check("AutoRebootOugaCheckBox")->isChecked());
    QVERIFY(check("AfterSalesClearDataCheckBox")->isChecked());
    QVERIFY(check("AfterSalesAutoRebootCheckBox")->isChecked());
    QVERIFY(check("AfterSalesFastbootDModeToggle")->isChecked());
    QVERIFY(!check("FlashABCheckBox")->isChecked());
    QVERIFY(!check("FixSuperCheckBox")->isChecked());
    QVERIFY(!check("PureFBDCheckBox")->isChecked());
    check("FlashABCheckBox")->setChecked(true);
    check("FixSuperCheckBox")->setChecked(true);
    QVERIFY(!check("FlashABCheckBox")->isChecked());
    check("PureFBDCheckBox")->setChecked(true);
    QVERIFY(!check("FixSuperCheckBox")->isChecked());
    check("PureFBDCheckBox")->setChecked(false);
    QVERIFY(!check("FlashABCheckBox")->isChecked());
    check("PartitionTableValidationDisabledCheckBox")->setChecked(true);
    QVERIFY(!check("PartitionTableValidationEnabledCheckBox")->isChecked());
    check("PartitionTableValidationDisabledCheckBox")->setChecked(false);
    QVERIFY(check("PartitionTableValidationEnabledCheckBox")->isChecked());
    auto preset = window.findChild<QComboBox *>("PayloadPartitionComboBox");
    QVERIFY(preset->isEditable());
    QCOMPARE(preset->count(), 6);
    QCOMPARE(preset->itemText(3), QString("高通修复FastbootD关键分区"));
    QCOMPARE(preset->itemText(4), QString("联发科修复FastbootD关键分区"));
    auto stop = window.findChild<QPushButton *>("OugaFlashStopPanel");
    QVERIFY(stop->isEnabled());
    stop->click();
    QVERIFY(window.findChild<QPlainTextEdit *>("OugaFlashLogTextBox")
                ->toPlainText()
                .contains("当前无可停止的任务"));
    QVERIFY(runner.trace.isEmpty());
    for (auto b : window.findChildren<QPushButton *>()) {
      QVERIFY(b->text() != "确认并执行…");
      QVERIFY(b->text() != "生成计划");
      QVERIFY(b->text() != "读取设备");
      QVERIFY(b->text() != "生成Super");
    }
    QVERIFY(!window.findChild<QPushButton *>("SelectPayloadFileButton")
                 ->icon()
                 .isNull());
    auto clippedControl = [&]() -> QString {
      for (auto control : window.findChildren<QWidget *>()) {
        if (!control->isVisible() || !(qobject_cast<QPushButton *>(control) ||
                                       qobject_cast<QLineEdit *>(control) ||
                                       qobject_cast<QComboBox *>(control)))
          continue;
        auto parent = control->parentWidget();
        if (parent && !parent->rect().contains(control->geometry()))
          return control->objectName() + " extends beyond " +
                 parent->objectName();
      }
      return {};
    };
    QString clipped = clippedControl();
    QVERIFY2(clipped.isEmpty(), qPrintable(clipped));
    QVERIFY(window.grab().save(dir + "/full.png"));
    check("AfterSalesPackageModeCheckBox")->setChecked(true);
    QVERIFY(!check("FullPackageModeCheckBox")->isChecked());
    QVERIFY(!window.findChild<QPushButton *>("AfterSalesUnpackPayloadButton")
                 ->isEnabled());
    QVERIFY(
        !window.findChild<QWidget *>("PartitionTableValidationSettingPanel")
             ->isEnabled());
    QCoreApplication::processEvents();
    clipped = clippedControl();
    QVERIFY2(clipped.isEmpty(), qPrintable(clipped));
    QVERIFY(window.grab().save(dir + "/sales.png"));
    check("AfterSalesBootloaderModeToggle")->setChecked(true);
    QVERIFY(!check("AfterSalesFastbootDModeToggle")->isChecked());
    check("AfterSalesAutoBrickRecoveryModeToggle")->setChecked(true);
    QVERIFY(!check("AfterSalesBootloaderModeToggle")->isChecked());
    check("AfterSalesAutoBrickRecoveryModeToggle")->setChecked(false);
    QVERIFY(!check("AfterSalesFastbootDModeToggle")->isChecked());
    check("AfterSalesPackageModeCheckBox")->setChecked(false);
    QVERIFY(check("FullPackageModeCheckBox")->isChecked());
    QVERIFY(runner.trace.isEmpty());
    window.findChild<QPushButton *>("StartFlashButton")->click();
    QVERIFY(runner.trace.isEmpty());
    QVERIFY(!window.isBusy());
    QVERIFY(window.close());
  }
  void widgetTableAndPageCache() {
    FakeRunner runner;
    OugaFlashWindow window(nullptr, &runner, dir + "/logs");
    window.show();
    QString full = dir + "/全量 包", sales = dir + "/售后 包";
    QVERIFY(QDir().mkpath(full));
    QVERIFY(QDir().mkpath(sales));
    QVERIFY(put(full + "/boot.img", QByteArray(512, 'b')));
    QVERIFY(put(full + "/xbl.img", QByteArray(512, 'x')));
    QVERIFY(put(sales + "/dtbo.img", QByteArray(512, 'd')));
    auto folder = window.findChild<QLineEdit *>("FolderPathTextBox");
    folder->setText(full);
    QVERIFY(QMetaObject::invokeMethod(folder, "editingFinished"));
    QTRY_VERIFY(!window.isBusy());
    auto table = window.findChild<QTableWidget *>("OugaPartitionTableDataGrid");
    QCOMPARE(table->rowCount(), 2);
    QCOMPARE(table->item(0, 1)->text(), QString("boot"));
    QVERIFY(!(table->item(0, 1)->flags() & Qt::ItemIsEditable));
    QVERIFY(!(table->item(0, 2)->flags() & Qt::ItemIsEditable));
    QVERIFY(table->item(0, 3)->flags() & Qt::ItemIsEditable);
    auto all = window.findChild<QCheckBox *>("SelectAllCheckBox");
    QCOMPARE(all->checkState(), Qt::Checked);
    all->setCheckState(Qt::Unchecked);
    QCOMPARE(table->item(0, 0)->checkState(), Qt::Unchecked);
    table->item(0, 0)->setCheckState(Qt::Checked);
    QCOMPARE(all->checkState(), Qt::PartiallyChecked);
    QString replacement = dir + "/替换 boot.img";
    QVERIFY(put(replacement, QByteArray(1024, 'r')));
    table->item(0, 3)->setText(replacement);
    QTRY_VERIFY(!window.isBusy());
    QCOMPARE(table->item(0, 1)->text(), QString("boot"));
    QCOMPARE(table->item(0, 3)->text(), replacement);
    QCOMPARE(table->item(0, 0)->checkState(), Qt::Checked);
    QVERIFY(table->item(0, 3)->toolTip().contains("SHA-256"));
    table->item(0, 3)->setText(dir + "/缺失.img");
    QTRY_VERIFY(!window.isBusy());
    QCOMPARE(table->item(0, 3)->text(), replacement);
    auto salesMode =
        window.findChild<QCheckBox *>("AfterSalesPackageModeCheckBox");
    salesMode->setChecked(true);
    QCOMPARE(table->rowCount(), 0);
    auto salesFolder =
        window.findChild<QLineEdit *>("AfterSalesFlashPackTextBox");
    salesFolder->setText(sales);
    QVERIFY(QMetaObject::invokeMethod(salesFolder, "editingFinished"));
    QTRY_VERIFY(!window.isBusy());
    QCOMPARE(table->rowCount(), 1);
    QCOMPARE(table->item(0, 1)->text(), QString("dtbo"));
    window.findChild<QCheckBox *>("FullPackageModeCheckBox")->setChecked(true);
    QCOMPARE(table->rowCount(), 2);
    QCOMPARE(table->item(0, 3)->text(), replacement);
    QCOMPARE(table->item(1, 0)->checkState(), Qt::Unchecked);
    QCOMPARE(folder->text(), full);
    QVERIFY(runner.trace.isEmpty());
    QVERIFY(window.grab().save(dir + "/loaded-table.png"));
    QVERIFY(window.close());
  }
  void widgetOutputDirectoryWaitsForPayload_data() {
    QTest::addColumn<bool>("drop");
    QTest::addColumn<bool>("imagesDirectory");
    QTest::newRow("typed-output") << false << false;
    QTest::newRow("dropped-output") << true << false;
    QTest::newRow("typed-images") << false << true;
    QTest::newRow("dropped-images") << true << true;
  }
  void widgetOutputDirectoryWaitsForPayload() {
    QFETCH(bool, drop);
    QFETCH(bool, imagesDirectory);
    FakeRunner runner;
    OugaFlashWindow window(nullptr, &runner, dir + "/logs");
    window.show();
    const QString source = dir + "/OTA.zip";
    const QString output = dir + (imagesDirectory ? "/Images" : "/解包 输出");
    QVERIFY(put(source, "PK test archive"));
    QVERIFY(QDir().mkpath(output));
    window.findChild<QLineEdit *>("PayloadFilePathTextBox")->setText(source);
    auto folder = window.findChild<QLineEdit *>("FolderPathTextBox");
    if (drop) {
      QMimeData mime;
      mime.setUrls({QUrl::fromLocalFile(output)});
      QDragEnterEvent drag(QPoint(30, 120), Qt::CopyAction, &mime, Qt::LeftButton,
                           Qt::NoModifier);
      QApplication::sendEvent(&window, &drag);
      QVERIFY(drag.isAccepted());
      QDropEvent event(QPointF(30, 120), Qt::CopyAction, &mime, Qt::LeftButton,
                       Qt::NoModifier);
      QApplication::sendEvent(&window, &event);
      QVERIFY(event.isAccepted());
    } else {
      folder->setText(output);
      QVERIFY(QMetaObject::invokeMethod(folder, "editingFinished"));
    }
    QTRY_VERIFY(!window.isBusy());
    QCOMPARE(folder->text(), output);
    QCOMPARE(window.findChild<QTableWidget *>("OugaPartitionTableDataGrid")
                 ->rowCount(), 0);
    const QString log = window.findChild<QPlainTextEdit *>("OugaFlashLogTextBox")
                            ->toPlainText();
    QVERIFY(log.contains("解包Payload"));
    QVERIFY(!log.contains("没有可确定用途") && !log.contains("已停止/失败"));
    auto progress = window.findChild<QProgressBar *>("FlashProgressBar");
    QCOMPARE(progress->value(), 0);
    QCOMPARE(progress->property("rate").toString(), QString("待解包"));
    QVERIFY(QDir(output).entryList(QDir::AllEntries | QDir::NoDotAndDotDot)
                .isEmpty());
    QVERIFY(window.grab().save(dir + "/waiting-for-payload.png"));
    window.findChild<QPushButton *>("StartFlashButton")->click();
    QVERIFY(!window.isBusy());
    QVERIFY(runner.trace.isEmpty());
    QVERIFY(!DeviceOperationLease::owner());
    QVERIFY(window.close());
  }
  void widgetOutputClearsPreviousImages() {
    FakeRunner runner;
    OugaFlashWindow window(nullptr, &runner, dir + "/logs");
    const QString ready = dir + "/ready", output = dir + "/output";
    QVERIFY(QDir().mkpath(ready));
    QVERIFY(QDir().mkpath(output));
    QVERIFY(put(ready + "/boot.img", QByteArray(512, 'b')));
    auto folder = window.findChild<QLineEdit *>("FolderPathTextBox");
    auto table = window.findChild<QTableWidget *>("OugaPartitionTableDataGrid");
    folder->setText(ready);
    QVERIFY(QMetaObject::invokeMethod(folder, "editingFinished"));
    QTRY_VERIFY(!window.isBusy());
    QCOMPARE(table->rowCount(), 1);
    window.findChild<QProgressBar *>("FlashProgressBar")->setValue(100);
    folder->setText(output);
    QVERIFY(QMetaObject::invokeMethod(folder, "editingFinished"));
    QVERIFY(!window.isBusy());
    QCOMPARE(table->rowCount(), 0);
    window.findChild<QCheckBox *>("AfterSalesPackageModeCheckBox")->setChecked(true);
    window.findChild<QCheckBox *>("FullPackageModeCheckBox")->setChecked(true);
    QCOMPARE(table->rowCount(), 0);
    QCOMPARE(folder->text(), output);
    window.findChild<QPushButton *>("StartFlashButton")->click();
    QVERIFY(runner.trace.isEmpty());
    QVERIFY(!window.isBusy());
  }
  void widgetOutputStillValidatesCandidates_data() {
    QTest::addColumn<QString>("kind");
    QTest::addColumn<bool>("valid");
    for (const QString kind : {"root", "images", "IMAGES", "RADIO"})
      QTest::newRow(qPrintable(kind)) << kind << true;
    for (const QString kind : {"bad-sparse", "bad-rawprogram", "ambiguous-sales"})
      QTest::newRow(qPrintable(kind)) << kind << false;
  }
  void widgetOutputStillValidatesCandidates() {
    QFETCH(QString, kind);
    QFETCH(bool, valid);
    const QString output = dir + "/包 中文路径";
    QString contents = output;
    if (kind == "images" || kind == "IMAGES" || kind == "RADIO")
      contents += "/" + kind;
    if (kind == "ambiguous-sales")
      contents += "/IMAGES/my_company";
    QVERIFY(QDir().mkpath(contents));
    if (kind == "bad-rawprogram")
      QVERIFY(put(contents + "/rawprogram0.xml", "<data><program"));
    else if (kind == "ambiguous-sales") {
      QVERIFY(put(contents + "/first.img", "1"));
      QVERIFY(put(contents + "/second.img", "2"));
    } else {
      auto bytes = kind == "bad-sparse" ? ::sparse() : QByteArray(512, 'b');
      if (kind == "bad-sparse")
        bytes.chop(1);
      QVERIFY(put(contents + "/boot.img", bytes));
    }
    FakeRunner runner;
    OugaFlashWindow window(nullptr, &runner, dir + "/logs");
    auto folder = window.findChild<QLineEdit *>("FolderPathTextBox");
    folder->setText(output);
    QVERIFY(QMetaObject::invokeMethod(folder, "editingFinished"));
    QTRY_VERIFY(!window.isBusy());
    QCOMPARE(window.findChild<QTableWidget *>("OugaPartitionTableDataGrid")
                 ->rowCount(), valid ? 1 : 0);
    const QString log = window.findChild<QPlainTextEdit *>("OugaFlashLogTextBox")
                            ->toPlainText();
    QCOMPARE(log.contains("已停止/失败"), !valid);
    QVERIFY(!log.contains("已选择解包输出目录"));
    QVERIFY(runner.trace.isEmpty());
  }
  void widgetSalesEmptyDirectoryStillFails() {
    FakeRunner runner;
    OugaFlashWindow window(nullptr, &runner, dir + "/logs");
    window.findChild<QCheckBox *>("AfterSalesPackageModeCheckBox")->setChecked(true);
    auto folder = window.findChild<QLineEdit *>("AfterSalesFlashPackTextBox");
    folder->setText(dir);
    QVERIFY(QMetaObject::invokeMethod(folder, "editingFinished"));
    QTRY_VERIFY(!window.isBusy());
    const QString log = window.findChild<QPlainTextEdit *>("OugaFlashLogTextBox")
                            ->toPlainText();
    QVERIFY(log.contains("没有可确定用途") && log.contains("已停止/失败"));
    QVERIFY(!log.contains("已选择解包输出目录"));
    QVERIFY(runner.trace.isEmpty());
  }
  void widgetZipPayloadPreparation_data() {
    QTest::addColumn<bool>("imagesDirectory");
    QTest::addColumn<bool>("corruptOutput");
    QTest::addColumn<QString>("toolConfiguration");
    QTest::newRow("zip-output-folder") << false << false << "configured";
    QTest::newRow("zip-existing-images-folder") << true << false << "configured";
    QTest::newRow("zip-corrupt-extracted-image") << false << true << "configured";
    QTest::newRow("zip-bundled-tools-unconfigured") << false << false << "bundled";
    QTest::newRow("zip-bundled-tools-empty-settings") << false << false << "empty";
    QTest::newRow("zip-bundled-tools-stale-settings") << true << false << "stale";
  }
  void widgetZipPayloadPreparation() {
    QFETCH(bool, imagesDirectory);
    QFETCH(bool, corruptOutput);
    QFETCH(QString, toolConfiguration);
    QSettings settings;
    const QVariant old7z = settings.value("Ouga/7z");
    const QVariant oldPayload = settings.value("Ouga/payload");
    const auto restore = qScopeGuard([&] {
      for (const auto &entry : {qMakePair(QString("Ouga/7z"), old7z),
                                qMakePair(QString("Ouga/payload"), oldPayload)}) {
        if (entry.second.isValid())
          settings.setValue(entry.first, entry.second);
        else
          settings.remove(entry.first);
      }
    });
    const QString helper = QCoreApplication::applicationFilePath();
    if (toolConfiguration == "configured") {
      settings.setValue("Ouga/7z", helper);
      settings.setValue("Ouga/payload", helper);
    } else {
      // Only test executables are discoverable here, never installed tools.
      const QString tools = ResourceExtractor::getResourcePath();
      QVERIFY(QDir().mkpath(tools + "/bin/7zip"));
      QVERIFY(QFile::copy(helper, tools + "/bin/7zip/7z.exe"));
      QVERIFY(put(tools + "/bin/7zip/7z.dll", "inert format-library fixture"));
      QVERIFY(QFile::copy(helper, tools + "/payload.exe"));
      for (const QString key : {"7z", "payload"}) {
        if (toolConfiguration == "bundled")
          settings.remove("Ouga/" + key);
        else
          settings.setValue("Ouga/" + key, toolConfiguration == "empty"
              ? QString() : dir + "/removed/" + key + ".exe");
      }
    }
    const QString sourceDirectory = dir + "/输入 ZIP";
    const QString output = dir + (imagesDirectory ? "/Images"
                               : corruptOutput ? "/corrupt-output" : "/解包 输出");
    QVERIFY(QDir().mkpath(sourceDirectory));
    QVERIFY(QDir().mkpath(output));
    const QString source = sourceDirectory + "/OTA.mock-archive";
    const QByteArray sourceBytes("PK test-only archive");
    QVERIFY(put(source, sourceBytes));
    FakeRunner runner;
    OugaFlashWindow window(nullptr, &runner, dir + "/logs");
    window.show();
    window.findChild<QLineEdit *>("PayloadFilePathTextBox")->setText(source);
    auto folder = window.findChild<QLineEdit *>("FolderPathTextBox");
    folder->setText(output);
    QVERIFY(QMetaObject::invokeMethod(folder, "editingFinished"));
    QVERIFY(!window.isBusy());
    const bool previousDialogPolicy =
        QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    const auto restoreDialogs = qScopeGuard([&] {
      QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, previousDialogPolicy);
    });
    int unexpectedFileDialogs = 0;
    QTimer dialogGuard;
    dialogGuard.setInterval(1);
    connect(&dialogGuard, &QTimer::timeout, &window, [&] {
      for (QWidget *widget : QApplication::topLevelWidgets())
        if (auto dialog = qobject_cast<QFileDialog *>(widget))
          if (dialog->isVisible()) {
            ++unexpectedFileDialogs;
            dialog->reject();
          }
    });
    dialogGuard.start();
    window.findChild<QPushButton *>("UnpackPayloadButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 10000);
    QCOMPARE(unexpectedFileDialogs, 0);
    const QString images = imagesDirectory ? output : output + "/images";
    QVERIFY(QFileInfo::exists(images + "/boot.img"));
    QVERIFY(!QFileInfo::exists(images + "/images"));
    auto table = window.findChild<QTableWidget *>("OugaPartitionTableDataGrid");
    QCOMPARE(table->rowCount(), corruptOutput ? 0 : 1);
    const QString log = window.findChild<QPlainTextEdit *>("OugaFlashLogTextBox")
                            ->toPlainText();
    if (corruptOutput) {
      QVERIFY(log.contains("SHA-256 不符"));
      QVERIFY(!log.contains("文件已准备"));
      window.findChild<QPushButton *>("StartFlashButton")->click();
    } else {
      QCOMPARE(folder->text(), images);
      QCOMPARE(table->item(0, 1)->text(), QString("boot"));
      QCOMPARE(QFileInfo(table->item(0, 3)->text()).canonicalFilePath(),
               QFileInfo(images + "/boot.img").canonicalFilePath());
      QCOMPARE(read(images + "/boot.img"), QByteArray(512, 'p'));
      QVERIFY(log.contains("Payload 已提取并重新校验"));
      QVERIFY(!log.contains("已停止/失败"));
      QVERIFY(window.grab().save(dir + "/payload-loaded.png"));
    }
    QCOMPARE(read(source), sourceBytes);
    QVERIFY(runner.trace.isEmpty());
    QVERIFY(!DeviceOperationLease::owner());
    QVERIFY(window.close());
  }
  void widgetDropOnlyPrepares() {
    FakeRunner runner;
    OugaFlashWindow window(nullptr, &runner, dir + "/logs");
    window.show();
    const QString source = dir + "/payload.bin";
    QVERIFY(put(source, payloadBytes()));
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(source)});
    QDragEnterEvent drag(QPoint(30, 120), Qt::CopyAction, &mime, Qt::LeftButton,
                         Qt::NoModifier);
    QApplication::sendEvent(&window, &drag);
    QVERIFY(drag.isAccepted());
    QDropEvent drop(QPointF(30, 120), Qt::CopyAction, &mime, Qt::LeftButton,
                    Qt::NoModifier);
    QApplication::sendEvent(&window, &drop);
    QCOMPARE(window.findChild<QLineEdit *>("PayloadFilePathTextBox")->text(),
             source);
    QVERIFY(!window.isBusy());
    QVERIFY(runner.trace.isEmpty());
    QCOMPARE(window.findChild<QTableWidget *>("OugaPartitionTableDataGrid")
                 ->rowCount(),
             0);
  }
  void widgetStartWorkflow_data() {
    QTest::addColumn<int>("outcome");
    QTest::newRow("cancel-confirmation") << 0;
    QTest::newRow("success") << 1;
    QTest::newRow("zero-exit-FAILED") << 2;
    QTest::newRow("nonzero") << 3;
  }
  void widgetStartWorkflow() {
    QFETCH(int, outcome);
    FakeRunner runner;
    runner.multipleDevices = false;
    if (outcome >= 2) {
      runner.failAt = "flash boot_a";
      runner.failCode = outcome == 2 ? 0 : 1;
      runner.failOutput =
          outcome == 2 ? "FAILED (remote: denied)" : "command exited";
    }
    QVERIFY(put(dir + "/boot.img", QByteArray(512, 'b')));
    QVERIFY(put(dir + "/xbl.img", QByteArray(512, 'x')));
    QString fake = dir + "/fake-fastboot.exe";
    QVERIFY(put(fake, "TEST: executed only by injected FakeRunner"));
    QSettings().setValue("Ouga/fastboot", fake);
    OugaFlashWindow window(nullptr, &runner, dir + "/logs");
    window.show();
    auto service = window.findChild<OugaFlashService *>();
    configure(*service);
    auto folder = window.findChild<QLineEdit *>("FolderPathTextBox");
    folder->setText(dir);
    QVERIFY(QMetaObject::invokeMethod(folder, "editingFinished"));
    QTRY_VERIFY(!window.isBusy());
    window.findChild<QCheckBox *>("ClearDataCheckBox")->setChecked(false);
    window.findChild<QCheckBox *>("AutoRebootOugaCheckBox")->setChecked(false);
    bool confirmed = false, wasDisabled = false, hasSerial = false;
    QTimer answer;
    connect(&answer, &QTimer::timeout, &window, [&] {
      auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      if (!dialog || dialog->objectName() != "OugaFlashConfirmDialog")
        return;
      confirmed = true;
      auto box = dialog->findChild<QDialogButtonBox *>();
      wasDisabled = !box->button(QDialogButtonBox::Ok)->isEnabled();
      hasSerial = dialog->findChild<QPlainTextEdit *>("OugaConfirmedPlan")
                      ->toPlainText()
                      .contains(runner.device.serial);
      if (!outcome)
        box->button(QDialogButtonBox::Cancel)->click();
      else {
        dialog->findChild<QCheckBox *>("OugaConfirmAgreement")
            ->setChecked(true);
        box->button(QDialogButtonBox::Ok)->click();
      }
    });
    answer.start(5);
    window.findChild<QPushButton *>("StartFlashButton")->click();
    QVERIFY(window.isBusy());
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 8000);
    answer.stop();
    QVERIFY(confirmed);
    QVERIFY(wasDisabled);
    QVERIFY(hasSerial);
    QVERIFY(runner.trace.contains("devices"));
    QVERIFY(runner.trace.contains("getvar all"));
    const bool written =
        std::any_of(runner.trace.cbegin(), runner.trace.cend(),
                    [](const QString &s) { return s.startsWith("flash "); });
    QCOMPARE(written, outcome != 0);
    auto progress = window.findChild<QProgressBar *>("FlashProgressBar");
    QCOMPARE(progress->value() == 100, outcome == 1);
    for (const QString &command : runner.trace)
      QVERIFY(!command.startsWith("erase ") && command != "-w" &&
              command != "reboot");
    if (outcome >= 2) {
      QCOMPARE(std::count_if(
                   runner.trace.cbegin(), runner.trace.cend(),
                   [](const QString &s) { return s.startsWith("flash "); }),
               1);
    }
    QVERIFY(!DeviceOperationLease::owner());
    QVERIFY(window.close());
  }
  void widgetStopDuringPrompt_data() {
    QTest::addColumn<QString>("stage");
    for (const auto &stage :
         {"tool", "device", "mode", "additional", "platform", "slot", "format"})
      QTest::newRow(stage) << QString::fromLatin1(stage);
  }
  void widgetStopDuringPrompt() {
    QFETCH(QString, stage);
    FakeRunner runner;
    runner.multipleDevices = stage == "device";
    runner.device.userspace = stage != "mode";
    QVERIFY(put(dir + "/boot.img", QByteArray(512, 'b')));
    if (stage != "platform")
      QVERIFY(put(dir + "/xbl.img", QByteArray(512, 'x')));
    if (stage == "slot") {
      QVERIFY(put(dir + "/my_company.img", QByteArray(512, 'c')));
      QVERIFY(put(dir + "/my_preload.img", QByteArray(512, 'p')));
    }
    QString fake = dir + "/fake-fastboot.exe";
    QVERIFY(put(fake, "TEST: injected runner only"));
    QSettings().setValue("Ouga/fastboot",
                         stage == "tool" ? dir + "/missing.exe" : fake);
    OugaFlashWindow window(nullptr, &runner, dir + "/logs");
    window.show();
    configure(*window.findChild<OugaFlashService *>());
    auto folder = window.findChild<QLineEdit *>("FolderPathTextBox");
    folder->setText(dir);
    QVERIFY(QMetaObject::invokeMethod(folder, "editingFinished"));
    QTRY_VERIFY(!window.isBusy());
    window.findChild<QCheckBox *>("ClearDataCheckBox")
        ->setChecked(stage == "format");
    window.findChild<QCheckBox *>("AutoRebootOugaCheckBox")->setChecked(false);
    window.findChild<QCheckBox *>("FlashABCheckBox")
        ->setChecked(stage == "additional" || stage == "slot");
    const QMap<QString, QString> titles = {
        {"tool", "选择 fastboot.exe"},
        {"device", "选择线刷设备"},
        {"mode", "准备线刷模式"},
        {"additional",
         "选择与设备及ROM匹配的my_company.img（不使用内置替代镜像）"},
        {"platform", "核对刷机包平台"},
        {"slot", "选择最终启动槽"},
        {"format", "缺少格式化依赖"}};
    bool stoppedInPrompt = false;
    QStringList unexpected;
    QTimer answer;
    connect(&answer, &QTimer::timeout, &window, [&] {
      auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      if (!dialog)
        return;
      if (stoppedInPrompt || dialog->windowTitle() != titles.value(stage)) {
        unexpected << dialog->windowTitle();
        dialog->reject();
        return;
      }
      stoppedInPrompt = true;
      window.findChild<QPushButton *>("OugaFlashStopPanel")->click();
      // Accept after requesting stop: this must not resume the old workflow.
      if (auto message = qobject_cast<QMessageBox *>(dialog))
        message->button(QMessageBox::Yes)->click();
      else
        dialog->done(QDialog::Accepted);
    });
    answer.start(5);
    window.findChild<QPushButton *>("StartFlashButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 8000);
    answer.stop();
    QVERIFY(stoppedInPrompt);
    QVERIFY2(unexpected.isEmpty(), qPrintable(unexpected.join(';')));
    for (const auto &command : runner.trace)
      QVERIFY2(command == "devices" || command.startsWith("getvar "),
               qPrintable(command));
    if (stage == "tool")
      QVERIFY(runner.trace.isEmpty());
    QVERIFY(!DeviceOperationLease::owner());
    QVERIFY(window.close());
  }
  void widgetStopAndCloseBoundary() {
    FakeRunner runner;
    runner.multipleDevices = false;
    runner.holdFlash = true;
    QVERIFY(put(dir + "/boot.img", QByteArray(512, 'b')));
    QVERIFY(put(dir + "/xbl.img", QByteArray(512, 'x')));
    QString fake = dir + "/fake-fastboot.exe";
    QVERIFY(put(fake, "TEST: FakeRunner only"));
    QSettings().setValue("Ouga/fastboot", fake);
    OugaFlashWindow window(nullptr, &runner, dir + "/logs");
    window.show();
    configure(*window.findChild<OugaFlashService *>());
    auto folder = window.findChild<QLineEdit *>("FolderPathTextBox");
    folder->setText(dir);
    QVERIFY(QMetaObject::invokeMethod(folder, "editingFinished"));
    QTRY_VERIFY(!window.isBusy());
    window.findChild<QCheckBox *>("ClearDataCheckBox")->setChecked(false);
    window.findChild<QCheckBox *>("AutoRebootOugaCheckBox")->setChecked(false);
    QTimer answer;
    connect(&answer, &QTimer::timeout, &window, [&] {
      auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      if (!dialog || dialog->objectName() != "OugaFlashConfirmDialog")
        return;
      dialog->findChild<QCheckBox *>("OugaConfirmAgreement")->setChecked(true);
      dialog->findChild<QPushButton *>("OugaConfirmWriteButton")->click();
    });
    answer.start(5);
    window.findChild<QPushButton *>("StartFlashButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(
        runner.active && runner.trace.last().startsWith("flash "), 8000);
    answer.stop();
    QVERIFY(!window.close());
    window.showMinimized();
    const int count = runner.trace.size();
    window.findChild<QPushButton *>("OugaFlashStopPanel")->click();
    QVERIFY(window.isBusy());
    QVERIFY(runner.active);
    QCOMPARE(runner.trace.size(), count);
    runner.complete(0, true, "OKAY\nFinished");
    QTRY_VERIFY(!window.isBusy());
    QCOMPARE(runner.trace.size(), count);
    QVERIFY(window.findChild<QProgressBar *>("FlashProgressBar")->value() <
            100);
    QVERIFY(!DeviceOperationLease::owner());
    window.showNormal();
    QVERIFY(window.close());
  }
  void widgetExistingOperationBlocksStart() {
    FakeRunner runner;
    OugaFlashWindow window(nullptr, &runner, dir + "/logs");
    QVERIFY(put(dir + "/boot.img", QByteArray(512, 'b')));
    auto folder = window.findChild<QLineEdit *>("FolderPathTextBox");
    folder->setText(dir);
    QVERIFY(QMetaObject::invokeMethod(folder, "editingFinished"));
    QTRY_VERIFY(!window.isBusy());
    QObject other;
    QVERIFY(DeviceOperationLease::acquire(&other));
    window.findChild<QPushButton *>("StartFlashButton")->click();
    QVERIFY(!window.isBusy());
    QVERIFY(runner.trace.isEmpty());
    QCOMPARE(DeviceOperationLease::owner(), &other);
    DeviceOperationLease::release(&other);
  }
  void archivePayloadOnly_data() {
    QTest::addColumn<bool>("scanImages");
    QTest::newRow("defer-image-scan-for-payload") << false;
    QTest::newRow("regular-archive-requires-images") << true;
  }
  void archivePayloadOnly() {
    QFETCH(bool, scanImages);
    QVERIFY(QDir().mkpath(dir + "/input"));
    QString source = dir + "/input/only-payload.mock-archive";
    QVERIFY(put(source, "test helper input"));
    OugaPreparation prep;
    QSignalSpy extracted(&prep, &OugaPreparation::archiveExtracted);
    QSignalSpy prepared(&prep, &OugaPreparation::prepared);
    QSignalSpy done(&prep, &OugaPreparation::finished);
    prep.extractArchive(QCoreApplication::applicationFilePath(), source,
                        dir + "/output", scanImages);
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 5000);
    QCOMPARE(done[0][0].toBool(), !scanImages);
    QCOMPARE(extracted.count(), scanImages ? 0 : 1);
    QCOMPARE(prepared.count(), 0);
    QVERIFY(QFileInfo::exists(dir + "/output/payload.bin"));
    if (!scanImages)
      QCOMPARE(extracted[0][0].toString(), dir + "/output");
  }
  void authorizedAdbDiscoveryAndRead() {
    OugaPreparation prep;
    QSignalSpy devices(&prep, &OugaPreparation::adbDevicesFound);
    QSignalSpy done(&prep, &OugaPreparation::finished);
    QSignalSpy read(&prep, &OugaPreparation::arbRead);
    // Explicitly run this test executable as a fake tool, never installed ADB.
    prep.discoverAdb(QCoreApplication::applicationFilePath());
    QTRY_COMPARE(done.count(), 1);
    QVERIFY(done[0][0].toBool());
    QCOMPARE(devices.count(), 1);
    QCOMPARE(devices[0][0].toStringList(), QStringList{"TEST-ADB"});
    prep.readCurrentArb(QCoreApplication::applicationFilePath(), "TEST-ADB",
                        dir + "/current.img");
    QTRY_COMPARE(done.count(), 2);
    QVERIFY(done[1][0].toBool());
    QCOMPARE(read.count(), 1);
    QCOMPARE(read[0][1].toUInt(), quint32(7));
    QCOMPARE(::read(dir + "/current.img"), arb(7));
    QVERIFY(!DeviceOperationLease::owner());
  }
};
int main(int argc, char **argv) {
  if (argc == 3 && std::strcmp(argv[2], "orange-test-helper") == 0) {
    // The real runner launches this test executable, never any device tool.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::fprintf(stderr, "FAILED (remote: injected test result)\n");
    return 0;
  }
  if (!qEnvironmentVariable("ORANGE_TEST_ARTIFACTS").isEmpty() && argc > 1) {
    const QString command = QString::fromLocal8Bit(argv[1]);
    if (command == "l" &&
        QString::fromLocal8Bit(argv[argc - 1]).endsWith(".mock-archive")) {
      std::printf("Path = payload.bin\nSize = 100\nAttributes = A\n\n");
      return 0;
    }
    if (command == "x" &&
        QString::fromLocal8Bit(argv[argc - 1]).endsWith(".mock-archive")) {
      QString output;
      for (int i = 2; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg.startsWith("-o"))
          output = arg.mid(2);
      }
      if (output.isEmpty())
        return 2;
      if (!put(output + "/payload.bin", payloadBytes()))
        return 3;
      std::printf("Everything is Ok\n");
      return 0;
    }
    if (command == "--out" && argc >= 6 &&
        QString::fromLocal8Bit(argv[argc - 1]).endsWith("payload.bin")) {
      // Only this executable's fixture format is accepted; no real tools.
      const QString source = QString::fromLocal8Bit(argv[argc - 1]);
      if (read(source) != payloadBytes())
        return 5;
      const QString output = QDir::fromNativeSeparators(
          QString::fromLocal8Bit(argv[2]));
      const bool corrupt = output.contains("/corrupt-output/");
      if (!put(output + "/boot.img", QByteArray(512, corrupt ? 'x' : 'p')))
        return 6;
      std::printf("Payload fixture extracted\n");
      return 0;
    }
    if (command == "devices" && argc == 2) {
      std::printf(
          "List of devices "
          "attached\nTEST-"
          "ADB\tdevice\nUNAUTHORIZED\tunauthorized\nOFFLINE\toffline\n");
      return 0;
    }
    if (command == "-s" && argc == 6 && std::strcmp(argv[2], "TEST-ADB") == 0) {
      if (std::strcmp(argv[3], "shell") == 0 &&
          std::strcmp(argv[4], "getprop") == 0 &&
          std::strcmp(argv[5], "ro.boot.slot_suffix") == 0) {
        std::printf("_a\n");
        return 0;
      }
      if (std::strcmp(argv[3], "exec-out") == 0 &&
          std::strcmp(argv[4], "cat") == 0 &&
          std::strcmp(argv[5], "/dev/block/by-name/xbl_config_a") == 0) {
        const auto bytes = arb(7);
        std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
        return 0;
      }
      return 4;
    }
  }
  QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
  QApplication app(argc, argv);
  OugaTests tests;
  return QTest::qExec(&tests, argc, argv);
}
#include "ougatests.moc"
