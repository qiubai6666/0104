#include "deviceoperationlease.h"
#include "ougaflashplanner.h"
#include "ougaflashservice.h"
#include "ougaflashwindow.h"
#include "ougapayloadextractor.h"
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
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QGroupBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMessageBox>
#include <QMutex>
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
#include <QThread>
#include <QUuid>
#include <QtEndian>
#include <QtTest>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif
#include <algorithm>
#include <chrono>
#include <climits>
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
// Manifest-only fixtures; extraction is performed by this test executable.
QByteArray payloadPartition(const QByteArray &name, int type,
                            bool oldMetadata = false,
                            const QByteArray &operationExtras = {}) {
  const QByteArray contents(512, 'p');
  const QByteArray info =
      vi(8) + vi(contents.size()) +
      pb(2, QCryptographicHash::hash(contents, QCryptographicHash::Sha256));
  QByteArray operation = type < 0 ? QByteArray() : vi(8) + vi(quint64(type));
  operation += operationExtras;
  QByteArray part = pb(1, name) + pb(7, info) + pb(8, operation);
  if (oldMetadata)
    part += pb(6, info);
  return pb(13, part);
}
QByteArray payloadCounterPartition(const QByteArray &name, int operations) {
  const QByteArray info = vi(8) + vi(512) +
      pb(2, QCryptographicHash::hash(QByteArray(512, 'p'), QCryptographicHash::Sha256));
  QByteArray part = pb(1, name) + pb(7, info);
  for (int n = 0; n < operations; ++n) part += pb(8, vi(8) + vi(0));
  return pb(13, part);
}
QByteArray payloadContainer(const QByteArray &manifest) {
  QByteArray h(24, 0);
  h.replace(0, 4, "CrAU");
  qToBigEndian<quint64>(2, reinterpret_cast<uchar *>(h.data() + 4));
  qToBigEndian<quint64>(manifest.size(),
                        reinterpret_cast<uchar *>(h.data() + 12));
  return h + manifest;
}
QByteArray payloadBytes(bool delta = false, quint64 minorVersion = 0,
                        bool oldMetadata = false) {
  return payloadContainer(payloadPartition("boot", delta ? 4 : 0,
                                           delta || oldMetadata) +
                          vi(12 << 3) + vi(minorVersion));
}
QByteArray mixedPayloadBytes() {
  return payloadContainer(payloadPartition("boot", 0) +
                          payloadPartition("system", 4, true) +
                          vi(12 << 3) + vi(9));
}
// Real, decodable full payload for the in-process extractor (block size 4096).
struct NativeOp {
  int type;
  quint64 start, blocks;
  QByteArray data;
};
QByteArray sha(const QByteArray &b) {
  return QCryptographicHash::hash(b, QCryptographicHash::Sha256);
}
// Encoders live in ougapayloadfixtures.cpp: codec headers declare a global
// read() that would shadow this file's ::read helper.
} // namespace
QByteArray xzBytes(const QByteArray &in);
QByteArray bzBytes(const QByteArray &in);
QByteArray zstdCompressedBytes();
QByteArray zstdBytes(const QByteArray &in, bool unknownSize = false,
                     bool rle = false);
namespace {
// image: expected partition contents; ops: data already encoded per type.
QByteArray nativePartition(const QByteArray &name, const QByteArray &image,
                           const QVector<NativeOp> &ops, QByteArray *blob) {
  QByteArray part = pb(1, name) + pb(7, vi(8) + vi(image.size()) + pb(2, sha(image)));
  for (const auto &op : ops) {
    QByteArray o = vi(8) + vi(quint64(op.type));
    if (!op.data.isEmpty())
      o += vi(2 << 3) + vi(quint64(blob->size())) + vi(3 << 3) +
           vi(quint64(op.data.size())) + pb(8, sha(op.data));
    o += pb(6, vi(8) + vi(op.start) + vi(16) + vi(op.blocks));
    *blob += op.data;
    part += pb(8, o);
  }
  return pb(13, part);
}
QByteArray nativeImage(int blocks, char seed) {
  QByteArray b(blocks * 4096, 0);
  for (int i = 0; i < b.size(); ++i)
    b[i] = char(seed + (i / 4096) * 7 + (i % 251));
  return b;
}
// boot: REPLACE + REPLACE_XZ + ZERO; vendor: REPLACE_BZ.
QByteArray nativePayloadBytes(QByteArray *boot, QByteArray *vendor) {
  *boot = nativeImage(4, 'a');
  boot->replace(3 * 4096, 4096, QByteArray(4096, 0));
  *vendor = nativeImage(2, 'v');
  QByteArray blob;
  QByteArray manifest = vi(3 << 3) + vi(4096);
  manifest += nativePartition(
      "boot", *boot,
      {{0, 0, 1, boot->left(4096)},
       {8, 1, 2, xzBytes(boot->mid(4096, 2 * 4096))},
       {6, 3, 1, {}}},
      &blob);
  manifest += nativePartition("vendor", *vendor, {{1, 0, 2, bzBytes(*vendor)}},
                              &blob);
  return payloadContainer(manifest) + blob;
}
// Real ZSTD compressed block + RAW/RLE + unknown-size, multi-block frames.
QByteArray nativeZstdPayloadBytes(QMap<QString, QByteArray> *expected) {
  QByteArray blob, manifest = vi(3 << 3) + vi(4096);
  (*expected)["boot"] = nativeImage(2, 'z');
  manifest += nativePartition("boot", expected->value("boot"),
      {{14, 0, 2, zstdCompressedBytes()}}, &blob);
  const QByteArray a = nativeImage(1, 'a'), b = nativeImage(1, 'b'),
                   c = nativeImage(1, 'c');
  (*expected)["vendor"] = a + b + c + QByteArray(4096, 0);
  manifest += nativePartition("vendor", expected->value("vendor"),
      {{8, 0, 1, xzBytes(a)}, {1, 1, 1, bzBytes(b)},
       {14, 2, 1, zstdBytes(c)}, {6, 3, 1, {}}}, &blob);
  (*expected)["system"] = nativeImage(40, 's');
  manifest += nativePartition("system", expected->value("system"),
      {{14, 0, 40, zstdBytes(expected->value("system"), true)}}, &blob);
  (*expected)["product"] = QByteArray(4 * 4096, 'r');
  manifest += nativePartition("product", expected->value("product"),
      {{14, 0, 4, zstdBytes(expected->value("product"), false, true)}}, &blob);
  return payloadContainer(manifest) + blob;
}
// Minimal single-entry ZIP; method 0 stores, 8 claims deflate.
QByteArray zipBytes(const QByteArray &name, const QByteArray &data,
                    quint16 method = 0, bool duplicate = false) {
  QByteArray out, cd;
  const int count = duplicate ? 2 : 1;
  for (int n = 0; n < count; ++n) {
    const QByteArray entry = n ? "IMAGES/" + name : name;
    QByteArray local(30, 0);
    le32(local, 0, 0x04034b50);
    le16(local, 8, method);
    le32(local, 18, quint32(data.size()));
    le32(local, 22, quint32(data.size()));
    le16(local, 26, quint16(entry.size()));
    QByteArray central(46, 0);
    le32(central, 0, 0x02014b50);
    le16(central, 10, method);
    le32(central, 20, quint32(data.size()));
    le32(central, 24, quint32(data.size()));
    le16(central, 28, quint16(entry.size()));
    le32(central, 42, quint32(out.size()));
    cd += central + entry;
    out += local + entry + data;
  }
  QByteArray end(22, 0);
  le32(end, 0, 0x06054b50);
  le16(end, 8, quint16(count));
  le16(end, 10, quint16(count));
  le32(end, 12, quint32(cd.size()));
  le32(end, 16, quint32(out.size()));
  return out + cd + end;
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
Device cowCapacityDevice(Platform platform, const QString &slot) {
  Device d = fixtureDevice(platform, slot);
  // 10 MiB Super - 4 MiB reserve - 2 MiB retained vendor = 4 MiB available
  // after COW cleanup. Before cleanup the 1 MiB snapshot is still occupied.
  d.sizes["super"] = 10 * 1024 * 1024;
  for (const QString name : {"system", "my_company", "my_preload"})
    for (const QString suffix : {"a", "b"})
      d.sizes[name + "_" + suffix] = 512 * 1024;
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
  QStringList trace, boundTrace;
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
    boundTrace << args.join(' ');
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
  QVector<Partition> cowCapacityImages() {
    return {image("system", QByteArray(2 * 1024 * 1024, 's')),
            image("my_company", QByteArray(1024 * 1024, 'c')),
            image("my_preload", QByteArray(1024 * 1024, 'p')),
            image("boot")};
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
  Device standardFbdDevice(Platform platform, const QString &slot,
                           int modemLayout) {
    // 0: no selected modem; 1: physical A/B modem; 2: slotless modem.
    Device device = fixtureDevice(platform, slot);
    for (const QString suffix : {"a", "b"}) {
      device.partitions.insert("modem_backup_" + suffix);
      device.sizes["modem_backup_" + suffix] = 1024 * 1024;
      if (modemLayout != 1) {
        device.partitions.remove("modem_" + suffix);
        device.sizes.remove("modem_" + suffix);
      }
    }
    if (modemLayout == 2) {
      device.partitions.insert("modem");
      device.sizes["modem"] = 1024 * 1024;
    }
    return device;
  }
  QVector<Partition> standardFbdImages(const QString &slot, int modemLayout) {
    // Opposite source suffixes must not override the actual active slot.
    const QString opposite = slot == "a" ? "b" : "a";
    QVector<Partition> ps = {image("system_" + opposite, QByteArray(128, 's')),
                             image("modem_backup", QByteArray(96, 'r')),
                             image("boot_" + opposite, QByteArray(64, 'b')),
                             image("persist", QByteArray(32, 'p'))};
    if (modemLayout)
      ps << image("modem_" + opposite, QByteArray(48, 'm'));
    return ps;
  }
  QStringList standardFbdCommands(FlashMode mode, Platform platform,
                                  const QString &slot, int modemLayout,
                                  bool clearData, bool autoReboot) {
    // Reference sequence with the agreed platform/target safeguards;
    // independent of Plan.steps (including the known after-sales differences).
    const bool deferred = modemLayout &&
        (mode == FlashMode::AfterSalesFastbootd ||
         (platform == Platform::Qualcomm && modemLayout == 1));
    QStringList expected = {"delete-logical-partition system_a-cow",
                             "flash persist"};
    if (modemLayout && !deferred)
      expected << (modemLayout == 2 ? "flash modem" : "flash modem_" + slot);
    expected << "flash boot_" + slot << "flash modem_backup_" + slot
             << "flash system_" + slot;
    if (deferred) {
      if (platform == Platform::Qualcomm && modemLayout == 1)
        expected << "reboot bootloader";
      if (modemLayout == 2)
        expected << "flash modem";
      else
        expected << "flash modem_a" << "flash modem_b";
      if (platform == Platform::Qualcomm && modemLayout == 1)
        expected << "reboot fastboot";
    }
    if (clearData) {
      expected << "erase userdata" << "erase metadata";
      if (platform == Platform::Qualcomm)
        expected << "-w";
    }
    if (autoReboot)
      expected << "reboot";
    return expected;
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
  void payloadDeviceModel_data() {
    QTest::addColumn<QByteArray>("properties");
    QTest::addColumn<QString>("expected");
    QTest::newRow("PLZ110") << QByteArray("ota_target_version=PLZ110_14.0.0.101\n") << QString("一加15T");
    QTest::newRow("PLQ110") << QByteArray("ota_target_version=PLQ110_14.0.0.101\n") << QString("一加 ACE 6");
    QTest::newRow("PLR110") << QByteArray("ota_target_version=PLR110_14.0.0.101\n") << QString("一加 ACE 6T");
    QTest::newRow("PLK110") << QByteArray("ota_target_version=PLK110_14.0.0.101\n") << QString("一加15");
    QTest::newRow("PLC110") << QByteArray("ota_target_version=PLC110_14.0.0.101\n") << QString("一加ACE5至尊版");
    QTest::newRow("PMB110") << QByteArray("ota_target_version=PMB110_14.0.0.101\n") << QString("一加ACE6至尊版");
    QTest::newRow("OPD2513") << QByteArray("ota_target_version=OPD2513_14.0.0.101\n") << QString("一加Pad3 Pro");
    QTest::newRow("OPD2413") << QByteArray("ota_target_version=OPD2413_14.0.0.101\n") << QString("一加Pad2 Pro");
    QTest::newRow("PKX110") << QByteArray("ota_target_version=PKX110_14.0.0.101\n") << QString("一加13T");
    QTest::newRow("OPD2508") << QByteArray("ota_target_version=OPD2508_14.0.0.101\n") << QString("一加平板 2");
    QTest::newRow("OPD2407") << QByteArray("ota_target_version=OPD2407_14.0.0.101\n") << QString("一加平板");
    QTest::newRow("PKR110") << QByteArray("ota_target_version=PKR110_14.0.0.101\n") << QString("一加ACE5 Pro");
    QTest::newRow("PKG110") << QByteArray("ota_target_version=PKG110_14.0.0.101\n") << QString("一加ACE5");
    QTest::newRow("PJZ110") << QByteArray("ota_target_version=PJZ110_14.0.0.101\n") << QString("一加13");
    QTest::newRow("OPD2404") << QByteArray("ota_target_version=OPD2404_14.0.0.101\n") << QString("一加Pad Pro");
    QTest::newRow("OPD2417") << QByteArray("ota_target_version=OPD2417_14.0.0.101\n") << QString("OPPO Pad SE");
    QTest::newRow("OPD2515") << QByteArray("ota_target_version=OPD2515_14.0.0.101\n") << QString("OPPO Pad Mini");
    QTest::newRow("OPD2102") << QByteArray("ota_target_version=OPD2102_14.0.0.101\n") << QString("OPPO Pad Air");
    QTest::newRow("OPD2301") << QByteArray("ota_target_version=OPD2301_14.0.0.101\n") << QString("OPPO Pad Air2");
    QTest::newRow("OPD2405") << QByteArray("ota_target_version=OPD2405_14.0.0.101\n") << QString("OPPO Pad 3");
    QTest::newRow("OPD2401") << QByteArray("ota_target_version=OPD2401_14.0.0.101\n") << QString("OPPO Pad 3 Pro");
    QTest::newRow("OPD2409") << QByteArray("ota_target_version=OPD2409_14.0.0.101\n") << QString("OPPO Pad 4 Pro");
    QTest::newRow("OPD2501") << QByteArray("ota_target_version=OPD2501_14.0.0.101\n") << QString("OPPO Pad Air5");
    QTest::newRow("OPD2506") << QByteArray("ota_target_version=OPD2506_14.0.0.101\n") << QString("OPPO Pad 5");
    QTest::newRow("OPD2511") << QByteArray("ota_target_version=OPD2511_14.0.0.101\n") << QString("OPPO Pad 5 Pro");
    QTest::newRow("OPD2601") << QByteArray("ota_target_version=OPD2601_14.0.0.101\n") << QString("OPPO Pad 6");
    QTest::newRow("PJX110") << QByteArray("ota_target_version=PJX110_14.0.0.101\n") << QString("一加ACE3 Pro");
    QTest::newRow("PJF110") << QByteArray("ota_target_version=PJF110_14.0.0.101\n") << QString("一加ACE3V");
    QTest::newRow("PJE110") << QByteArray("ota_target_version=PJE110_14.0.0.101\n") << QString("一加ACE3");
    QTest::newRow("PJD110") << QByteArray("ota_target_version=PJD110_14.0.0.101\n") << QString("一加12");
    QTest::newRow("PJA110") << QByteArray("ota_target_version=PJA110_14.0.0.101\n") << QString("一加ACE2 Pro");
    QTest::newRow("PHP110") << QByteArray("ota_target_version=PHP110_14.0.0.101\n") << QString("一加ACE2v");
    QTest::newRow("PHK110") << QByteArray("ota_target_version=PHK110_14.0.0.101\n") << QString("一加ACE2");
    QTest::newRow("PHB110") << QByteArray("ota_target_version=PHB110_14.0.0.101\n") << QString("一加11");
    QTest::newRow("PGP110") << QByteArray("ota_target_version=PGP110_14.0.0.101\n") << QString("一加ACE Pro");
    QTest::newRow("PGZ110") << QByteArray("ota_target_version=PGZ110_14.0.0.101\n") << QString("一加ACE竞速版");
    QTest::newRow("PKGM10") << QByteArray("ota_target_version=PKGM10_14.0.0.101\n") << QString("一加ACE");
    QTest::newRow("NE2210") << QByteArray("ota_target_version=NE2210_14.0.0.101\n") << QString("一加10Pro");
    QTest::newRow("martini") << QByteArray("ota_target_version=martini_14.0.0.101\n") << QString("一加 9RT");
    QTest::newRow("lemonades") << QByteArray("ota_target_version=lemonades_14.0.0.101\n") << QString("一加 9R");
    QTest::newRow("lemonadep") << QByteArray("ota_target_version=lemonadep_14.0.0.101\n") << QString("一加 9 Pro");
    QTest::newRow("lemonade") << QByteArray("ota_target_version=lemonade_14.0.0.101\n") << QString("一加 9");
    QTest::newRow("kebab") << QByteArray("ota_target_version=kebab_14.0.0.101\n") << QString("一加 8T");
    QTest::newRow("instantnoodlep") << QByteArray("ota_target_version=instantnoodlep_14.0.0.101\n") << QString("一加 8 Pro");
    QTest::newRow("instantnoodle") << QByteArray("ota_target_version=instantnoodle_14.0.0.101\n") << QString("一加 8");
    QTest::newRow("hotdogg") << QByteArray("ota_target_version=hotdogg_14.0.0.101\n") << QString("一加 7T Pro");
    QTest::newRow("RMX3370") << QByteArray("ota_target_version=RMX3370_14.0.0.101\n") << QString("真我GT Neo2");
    QTest::newRow("RMX3357") << QByteArray("ota_target_version=RMX3357_14.0.0.101\n") << QString("真我GT neo2T");
    QTest::newRow("RMX3562") << QByteArray("ota_target_version=RMX3562_14.0.0.101\n") << QString("真我GT neo3 150w");
    QTest::newRow("RMX3560") << QByteArray("ota_target_version=RMX3560_14.0.0.101\n") << QString("真我GT neo3 80w");
    QTest::newRow("RMX3706") << QByteArray("ota_target_version=RMX3706_14.0.0.101\n") << QString("真我GT neo5 150w");
    QTest::newRow("RMX3708") << QByteArray("ota_target_version=RMX3708_14.0.0.101\n") << QString("真我GT neo5 240w");
    QTest::newRow("RMX3700") << QByteArray("ota_target_version=RMX3700_14.0.0.101\n") << QString("真我GT neo5 SE");
    QTest::newRow("RMX3850") << QByteArray("ota_target_version=RMX3850_14.0.0.101\n") << QString("真我GT neo6 SE");
    QTest::newRow("RMX3852") << QByteArray("ota_target_version=RMX3852_14.0.0.101\n") << QString("真我GT neo6");
    QTest::newRow("RMX3366") << QByteArray("ota_target_version=RMX3366_14.0.0.101\n") << QString("真我GT大师探索版");
    QTest::newRow("RMX3300") << QByteArray("ota_target_version=RMX3300_14.0.0.101\n") << QString("真我GT2 Pro");
    QTest::newRow("RMX3551") << QByteArray("ota_target_version=RMX3551_14.0.0.101\n") << QString("真我GT2大师探索版");
    QTest::newRow("RMX3310") << QByteArray("ota_target_version=RMX3310_14.0.0.101\n") << QString("真我GT2");
    QTest::newRow("RMX3820") << QByteArray("ota_target_version=RMX3820_14.0.0.101\n") << QString("真我GT5 150w");
    QTest::newRow("RMX3823") << QByteArray("ota_target_version=RMX3823_14.0.0.101\n") << QString("真我GT5 240w");
    QTest::newRow("RMX3888") << QByteArray("ota_target_version=RMX3888_14.0.0.101\n") << QString("真我GT5 Pro");
    QTest::newRow("RMX3800") << QByteArray("ota_target_version=RMX3800_14.0.0.101\n") << QString("真我GT6");
    QTest::newRow("RMX5090") << QByteArray("ota_target_version=RMX5090_14.0.0.101\n") << QString("真我GT7 Pro竞速版");
    QTest::newRow("RMX5010") << QByteArray("ota_target_version=RMX5010_14.0.0.101\n") << QString("真我GT7 Pro");
    QTest::newRow("RMX6688") << QByteArray("ota_target_version=RMX6688_14.0.0.101\n") << QString("真我GT7");
    QTest::newRow("RMX5200") << QByteArray("ota_target_version=RMX5200_14.0.0.101\n") << QString("真我GT8 Pro");
    QTest::newRow("RMX6699") << QByteArray("ota_target_version=RMX6699_14.0.0.101\n") << QString("真我GT8");
    QTest::newRow("RMX8899") << QByteArray("ota_target_version=RMX8899_14.0.0.101\n") << QString("真我Neo8");
    QTest::newRow("RMX5080") << QByteArray("ota_target_version=RMX5080_14.0.0.101\n") << QString("真我GT neo7 SE");
    QTest::newRow("RMX5062") << QByteArray("ota_target_version=RMX5062_14.0.0.101\n") << QString("真我GT neo7 Turbo");
    QTest::newRow("RMX5060") << QByteArray("ota_target_version=RMX5060_14.0.0.101\n") << QString("真我GT neo7");
    QTest::newRow("RMX5071") << QByteArray("ota_target_version=RMX5071_14.0.0.101\n") << QString("真我GT neo7X");
    QTest::newRow("no-metadata") << QByteArray() << QString();
    QTest::newRow("missing-version") << QByteArray("FILE_HASH=xyz\nFILE_SIZE=512\n") << QString();
    QTest::newRow("empty-version") << QByteArray("ota_target_version=  \r\n") << QString();
    QTest::newRow("short-version") << QByteArray("ota_target_version=abc\n") << QString();
    QTest::newRow("unknown-model") << QByteArray("ota_target_version=ABC123_14.0\n") << QString("未知机型");
    QTest::newRow("prefix-is-not-model") << QByteArray("ota_target_version=PLZ1100_14.0\n") << QString("未知机型");
    QTest::newRow("longest-model") << QByteArray("ota_target_version=lemonadep_14.0\n") << QString("一加 9 Pro");
    QTest::newRow("longest-noodles") << QByteArray("ota_target_version=instantnoodlep_14.0\n") << QString("一加 8 Pro");
    QTest::newRow("bom-crlf") << (QByteArray::fromHex("efbbbf") + "ota_target_version=  PJZ110_15.0 \r\n") << QString("一加13");
    QTest::newRow("exact-code") << QByteArray("ota_target_version=kebab\n") << QString("一加 8T");
    QTest::newRow("first-version-only") << QByteArray("ota_target_version=PJD110_14.0\nota_target_version=PJZ110_15.0\n") << QString("一加12");
    QTest::newRow("oversized-metadata") << (QByteArray("ota_target_version=PJZ110_15.0\n") + QByteArray(1024 * 1024, 'x')) << QString();
  }
  void payloadDeviceModel() {
    QFETCH(QByteArray, properties);
    QFETCH(QString, expected);
    const QString sourceDir = dir + "/中文 空格 输入";
    QVERIFY(QDir().mkpath(sourceDir));
    const QString source = sourceDir + "/payload.bin";
    QVERIFY(put(source, "unchanged source"));
    const QString sidecar = sourceDir + "/payload_properties.txt";
    if (!properties.isNull())
      QVERIFY(put(sidecar, properties));
    // A metadata file elsewhere must not be used as a fallback.
    QVERIFY(put(dir + "/payload_properties.txt", "ota_target_version=PLZ110_15.0\n"));
    for (const QString &name : {source, sourceDir + "/ota.zip"})
      QCOMPARE(OugaPackage::payloadDeviceModel(name), expected);
    QCOMPARE(read(source), QByteArray("unchanged source"));
    if (!properties.isNull())
      QCOMPARE(read(sidecar), properties);
  }
  void payloadDeviceModelDoesNotReadDirectory() {
    const QString sidecar = dir + "/payload_properties.txt";
    QVERIFY(QDir().mkpath(sidecar));
    QCOMPARE(OugaPackage::payloadDeviceModel(dir + "/payload.bin"), QString());
  }
  void widgetPayloadModelWarning_data() {
    QTest::addColumn<bool>("cancel");
    QTest::newRow("warning-before-unpack") << false;
    QTest::newRow("cancel-does-not-restart") << true;
  }
  void widgetPayloadModelWarning() {
    QFETCH(bool, cancel);
    QByteArray boot, vendor;
    const QString sourceDir = dir + "/input";
    QVERIFY(QDir().mkpath(sourceDir));
    const QString source = sourceDir + "/payload.bin", output = dir + "/output";
    QVERIFY(put(source, nativePayloadBytes(&boot, &vendor)));
    QVERIFY(put(sourceDir + "/payload_properties.txt", cancel
        ? "ota_target_version=ABC123_15.0\n"
        : "ota_target_version=PJZ110_15.0\n"));
    QVERIFY(QDir().mkpath(output));
    FakeRunner runner;
    OugaFlashWindow window(nullptr, &runner, dir + "/logs");
    window.show();
    auto preparation = window.findChild<OugaPreparation *>();
    auto log = window.findChild<QPlainTextEdit *>("OugaFlashLogTextBox");
    auto button = window.findChild<QPushButton *>("UnpackPayloadButton");
    auto progress = window.findChild<QProgressBar *>("FlashProgressBar");
    QSignalSpy ready(preparation, &OugaPreparation::prepared);
    window.findChild<QLineEdit *>("PayloadFilePathTextBox")->setText(source);
    auto folder = window.findChild<QLineEdit *>("FolderPathTextBox");
    folder->setText(output);
    QVERIFY(QMetaObject::invokeMethod(folder, "editingFinished"));
    int beats = 0;
    QTimer heartbeat;
    heartbeat.setInterval(20);
    connect(&heartbeat, &QTimer::timeout, &window, [&] { ++beats; });
    heartbeat.start();
    QElapsedTimer clock;
    clock.start();
    button->click();
    QTRY_VERIFY_WITH_TIMEOUT(log->toPlainText().contains("5秒后开始解包..."), 2000);
    QVERIFY(window.isBusy());
    QVERIFY(!preparation->busy());
    QVERIFY(!log->toPlainText().contains("开始解包，"));
    QCOMPARE(progress->property("rate").toString(), QString("5秒后开始解包..."));
    QVERIFY(!QFileInfo::exists(output + "/images/boot.img"));
    if (cancel) {
      window.findChild<QPushButton *>("OugaFlashStopPanel")->click();
      QVERIFY(!window.isBusy());
      QCOMPARE(ready.count(), 0);
      // A new task completes before the cancelled warning's timer expires.
      QVERIFY(put(sourceDir + "/payload_properties.txt", "FILE_HASH=no-model\n"));
      button->click();
      QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 3000);
      QCOMPARE(ready.count(), 1);
      QTest::qWait(qMax<qint64>(0, 5300 - clock.elapsed()));
      QCOMPARE(ready.count(), 1);
      QVERIFY(!window.isBusy());
      QVERIFY(log->toPlainText().contains("解析刷机包对应机型失败，跳过解析步骤..."));
    } else {
      QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 15000);
      QVERIFY(clock.elapsed() >= 5000);
      QCOMPARE(ready.count(), 1);
    }
    QVERIFY(beats >= 20); // event loop stays alive throughout the warning
    const QString text = log->toPlainText();
    const QString modelMessage = cancel ? "刷机包对应的机型为未知机型..."
                                        : "刷机包对应的机型为一加13...";
    const auto checking = text.indexOf("正在判断刷机包对应的机型...");
    const auto identified = text.indexOf(modelMessage);
    const auto unpacking = text.indexOf("开始解包，");
    QVERIFY(checking >= 0);
    QVERIFY(identified > checking);
    QVERIFY(unpacking > identified);
    QVERIFY(text.contains("Payload解包成功！"));
    QCOMPARE(read(output + "/images/boot.img"), boot);
    QCOMPARE(read(output + "/images/vendor.img"), vendor);
    QCOMPARE(read(source), nativePayloadBytes(&boot, &vendor));
    QVERIFY(runner.trace.isEmpty());
    QVERIFY(window.close());
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
    QTest::newRow("root-raw") << "" << "boot.raw" << true;
    QTest::newRow("radio-iso") << "RADIO" << "modem.ISO" << true;
    QTest::newRow("radio-sparse") << "RADIO" << "modem.sparse" << true;
    QTest::newRow("uppercase-sparse") << "" << "vendor.SPARSE" << true;
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
  void referenceImageFormats_data() {
    QTest::addColumn<QString>("subdirectory");
    QTest::addColumn<QString>("filename");
    QTest::addColumn<QString>("target");
    QTest::addColumn<bool>("isSparse");
    QTest::newRow("raw-root") << "" << "boot.raw" << "boot" << false;
    QTest::newRow("iso-root") << "" << "boot.iso" << "boot" << false;
    QTest::newRow("iso-radio") << "RADIO" << "modem.ISO" << "modem" << false;
    QTest::newRow("sparse-iso-images") << "IMAGES" << "vendor.iso" << "vendor" << true;
    QTest::newRow("sparse-root") << "" << "vendor.sparse" << "vendor" << true;
    QTest::newRow("raw-images") << "IMAGES" << "init_boot.RAW" << "init_boot" << false;
    QTest::newRow("sparse-radio") << "RADIO" << "modem.SPARSE" << "modem" << true;
    QTest::newRow("sparse-after-sales") << "IMAGES/my_company" << "package.sparse" << "my_company" << true;
  }
  void referenceImageFormats() {
    QFETCH(QString, subdirectory);
    QFETCH(QString, filename);
    QFETCH(QString, target);
    QFETCH(bool, isSparse);
    const QString folder = QDir(dir).filePath(subdirectory);
    QVERIFY(QDir().mkpath(folder));
    const QString file = QDir(folder).filePath(filename);
    const QByteArray bytes = isSparse ? ::sparse() : QByteArray(512, 'r');
    QVERIFY(put(file, bytes));
    QString error;
    const auto images = OugaPackage::scan(dir, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(images.size(), 1);
    QCOMPARE(images[0].name, target);
    // Windows canonical paths retain the spelling used to query the file.
    QCOMPARE(images[0].path.toCaseFolded(),
             QFileInfo(file).canonicalFilePath().toCaseFolded());
    QCOMPARE(images[0].bytes, bytes.size());
    QCOMPARE(images[0].expandedBytes, 512);
    QCOMPARE(images[0].sha256, QCryptographicHash::hash(bytes, QCryptographicHash::Sha256));
    QCOMPARE(Ouga::baseName(target + "_b." + QFileInfo(filename).suffix()), target);
  }
  void referenceImageFormatBoundaries() {
    QString error;
    QVERIFY(put(dir + "/boot.raw", QByteArray(512, 'b')));
    QVERIFY(put(dir + "/boot.sparse", ::sparse()));
    QVERIFY(OugaPackage::scan(dir, &error).isEmpty());
    QVERIFY2(error.contains("多个来源"), qPrintable(error));
    const QString damaged = dir + "/damaged";
    QVERIFY(QDir().mkpath(damaged));
    QVERIFY(put(damaged + "/vendor.sparse", ::sparse().left(40)));
    QVERIFY(OugaPackage::scan(damaged, &error).isEmpty());
    QVERIFY(!error.isEmpty());
    const QString blocked = dir + "/blocked";
    QVERIFY(QDir().mkpath(blocked));
    QVERIFY(put(blocked + "/boot.raw", QByteArray(512, 'b')));
    for (const QString name : {"misc.sparse", "frp.raw", "metadata.raw", "misc.iso", "frp.iso", "script.bin", "payload.bin"})
      QVERIFY(put(blocked + "/" + name, QByteArray(512, 'x')));
    const auto images = OugaPackage::scan(blocked, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(images.size(), 1);
    QCOMPARE(images[0].name, "boot");
    const QString ambiguous = dir + "/ambiguous/my_company";
    QVERIFY(QDir().mkpath(ambiguous));
    QVERIFY(put(ambiguous + "/candidate.img", QByteArray(512, 'a')));
    QVERIFY(put(ambiguous + "/candidate.raw", QByteArray(512, 'b')));
    QVERIFY(OugaPackage::scan(dir + "/ambiguous", &error).isEmpty());
    QVERIFY2(error.contains("多个候选"), qPrintable(error));
  }
  void parallelScanKeepsHashesAndOrder() {
    const QStringList names = {"boot", "vendor", "system"};
    for (const QString &name : names)
      QVERIFY(put(dir + "/" + name + ".img", QByteArray(1024 * 1024, name.at(0).toLatin1())));
    QString error;
    const auto images = OugaPackage::scan(dir, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(images.size(), 3);
    QStringList sorted = names;
    sorted.sort();
    for (int i = 0; i < images.size(); ++i) {
      QCOMPARE(images[i].name, sorted[i]);
      QCOMPARE(images[i].sha256, QCryptographicHash::hash(
          QByteArray(1024 * 1024, sorted[i].at(0).toLatin1()), QCryptographicHash::Sha256));
    }
    QVERIFY(put(dir + "/vendor.img", QByteArray()));
    QVERIFY(OugaPackage::scan(dir, &error).isEmpty());
    QVERIFY(!error.isEmpty());
  }
  void payloadExcludedImageStillVerified_data() {
    QTest::addColumn<bool>("corrupt");
    QTest::newRow("excluded-valid") << false;
    QTest::newRow("excluded-corrupt") << true;
  }
  void payloadExcludedImageStillVerified() {
    QFETCH(bool, corrupt);
    const QString file = dir + "/payload.bin";
    const QString output = dir + (corrupt ? "-excluded-corrupt" : "-excluded-ok");
    QVERIFY(put(file, payloadContainer(payloadPartition("boot", 0) + payloadPartition("misc", 0))));
    OugaPreparation prep;
    QSignalSpy done(&prep, &OugaPreparation::finished), ready(&prep, &OugaPreparation::prepared);
    prep.payload(QCoreApplication::applicationFilePath(), file, output);
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 10000);
    QCOMPARE(done[0][0].toBool(), !corrupt);
    QCOMPARE(ready.size(), corrupt ? 0 : 1);
    if (!corrupt) {
      const auto images = qvariant_cast<QVector<Ouga::Partition>>(ready[0][0]);
      QCOMPARE(images.size(), 1);
      QCOMPARE(images[0].name, "boot");
      QCOMPARE(images[0].sha256, QCryptographicHash::hash(QByteArray(512, 'p'), QCryptographicHash::Sha256));
    } else
      QVERIFY(done[0][1].toString().contains("SHA-256"));
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
  void payloadClassification_data() {
    QTest::addColumn<int>("type");
    QTest::addColumn<quint64>("minorVersion");
    QTest::addColumn<bool>("oldMetadata");
    QTest::addColumn<QByteArray>("operationExtras");
    QTest::addColumn<bool>("requiresOldImage");
    QTest::addColumn<bool>("valid");
    for (int type : {0, 1, 6, 7, 8, 14})
      QTest::newRow(qPrintable(QString("full-type-%1-minor9").arg(type)))
          << type << quint64(9) << false << QByteArray() << false << true;
    QTest::newRow("full-with-old-metadata")
        << 8 << quint64(9) << true << QByteArray() << false << true;
    for (int type : {2, 3, 4, 5, 9, 10, 11, 12, 13})
      QTest::newRow(qPrintable(QString("source-type-%1-minor0").arg(type)))
          << type << quint64(0) << true << QByteArray() << true << true;
    QTest::newRow("source-without-old-metadata")
        << 4 << quint64(0) << false << QByteArray() << true << true;
    QTest::newRow("source-hash")
        << 0 << quint64(0) << true << pb(9, QByteArray(32, 's')) << true << true;
    QTest::newRow("source-extents")
        << 0 << quint64(0) << true << pb(4, vi(8) + vi(0) + vi(16) + vi(1))
        << true << true;
    QTest::newRow("source-length")
        << 0 << quint64(0) << true << (vi(5 << 3) + vi(512)) << true << true;
    QTest::newRow("empty-source-metadata")
        << 0 << quint64(9) << true << (pb(9, {}) + vi(5 << 3) + vi(0))
        << false << true;
    QTest::newRow("unknown-operation")
        << 15 << quint64(9) << false << QByteArray() << false << false;
    QTest::newRow("missing-operation-type")
        << -1 << quint64(9) << false << QByteArray() << false << false;
    QTest::newRow("wrong-type-wire")
        << -1 << quint64(9) << false << pb(1, "not-an-enum") << false << false;
    QTest::newRow("truncated-operation")
        << 0 << quint64(9) << false << QByteArray(1, char(0x80)) << false << false;
  }
  void payloadClassification() {
    QFETCH(int, type);
    QFETCH(quint64, minorVersion);
    QFETCH(bool, oldMetadata);
    QFETCH(QByteArray, operationExtras);
    QFETCH(bool, requiresOldImage);
    QFETCH(bool, valid);
    const QByteArray bytes = payloadContainer(
        payloadPartition("boot", type, oldMetadata, operationExtras) +
        vi(12 << 3) + vi(minorVersion));
    const QString file = dir + "/payload.bin";
    QVERIFY(put(file, bytes));
    QVector<OugaPayloadEntry> entries;
    bool delta = false;
    QString error;
    QCOMPARE(OugaPackage::payloadManifest(file, &entries, &delta, &error), valid);
    if (valid) {
      QCOMPARE(entries.size(), 1);
      QCOMPARE(delta, requiresOldImage);
      QCOMPARE(entries[0].requiresOldImage, requiresOldImage);
      QCOMPARE(entries[0].oldSize, oldMetadata ? quint64(512) : quint64(0));
    } else {
      QVERIFY(!error.isEmpty());
    }
    QCOMPARE(read(file), bytes);
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
  // Independent oracle: SMT/Violet SuperMaker skips the implicit default
  // group, resolves group_name -> group -> default, and strips IMAGES only
  // when the normal image path is missing.
  void referenceSuperGroups_data() {
    QTest::addColumn<QString>("scenario");
    QTest::addColumn<bool>("accepted");
    for (const QString value : {"implicit-default", "explicit-default",
                               "default-without-size", "legacy-default",
                               "named-unlimited", "named-bounded",
                               "group-name-precedence", "mixed-default"})
      QTest::newRow(qPrintable(value)) << value << true;
    for (const QString value : {"duplicate-default", "duplicate-named",
                               "unknown-group", "bad-group-size",
                               "oversized-group", "insufficient-group"})
      QTest::newRow(qPrintable(value)) << value << false;
  }
  void referenceSuperGroups() {
    QFETCH(QString, scenario);
    QFETCH(bool, accepted);
    QVERIFY(put(dir + "/system.img", QByteArray(512, 's')));
    QJsonArray groups;
    QJsonObject part{{"name", "system_a"}, {"size", "4096"},
                     {"path", "system.img"}};
    if (scenario.contains("default") && scenario != "implicit-default") {
      QJsonObject group{{"name", "default"}, {"maximum_size", "0"}};
      if (scenario == "default-without-size") group.remove("maximum_size");
      groups.append(group);
      if (scenario == "duplicate-default") groups.append(group);
      if (scenario == "legacy-default") part["group"] = "default";
      else part["group_name"] = "default";
    }
    const bool named = scenario.startsWith("named-") ||
        scenario == "group-name-precedence" || scenario == "mixed-default" ||
        scenario == "duplicate-named" || scenario == "bad-group-size" ||
        scenario == "oversized-group" || scenario == "insufficient-group";
    QString maximum = scenario == "named-unlimited" ? "0" : "4194304";
    if (scenario == "bad-group-size") maximum = "bad";
    if (scenario == "oversized-group") maximum = "8388609";
    if (scenario == "insufficient-group") maximum = "512";
    if (named) {
      QJsonObject group{{"name", "g"}, {"maximum_size", maximum}};
      groups.append(group);
      if (scenario == "duplicate-named") groups.append(group);
      if (scenario != "mixed-default") part["group_name"] = "g";
      if (scenario == "group-name-precedence") part["group"] = "default";
    }
    if (scenario == "unknown-group") part["group_name"] = "missing";
    QJsonObject def{
        {"block_devices", QJsonArray{QJsonObject{{"name", "super"},
            {"size", "8388608"}, {"alignment", "4096"}}}},
        {"groups", groups}, {"partitions", QJsonArray{part}}};
    const QByteArray original = QJsonDocument(def).toJson();
    const QString path = dir + "/super_def.json";
    QVERIFY(put(path, original));
    QStringList args;
    QSet<QString> merged;
    QString error;
    QCOMPARE(OugaPackage::lpmakeArguments(dir, dir + "/out.img", &args,
                                         &merged, &error), accepted);
    QCOMPARE(read(path), original);
    QCOMPARE(read(dir + "/system.img"), QByteArray(512, 's'));
    if (!accepted) {
      QVERIFY2(!error.isEmpty(), qPrintable(scenario));
      return;
    }
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QStringList expected{"--metadata-size", "65536", "--metadata-slots", "2",
        "--block-size", "4096", "--super-name", "super", "--device",
        "super:8388608:4096:0", "--sparse", "--output", dir + "/out.img"};
    if (named) expected << "--group" << "g:" + maximum;
    const QString group = named && scenario != "mixed-default" ? "g" : "default";
    expected << "--partition" << "system_a:readonly:4096:" + group
             << "--image" << "system_a=" + QFileInfo(dir + "/system.img").canonicalFilePath();
    QCOMPARE(args, expected);
    QCOMPARE(merged, QSet<QString>{"system"});
  }
  void referenceSuperPaths_data() {
    QTest::addColumn<QString>("scenario");
    QTest::addColumn<bool>("accepted");
    for (const QString value : {"normal", "windows-separators", "flattened",
                               "normal-wins", "meta-relative"})
      QTest::newRow(qPrintable(value)) << value << true;
    for (const QString value : {"missing", "outside", "duplicate-defs",
                               "ambiguous-relative", "oversized-image"})
      QTest::newRow(qPrintable(value)) << value << false;
  }
  void referenceSuperPaths() {
    QFETCH(QString, scenario);
    QFETCH(bool, accepted);
    const QString root = dir + "/售后 包";
    QVERIFY(QDir().mkpath(root + "/IMAGES"));
    QVERIFY(QDir().mkpath(root + "/META"));
    QString source = root + "/IMAGES/system.img";
    QString relative = "IMAGES/system.img";
    QString json = root + "/super_def.json";
    if (scenario == "windows-separators") relative = "IMAGES\\system.img";
    if (scenario == "flattened") source = root + "/system.img";
    if (scenario == "meta-relative" || scenario == "ambiguous-relative") {
      relative = "system.img";
      json = root + "/META/super_def.json";
      source = root + "/META/system.img";
      if (scenario == "ambiguous-relative")
        QVERIFY(put(root + "/system.img", QByteArray(512, 'a')));
    }
    if (scenario == "outside") {
      relative = "../outside.img";
      source = dir + "/outside.img";
    }
    if (scenario != "missing")
      QVERIFY(put(source, QByteArray(scenario == "oversized-image" ? 8192 : 512, 's')));
    if (scenario == "normal-wins")
      QVERIFY(put(root + "/system.img", QByteArray(512, 'f')));
    QJsonObject def{
        {"block_devices", QJsonArray{QJsonObject{{"name", "super"},
            {"size", "8388608"}, {"alignment", "4096"}}}},
        {"groups", QJsonArray{QJsonObject{{"name", "g"}, {"maximum_size", "4194304"}}}},
        {"partitions", QJsonArray{QJsonObject{{"name", "system_a"},
            {"size", "4096"}, {"group_name", "g"}, {"path", relative}}}}};
    const QByteArray bytes = QJsonDocument(def).toJson();
    QVERIFY(put(json, bytes));
    if (scenario == "duplicate-defs")
      QVERIFY(put(root + "/META/super_def.json", bytes));
    QStringList args;
    QSet<QString> merged;
    QString error;
    QCOMPARE(OugaPackage::lpmakeArguments(root, dir + "/out.img", &args,
                                         &merged, &error), accepted);
    QCOMPARE(read(json), bytes);
    if (!accepted) {
      QVERIFY(!error.isEmpty());
      return;
    }
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(args.value(args.indexOf("--image") + 1),
             "system_a=" + QFileInfo(source).canonicalFilePath());
    QCOMPARE(merged, QSet<QString>{"system"});
    QCOMPARE(read(source), QByteArray(512, 's'));
  }
  void referenceSuperNative_data() {
    QTest::addColumn<QString>("group");
    QTest::addColumn<bool>("flattened");
    for (const QString group : {"implicit", "default", "bounded", "unlimited"})
      for (bool flattened : {false, true})
        QTest::newRow(qPrintable(group + (flattened ? "-flat" : "-images")))
            << group << flattened;
  }
  void referenceSuperNative() {
    // Opt-in local image construction only. Never substitute a device tool.
    const QString tool = qEnvironmentVariable("ORANGE_TEST_LPMAKE");
    if (tool.isEmpty()) QSKIP("Set ORANGE_TEST_LPMAKE for local Super tool validation");
    QVERIFY(QFileInfo(tool).isExecutable());
    QFETCH(QString, group);
    QFETCH(bool, flattened);
    const QString root = dir + "/售后 包", output = dir + "/output";
    QVERIFY(QDir().mkpath(root + "/IMAGES"));
    const QString source = root + (flattened ? "/system.img" : "/IMAGES/system.img");
    const QByteArray content(512, 's');
    QVERIFY(put(source, content));
    QJsonArray groups;
    if (group != "implicit")
      groups.append(QJsonObject{{"name", group == "default" ? "default" : "g"},
          {"maximum_size", group == "bounded" ? "4194304" : "0"}});
    QJsonObject part{{"name", "system_a"}, {"size", "4096"},
                     {"path", "IMAGES/system.img"}};
    if (group != "implicit") part["group_name"] = group == "default" ? "default" : "g";
    QJsonObject def{
        {"block_devices", QJsonArray{QJsonObject{{"name", "super"},
            {"size", "8388608"}, {"alignment", "4096"}}}},
        {"groups", groups}, {"partitions", QJsonArray{part}}};
    const QByteArray original = QJsonDocument(def).toJson();
    QVERIFY(put(root + "/super_def.json", original));
    OugaPreparation preparation;
    QSignalSpy done(&preparation, &OugaPreparation::finished);
    QSignalSpy ready(&preparation, &OugaPreparation::prepared);
    QString logs;
    connect(&preparation, &OugaPreparation::log, this,
            [&logs](const QString &message) { logs += message + '\n'; });
    preparation.makeSuper(tool, root, output);
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 20000);
    QVERIFY2(done[0][0].toBool(), qPrintable(logs + done[0][1].toString()));
    QCOMPARE(ready.count(), 1);
    QCOMPARE(read(source), content);
    QCOMPARE(read(root + "/super_def.json"), original);
    Partition super;
    QString error;
    QVERIFY2(OugaPackage::inspect("super", output + "/super.img", &super, &error),
             qPrintable(error));
    QCOMPARE(super.merged, QSet<QString>{"system"});
    QVERIFY(super.expandedBytes > 4096);
    const auto prepared = qvariant_cast<QVector<Partition>>(ready[0][0]);
    QCOMPARE(prepared.size(), 2);
    QVERIFY(std::any_of(prepared.cbegin(), prepared.cend(),
                       [](const Partition &image) { return image.name == "super"; }));
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
    QCOMPARE(criticalImages(Platform::Qualcomm), afterSalesCritical(Platform::Qualcomm));
    QCOMPARE(criticalImages(Platform::MediaTek), afterSalesCritical(Platform::MediaTek));
    QCOMPARE(criticalExtractionImages(Platform::Qualcomm),
             QStringList({"boot", "recovery", "dtbo", "modem", "vbmeta", "vendor_boot",
                          "init_boot", "vbmeta_system", "vbmeta_vendor"}));
    QCOMPARE(criticalExtractionImages(Platform::MediaTek),
             QStringList({"boot", "init_boot", "dtbo", "lk", "vbmeta", "vendor_boot",
                          "vbmeta_system", "vbmeta_vendor"}));
    QVERIFY(criticalExtractionImages(Platform::Unknown).isEmpty());
    QVERIFY(criticalExtractionImages(Platform(99)).isEmpty());
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
  void abStagedSequence_data() {
    QTest::addColumn<int>("platform");
    QTest::addColumn<QString>("slot");
    QTest::addColumn<QString>("target");
    QTest::addColumn<bool>("slotlessModem");
    for (int platform : {1, 2})
      for (const QString slot : {"a", "b"})
        for (const QString target : {"a", "b"})
          for (bool slotless : {false, true})
            QTest::newRow(qPrintable(QString("p%1-%2-to%3-modem-%4")
                                         .arg(platform).arg(slot, target)
                                         .arg(slotless ? "slotless" : "dual")))
                << platform << slot << target << slotless;
  }
  void abStagedSequence() {
    QFETCH(int, platform);
    QFETCH(QString, slot);
    QFETCH(QString, target);
    QFETCH(bool, slotlessModem);
    const auto pf = Platform(platform);
    auto device = fixtureDevice(pf, slot);
    if (slotlessModem) {
      for (const QString suffix : {"a", "b"}) {
        device.partitions.remove("modem_" + suffix);
        device.sizes.remove("modem_" + suffix);
      }
      device.partitions.insert("modem");
      device.sizes["modem"] = 1024 * 1024;
    }
    QVector<Partition> ps = {image("system", QByteArray(1536, 's')),
                             image("my_preload"), image("boot", QByteArray(1024, 'b')),
                             image("my_company"), image("vbmeta", QByteArray(256, 'v')),
                             image("persist", QByteArray(128, 'p')),
                             image("modem", QByteArray(64, 'm'))};
    auto opts = options(FlashMode::BothSlots, pf);
    opts.targetSlot = target;
    Plan plan, reordered;
    QString error;
    QVERIFY2(OugaFlashPlanner::build(ps, device, opts, &plan, &error), qPrintable(error));
    QStringList expected{"delete-logical-partition system_a-cow"};
    if (slotlessModem)
      expected << "flash modem";
    expected << "flash persist" << "flash vbmeta_a" << "flash vbmeta_b"
             << "flash boot_a" << "flash boot_b";
    if (slot != target) {
      expected << "set_active " + target;
      for (const QString name : {"my_company", "my_preload", "system"})
        for (const QString suffix : {"a", "b"})
          expected << "delete-logical-partition " + name + "_" + suffix;
      expected << "create-logical-partition my_company_" + target + " 512"
               << "create-logical-partition my_preload_" + target + " 512"
               << "create-logical-partition system_" + target + " 1536";
    }
    expected << "flash my_company_" + target << "flash my_preload_" + target
             << "flash system_" + target;
    if (!slotlessModem) {
      if (pf == Platform::Qualcomm)
        expected << "reboot bootloader";
      expected << "flash modem_a" << "flash modem_b";
      if (pf == Platform::Qualcomm)
        expected << "reboot fastboot";
    }
    QCOMPARE(commands(plan), expected);
    QCOMPARE(plan.flashCount, slotlessModem ? 9 : 10);
    QCOMPARE(plan.totalBytes, qint64(slotlessModem ? 5312 : 5376));
    for (const auto &step : plan.steps)
      if (step.arguments.value(0) == "flash")
        QCOMPARE(step.userspace, pf != Platform::Qualcomm ||
                                    !step.target.startsWith("modem_"));
    std::reverse(ps.begin(), ps.end());
    QVERIFY2(OugaFlashPlanner::build(ps, device, opts, &reordered, &error), qPrintable(error));
    QCOMPARE(planText(plan), planText(reordered));
  }
  void abPrewriteBoundary_data() {
    QTest::addColumn<int>("platform");
    QTest::addColumn<QString>("slot");
    QTest::addColumn<QString>("boundary");
    QTest::addColumn<int>("outcome");
    for (int platform : {1, 2})
      for (const QString slot : {"a", "b"})
        for (const QString boundary : {"persist", "boot_a", "boot_b"})
          for (int outcome : {0, 1, 2, 3})
            QTest::newRow(qPrintable(QString("p%1-%2-%3-outcome%4")
                                         .arg(platform).arg(slot, boundary).arg(outcome)))
                << platform << slot << boundary << outcome;
  }
  void abPrewriteBoundary() {
    QFETCH(int, platform);
    QFETCH(QString, slot);
    QFETCH(QString, boundary);
    QFETCH(int, outcome);
    FakeRunner runner;
    runner.device = fixtureDevice(Platform(platform), slot);
    OugaFlashService service(&runner);
    configure(service);
    auto opts = options(FlashMode::BothSlots, Platform(platform));
    opts.targetSlot = slot == "a" ? "b" : "a";
    opts.clearData = opts.autoReboot = opts.formatToolsReady = true;
    for (const QString name : {"mke2fs.exe", "make_f2fs.exe", "mke2fs.conf"})
      QVERIFY(put(dir + "/" + name, "fixture-only: never executed"));
    Plan plan;
    QString error;
    QVERIFY2(OugaFlashPlanner::build(images(), runner.device, opts, &plan, &error),
             qPrintable(error));
    if (outcome == 3) {
      runner.after = [&](const QStringList &args) {
        if (args.value(0) == "flash" && args.value(1) == boundary)
          service.requestStop();
      };
    } else {
      runner.failAt = "flash " + boundary + " ";
      runner.failCode = outcome == 0 ? 0 : (outcome == 1 ? 2 : -1);
      runner.normal = outcome != 2;
      runner.failOutput = outcome == 0 ? "FAILED (remote: denied)" : "test failure";
    }
    QSignalSpy done(&service, &OugaFlashService::finished);
    QSignalSpy progress(&service, &OugaFlashService::progress);
    service.execute(plan);
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 3000);
    QVERIFY(!done[0][0].toBool());
    QVERIFY2(runner.trace.join('\n').contains("flash " + boundary + " "),
             qPrintable(runner.trace.join('\n')));
    QCOMPARE(runner.device.slot, slot);
    for (const QString &cmd : runner.trace) {
      QVERIFY2(!cmd.startsWith("set_active"), qPrintable(cmd));
      QVERIFY2(!cmd.startsWith("create-logical-partition"), qPrintable(cmd));
      QVERIFY2(!cmd.startsWith("delete-logical-partition") || cmd.endsWith("-cow"), qPrintable(cmd));
      QVERIFY2(!cmd.startsWith("erase ") && cmd != "-w" && !cmd.startsWith("reboot"), qPrintable(cmd));
      QVERIFY2(!cmd.startsWith("flash my_") && !cmd.startsWith("flash system_") &&
                   !cmd.startsWith("flash modem_"), qPrintable(cmd));
    }
    for (const auto &row : progress)
      QVERIFY(row[2].toString() != "全部步骤成功");
    QVERIFY(!service.busy());
    QVERIFY(!DeviceOperationLease::owner());
  }
  void abSuccessfulExecution_data() {
    QTest::addColumn<int>("platform");
    QTest::addColumn<QString>("slot");
    QTest::addColumn<QString>("target");
    for (int platform : {1, 2})
      for (const QString slot : {"a", "b"})
        for (const QString target : {"a", "b"})
          QTest::newRow(qPrintable(QString("p%1-%2-to%3")
                                       .arg(platform).arg(slot, target)))
              << platform << slot << target;
  }
  void abSuccessfulExecution() {
    QFETCH(int, platform);
    QFETCH(QString, slot);
    QFETCH(QString, target);
    FakeRunner runner;
    runner.device = fixtureDevice(Platform(platform), slot);
    QMap<QString, QString> slotsAtWrite;
    runner.after = [&](const QStringList &args) {
      if (args.value(0) == "flash")
        slotsAtWrite[args.value(1)] = runner.device.slot;
    };
    OugaFlashService service(&runner);
    configure(service);
    auto opts = options(FlashMode::BothSlots, Platform(platform));
    opts.targetSlot = target;
    Plan plan;
    QString error;
    QVERIFY2(OugaFlashPlanner::build(images(), runner.device, opts, &plan, &error),
             qPrintable(error));
    QSignalSpy done(&service, &OugaFlashService::finished);
    QSignalSpy progress(&service, &OugaFlashService::progress);
    service.execute(plan);
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 3000);
    QVERIFY2(done[0][0].toBool(), qPrintable(done[0][1].toString()));
    QCOMPARE(runner.device.slot, target);
    QCOMPARE(runner.device.userspace, true);
    for (const QString physical : {"boot_a", "boot_b", "persist"})
      QCOMPARE(slotsAtWrite.value(physical), slot);
    for (const QString logical : {"system", "my_company", "my_preload"})
      QCOMPARE(slotsAtWrite.value(logical + "_" + target), target);
    QCOMPARE(slotsAtWrite.value("modem_a"), target);
    QCOMPARE(slotsAtWrite.value("modem_b"), target);
    QStringList executed;
    for (const QString &cmd : runner.trace) {
      if (cmd.startsWith("getvar "))
        continue;
      executed << (cmd.startsWith("flash ") ? cmd.section(' ', 0, 1) : cmd);
    }
    QCOMPARE(executed, commands(plan));
    QVERIFY(!service.busy());
    QVERIFY(!DeviceOperationLease::owner());
    const auto sessions = QDir(dir + "/logs").entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    QCOMPARE(sessions.size(), 1);
    const auto result = QJsonDocument::fromJson(
        read(dir + "/logs/" + sessions[0] + "/result.json")).object();
    QVERIFY(result["success"].toBool());
    QCOMPARE(result["completedWrites"].toInt(), plan.flashCount);
    QVERIFY(!progress.isEmpty());
    QCOMPARE(progress.last()[0].toInt(), plan.flashCount);
    QCOMPARE(progress.last()[1].toInt(), plan.flashCount);
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
  void cowCapacitySequence_data() {
    QTest::addColumn<int>("platform");
    QTest::addColumn<int>("mode");
    QTest::addColumn<QString>("slot");
    QTest::addColumn<bool>("cleanedBeforeRebuild");
    for (int platform : {1, 2}) {
      for (const QString slot : {"a", "b"}) {
        QTest::newRow(qPrintable(QString("ab-p%1-%2").arg(platform).arg(slot)))
            << platform << int(FlashMode::BothSlots) << slot << true;
        QTest::newRow(qPrintable(QString("force-p%1-%2").arg(platform).arg(slot)))
            << platform << int(FlashMode::Force) << slot << (slot == "b");
        if (platform == 1)
          QTest::newRow(qPrintable(QString("only-fbd-%1").arg(slot)))
              << platform << int(FlashMode::OnlyFastbootd) << slot << false;
      }
    }
  }
  void cowCapacitySequence() {
    QFETCH(int, platform);
    QFETCH(int, mode);
    QFETCH(QString, slot);
    QFETCH(bool, cleanedBeforeRebuild);
    const auto device = cowCapacityDevice(Platform(platform), slot);
    const auto ps = cowCapacityImages();
    auto opts = options(FlashMode(mode), Platform(platform));
    opts.targetSlot = slot == "a" ? "b" : "a";
    Plan plan;
    QString error;
    const bool ok = OugaFlashPlanner::build(ps, device, opts, &plan, &error);
    QCOMPARE(ok, cleanedBeforeRebuild);
    if (!ok) {
      QVERIFY2(error.contains("容量不足/未知"), qPrintable(error));
      QVERIFY(plan.steps.isEmpty());
      return;
    }
    QVERIFY(plan.device.sameLayout(device));
    QCOMPARE(plan.device.sizes["system_a-cow"], quint64(1024 * 1024));
    const auto cmds = commands(plan);
    const int cow = cmds.indexOf("delete-logical-partition system_a-cow");
    const int create = cmds.indexOf("create-logical-partition system_" +
                                  plan.options.targetSlot + " 2097152");
    QVERIFY(cow >= 0 && create > cow);
    QCOMPARE(cmds.count("delete-logical-partition system_a-cow"), 1);
    for (const QString suffix : {"a", "b"})
      QVERIFY(!cmds.contains("delete-logical-partition vendor_" + suffix));
    Plan regenerated;
    QVERIFY2(OugaFlashPlanner::build(ps, plan.device, plan.options, &regenerated,
                                    &error), qPrintable(error));
    QCOMPARE(planText(regenerated), planText(plan));
    QCOMPARE(regenerated.totalBytes, plan.totalBytes);
  }
  void cowCapacityGuards_data() {
    QTest::addColumn<int>("platform");
    QTest::addColumn<bool>("force");
    QTest::addColumn<QString>("condition");
    QTest::addColumn<QString>("reason");
    const QMap<QString, QString> conditions = {
        {"missing-super", "容量不足/未知"},
        {"small-super", "容量不足/未知"},
        {"oversize-image", "容量不足/未知"},
        {"retained-vendor", "容量不足/未知"},
        {"used-overflow", "占用容量溢出"},
        {"freed-overflow", "释放容量溢出"},
        {"needed-overflow", "镜像总大小溢出"}};
    for (int platform : {1, 2})
      for (bool force : {false, true})
        for (auto it = conditions.cbegin(); it != conditions.cend(); ++it)
          QTest::newRow(qPrintable(QString("p%1-%2-%3").arg(platform)
                                       .arg(force ? "force" : "ab", it.key())))
              << platform << force << it.key() << it.value();
  }
  void cowCapacityGuards() {
    QFETCH(int, platform);
    QFETCH(bool, force);
    QFETCH(QString, condition);
    QFETCH(QString, reason);
    auto device = cowCapacityDevice(Platform(platform), "b");
    auto ps = cowCapacityImages();
    if (condition == "missing-super")
      device.sizes.remove("super");
    else if (condition == "small-super")
      device.sizes["super"] -= 512;
    else if (condition == "oversize-image")
      ++ps[0].expandedBytes; // Rounded to another sector, not rounded down.
    else if (condition == "retained-vendor")
      device.sizes["vendor_b"] += 512;
    else if (condition == "used-overflow")
      device.sizes["vendor_a"] = quint64(LLONG_MAX);
    else if (condition == "freed-overflow")
      device.sizes["system_a"] = quint64(LLONG_MAX);
    else if (condition == "needed-overflow")
      ps[0].expandedBytes = LLONG_MAX;
    auto opts = options(force ? FlashMode::Force : FlashMode::BothSlots,
                        Platform(platform));
    opts.targetSlot = "a";
    Plan plan;
    QString error;
    QVERIFY(!OugaFlashPlanner::build(ps, device, opts, &plan, &error));
    QVERIFY2(error.contains(reason), qPrintable(error));
    QVERIFY(plan.steps.isEmpty());
    QCOMPARE(plan.flashCount, 0);
  }
  void cowCapacityExecution_data() {
    QTest::addColumn<int>("platform");
    QTest::addColumn<QString>("branch");
    QTest::addColumn<int>("outcome");
    for (int platform : {1, 2})
      for (const QString branch : {"ab-a", "ab-b", "force-b"})
        for (int outcome = 0; outcome < 6; ++outcome)
          QTest::newRow(qPrintable(QString("p%1-%2-result%3").arg(platform)
                                       .arg(branch).arg(outcome)))
              << platform << branch << outcome;
  }
  void cowCapacityExecution() {
    QFETCH(int, platform);
    QFETCH(QString, branch);
    QFETCH(int, outcome);
    const QString initialSlot = branch == "ab-a" ? "a" : "b";
    FakeRunner runner;
    runner.device = cowCapacityDevice(Platform(platform), initialSlot);
    OugaFlashService service(&runner);
    configure(service);
    auto opts = options(branch.startsWith("ab") ? FlashMode::BothSlots
                                                : FlashMode::Force,
                        Platform(platform));
    opts.targetSlot = initialSlot == "a" ? "b" : "a";
    opts.clearData = opts.autoReboot = opts.formatToolsReady = true;
    for (const QString name : {"mke2fs.exe", "make_f2fs.exe", "mke2fs.conf"})
      QVERIFY(put(dir + "/" + name, "fixture-only: never executed"));
    Plan plan;
    QString error;
    QVERIFY2(OugaFlashPlanner::build(cowCapacityImages(), runner.device, opts,
                                    &plan, &error), qPrintable(error));
    if (outcome == 5) {
      runner.after = [&](const QStringList &args) {
        if (args == QStringList{"delete-logical-partition", "system_a-cow"})
          service.requestStop();
      };
    } else if (outcome != 0) {
      runner.failAt = "delete-logical-partition system_a-cow";
      runner.failCode = outcome == 1 ? 0 : (outcome == 3 ? -1 : 2);
      runner.normal = outcome != 3;
      runner.failOutput = outcome == 4
          ? "FAILED (remote: transport disconnected)"
          : "FAILED (remote: deletion denied)";
    }
    QSignalSpy done(&service, &OugaFlashService::finished);
    QSignalSpy progress(&service, &OugaFlashService::progress);
    service.execute(plan);
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 3000);
    const QString trace = runner.trace.join('\n');
    QVERIFY2(trace.contains("delete-logical-partition system_a-cow"), qPrintable(trace));
    QCOMPARE(done[0][0].toBool(), outcome == 0);
    if (outcome == 0) {
      QCOMPARE(runner.device.slot, plan.options.targetSlot);
      QVERIFY(!runner.device.sizes.contains("system_a-cow"));
      QCOMPARE(runner.device.sizes["system_" + plan.options.targetSlot],
               quint64(2 * 1024 * 1024));
      QCOMPARE(runner.device.sizes["vendor_a"], quint64(1024 * 1024));
      QCOMPARE(runner.device.sizes["vendor_b"], quint64(1024 * 1024));
      QVERIFY(trace.contains("erase userdata"));
      QVERIFY(trace.contains("erase metadata"));
      QCOMPARE(runner.trace.contains("-w"), platform == 1);
      QVERIFY(runner.trace.contains("reboot"));
      QCOMPARE(progress.last()[0].toInt(), plan.flashCount);
      QCOMPARE(progress.last()[1].toInt(), plan.flashCount);
      QCOMPARE(progress.last()[2].toString(), QString("全部步骤成功"));
    } else {
      QCOMPARE(runner.device.slot, initialSlot);
      for (const QString &cmd : runner.trace) {
        QVERIFY2(!cmd.startsWith("set_active") && !cmd.startsWith("flash ") &&
                 !cmd.startsWith("create-logical-partition") &&
                 !cmd.startsWith("erase ") && cmd != "-w" &&
                 !cmd.startsWith("reboot"), qPrintable(cmd));
        QVERIFY2(!cmd.startsWith("delete-logical-partition") ||
                 cmd == "delete-logical-partition system_a-cow", qPrintable(cmd));
      }
      for (const auto &row : progress)
        QVERIFY(row[2].toString() != "全部步骤成功");
    }
    QVERIFY(plan.device.sizes.contains("system_a-cow"));
    QVERIFY(!service.busy());
    QVERIFY(!DeviceOperationLease::owner());
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
  void standardFbdSequence_data() {
    QTest::addColumn<int>("mode");
    QTest::addColumn<int>("platform");
    QTest::addColumn<QString>("slot");
    QTest::addColumn<int>("modemLayout");
    QTest::addColumn<bool>("clearData");
    QTest::addColumn<bool>("autoReboot");
    for (FlashMode mode : {FlashMode::Normal, FlashMode::AfterSalesFastbootd})
      for (Platform platform : {Platform::Qualcomm, Platform::MediaTek})
        for (const QString slot : {"a", "b"})
          for (int modemLayout = 0; modemLayout < 3; ++modemLayout)
            for (bool clearData : {false, true})
              for (bool autoReboot : {false, true})
                QTest::newRow(qPrintable(QString("m%1-p%2-%3-modem%4-clear%5-reboot%6")
                    .arg(int(mode)).arg(int(platform)).arg(slot).arg(modemLayout)
                    .arg(clearData).arg(autoReboot)))
                    << int(mode) << int(platform) << slot << modemLayout
                    << clearData << autoReboot;
  }
  void standardFbdSequence() {
    QFETCH(int, mode);
    QFETCH(int, platform);
    QFETCH(QString, slot);
    QFETCH(int, modemLayout);
    QFETCH(bool, clearData);
    QFETCH(bool, autoReboot);
    const Platform pf = Platform(platform);
    const FlashMode fm = FlashMode(mode);
    const Device device = standardFbdDevice(pf, slot, modemLayout);
    const auto ps = standardFbdImages(slot, modemLayout);
    auto opts = options(fm, pf);
    opts.clearData = clearData;
    opts.autoReboot = autoReboot;
    opts.formatToolsReady = true;
    Plan plan;
    QString error;
    QVERIFY2(OugaFlashPlanner::build(ps, device, opts, &plan, &error), qPrintable(error));
    QCOMPARE(commands(plan), standardFbdCommands(fm, pf, slot, modemLayout,
                                                clearData, autoReboot));
    QCOMPARE(plan.device.partitions, device.partitions);
    QCOMPARE(plan.device.sizes, device.sizes);
    QCOMPARE(plan.options.targetSlot, slot);
    const bool dual = modemLayout == 1 &&
                      (pf == Platform::Qualcomm || fm == FlashMode::AfterSalesFastbootd);
    QCOMPARE(plan.flashCount, 4 + (modemLayout ? (dual ? 2 : 1) : 0));
    QCOMPARE(plan.totalBytes, qint64(320 + (modemLayout ? (dual ? 96 : 48) : 0)));
    for (const auto &step : plan.steps) {
      const bool bootloaderModem = pf == Platform::Qualcomm && modemLayout == 1 &&
                                   step.arguments.value(0) == "flash" &&
                                   (step.target == "modem_a" || step.target == "modem_b");
      if (step.kind != Step::ModeSwitch)
        QCOMPARE(step.userspace, !bootloaderModem);
    }
  }
  void standardFbdExecution_data() {
    QTest::addColumn<int>("mode");
    QTest::addColumn<int>("platform");
    QTest::addColumn<QString>("slot");
    QTest::addColumn<int>("modemLayout");
    for (FlashMode mode : {FlashMode::Normal, FlashMode::AfterSalesFastbootd})
      for (Platform platform : {Platform::Qualcomm, Platform::MediaTek})
        for (const QString slot : {"a", "b"})
          for (int modemLayout : {1, 2})
            QTest::newRow(qPrintable(QString("m%1-p%2-%3-modem%4")
                .arg(int(mode)).arg(int(platform)).arg(slot).arg(modemLayout)))
                << int(mode) << int(platform) << slot << modemLayout;
  }
  void standardFbdExecution() {
    QFETCH(int, mode);
    QFETCH(int, platform);
    QFETCH(QString, slot);
    QFETCH(int, modemLayout);
    const Platform pf = Platform(platform);
    const FlashMode fm = FlashMode(mode);
    FakeRunner runner;
    runner.device = standardFbdDevice(pf, slot, modemLayout);
    // Readable markers satisfy dependency preflight only. FakeRunner never
    // executes these files or falls back to real platform-tools.
    for (const QString name : {"mke2fs.exe", "make_f2fs.exe", "mke2fs.conf"})
      QVERIFY(put(dir + "/" + name, "fixture-only"));
    OugaFlashService service(&runner);
    configure(service);
    auto opts = options(fm, pf);
    opts.clearData = opts.autoReboot = opts.formatToolsReady = true;
    Plan plan;
    QString error;
    QVERIFY2(OugaFlashPlanner::build(standardFbdImages(slot, modemLayout), runner.device,
                                   opts, &plan, &error), qPrintable(error));
    QSignalSpy done(&service, &OugaFlashService::finished);
    QSignalSpy progress(&service, &OugaFlashService::progress);
    service.execute(plan);
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 3000);
    QVERIFY2(done[0][0].toBool(), qPrintable(done[0][1].toString()));
    QStringList actual;
    for (const QString &line : runner.trace) {
      const QStringList args = line.split(' ');
      if (args.value(0) != "getvar" && args.value(0) != "devices")
        actual << args.mid(0, args.value(0) == "flash" ? 2 : -1).join(' ');
    }
    QCOMPARE(actual, standardFbdCommands(fm, pf, slot, modemLayout, true, true));
    QVERIFY(!progress.isEmpty());
    QCOMPARE(progress.last()[0].toInt(), plan.flashCount);
    QCOMPARE(progress.last()[1].toInt(), plan.flashCount);
    QCOMPARE(runner.device.slot, slot);
    QVERIFY(!DeviceOperationLease::owner());
  }
  void standardFbdFailureBoundary_data() {
    QTest::addColumn<int>("mode");
    QTest::addColumn<int>("platform");
    QTest::addColumn<QString>("slot");
    QTest::addColumn<int>("failure");
    for (FlashMode mode : {FlashMode::Normal, FlashMode::AfterSalesFastbootd})
      for (Platform platform : {Platform::Qualcomm, Platform::MediaTek})
        for (const QString slot : {"a", "b"})
          for (int failure = 0; failure < 6; ++failure)
            QTest::newRow(qPrintable(QString("m%1-p%2-%3-failure%4")
                .arg(int(mode)).arg(int(platform)).arg(slot).arg(failure)))
                << int(mode) << int(platform) << slot << failure;
  }
  void standardFbdFailureBoundary() {
    QFETCH(int, mode);
    QFETCH(int, platform);
    QFETCH(QString, slot);
    QFETCH(int, failure);
    const Platform pf = Platform(platform);
    const FlashMode fm = FlashMode(mode);
    FakeRunner runner;
    runner.device = standardFbdDevice(pf, slot, 1);
    for (const QString name : {"mke2fs.exe", "make_f2fs.exe", "mke2fs.conf"})
      QVERIFY(put(dir + "/" + name, "fixture-only"));
    OugaFlashService service(&runner);
    configure(service);
    auto opts = options(fm, pf);
    opts.clearData = opts.autoReboot = opts.formatToolsReady = true;
    Plan plan;
    QString error;
    QVERIFY2(OugaFlashPlanner::build(standardFbdImages(slot, 1), runner.device,
                                   opts, &plan, &error), qPrintable(error));
    const bool dual = pf == Platform::Qualcomm || fm == FlashMode::AfterSalesFastbootd;
    const QString firstModem = "flash modem_" + (dual ? QString("a") : slot);
    const QString lastModem = "flash modem_" + (dual ? QString("b") : slot);
    if (failure == 0) {
      runner.failAt = firstModem;
      runner.failCode = 0; // FAILED must override a zero exit code.
    } else if (failure == 1) {
      runner.failAt = lastModem;
      runner.failCode = 2;
      runner.failOutput = "unclassified native output";
    } else if (failure == 2) {
      runner.failAt = firstModem;
      runner.normal = false;
      runner.failCode = -1;
    } else if (failure == 3) {
      runner.after = [&](const QStringList &args) {
        if (args.mid(0, 2).join(' ') == firstModem)
          service.requestStop();
      };
    } else if (failure == 4) {
      runner.failAt = "delete-logical-partition system_a-cow";
      runner.failOutput = "FAILED transport read failed: partition does not exist";
    } else {
      runner.failAt = pf == Platform::Qualcomm ? "reboot bootloader" : "flash boot_" + slot;
    }
    QSignalSpy done(&service, &OugaFlashService::finished);
    QSignalSpy progress(&service, &OugaFlashService::progress);
    service.execute(plan);
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 3000);
    QVERIFY(!done[0][0].toBool());
    const QString boundary = failure == 3 ? firstModem : runner.failAt;
    QVERIFY(!runner.trace.isEmpty());
    QVERIFY2(runner.trace.last().startsWith(boundary), qPrintable(runner.trace.join('\n')));
    for (const QString &line : runner.trace)
      QVERIFY(!line.startsWith("erase ") && line != "-w" && line != "reboot" &&
              line != "reboot fastboot" && !line.startsWith("set_active ") &&
              !line.startsWith("create-logical-partition "));
    for (const auto &row : progress)
      QVERIFY(row[2].toString() != "全部步骤成功");
    QCOMPARE(runner.device.slot, slot);
    QVERIFY(!DeviceOperationLease::owner());
  }
private:
  // Independent reference order: do not derive this oracle from production helpers.
  QStringList afterSalesCritical(Platform platform) {
    return platform == Platform::Qualcomm
        ? QStringList{"boot", "dtbo", "init_boot", "modem", "recovery",
                      "vbmeta", "vbmeta_system", "vbmeta_vendor", "vendor_boot"}
        : QStringList{"boot", "dtbo", "init_boot", "lk", "vbmeta",
                      "vbmeta_system", "vbmeta_vendor", "vendor_boot"};
  }
  QVector<Partition> repairImages(Platform platform) {
    QVector<Partition> ps;
    for (const QString &name : afterSalesCritical(platform)) ps << image(name);
    // Neither source enumeration nor UI selection order should affect writes.
    std::reverse(ps.begin(), ps.end());
    return ps;
  }
  Device repairDevice(Platform platform, const QString &start, int layout) {
    auto device = fixtureDevice(platform, start, false);
    for (const QString &name : afterSalesCritical(platform)) {
      if (layout == 0 || (layout == 2 && name != "boot" && name != "modem" && name != "lk")) continue;
      device.partitions.remove(name + "_a");
      device.partitions.remove(name + "_b");
      device.sizes.remove(name + "_a");
      device.sizes.remove(name + "_b");
      device.partitions.insert(name);
      device.sizes[name] = 1024 * 1024;
      device.variables["has-slot:" + name] = "no";
    }
    return device;
  }
  QStringList repairCommands(Platform platform, int layout) {
    QStringList cmds;
    for (const QString &name : afterSalesCritical(platform)) {
      if (layout == 1 || (layout == 2 && (name == "boot" || name == "modem" || name == "lk")))
        cmds << "flash " + name;
      else cmds << "flash " + name + "_a" << "flash " + name + "_b";
    }
    return cmds << "reboot fastboot";
  }
  QStringList repairTrace(const FakeRunner &runner) {
    QStringList out;
    for (const QString &cmd : runner.trace)
      if (!cmd.startsWith("getvar ") && cmd != "devices")
        out << (cmd.startsWith("flash ") ? cmd.section(' ', 0, 1) : cmd);
    return out;
  }
  Device afterSalesDevice(Platform platform, const QString &start,
                          int largerSlot, bool slotless) {
    Device device = fixtureDevice(platform, start, false);
    if (largerSlot == 1) device.sizes["system_a"] += 1024;
    if (largerSlot == 2) device.sizes["system_b"] += 1024;
    for (const QString name : {"modem_backup", "abl"})
      for (const QString slot : {"a", "b"}) {
        device.partitions.insert(name + "_" + slot);
        device.sizes[name + "_" + slot] = 1024 * 1024;
      }
    if (slotless)
      for (const QString name : {"boot", "modem", "modem_backup"}) {
        for (const QString slot : {"a", "b"}) {
          device.partitions.remove(name + "_" + slot);
          device.sizes.remove(name + "_" + slot);
        }
        device.partitions.insert(name);
        device.sizes[name] = 1024 * 1024;
        device.variables["has-slot:" + name] = "no";
      }
    return device;
  }
  QVector<Partition> afterSalesImages(Platform platform) {
    QVector<Partition> ps;
    for (const QString &name : afterSalesCritical(platform))
      ps << image(name, QByteArray(64, 'c'));
    if (platform == Platform::MediaTek)
      ps << image("modem", QByteArray(32, 'm'));
    ps << image("modem_backup_b", QByteArray(96, 'b'))
       << image("abl_b", QByteArray(128, 'a'))
       << image("system", QByteArray(512, 's'))
       << image("super", superBytes());
    return ps;
  }
  QStringList afterSalesCommands(Platform platform, const QString &finalSlot,
                                 bool slotless, bool clear, bool reboot) {
    QStringList cmds;
    for (const QString &name : afterSalesCritical(platform)) {
      cmds << "flash " + name + (slotless && (name == "boot" || name == "modem") ? "" : "_a");
      if (!(slotless && (name == "boot" || name == "modem")))
        cmds << "flash " + name + "_b";
    }
    cmds << "reboot fastboot" << "delete-logical-partition system_a-cow";
    if (platform == Platform::MediaTek) {
      cmds << QString(slotless ? "flash modem" : "flash modem_a");
      if (!slotless) cmds << "flash modem_b";
    }
    cmds << QString(slotless ? "flash modem_backup" : "flash modem_backup_a");
    if (!slotless) cmds << "flash modem_backup_b";
    cmds << "flash abl_a" << "flash abl_b" << "getvar all"
         << "set_active " + finalSlot;
    if (clear) {
      cmds << "erase userdata" << "erase metadata";
      if (platform == Platform::Qualcomm) cmds << "-w";
    }
    if (reboot) cmds << "reboot";
    return cmds;
  }
private slots:
  void repairFastbootdSequence_data() {
    QTest::addColumn<int>("platform");
    QTest::addColumn<QString>("start");
    QTest::addColumn<int>("layout");
    for (int pf : {1, 2})
      for (const QString slot : {"a", "b"})
        for (int layout = 0; layout < 3; ++layout)
          QTest::newRow(qPrintable(QString("%1-%2-layout%3").arg(pf).arg(slot).arg(layout)))
              << pf << slot << layout;
  }
  void repairFastbootdSequence() {
    QFETCH(int, platform); QFETCH(QString, start); QFETCH(int, layout);
    const Platform pf = Platform(platform);
    const auto ps = repairImages(pf);
    const auto device = repairDevice(pf, start, layout);
    auto opts = options(FlashMode::RepairFastbootd, pf);
    // A repair must not inherit normal flashing's selected destructive options.
    opts.clearData = opts.autoReboot = true;
    opts.formatToolsReady = false;
    Plan plan; QString error;
    QVERIFY2(OugaFlashPlanner::build(ps, device, opts, &plan, &error), qPrintable(error));
    const auto expected = repairCommands(pf, layout);
    QCOMPARE(commands(plan), expected);
    QCOMPARE(plan.options.targetSlot, start);
    QVERIFY(!plan.options.clearData && !plan.options.autoReboot);
    QCOMPARE(plan.flashCount, expected.size() - 1);
    QCOMPARE(plan.totalBytes, qint64(plan.flashCount) * 512);
    QCOMPARE(plan.steps.last().kind, Step::ModeSwitch);
    QVERIFY(plan.steps.last().userspace);
    for (const auto &s : plan.steps)
      if (!s.image.isEmpty()) QVERIFY(!s.userspace);
  }
  void repairFastbootdExecution_data() { repairFastbootdSequence_data(); }
  void repairFastbootdExecution() {
    QFETCH(int, platform); QFETCH(QString, start); QFETCH(int, layout);
    const Platform pf = Platform(platform);
    FakeRunner runner;
    runner.device = repairDevice(pf, start, layout);
    auto opts = options(FlashMode::RepairFastbootd, pf);
    opts.clearData = opts.autoReboot = true;
    Plan plan; QString error;
    QVERIFY2(OugaFlashPlanner::build(repairImages(pf), runner.device, opts, &plan, &error), qPrintable(error));
    QVERIFY(put(dir + "/fastboot.exe", "fake"));
    OugaFlashService service(&runner);
    service.configure(dir + "/fastboot.exe", dir + "/logs");
    service.setTiming({1, 1500, 1000, 3, 0});
    int fbdProbes = 0;
    runner.beforeProbe = [&](Device &d, int) { if (d.userspace) ++fbdProbes; };
    QSignalSpy done(&service, &OugaFlashService::finished);
    QSignalSpy progress(&service, &OugaFlashService::progress);
    service.execute(plan);
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 3000);
    QVERIFY2(done[0][0].toBool(), qPrintable(done[0][1].toString()));
    QCOMPARE(repairTrace(runner), repairCommands(pf, layout));
    for (const QString &cmd : runner.boundTrace)
      QVERIFY(cmd.startsWith("-s TEST-SERIAL "));
    QVERIFY(runner.device.userspace);
    QCOMPARE(runner.device.slot, start);
    QVERIFY(fbdProbes >= 3);
    QCOMPARE(progress.last()[0].toInt(), plan.flashCount);
    QCOMPARE(progress.last()[1].toInt(), plan.flashCount);
    const QDir logs(dir + "/logs");
    const auto sessions = logs.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    QCOMPARE(sessions.size(), 1);
    const QDir session(logs.filePath(sessions[0]));
    const auto result = QJsonDocument::fromJson(read(session.filePath("result.json"))).object();
    QVERIFY(result["success"].toBool());
    QCOMPARE(result["completedWrites"].toInt(), plan.flashCount);
    QVERIFY(!DeviceOperationLease::owner());
  }
  void repairFastbootdPreflight_data() {
    QTest::addColumn<int>("platform"); QTest::addColumn<int>("failure");
    for (int pf : {1, 2})
      for (int failure = 0; failure < 10; ++failure)
        QTest::newRow(qPrintable(QString("%1-invalid%2").arg(pf).arg(failure))) << pf << failure;
  }
  void repairFastbootdPreflight() {
    QFETCH(int, platform); QFETCH(int, failure);
    const Platform pf = Platform(platform);
    auto device = repairDevice(pf, "b", 0);
    auto ps = repairImages(pf);
    auto opts = options(FlashMode::RepairFastbootd, pf);
    opts.validateTable = false; // Cannot disable basic target/image safety.
    if (failure == 0) ps.removeLast();
    if (failure == 1) ps.last().selected = false;
    if (failure == 2) ps << ps.first();
    if (failure == 3) device.sizes.remove("vendor_boot_b");
    if (failure == 4) device.sizes["vendor_boot_b"] = 16;
    if (failure == 5) device.partitions.remove("vendor_boot_b");
    if (failure == 6) device.unlocked = false;
    if (failure == 7) device.unlockKnown = false;
    if (failure == 8) device.userspace = true;
    if (failure == 9) opts.packagePlatform = pf == Platform::Qualcomm ? Platform::MediaTek : Platform::Qualcomm;
    Plan plan; QString error;
    QVERIFY(!OugaFlashPlanner::build(ps, device, opts, &plan, &error));
    QVERIFY(!error.isEmpty());
  }
  void repairFastbootdFailureBoundary_data() {
    QTest::addColumn<int>("platform"); QTest::addColumn<int>("failure");
    QTest::addColumn<QString>("target");
    for (int pf : {1, 2}) {
      for (int failure = 0; failure < 5; ++failure)
        for (const QString target : {"boot_a", "boot_b", "dtbo_b", "vendor_boot_b"})
          QTest::newRow(qPrintable(QString("%1-failure%2-%3").arg(pf).arg(failure).arg(target))) << pf << failure << target;
      for (int failure = 5; failure <= 10; ++failure)
        QTest::newRow(qPrintable(QString("%1-mode-or-image%2").arg(pf).arg(failure))) << pf << failure << QString("vendor_boot_b");
    }
  }
  void repairFastbootdFailureBoundary() {
    QFETCH(int, platform); QFETCH(int, failure); QFETCH(QString, target);
    const Platform pf = Platform(platform);
    FakeRunner runner;
    runner.device = repairDevice(pf, "b", 0);
    const auto ps = repairImages(pf);
    auto opts = options(FlashMode::RepairFastbootd, pf);
    opts.clearData = opts.autoReboot = true;
    Plan plan; QString error;
    QVERIFY2(OugaFlashPlanner::build(ps, runner.device, opts, &plan, &error), qPrintable(error));
    QVERIFY(put(dir + "/fastboot.exe", "fake"));
    OugaFlashService service(&runner);
    service.configure(dir + "/fastboot.exe", dir + "/logs");
    service.setTiming({1, 200, 100, 3, 0});
    bool injected = false;
    if (failure < 3 || failure == 7) {
      runner.failAt = failure == 7 ? "reboot fastboot" : "flash " + target;
      runner.failCode = failure == 0 ? 0 : 2;
      runner.normal = failure != 2;
      runner.failOutput = failure == 1 ? "OKAY\nFinished" : "FAILED (remote: injected repair failure)";
    } else if (failure == 5) {
      runner.stuckMode = true;
    } else if (failure == 9) {
      runner.beforeProbe = [&](Device &d, int) {
        if (d.userspace) { injected = true; d.sizes["boot_a"] += 1; }
      };
    } else {
      runner.after = [&](const QStringList &args) {
        const QString trigger = failure == 8 ? "boot_a" : target;
        if (args.value(0) != "flash" || args.value(1) != trigger) return;
        injected = true;
        if (failure == 3) service.requestStop();
        if (failure == 4) runner.device.serial = "REPLACEMENT";
        if (failure == 6) runner.disconnected = true;
        if (failure == 8) {
          const auto vendor = std::find_if(ps.cbegin(), ps.cend(), [](const Partition &p) {
            return p.name == "vendor_boot";
          });
          QVERIFY(vendor != ps.cend());
          QVERIFY(put(vendor->path, QByteArray(512, 'x')));
        }
        if (failure == 10) runner.device.slot = "a";
      };
    }
    QSignalSpy done(&service, &OugaFlashService::finished);
    QSignalSpy progress(&service, &OugaFlashService::progress);
    service.execute(plan);
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 3000);
    QVERIFY(!done[0][0].toBool());
    const auto trace = repairTrace(runner);
    if (failure < 3 || failure == 7) {
      QVERIFY(std::any_of(runner.trace.cbegin(), runner.trace.cend(), [&](const QString &cmd) { return cmd.startsWith(runner.failAt); }));
    } else if (failure != 5) {
      QVERIFY(injected);
    }
    if (failure < 4) {
      const int index = repairCommands(pf, 0).indexOf("flash " + target);
      QVERIFY(index >= 0);
      QCOMPARE(trace, repairCommands(pf, 0).mid(0, index + 1));
      // Progress signals describe the current stage; the durable result records
      // the just-finished write even if stop prevents another stage signal.
      QCOMPARE(progress.last()[0].toInt(), index);
      const QDir logs(dir + "/logs");
      const auto sessions = logs.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
      QCOMPARE(sessions.size(), 1);
      const auto result = QJsonDocument::fromJson(read(QDir(logs.filePath(sessions[0])).filePath("result.json"))).object();
      QCOMPARE(result["completedWrites"].toInt(), index + (failure == 3 ? 1 : 0));
      QVERIFY(!result["success"].toBool());
    }
    if (failure == 8) QVERIFY(!trace.contains("flash vendor_boot_a"));
    if (failure == 8) QVERIFY(!trace.contains("reboot fastboot"));
    if (failure == 4) {
      // The old serial remains bound. A replacement must not receive a command;
      // a last-write replacement can be detected by the attempted old-serial reboot.
      for (const QString &cmd : runner.boundTrace) QVERIFY(cmd.startsWith("-s TEST-SERIAL "));
      if (target != "vendor_boot_b") QVERIFY(!trace.contains("reboot fastboot"));
      QVERIFY(!runner.device.userspace);
    }
    for (const QString &cmd : trace)
      QVERIFY(!cmd.startsWith("erase ") && !cmd.startsWith("set_active ") && cmd != "-w" && cmd != "reboot");
    QCOMPARE(runner.device.slot, failure == 10 ? QString("a") : QString("b"));
    QVERIFY(!DeviceOperationLease::owner());
  }
  void afterSalesBootloaderSequence_data() {
    QTest::addColumn<int>("platform");
    QTest::addColumn<QString>("start");
    QTest::addColumn<int>("largerSlot");
    QTest::addColumn<bool>("slotless");
    QTest::addColumn<bool>("clear");
    QTest::addColumn<bool>("reboot");
    for (int pf : {1, 2})
      for (const QString slot : {"a", "b"})
        for (int larger : {0, 1, 2})
          for (bool noSlot : {false, true})
            for (int end = 0; end < 4; ++end)
              QTest::newRow(qPrintable(QString("%1-%2-size%3-noslot%4-end%5")
                  .arg(pf).arg(slot).arg(larger).arg(noSlot).arg(end)))
                  << pf << slot << larger << noSlot << bool(end & 1) << bool(end & 2);
  }
  void afterSalesBootloaderSequence() {
    QFETCH(int, platform); QFETCH(QString, start); QFETCH(int, largerSlot);
    QFETCH(bool, slotless); QFETCH(bool, clear); QFETCH(bool, reboot);
    const Platform pf = Platform(platform);
    const Device device = afterSalesDevice(pf, start, largerSlot, slotless);
    auto ps = afterSalesImages(pf);
    auto opts = options(FlashMode::AfterSalesBootloader, pf);
    opts.afterSuper = true;
    opts.clearData = clear; opts.autoReboot = reboot; opts.formatToolsReady = true;
    Plan tail; QString error;
    QVERIFY2(OugaFlashPlanner::build(ps, device, opts, &tail, &error), qPrintable(error));
    const QString finalSlot = largerSlot == 2 ? "b" : "a";
    QCOMPARE(tail.options.targetSlot, finalSlot);
    const QStringList expected = afterSalesCommands(pf, finalSlot, slotless, clear, reboot);
    QCOMPARE(commands(tail), expected);
    qint64 bytes = 0; int writes = 0;
    for (const QString &cmd : expected) {
      if (!cmd.startsWith("flash ")) continue;
      ++writes;
      const QString name = baseName(cmd.mid(6));
      const auto found = std::find_if(ps.cbegin(), ps.cend(), [&](const Partition &p) {
        return baseName(p.name) == name;
      });
      QVERIFY(found != ps.cend()); bytes += found->expandedBytes;
    }
    QCOMPARE(tail.flashCount, writes); QCOMPARE(tail.totalBytes, bytes);
    QVERIFY(tail.device.sameSnapshot(device));
    for (const Step &s : tail.steps)
      if (!s.image.isEmpty())
        QCOMPARE(s.userspace, !afterSalesCritical(pf).contains(baseName(s.target)));
    opts.afterSuper = false;
    Plan full;
    QVERIFY2(OugaFlashPlanner::build(ps, device, opts, &full, &error), qPrintable(error));
    QCOMPARE(commands(full), QStringList({"erase super", "flash super"}) + expected);
    QCOMPARE(full.steps[2].kind, Step::Wait);
    QCOMPARE(full.steps[2].waitMs, 120000);
    QCOMPARE(full.steps[3].kind, Step::Checkpoint);
    QCOMPARE(full.flashCount, writes + 1);
    QCOMPARE(full.totalBytes, bytes + ps.last().expandedBytes);
  }
  void afterSalesBootloaderPreflight_data() {
    QTest::addColumn<int>("platform"); QTest::addColumn<int>("failure");
    for (int pf : {1, 2})
      for (int failure = 0; failure < 6; ++failure)
        QTest::newRow(qPrintable(QString("%1-invalid%2").arg(pf).arg(failure))) << pf << failure;
  }
  void afterSalesBootloaderPreflight() {
    QFETCH(int, platform); QFETCH(int, failure);
    const Platform pf = Platform(platform);
    auto device = afterSalesDevice(pf, "a", 2, false);
    auto ps = afterSalesImages(pf);
    auto opts = options(FlashMode::AfterSalesBootloader, pf);
    if (failure == 0) device.sizes.remove("vendor_b");
    if (failure == 1) device.sizes["system_b"] = quint64(LLONG_MAX);
    if (failure == 2) device.sizes["modem_backup_b"] = 16;
    if (failure == 3) device.sizes.remove("modem_backup_b");
    if (failure == 4) device.partitions.remove("modem_backup_b");
    if (failure == 5) ps.removeFirst();
    QString error; Plan plan;
    // Reject invalid tail before the irreversible erase/flash Super stage too.
    QVERIFY(!OugaFlashPlanner::build(ps, device, opts, &plan, &error));
    QVERIFY(!error.isEmpty());
    opts.afterSuper = true;
    QVERIFY(!OugaFlashPlanner::build(ps, device, opts, &plan, &error));
  }
  void afterSalesBootloaderExecution_data() {
    QTest::addColumn<int>("platform");
    QTest::addColumn<QString>("start");
    QTest::addColumn<int>("largerSlot");
    for (int pf : {1, 2})
      for (const QString slot : {"a", "b"})
        for (int larger : {0, 1, 2})
          QTest::newRow(qPrintable(QString("%1-%2-size%3").arg(pf).arg(slot).arg(larger)))
              << pf << slot << larger;
  }
  void afterSalesBootloaderExecution() {
    QFETCH(int, platform); QFETCH(QString, start); QFETCH(int, largerSlot);
    const Platform pf = Platform(platform);
    FakeRunner runner;
    runner.device = afterSalesDevice(pf, start, largerSlot, false);
    auto ps = afterSalesImages(pf);
    auto opts = options(FlashMode::AfterSalesBootloader, pf);
    opts.clearData = opts.autoReboot = true; opts.formatToolsReady = true;
    Plan plan; QString error;
    QVERIFY2(OugaFlashPlanner::build(ps, runner.device, opts, &plan, &error), qPrintable(error));
    QVERIFY(put(dir + "/fastboot.exe", "fake"));
    QVERIFY(put(dir + "/make_f2fs.exe", "fake"));
    QVERIFY(put(dir + "/mke2fs.exe", "fake"));
    QVERIFY(put(dir + "/mke2fs.conf", "fake"));
    OugaFlashService service(&runner);
    service.setTiming({1, 1500, 1000, 3, 0});
    service.configure(dir + "/fastboot.exe", dir + "/logs");
    QSignalSpy done(&service, &OugaFlashService::finished);
    QSignalSpy checkpoint(&service, &OugaFlashService::checkpoint);
    QSignalSpy progress(&service, &OugaFlashService::progress);
    service.execute(plan);
    QTRY_COMPARE_WITH_TIMEOUT(checkpoint.count(), 1, 3000);
    QVERIFY(service.paused());
    QCOMPARE(runner.device.slot, start);
    QVERIFY(!runner.trace.join('\n').contains("flash boot"));
    service.confirmCheckpoint(true);
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 3000);
    QVERIFY2(done[0][0].toBool(), qPrintable(done[0][1].toString()));
    QStringList actual;
    for (const QString &cmd : runner.trace)
      if (!cmd.startsWith("getvar ") && cmd != "devices")
        actual << (cmd.startsWith("flash ") ? cmd.section(' ', 0, 1) : cmd);
    const QString finalSlot = largerSlot == 2 ? "b" : "a";
    auto expected = afterSalesCommands(pf, finalSlot, false, true, true);
    expected.removeAll("getvar all");
    QCOMPARE(actual, QStringList({"erase super", "flash super"}) + expected);
    QCOMPARE(runner.device.slot, finalSlot);
    QCOMPARE(progress.last()[0].toInt(), plan.flashCount);
    QVERIFY(!DeviceOperationLease::owner());
  }
  void afterSalesBootloaderFailureBoundary_data() {
    QTest::addColumn<int>("platform"); QTest::addColumn<int>("failure");
    for (int pf : {1, 2})
      for (int failure = 0; failure < 11; ++failure)
        QTest::newRow(qPrintable(QString("%1-failure%2").arg(pf).arg(failure))) << pf << failure;
  }
  void afterSalesBootloaderFailureBoundary() {
    QFETCH(int, platform); QFETCH(int, failure);
    const Platform pf = Platform(platform);
    FakeRunner runner;
    runner.device = afterSalesDevice(pf, "a", 2, false);
    auto ps = afterSalesImages(pf);
    auto opts = options(FlashMode::AfterSalesBootloader, pf);
    opts.afterSuper = true; opts.clearData = opts.autoReboot = true; opts.formatToolsReady = true;
    Plan plan; QString error;
    QVERIFY2(OugaFlashPlanner::build(ps, runner.device, opts, &plan, &error), qPrintable(error));
    QVERIFY(put(dir + "/fastboot.exe", "fake"));
    QVERIFY(put(dir + "/make_f2fs.exe", "fake"));
    QVERIFY(put(dir + "/mke2fs.exe", "fake")); QVERIFY(put(dir + "/mke2fs.conf", "fake"));
    OugaFlashService service(&runner);
    service.setTiming({1, 500, 1000, 3, 0});
    service.configure(dir + "/fastboot.exe", dir + "/logs");
    if (failure < 3) {
      runner.failAt = failure == 0 ? "flash dtbo_b" : "flash modem_backup_b";
      runner.failCode = failure == 0 ? 0 : 2;
      runner.normal = failure != 2;
    } else if (failure == 3) {
      runner.failAt = "reboot fastboot";
    } else if (failure == 4) {
      runner.after = [&](const QStringList &a) {
        if (a.mid(0, 2).join(' ') == "flash abl_b") service.requestStop();
      };
    } else {
      runner.after = [&](const QStringList &a) {
        if (failure == 10 && a == QStringList({"reboot", "fastboot"})) {
          runner.device.sizes["system_a"] += 2 * 1024 * 1024;
          return;
        }
        if (a.mid(0, 2).join(' ') != "flash abl_b") return;
        if (failure == 5) runner.device.sizes["system_a"] += 2 * 1024 * 1024;
        if (failure == 6) runner.device.sizes.remove("system_b");
        if (failure == 7) runner.device.serial = "REPLACEMENT";
        if (failure == 8) runner.device.slot = "b";
        if (failure == 9) runner.disconnected = true;
      };
    }
    QSignalSpy done(&service, &OugaFlashService::finished);
    service.execute(plan);
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 3000);
    QVERIFY(!done[0][0].toBool());
    if (failure < 4) {
      QVERIFY(std::any_of(runner.trace.cbegin(), runner.trace.cend(),
                         [&](const QString &cmd) { return cmd.startsWith(runner.failAt); }));
    } else {
      QVERIFY(std::any_of(runner.trace.cbegin(), runner.trace.cend(),
                         [](const QString &cmd) { return cmd.startsWith("flash abl_b "); }));
      if (failure >= 5)
        QVERIFY(runner.trace.last().startsWith("getvar "));
    }
    if (failure == 10)
      QVERIFY(done[0][1].toString().contains("售后启动槽与确认计划不一致"));
    for (const QString &cmd : runner.trace)
      QVERIFY(!cmd.startsWith("set_active ") && !cmd.startsWith("erase ") && cmd != "-w" && cmd != "reboot");
    QVERIFY(!DeviceOperationLease::owner());
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
    QCOMPARE(cmds.count("flash modem_backup_a"), 1);
    if (pf == Platform::MediaTek) {
      QCOMPARE(cmds.count("flash modem_b"), 1);
      QCOMPARE(cmds.count("flash modem_a"), 1);
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
  void payloadPreparationClassification_data() {
    QTest::addColumn<bool>("sourceOperation");
    QTest::addColumn<bool>("oldMetadata");
    QTest::addColumn<bool>("provideOldImages");
    QTest::addColumn<bool>("mixed");
    QTest::addColumn<QStringList>("selected");
    QTest::addColumn<bool>("success");
    QTest::newRow("full-minor9-no-old-images")
        << false << false << false << false << QStringList() << true;
    QTest::newRow("full-old-metadata-no-old-images")
        << false << true << false << false << QStringList() << true;
    QTest::newRow("source-copy-needs-old-images")
        << true << true << false << false << QStringList() << false;
    QTest::newRow("source-copy-missing-baseline-metadata")
        << true << false << true << false << QStringList() << false;
    QTest::newRow("source-copy-matching-old-images")
        << true << true << true << false << QStringList() << true;
    QTest::newRow("selected-full-partition-in-mixed-payload")
        << false << false << false << true << QStringList{"boot"} << true;
    QTest::newRow("selected-source-partition-in-mixed-payload")
        << false << false << false << true << QStringList{"system"} << false;
  }
  void payloadPreparationClassification() {
    QFETCH(bool, sourceOperation);
    QFETCH(bool, oldMetadata);
    QFETCH(bool, provideOldImages);
    QFETCH(bool, mixed);
    QFETCH(QStringList, selected);
    QFETCH(bool, success);
    QByteArray bytes;
    if (mixed)
      bytes = mixedPayloadBytes();
    else if (sourceOperation && !oldMetadata)
      bytes = payloadContainer(payloadPartition("boot", 4) + vi(12 << 3) + vi(9));
    else
      bytes = payloadBytes(sourceOperation, 9, oldMetadata);
    const QString file = dir + "/payload.bin", output = dir + "-out";
    QVERIFY(put(file, bytes));
    const QString oldDirectory = provideOldImages ? dir + "/old" : QString();
    if (provideOldImages) {
      QVERIFY(QDir().mkpath(oldDirectory));
      QVERIFY(put(oldDirectory + "/boot.img", QByteArray(512, 'p')));
    }
    OugaPreparation prep;
    QSignalSpy done(&prep, &OugaPreparation::finished),
        ready(&prep, &OugaPreparation::prepared);
    prep.payload(QCoreApplication::applicationFilePath(), file, output, selected,
                 oldDirectory);
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 10000);
    QCOMPARE(done[0][0].toBool(), success);
    QCOMPARE(ready.count(), success ? 1 : 0);
    if (success)
      QCOMPARE(read(output + "/boot.img"), QByteArray(512, 'p'));
    else {
      QVERIFY(done[0][1].toString().contains("旧镜像"));
      QVERIFY(!QFileInfo::exists(output + "/boot.img"));
    }
    QCOMPARE(read(file), bytes);
    QVERIFY(!DeviceOperationLease::owner());
  }
  void nativePayloadExtraction_data() {
    QTest::addColumn<bool>("zip");
    QTest::addColumn<QStringList>("selected");
    QTest::newRow("payload-all") << false << QStringList();
    QTest::newRow("stored-zip-all") << true << QStringList();
    QTest::newRow("stored-zip-selected") << true << QStringList{"vendor"};
  }
  void nativePayloadExtraction() {
    QFETCH(bool, zip);
    QFETCH(QStringList, selected);
    QByteArray boot, vendor;
    const QByteArray payload = nativePayloadBytes(&boot, &vendor);
    QVERIFY(!boot.isEmpty() && !vendor.isEmpty());
    const QByteArray bytes = zip ? zipBytes("payload.bin", payload) : payload;
    const QString file = dir + (zip ? "/ota.zip" : "/payload.bin");
    const QString output = dir + "-native";
    QVERIFY(put(file, bytes));
    OugaPreparation prep;
    QSignalSpy done(&prep, &OugaPreparation::finished),
        ready(&prep, &OugaPreparation::prepared),
        progress(&prep, &OugaPreparation::payloadProgress),
        rows(&prep, &OugaPreparation::payloadPartitionFinished);
    // No tool: the in-process path must not depend on payload.exe.
    prep.payload(QString(), file, output, selected);
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 20000);
    QVERIFY2(done[0][0].toBool(), qPrintable(done[0][1].toString()));
    QCOMPARE(ready.count(), 1);
    const auto images = qvariant_cast<QVector<Ouga::Partition>>(ready[0][0]);
    QCOMPARE(images.size(), selected.isEmpty() ? 2 : 1);
    for (const auto &image : images) {
      const QByteArray &expected = image.name == "boot" ? boot : vendor;
      QCOMPARE(read(image.path), expected);
      QCOMPARE(image.sha256, sha(expected));
      QCOMPARE(OugaPackage::digest(image.path, nullptr), image.sha256);
    }
    QCOMPARE(QFileInfo::exists(output + "/boot.img"), selected.isEmpty());
    QCOMPARE(rows.count(), images.size());
    QVERIFY(!progress.isEmpty());
    QCOMPARE(progress.last()[0].toInt(), 100);
    for (int i = 1; i < progress.size(); ++i)
      QVERIFY(progress[i][0].toInt() > progress[i - 1][0].toInt());
    QVERIFY(QDir(output).entryList({"*.partial"}, QDir::Files).isEmpty());
    QCOMPARE(read(file), bytes);
    QVERIFY(!DeviceOperationLease::owner());
  }
  void nativePayloadRejectsTamperedData() {
    QByteArray boot, vendor;
    QByteArray payload = nativePayloadBytes(&boot, &vendor);
    payload[payload.size() - 1] = char(payload.at(payload.size() - 1) ^ 0x55);
    const QString file = dir + "/payload.bin", output = dir + "-tampered";
    QVERIFY(put(file, payload));
    OugaPreparation prep;
    QSignalSpy done(&prep, &OugaPreparation::finished),
        ready(&prep, &OugaPreparation::prepared),
        rows(&prep, &OugaPreparation::payloadPartitionFinished);
    prep.payload(QString(), file, output);
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 20000);
    QVERIFY(!done[0][0].toBool());
    QVERIFY(done[0][1].toString().contains("sha256"));
    QCOMPARE(ready.count(), 0);
    QVERIFY(!QFileInfo::exists(output + "/vendor.img"));
    QVERIFY(QDir(output).entryList({"*.partial"}, QDir::Files).isEmpty());
    // boot was complete and verified; vendor is reported as failed.
    QCOMPARE(read(output + "/boot.img"), boot);
    QVERIFY(std::any_of(rows.cbegin(), rows.cend(), [](const QList<QVariant> &r) {
      return r[0].toString() == "vendor" && !r[1].toBool();
    }));
  }
  void nativePayloadRepeatedOperations_data() {
    QTest::addColumn<int>("type");
    QTest::addColumn<int>("workers");
    for (int type : {0, 1, 8, 14})
      for (int workers : {1, 4})
        QTest::newRow(qPrintable(QString("type-%1-workers-%2").arg(type).arg(workers)))
            << type << workers;
  }
  void nativePayloadRepeatedOperations() {
    QFETCH(int, type);
    QFETCH(int, workers);
    QByteArray blob, manifest = vi(3 << 3) + vi(4096);
    QMap<QString, QByteArray> expected;
    // Different input/output sizes exercise reused buffers, hashes and XZ state.
    for (const QString &name : {QString("boot"), QString("vendor")}) {
      QByteArray image;
      QVector<NativeOp> operations;
      quint64 block = 0;
      for (int i = 0; i < 48; ++i) {
        const int blocks = 1 + i % 5;
        const QByteArray raw = nativeImage(blocks, char('a' + i));
        const QByteArray encoded = type == 8 ? xzBytes(raw) :
                                   type == 1 ? bzBytes(raw) :
                                   type == 14 ? zstdBytes(raw, i % 2) : raw;
        QVERIFY(!encoded.isEmpty());
        operations << NativeOp{type, block, quint64(blocks), encoded};
        image += raw;
        block += quint64(blocks);
      }
      expected[name] = image;
      manifest += nativePartition(name.toUtf8(), image, operations, &blob);
    }
    const QString file = dir + "/payload.bin", output = dir + "/images";
    const QByteArray bytes = payloadContainer(manifest) + blob;
    QVERIFY(put(file, bytes));
    QVERIFY(QDir().mkpath(output));
    QVector<OugaPayloadEntry> entries;
    OugaPayloadLayout layout;
    bool delta = false;
    QString error;
    QVERIFY(OugaPackage::payloadManifest(file, &entries, &delta, &error, &layout));
    std::atomic_bool cancel{false};
    error = OugaPayloadExtractor::extract(file, layout, entries, output, workers,
                                          cancel, {});
    QVERIFY2(error.isEmpty(), qPrintable(error));
    for (auto i = expected.cbegin(); i != expected.cend(); ++i) {
      const QString target = output + '/' + i.key() + ".img";
      QCOMPARE(read(target), i.value());
      QCOMPARE(OugaPackage::digest(target, nullptr), sha(i.value()));
    }
    QCOMPARE(read(file), bytes);
  }
  void nativePayloadReusesWorkers() {
    QByteArray blob, manifest = vi(3 << 3) + vi(4096);
    const QByteArray image = nativeImage(1, 'x');
    const QStringList names = {"boot", "vendor", "system", "system_ext", "product",
                               "odm", "modem", "tz", "dsp", "abl", "dtbo", "vbmeta"};
    for (const QString &name : names)
      manifest += nativePartition(name.toUtf8(), image, {{0, 0, 1, image}}, &blob);
    const QString file = dir + "/payload.bin", output = dir + "/images";
    QVERIFY(put(file, payloadContainer(manifest) + blob));
    QVERIFY(QDir().mkpath(output));
    QVector<OugaPayloadEntry> entries;
    OugaPayloadLayout layout;
    bool delta = false;
    QString error;
    QVERIFY(OugaPackage::payloadManifest(file, &entries, &delta, &error, &layout));
    QSet<Qt::HANDLE> threads;
    QMutex mutex;
    std::atomic_bool cancel{false};
    OugaPayloadExtractor::Callbacks callbacks;
    callbacks.progress = [&](quint64 done, quint64) {
      if (!done)
        return;
      QMutexLocker lock(&mutex);
      threads.insert(QThread::currentThreadId());
    };
    error = OugaPayloadExtractor::extract(file, layout, entries, output, 1,
                                          cancel, callbacks);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    // QFuture may execute a queued job in the waiting caller as well.
    QVERIFY(!threads.isEmpty());
    QVERIFY2(threads.size() <= 2, "A thread was recreated for every partition");
    for (const QString &name : names)
      QCOMPARE(read(output + '/' + name + ".img"), image);
  }
  void nativePayloadXzBounds_data() {
    QTest::addColumn<int>("fault");
    QTest::newRow("truncated-footer") << 0;
    QTest::newRow("corrupted-stream") << 1;
    QTest::newRow("short-output") << 2;
    QTest::newRow("oversized-output") << 3;
    QTest::newRow("trailing-junk") << 4;
  }
  void nativePayloadXzBounds() {
    QFETCH(int, fault);
    QByteArray image = nativeImage(2, 'q'), encoded = xzBytes(image);
    QVERIFY(!encoded.isEmpty());
    if (fault == 0)
      encoded.chop(1);
    else if (fault == 1)
      encoded[encoded.size() / 2] ^= 0x44;
    else if (fault == 2)
      image += QByteArray(4096, 0);
    else if (fault == 3)
      image.chop(4096);
    else
      encoded += "junk";
    QByteArray blob;
    const QByteArray manifest = vi(3 << 3) + vi(4096) +
        nativePartition("boot", image,
                         {{8, 0, quint64(image.size() / 4096), encoded}}, &blob);
    const QString file = dir + "/payload.bin", output = dir + "-invalid-xz";
    QVERIFY(put(file, payloadContainer(manifest) + blob));
    OugaPreparation preparation;
    QSignalSpy done(&preparation, &OugaPreparation::finished),
               ready(&preparation, &OugaPreparation::prepared);
    preparation.payload({}, file, output);
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 20000);
    QVERIFY(!done[0][0].toBool());
    QVERIFY2(done[0][1].toString().contains("解压失败"),
             qPrintable(done[0][1].toString()));
    QCOMPARE(ready.count(), 0);
    QVERIFY(!QFileInfo::exists(output + "/boot.img"));
    QVERIFY(QDir(output).entryList({"*.partial"}, QDir::Files).isEmpty());
  }
  void nativePayloadZstdExtraction_data() {
    QTest::addColumn<bool>("zip");
    QTest::addColumn<QStringList>("selected");
    QTest::newRow("payload-all") << false << QStringList();
    QTest::newRow("stored-zip-all") << true << QStringList();
    QTest::newRow("payload-selected-compressed") << false << QStringList{"boot"};
    QTest::newRow("stored-zip-selected-unknown-size") << true << QStringList{"system"};
  }
  void nativePayloadZstdExtraction() {
    QFETCH(bool, zip);
    QFETCH(QStringList, selected);
    QMap<QString, QByteArray> expected;
    const QByteArray payload = nativeZstdPayloadBytes(&expected);
    const QByteArray bytes = zip ? zipBytes("payload.bin", payload) : payload;
    const QString file = dir + (zip ? "/ota 包.zip" : "/payload.bin"),
                  output = dir + "-zstd 中文 images";
    QVERIFY(put(file, bytes));
    OugaPreparation preparation;
    QSignalSpy done(&preparation, &OugaPreparation::finished),
               ready(&preparation, &OugaPreparation::prepared),
               progress(&preparation, &OugaPreparation::payloadProgress),
               rows(&preparation, &OugaPreparation::payloadPartitionFinished);
    preparation.payload({}, file, output, selected); // No external dumper exists.
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 20000);
    QVERIFY2(done[0][0].toBool(), qPrintable(done[0][1].toString()));
    QCOMPARE(ready.count(), 1);
    const auto images = qvariant_cast<QVector<Ouga::Partition>>(ready[0][0]);
    QCOMPARE(images.size(), selected.isEmpty() ? 4 : selected.size());
    for (const auto &image : images) {
      QCOMPARE(read(image.path), expected.value(image.name));
      QCOMPARE(image.sha256, sha(expected.value(image.name)));
      QCOMPARE(OugaPackage::digest(image.path, nullptr), image.sha256);
    }
    QCOMPARE(rows.count(), images.size());
    for (const auto &row : rows)
      QVERIFY(row[1].toBool());
    QVERIFY(progress.size() >= 2);
    QCOMPARE(progress.last()[0].toInt(), 100);
    for (int i = 1; i < progress.size(); ++i)
      QVERIFY(progress[i][0].toInt() > progress[i - 1][0].toInt());
    for (auto i = expected.cbegin(); i != expected.cend(); ++i)
      QCOMPARE(QFileInfo::exists(output + '/' + i.key() + ".img"),
               selected.isEmpty() || selected.contains(i.key()));
    QVERIFY(QDir(output).entryList({"*.partial"}, QDir::Files).isEmpty());
    QCOMPARE(read(file), bytes);
  }
  void nativePayloadZstdBounds_data() {
    QTest::addColumn<int>("fault");
    const char *names[] = {"truncated-checksum", "invalid-checksum", "short-output",
        "oversized-output", "trailing-junk", "concatenated-frames", "skippable-frame",
        "dictionary-required", "oversized-window", "reserved-block-type",
        "truncated-frame-header", "unknown-size-short-output",
        "unknown-size-oversized-output", "corrupt-compressed-data"};
    for (int i = 0; i < int(sizeof(names) / sizeof(names[0])); ++i)
      QTest::newRow(names[i]) << i;
  }
  void nativePayloadZstdBounds() {
    QFETCH(int, fault);
    const QByteArray original = nativeImage(2, 'z');
    QByteArray image = original, encoded = zstdCompressedBytes();
    QVERIFY(!encoded.isEmpty());
    switch (fault) {
    case 0: encoded.chop(1); break;
    case 1: encoded[encoded.size() - 1] ^= 0x5a; break;
    case 2: image += QByteArray(4096, 0); break;
    case 3: image.chop(4096); break;
    case 4: encoded += "junk"; break;
    case 5: encoded += zstdCompressedBytes(); break;
    case 6: encoded = QByteArray::fromHex("502a4d1800000000"); break;
    case 7:
      encoded = zstdBytes(original);
      encoded[4] = char(0xa1); // 1-byte dictionary ID before the 4-byte FCS
      encoded.insert(5, char(7));
      break;
    case 8:
      encoded = zstdBytes(original, true);
      encoded[5] = char(0x98); // 512 MiB window, above decoder policy
      break;
    case 9:
      encoded = zstdBytes(original);
      encoded[9] = char((quint8(encoded[9]) & ~6u) | 6u);
      break;
    case 10: encoded = QByteArray::fromHex("28b52ffda0"); break;
    case 11:
      encoded = zstdBytes(original, true);
      image += QByteArray(4096, 0);
      break;
    case 12:
      encoded = zstdBytes(original, true);
      image.chop(4096);
      break;
    case 13: encoded[encoded.size() - 7] ^= 0x40; break;
    }
    QByteArray blob, manifest = vi(3 << 3) + vi(4096);
    const QByteArray boot = nativeImage(1, 'a');
    manifest += nativePartition("boot", boot, {{0, 0, 1, boot}}, &blob);
    manifest += nativePartition("vendor", image,
        {{14, 0, quint64(image.size() / 4096), encoded}}, &blob);
    const QString file = dir + "/payload.bin", output = dir + "-invalid-zstd";
    const QByteArray bytes = payloadContainer(manifest) + blob;
    QVERIFY(put(file, bytes));
    OugaPreparation preparation;
    QSignalSpy done(&preparation, &OugaPreparation::finished),
               ready(&preparation, &OugaPreparation::prepared),
               progress(&preparation, &OugaPreparation::payloadProgress);
    preparation.payload({}, file, output);
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 20000);
    QVERIFY(!done[0][0].toBool());
    QVERIFY2(done[0][1].toString().contains("解压失败"),
             qPrintable(done[0][1].toString()));
    QCOMPARE(ready.count(), 0);
    for (const auto &row : progress)
      QVERIFY(row[0].toInt() < 100);
    QCOMPARE(read(output + "/boot.img"), boot);
    QVERIFY(!QFileInfo::exists(output + "/vendor.img"));
    QVERIFY(QDir(output).entryList({"*.partial"}, QDir::Files).isEmpty());
    QCOMPARE(read(file), bytes);
  }
  void nativePayloadZstdTamperedData() {
    QMap<QString, QByteArray> expected;
    QByteArray bytes = nativeZstdPayloadBytes(&expected);
    const QString file = dir + "/payload.bin", output = dir + "-bad-zstd-digest";
    QVERIFY(put(file, bytes));
    QVector<OugaPayloadEntry> entries;
    OugaPayloadLayout layout;
    bool delta = false;
    QString error;
    QVERIFY(OugaPackage::payloadManifest(file, &entries, &delta, &error, &layout));
    QCOMPARE(entries[0].ops[0].type, quint32(14));
    bytes[qsizetype(layout.dataOffset)] ^= 0x40; // Manifest hash is unchanged.
    QVERIFY(put(file, bytes));
    OugaPreparation preparation;
    QSignalSpy done(&preparation, &OugaPreparation::finished),
               ready(&preparation, &OugaPreparation::prepared),
               progress(&preparation, &OugaPreparation::payloadProgress);
    preparation.payload({}, file, output);
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 20000);
    QVERIFY(!done[0][0].toBool());
    QVERIFY(done[0][1].toString().contains("sha256"));
    QCOMPARE(ready.count(), 0);
    for (const auto &row : progress)
      QVERIFY(row[0].toInt() < 100);
    QVERIFY(!QFileInfo::exists(output + "/boot.img"));
    QVERIFY(QDir(output).entryList({"*.partial"}, QDir::Files).isEmpty());
  }
  void nativePayloadRejectsWrongPartitionHash_data() {
    QTest::addColumn<int>("type");
    QTest::addColumn<QString>("name");
    QTest::newRow("raw") << 0 << QString("vendor");
    QTest::newRow("bz") << 1 << QString("vendor");
    QTest::newRow("xz") << 8 << QString("vendor");
    QTest::newRow("zstd") << 14 << QString("vendor");
    QTest::newRow("zero") << 6 << QString("vendor");
    QTest::newRow("discard") << 7 << QString("vendor");
    QTest::newRow("excluded-misc") << 0 << QString("misc");
  }
  void nativePayloadRejectsWrongPartitionHash() {
    QFETCH(int, type);
    QFETCH(QString, name);
    const QByteArray actual = type == 6 || type == 7
        ? QByteArray(8192, 0) : nativeImage(2, 'z');
    QByteArray declared = actual;
    declared[0] ^= 1; // Only the final image hash is wrong; every operation is valid.
    const QByteArray encoded = type == 0 ? actual : type == 1 ? bzBytes(actual)
        : type == 8 ? xzBytes(actual) : type == 14 ? zstdCompressedBytes()
        : QByteArray();
    const QByteArray boot = nativeImage(1, 'b');
    QByteArray blob, manifest = vi(3 << 3) + vi(4096);
    manifest += nativePartition("boot", boot, {{0, 0, 1, boot}}, &blob);
    manifest += nativePartition(name.toUtf8(), declared, {{type, 0, 2, encoded}}, &blob);
    const QByteArray source = payloadContainer(manifest) + blob;
    const QString file = dir + "/payload.bin", output = dir + "-wrong-partition-hash";
    QVERIFY(put(file, source));
    OugaPreparation prep;
    QSignalSpy done(&prep, &OugaPreparation::finished),
        ready(&prep, &OugaPreparation::prepared),
        progress(&prep, &OugaPreparation::payloadProgress),
        rows(&prep, &OugaPreparation::payloadPartitionFinished);
    prep.payload({}, file, output);
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 20000);
    QVERIFY2(!done[0][0].toBool(), "A wrong partition digest must not report successful extraction");
    QVERIFY(done[0][1].toString().contains("SHA-256"));
    QCOMPARE(ready.count(), 0);
    for (const auto &row : progress) QVERIFY(row[0].toInt() < 100);
    QCOMPARE(read(output + "/boot.img"), boot); // Earlier verified output survives.
    QVERIFY(!QFileInfo::exists(output + '/' + name + ".img"));
    QVERIFY(QDir(output).entryList({"*.partial"}, QDir::Files).isEmpty());
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows[0][0].toString(), QString("boot"));
    QVERIFY(rows[0][1].toBool());
    QCOMPARE(rows[1][0].toString(), name);
    QVERIFY(!rows[1][1].toBool());
    QCOMPARE(read(file), source);
  }
  void nativePayloadValidatesWrittenBytes_data() {
    QTest::addColumn<QString>("mutation");
    QTest::newRow("unchanged-multichunk") << QString();
    QTest::newRow("changed-byte") << QString("byte");
    QTest::newRow("truncated-output") << QString("truncate");
  }
  void nativePayloadValidatesWrittenBytes() {
    QFETCH(QString, mutation);
    const QByteArray image = nativeImage(769, 'a');
    QByteArray blob;
    const QByteArray manifest = vi(3 << 3) + vi(4096) +
        nativePartition("boot", image, {{0, 0, 769, image}}, &blob);
    const QString file = dir + "/payload.bin", output = dir + "/images";
    QVERIFY(put(file, payloadContainer(manifest) + blob));
    QVERIFY(QDir().mkpath(output));
    QVector<OugaPayloadEntry> entries;
    OugaPayloadLayout layout;
    bool delta = false;
    QString error;
    QVERIFY(OugaPackage::payloadManifest(file, &entries, &delta, &error, &layout));
    std::atomic_bool cancel{false};
    bool changed = false;
    int finished = 0;
    OugaPayloadExtractor::Callbacks callbacks;
    callbacks.progress = [&](quint64 done, quint64 total) {
      if (done != total || mutation.isEmpty()) return;
      QFile partial(output + "/boot.img.partial");
      if (!partial.open(QIODevice::ReadWrite | QIODevice::Unbuffered)) return;
      changed = mutation == "truncate" ? partial.resize(4096)
                                       : partial.seek(123) && partial.write("!", 1) == 1;
    };
    callbacks.finished = [&](const QString &) { ++finished; };
    error = OugaPayloadExtractor::extract(file, layout, entries, output, 1, cancel, callbacks);
    if (mutation.isEmpty()) {
      QVERIFY2(error.isEmpty(), qPrintable(error));
      QCOMPARE(finished, 1);
      QCOMPARE(read(output + "/boot.img"), image);
    } else {
      QVERIFY(changed);
      QVERIFY2(!error.isEmpty(), "Final on-disk bytes were modified but extraction reported success");
      QCOMPARE(finished, 0);
      QVERIFY(!QFileInfo::exists(output + "/boot.img"));
    }
    QVERIFY(QDir(output).entryList({"*.partial"}, QDir::Files).isEmpty());
  }
  void nativePayloadEligibility() {
    QByteArray boot, vendor;
    const QByteArray payload = nativePayloadBytes(&boot, &vendor);
    const QString file = dir + "/payload.bin";
    QVERIFY(put(file, payload));
    QVector<OugaPayloadEntry> entries;
    bool delta = true;
    QString error;
    OugaPayloadLayout layout;
    QVERIFY2(OugaPackage::payloadManifest(file, &entries, &delta, &error, &layout),
             qPrintable(error));
    QVERIFY(!delta);
    QCOMPARE(layout.blockSize, quint64(4096));
    QVERIFY(OugaPayloadExtractor::supported(entries, {}, layout));
    QCOMPARE(entries[0].ops.size(), 3);
    auto gap = entries;
    gap[0].ops.removeLast(); // ZERO extent missing: not an exact tiling
    QVERIFY(!OugaPayloadExtractor::supported(gap, {}, layout));
    auto overlap = entries;
    overlap[0].ops[2].destination[0].start = 2;
    QVERIFY(!OugaPayloadExtractor::supported(overlap, {}, layout));
    auto unhashed = entries;
    unhashed[0].ops[1].dataHash.clear();
    QVERIFY(!OugaPayloadExtractor::supported(unhashed, {}, layout));
    auto zstd = entries;
    zstd[1].ops[0].type = 14;
    QVERIFY(OugaPayloadExtractor::supported(zstd, {}, layout));
    QVERIFY(OugaPayloadExtractor::supported(zstd, {"boot"}, layout));
    auto unsupported = entries;
    unsupported[1].ops[0].type = 15;
    QVERIFY(!OugaPayloadExtractor::supported(unsupported, {}, layout));
    auto outside = entries;
    outside[1].ops[0].dataOffset = layout.end;
    QVERIFY(!OugaPayloadExtractor::supported(outside, {}, layout));
    // Manifest-only fixtures (no extents) keep using payload.exe.
    QVERIFY(put(file, payloadBytes()));
    QVERIFY(OugaPackage::payloadManifest(file, &entries, &delta, &error, &layout));
    QVERIFY(!OugaPayloadExtractor::supported(entries, {}, layout));
  }
  void payloadZipLocation() {
    QByteArray boot, vendor;
    const QByteArray payload = nativePayloadBytes(&boot, &vendor);
    const QString file = dir + "/ota.zip";
    quint64 offset = 0, size = 0;
    QVERIFY(put(file, zipBytes("payload.bin", payload)));
    QCOMPARE(OugaPayloadExtractor::locate(file, &offset, &size),
             OugaPayloadExtractor::Zip::Stored);
    QCOMPARE(offset, quint64(30 + 11));
    QCOMPARE(size, quint64(payload.size()));
    QVERIFY(put(file, zipBytes("payload.bin", payload, 8)));
    QCOMPARE(OugaPayloadExtractor::locate(file, &offset, &size),
             OugaPayloadExtractor::Zip::Other);
    QVector<OugaPayloadEntry> entries;
    bool delta = false;
    QString error;
    QVERIFY(!OugaPackage::payloadManifest(file, &entries, &delta, &error));
    QVERIFY(error.contains("需先解压"));
    QVERIFY(put(file, zipBytes("payload.bin", payload, 0, true)));
    QCOMPARE(OugaPayloadExtractor::locate(file, &offset, &size),
             OugaPayloadExtractor::Zip::Other);
    QVERIFY(put(file, zipBytes("payload.bin", "not a payload")));
    QCOMPARE(OugaPayloadExtractor::locate(file, &offset, &size),
             OugaPayloadExtractor::Zip::Other);
    QVERIFY(put(file, "PK test-only archive"));
    QCOMPARE(OugaPayloadExtractor::locate(file, &offset, &size),
             OugaPayloadExtractor::Zip::Other);
    QVERIFY(put(file, payload));
    QCOMPARE(OugaPayloadExtractor::locate(file, &offset, &size),
             OugaPayloadExtractor::Zip::NotZip);
  }
  void nativePayloadCancel() {
    QByteArray boot, vendor;
    const QString file = dir + "/payload.bin", output = dir + "-cancel";
    QVERIFY(put(file, nativePayloadBytes(&boot, &vendor)));
    OugaPreparation prep;
    QSignalSpy done(&prep, &OugaPreparation::finished),
        ready(&prep, &OugaPreparation::prepared);
    prep.payload(QString(), file, output);
    prep.cancel();
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 20000);
    QVERIFY(!done[0][0].toBool());
    QCOMPARE(ready.count(), 0);
    QVERIFY(QDir(output).entryList({"*.partial"}, QDir::Files).isEmpty());
    QVERIFY(!DeviceOperationLease::owner());
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
  void widgetIndependentTaskbarWindow() {
    FakeRunner runner;
    QWidget launcher;
    launcher.setWindowFlags(Qt::Window | Qt::WindowStaysOnTopHint);
    launcher.show();
    OugaFlashWindow window(&launcher, &runner, dir + "/logs");
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.isWindow());
    QVERIFY(!window.parentWidget());
    QVERIFY(!window.windowFlags().testFlag(Qt::WindowStaysOnTopHint));
    QVERIFY(window.windowFlags().testFlag(Qt::WindowMinimizeButtonHint));
#ifdef Q_OS_WIN
    const HWND handle = reinterpret_cast<HWND>(window.winId());
    QVERIFY(GetWindow(handle, GW_OWNER) == nullptr);
    const LONG_PTR styles = GetWindowLongPtr(handle, GWL_EXSTYLE);
    QVERIFY(!(styles & WS_EX_TOPMOST));
    QVERIFY(!(styles & WS_EX_TOOLWINDOW));
#endif
    window.showMinimized();
    QTRY_VERIFY(window.isMinimized());
#ifdef Q_OS_WIN
    QVERIFY(IsIconic(handle));
    QVERIFY(GetWindow(handle, GW_OWNER) == nullptr);
#endif
    window.showNormal();
    QTRY_VERIFY(!window.isMinimized());
    QVERIFY(window.isVisible());
    QVERIFY(runner.trace.isEmpty());
  }
  void widgetLauncherLifetime() {
    FakeRunner runner;
    auto launcher = new QWidget;
    QPointer<OugaFlashWindow> window =
        new OugaFlashWindow(launcher, &runner, dir + "/logs");
    window->show();
    delete launcher;
    QTRY_VERIFY(window.isNull());
    QVERIFY(runner.trace.isEmpty());
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
    auto progress = window.findChild<QProgressBar *>("FlashProgressBar");
    auto preparation = window.findChild<OugaPreparation *>();
    QVERIFY(progress && preparation);
    QList<int> archiveValues, payloadValues;
    bool extractionReset = false;
    connect(preparation, &OugaPreparation::archiveProgress, &window,
            [&](int percent) {
              archiveValues << percent;
              QCOMPARE(progress->value(), percent);
              QCOMPARE(progress->property("rate").toString(),
                       QString("解压中 %1%").arg(percent));
              if (percent == 42)
                QVERIFY(window.grab().save(dir + "/archive-progress.png"));
            });
    connect(preparation, &OugaPreparation::busyChanged, &window, [&](bool busy) {
      if (busy && progress->property("rate").toString() == "解包中...") {
        QCOMPARE(progress->value(), 0);
        extractionReset = true;
      }
    });
    connect(preparation, &OugaPreparation::payloadProgress, &window, [&](int percent) {
      payloadValues << percent;
      QCOMPARE(progress->value(), percent);
      QCOMPARE(progress->property("rate").toString(), QString("解包中 %1%").arg(percent));
      if (percent == 99)
        QVERIFY(window.grab().save(dir + "/payload-progress.png"));
    });
    window.findChild<QPushButton *>("UnpackPayloadButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 10000);
    QCOMPARE(unexpectedFileDialogs, 0);
    QCOMPARE(archiveValues, (QList<int>{0, 12, 42, 64, 80, 99, 100}));
    QVERIFY(extractionReset);
    QCOMPARE(payloadValues, corruptOutput ? QList<int>({0, 99}) : QList<int>({0, 99, 100}));
    QCOMPARE(progress->value(), corruptOutput ? 99 : 100);
    QCOMPARE(progress->property("rate").toString(), corruptOutput ? QString("已停止") : QString("完成"));
    const QString images = imagesDirectory ? output : output + "/images";
    QVERIFY(QFileInfo::exists(images + "/boot.img"));
    QVERIFY(!QFileInfo::exists(images + "/images"));
    auto table = window.findChild<QTableWidget *>("OugaPartitionTableDataGrid");
    QCOMPARE(table->rowCount(), corruptOutput ? 0 : 1);
    const QString log = window.findChild<QPlainTextEdit *>("OugaFlashLogTextBox")
                            ->toPlainText();
    QVERIFY(log.contains("正在预解压 payload.bin..."));
    QVERIFY(log.contains("正在从 ZIP 解压 payload.bin ("));
    QVERIFY(log.contains("payload.bin 解压完成"));
    QVERIFY(log.contains("开始解包，输出将实时显示在日志窗口中。"));
    QVERIFY(log.contains("发现 1 个分区: boot"));
    QVERIFY(log.contains("开始解包分区..."));
    QVERIFY(!log.contains("解压及路径校验成功"));
    QVERIFY(!log.contains("Path = "));
    QVERIFY(!log.contains("Everything is Ok"));
    QVERIFY(!log.contains("ops/s"));
    QVERIFY(!log.contains(QChar(0x1b)));
    if (corruptOutput) {
      QVERIFY(log.contains("[提取] boot.img... 失败"));
      QVERIFY(!log.contains("[提取] boot.img... OK"));
      QVERIFY(log.contains("SHA-256 不符"));
      QVERIFY(!log.contains("文件已准备"));
      window.findChild<QPushButton *>("StartFlashButton")->click();
    } else {
      QCOMPARE(folder->text(), images);
      QCOMPARE(table->item(0, 1)->text(), QString("boot"));
      QCOMPARE(QFileInfo(table->item(0, 3)->text()).canonicalFilePath(),
               QFileInfo(images + "/boot.img").canonicalFilePath());
      QCOMPARE(read(images + "/boot.img"), QByteArray(512, 'p'));
      QVERIFY(log.contains("[提取] boot.img... OK"));
      QVERIFY(log.contains("Payload解包成功！"));
      QVERIFY(log.contains("解包完成，文件保存在: " + images));
      QVERIFY(log.contains("已加载 1 个镜像文件"));
      QVERIFY(!log.contains("已停止/失败"));
      QVERIFY(window.grab().save(dir + "/payload-loaded.png"));
    }
    QCOMPARE(read(source), sourceBytes);
    QVERIFY(runner.trace.isEmpty());
    QVERIFY(!DeviceOperationLease::owner());
    QVERIFY(window.close());
  }
  void widgetStoredZipPayloadInPlace() {
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
    // Neither tool may run: a configured path that does not exist proves it.
    settings.setValue("Ouga/7z", dir + "/absent/7z.exe");
    settings.setValue("Ouga/payload", dir + "/absent/payload.exe");
    QByteArray boot, vendor;
    const QByteArray bytes = zipBytes("payload.bin", nativePayloadBytes(&boot, &vendor));
    const QString source = dir + "/输入/ota.zip", output = dir + "/输出";
    QVERIFY(QDir().mkpath(dir + "/输入"));
    QVERIFY(put(source, bytes));
    QVERIFY(QDir().mkpath(output));
    FakeRunner runner;
    OugaFlashWindow window(nullptr, &runner, dir + "/logs");
    window.show();
    window.findChild<QLineEdit *>("PayloadFilePathTextBox")->setText(source);
    auto folder = window.findChild<QLineEdit *>("FolderPathTextBox");
    folder->setText(output);
    QVERIFY(QMetaObject::invokeMethod(folder, "editingFinished"));
    auto progress = window.findChild<QProgressBar *>("FlashProgressBar");
    window.findChild<QPushButton *>("UnpackPayloadButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 20000);
    const QString log = window.findChild<QPlainTextEdit *>("OugaFlashLogTextBox")
                            ->toPlainText();
    QVERIFY2(log.contains("Payload解包成功！"), log.toUtf8().constData());
    QVERIFY(!log.contains("预解压"));
    QVERIFY(log.contains("发现 2 个分区: boot, vendor"));
    QVERIFY(log.contains("[提取] boot.img... OK"));
    QVERIFY(log.contains("[提取] vendor.img... OK"));
    QCOMPARE(progress->value(), 100);
    QCOMPARE(read(output + "/images/boot.img"), boot);
    QCOMPARE(read(output + "/images/vendor.img"), vendor);
    QVERIFY(QDir(output).entryList({"ouga-payload-source*"}, QDir::Dirs).isEmpty());
    QCOMPARE(window.findChild<QTableWidget *>("OugaPartitionTableDataGrid")->rowCount(), 2);
    QCOMPARE(read(source), bytes);
    QVERIFY(runner.trace.isEmpty());
    QVERIFY(window.close());
  }
  void widgetNativeZstdUnpackResponsive() {
    QSettings settings;
    const QStringList keys = {"Ouga/7z", "Ouga/payload"};
    QMap<QString, QVariant> previous;
    for (const auto &key : keys) {
      previous.insert(key, settings.value(key));
      settings.setValue(key, dir + "/absent/tool.exe");
    }
    const auto restoreSettings = qScopeGuard([&] {
      for (auto i = previous.cbegin(); i != previous.cend(); ++i)
        if (i.value().isValid()) settings.setValue(i.key(), i.value());
        else settings.remove(i.key());
    });
    const QByteArray part = nativeImage(2, 'z'), encoded = zstdCompressedBytes();
    QByteArray image, blob;
    QVector<NativeOp> operations;
    // Enough actual operations to expose a blocking decode loop in the GUI.
    for (int i = 0; i < 8192; ++i) {
      image += part;
      operations << NativeOp{14, quint64(i * 2), 2, encoded};
    }
    const QByteArray manifest = vi(3 << 3) + vi(4096) +
        nativePartition("system", image, operations, &blob);
    const QByteArray bytes = zipBytes("payload.bin", payloadContainer(manifest) + blob);
    const QString source = dir + "/输入 ZSTD/ota 包.zip", output = dir + "/输出";
    QVERIFY(QDir().mkpath(QFileInfo(source).absolutePath()));
    QVERIFY(put(source, bytes));
    QVERIFY(QDir().mkpath(output));
    FakeRunner runner;
    OugaFlashWindow window(nullptr, &runner, dir + "/logs");
    window.show();
    window.findChild<QLineEdit *>("PayloadFilePathTextBox")->setText(source);
    auto folder = window.findChild<QLineEdit *>("FolderPathTextBox");
    folder->setText(output);
    QVERIFY(QMetaObject::invokeMethod(folder, "editingFinished"));
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 20000);
    auto preparation = window.findChild<OugaPreparation *>();
    auto progress = window.findChild<QProgressBar *>("FlashProgressBar");
    QVERIFY(preparation && progress);
    QSignalSpy values(preparation, &OugaPreparation::payloadProgress);
    int unexpectedDialogs = 0, ticksWhileBusy = 0;
    qint64 maxGap = 0;
    QElapsedTimer clock;
    clock.start();
    QTimer heartbeat;
    heartbeat.setInterval(5);
    connect(&heartbeat, &QTimer::timeout, &window, [&] {
      const qint64 gap = clock.restart();
      if (window.isBusy()) {
        ++ticksWhileBusy;
        maxGap = qMax(maxGap, gap);
      }
      for (QWidget *widget : QApplication::topLevelWidgets())
        if (auto dialog = qobject_cast<QFileDialog *>(widget))
          if (dialog->isVisible()) {
            ++unexpectedDialogs;
            dialog->reject();
          }
    });
    bool capturedProgress = false;
    connect(preparation, &OugaPreparation::payloadProgress, &window, [&](int percent) {
      QCOMPARE(progress->value(), percent);
      if (!capturedProgress && percent >= 42 && percent < 100) {
        capturedProgress = true;
        QVERIFY(window.grab().save(dir + "/zstd-unpack-progress.png"));
      }
    });
    heartbeat.start();
    QElapsedTimer elapsed;
    elapsed.start();
    window.findChild<QPushButton *>("UnpackPayloadButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 30000);
    heartbeat.stop();
    const QString log = window.findChild<QPlainTextEdit *>("OugaFlashLogTextBox")
                            ->toPlainText();
    QVERIFY2(log.contains("Payload解包成功！"), qPrintable(log));
    QVERIFY(log.contains("发现 1 个分区: system"));
    QVERIFY(log.contains("[提取] system.img... OK"));
    QVERIFY(!log.contains("预解压"));
    QCOMPARE(unexpectedDialogs, 0);
    QVERIFY2(ticksWhileBusy >= 2, "UI heartbeat did not run during extraction");
    QVERIFY2(maxGap < 1000, qPrintable(QString("UI heartbeat gap: %1 ms").arg(maxGap)));
    QVERIFY(capturedProgress);
    QVERIFY(values.size() >= 3);
    for (int i = 1; i < values.size(); ++i)
      QVERIFY(values[i][0].toInt() > values[i - 1][0].toInt());
    QCOMPARE(progress->value(), 100);
    QCOMPARE(read(output + "/images/system.img"), image);
    QCOMPARE(OugaPackage::digest(output + "/images/system.img", nullptr), sha(image));
    QCOMPARE(read(source), bytes);
    QCOMPARE(window.findChild<QTableWidget *>("OugaPartitionTableDataGrid")->rowCount(), 1);
    QVERIFY(runner.trace.isEmpty());
    QVERIFY(window.grab().save(dir + "/zstd-unpack-complete.png"));
    qInfo("ZSTD GUI fixture: elapsed=%lld ms, busy heartbeats=%d, max gap=%lld ms",
          elapsed.elapsed(), ticksWhileBusy, maxGap);
    QVERIFY(window.close());
  }
  void widgetUnpackPreservesExistingImages_data() {
    QTest::addColumn<bool>("imagesDirectory");
    QTest::addColumn<bool>("zip");
    for (bool imagesDirectory : {false, true})
      for (bool zip : {false, true})
        QTest::newRow(qPrintable(QString("%1-%2")
            .arg(imagesDirectory ? "images" : "parent")
            .arg(zip ? "zip" : "payload"))) << imagesDirectory << zip;
  }
  void widgetUnpackPreservesExistingImages() {
    QFETCH(bool, imagesDirectory);
    QFETCH(bool, zip);
    QByteArray boot, vendor;
    const QByteArray payload = nativePayloadBytes(&boot, &vendor);
    const QByteArray sourceBytes = zip ? zipBytes("payload.bin", payload) : payload;
    const QString sourceDirectory = dir + "/输入 包";
    const QString source = sourceDirectory + (zip ? "/ota.zip" : "/payload.bin");
    const QString output = dir + "/解包 输出", existing = output + "/images";
    QVERIFY(QDir().mkpath(sourceDirectory));
    QVERIFY(QDir().mkpath(existing));
    QVERIFY(put(source, sourceBytes));
    const QByteArray oldImage = nativeImage(1, 's');
    QVERIFY(put(existing + "/system.img", oldImage));
    QVERIFY(put(existing + "/.settings.json", "preserve settings"));
    QVERIFY(put(existing + "/vendor.img.partial", "preserve partial image"));
    const QStringList oldFiles = QDir(existing).entryList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    FakeRunner runner;
    OugaFlashWindow window(nullptr, &runner, dir + "/logs");
    window.show();
    window.findChild<QLineEdit *>("PayloadFilePathTextBox")->setText(source);
    auto folder = window.findChild<QLineEdit *>("FolderPathTextBox");
    folder->setText(imagesDirectory ? existing : output);
    QVERIFY(QMetaObject::invokeMethod(folder, "editingFinished"));
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 20000);
    QString previous = existing;
    // A second click must choose a sibling, not overwrite or nest in the result.
    for (int attempt = 0; attempt < 2; ++attempt) {
      window.findChild<QPushButton *>("UnpackPayloadButton")->click();
      QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 20000);
      const QString log = window.findChild<QPlainTextEdit *>("OugaFlashLogTextBox")
                              ->toPlainText();
      QCOMPARE(log.count("Payload解包成功！"), attempt + 1);
      const QString actual = QDir::cleanPath(folder->text());
      QVERIFY2(actual != previous, qPrintable(actual));
      QCOMPARE(QFileInfo(actual).absolutePath(), QDir(output).absolutePath());
      QVERIFY2(QFileInfo(actual).fileName().startsWith("images-"), qPrintable(actual));
      QCOMPARE(read(actual + "/boot.img"), boot);
      QCOMPARE(read(actual + "/vendor.img"), vendor);
      QCOMPARE(read(existing + "/system.img"), oldImage);
      QCOMPARE(read(existing + "/.settings.json"), QByteArray("preserve settings"));
      QCOMPARE(read(existing + "/vendor.img.partial"), QByteArray("preserve partial image"));
      QCOMPARE(QDir(existing).entryList(
          QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System), oldFiles);
      QCOMPARE(window.findChild<QProgressBar *>("FlashProgressBar")->value(), 100);
      QCOMPARE(window.findChild<QTableWidget *>("OugaPartitionTableDataGrid")->rowCount(), 2);
      if (attempt)
        QCOMPARE(read(previous + "/boot.img"), boot);
      previous = actual;
    }
    const QString log = window.findChild<QPlainTextEdit *>("OugaFlashLogTextBox")
                            ->toPlainText();
    QVERIFY(log.contains("输出目录已有文件"));
    QCOMPARE(read(source), sourceBytes);
    QVERIFY(runner.trace.isEmpty());
    QVERIFY(window.close());
  }
  void widgetUnpackInsideSourceDirectory_data() {
    QTest::addColumn<bool>("zip");
    QTest::addColumn<int>("existing");
    QTest::addColumn<bool>("imagesDirectory");
    for (bool zip : {false, true})
      for (int existing : {0, 1, 2})
        for (bool imagesDirectory : {false, true})
          QTest::newRow(qPrintable(QString("%1-%2-%3")
              .arg(zip ? "zip" : "payload").arg(existing)
              .arg(imagesDirectory ? "images" : "parent")))
              << zip << existing << imagesDirectory;
  }
  void widgetUnpackInsideSourceDirectory() {
    QFETCH(bool, zip);
    QFETCH(int, existing);
    QFETCH(bool, imagesDirectory);
    QByteArray boot, vendor;
    const QByteArray payload = nativePayloadBytes(&boot, &vendor);
    const QByteArray bytes = zip ? zipBytes("payload.bin", payload) : payload;
    const QString input = dir + "/中文 包目录";
    const QString source = input + (zip ? "/全量 包.zip" : "/payload.bin");
    const QString preferred = input + "/images";
    QVERIFY(QDir().mkpath(input));
    QVERIFY(put(source, bytes));
    if (existing || imagesDirectory)
      QVERIFY(QDir().mkpath(preferred));
    if (existing == 2) {
      QVERIFY(put(preferred + "/system.img", "existing image"));
      QVERIFY(put(preferred + "/vendor.img.partial", "existing partial"));
      QVERIFY(put(preferred + "/.settings.json", "existing settings"));
    }
    FakeRunner runner;
    OugaFlashWindow window(nullptr, &runner, dir + "/logs");
    window.show();
    window.findChild<QLineEdit *>("PayloadFilePathTextBox")->setText(source);
    auto folder = window.findChild<QLineEdit *>("FolderPathTextBox");
    folder->setText(imagesDirectory ? preferred : input);
    QVERIFY(QMetaObject::invokeMethod(folder, "editingFinished"));
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 20000);
    QString previous;
    for (int attempt = 0; attempt < 2; ++attempt) {
      window.findChild<QPushButton *>("UnpackPayloadButton")->click();
      QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 20000);
      const QString log = window.findChild<QPlainTextEdit *>("OugaFlashLogTextBox")
                              ->toPlainText();
      QVERIFY2(log.contains("Payload解包成功！"), qPrintable(log));
      const QString actual = folder->text();
      QCOMPARE(QFileInfo(actual).absolutePath(), input);
      if (!attempt && existing != 2)
        QCOMPARE(actual, preferred);
      else
        QVERIFY(QFileInfo(actual).fileName().startsWith("images-"));
      QVERIFY(actual != previous);
      QCOMPARE(read(actual + "/boot.img"), boot);
      QCOMPARE(read(actual + "/vendor.img"), vendor);
      QCOMPARE(window.findChild<QProgressBar *>("FlashProgressBar")->value(), 100);
      QCOMPARE(window.findChild<QTableWidget *>("OugaPartitionTableDataGrid")->rowCount(), 2);
      if (attempt) {
        QCOMPARE(read(previous + "/boot.img"), boot);
        QCOMPARE(read(previous + "/vendor.img"), vendor);
      }
      previous = actual;
    }
    QCOMPARE(read(source), bytes);
    if (existing == 2) {
      QCOMPARE(read(preferred + "/system.img"), QByteArray("existing image"));
      QCOMPARE(read(preferred + "/vendor.img.partial"), QByteArray("existing partial"));
      QCOMPARE(read(preferred + "/.settings.json"), QByteArray("existing settings"));
      QVERIFY(!QFileInfo::exists(preferred + "/boot.img"));
    }
    QVERIFY(runner.trace.isEmpty());
    QVERIFY(window.close());
  }
  void widgetUnpackKeepsUnsafeOutputBlocked() {
    QByteArray boot, vendor;
    const QByteArray bytes = nativePayloadBytes(&boot, &vendor);
    const QString input = dir + "/input", source = input + "/payload.bin";
    const QString output = input;
    QVERIFY(QDir().mkpath(input));
    QVERIFY(put(source, bytes));
    const QString preferred = output + "/images";
    QVERIFY(put(preferred, "not a directory"));
    FakeRunner runner;
    OugaFlashWindow window(nullptr, &runner, dir + "/logs");
    window.show();
    window.findChild<QLineEdit *>("PayloadFilePathTextBox")->setText(source);
    auto folder = window.findChild<QLineEdit *>("FolderPathTextBox");
    folder->setText(output);
    QVERIFY(QMetaObject::invokeMethod(folder, "editingFinished"));
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 20000);
    window.findChild<QPushButton *>("UnpackPayloadButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 20000);
    const QString log = window.findChild<QPlainTextEdit *>("OugaFlashLogTextBox")
                            ->toPlainText();
    QVERIFY2(log.contains("输出") && log.contains("已停止/失败"), qPrintable(log));
    QVERIFY(!log.contains("Payload解包成功！"));
    QCOMPARE(folder->text(), output);
    QCOMPARE(read(source), bytes);
    QCOMPARE(read(preferred), QByteArray("not a directory"));
    QVERIFY(QDir(output).entryList({"images-*"}, QDir::Dirs).isEmpty());
    QVERIFY(runner.trace.isEmpty());
    QVERIFY(window.close());
  }
  void preparationOutputGuards_data() {
    QTest::addColumn<int>("kind");
    QTest::newRow("payload-output-is-source-parent") << 0;
    QTest::newRow("payload-output-is-source-file") << 1;
    QTest::newRow("payload-output-occupied") << 2;
    QTest::newRow("super-output-inside-input") << 3;
    QTest::newRow("super-output-is-input") << 4;
  }
  void preparationOutputGuards() {
    QFETCH(int, kind);
    const QString input = dir + "/input";
    const QString source = input + "/payload.bin";
    QByteArray boot, vendor;
    const QByteArray bytes = nativePayloadBytes(&boot, &vendor);
    QVERIFY(QDir().mkpath(input));
    QVERIFY(put(source, bytes));
    const QString output = kind == 0 || kind == 4 ? input
                         : kind == 1 ? source : input + "/images";
    if (kind == 2) {
      QVERIFY(QDir().mkpath(output));
      QVERIFY(put(output + "/.keep", "keep"));
    }
    OugaPreparation prep;
    QSignalSpy done(&prep, &OugaPreparation::finished),
        ready(&prep, &OugaPreparation::prepared);
    if (kind >= 3)
      prep.makeSuper(QString(), input, output);
    else
      prep.payload(QString(), source, output);
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 10000);
    QVERIFY(!done[0][0].toBool());
    QVERIFY2(done[0][1].toString().contains("输出"),
             qPrintable(done[0][1].toString()));
    QCOMPARE(ready.count(), 0);
    QCOMPARE(read(source), bytes);
    if (kind == 2)
      QCOMPARE(read(output + "/.keep"), QByteArray("keep"));
    else if (kind == 3)
      QVERIFY(!QFileInfo::exists(output));
    QVERIFY(!QFileInfo::exists(output + "/boot.img"));
  }
  void widgetPayloadInspectionCanBeStopped_data() {
    QTest::addColumn<bool>("manifestStage");
    QTest::newRow("metadata-read") << false;
    QTest::newRow("manifest-read") << true;
  }
  void widgetPayloadInspectionCanBeStopped() {
    QFETCH(bool, manifestStage);
    QByteArray boot, vendor;
    const QString source = dir + "/payload.bin", output = dir + "/output";
    QVERIFY(put(source, nativePayloadBytes(&boot, &vendor)));
    QVERIFY(QDir().mkpath(output));
    FakeRunner runner;
    OugaFlashWindow window(nullptr, &runner, dir + "/logs");
    window.show();
    auto preparation = window.findChild<OugaPreparation *>();
    QSignalSpy ready(preparation, &OugaPreparation::prepared);
    window.findChild<QLineEdit *>("PayloadFilePathTextBox")->setText(source);
    auto folder = window.findChild<QLineEdit *>("FolderPathTextBox");
    folder->setText(output);
    QVERIFY(QMetaObject::invokeMethod(folder, "editingFinished"));
    bool stopScheduled = false, stopIssued = false;
    auto stopInspection = [&] {
      QVERIFY(window.isBusy());
      QVERIFY(!preparation->busy());
      window.findChild<QPushButton *>("OugaFlashStopPanel")->click();
      QVERIFY(window.isBusy()); // keep the worker's owner alive until completion
      stopIssued = true;
    };
    if (manifestStage) {
      auto log = window.findChild<QPlainTextEdit *>("OugaFlashLogTextBox");
      connect(log, &QPlainTextEdit::textChanged, &window, [&, log] {
        if (!stopScheduled && log->toPlainText().contains("输出目录:")) {
          stopScheduled = true;
          // This runs after runPayload has launched manifest inspection but
          // before its completion callback. Preserve coverage of that stage.
          QTimer::singleShot(0, &window, stopInspection);
        }
      });
    }
    window.findChild<QPushButton *>("UnpackPayloadButton")->click();
    QVERIFY(window.isBusy());
    QVERIFY(!preparation->busy());
    if (!manifestStage)
      stopInspection();
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 20000);
    QVERIFY(stopIssued);
    QCOMPARE(ready.count(), 0);
    QVERIFY(!QFileInfo::exists(output + "/images/boot.img"));
    QVERIFY(!QFileInfo::exists(output + "/images/vendor.img"));
    QVERIFY(runner.trace.isEmpty());
    QVERIFY(window.close());
  }
  void widgetPayloadPartitionLog() {
    FakeRunner runner;
    OugaFlashWindow window(nullptr, &runner, dir + "/logs");
    auto preparation = window.findChild<OugaPreparation *>();
    auto log = window.findChild<QPlainTextEdit *>("OugaFlashLogTextBox");
    QVERIFY(preparation && log);
    preparation->payloadPartitionStarted("boot");
    preparation->payloadPartitionStarted("vendor");
    preparation->payloadPartitionStarted("boot"); // repeated terminal frames
    preparation->payloadPartitionFinished("boot", true);
    preparation->payloadPartitionStarted("system");
    preparation->payloadPartitionFinished("system", true);
    preparation->payloadPartitionFinished("vendor", false);
    const QString text = log->toPlainText();
    QCOMPARE(text.count("[提取] boot.img... OK"), 1);
    QCOMPARE(text.count("[提取] vendor.img... 失败"), 1);
    QCOMPARE(text.count("[提取] system.img... OK"), 1);
    QVERIFY(!text.contains("vendor.img... OK"));
    QVERIFY(!text.contains("OK OK"));
    QVERIFY(runner.trace.isEmpty());
  }
  void widgetDropReferenceImage_data() {
    QTest::addColumn<QString>("extension");
    QTest::addColumn<bool>("afterSales");
    QTest::newRow("full-raw") << "raw" << false;
    QTest::newRow("full-iso") << "ISO" << false;
    QTest::newRow("after-sales-iso") << "iso" << true;
    QTest::newRow("full-sparse") << "SPARSE" << false;
    QTest::newRow("after-sales-raw") << "RAW" << true;
    QTest::newRow("after-sales-sparse") << "sparse" << true;
  }
  void widgetDropReferenceImage() {
    QFETCH(QString, extension);
    QFETCH(bool, afterSales);
    FakeRunner runner;
    OugaFlashWindow window(nullptr, &runner, dir + "/logs");
    window.show();
    window.findChild<QCheckBox *>("AfterSalesPackageModeCheckBox")->setChecked(afterSales);
    const QString source = dir + "/boot." + extension;
    QVERIFY(put(source, extension.compare("sparse", Qt::CaseInsensitive) == 0
                            ? ::sparse() : QByteArray(512, 'b')));
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(source)});
    QDragEnterEvent drag(QPoint(30, 120), Qt::CopyAction, &mime, Qt::LeftButton,
                         Qt::NoModifier);
    QApplication::sendEvent(&window, &drag);
    QVERIFY(drag.isAccepted());
    QDropEvent drop(QPointF(30, 120), Qt::CopyAction, &mime, Qt::LeftButton,
                    Qt::NoModifier);
    QApplication::sendEvent(&window, &drop);
    QVERIFY(drop.isAccepted());
    QTRY_VERIFY(!window.isBusy());
    auto table = window.findChild<QTableWidget *>("OugaPartitionTableDataGrid");
    QCOMPARE(table->rowCount(), 1);
    QCOMPARE(table->item(0, 1)->text(), "boot");
    QVERIFY(runner.trace.isEmpty());
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
  void archiveProgress_data() {
    QTest::addColumn<QString>("outcome");
    QTest::addColumn<bool>("scanImages");
    QTest::addColumn<bool>("success");
    QTest::newRow("fragmented-cr-backspace-two-streams")
        << "success" << false << true;
    QTest::newRow("zero-exit-with-failed-output")
        << "failed-marker" << false << false;
    QTest::newRow("nonzero-exit") << "nonzero" << false << false;
    QTest::newRow("validation-fails-after-tool-100")
        << "scan-failure" << true << false;
    QTest::newRow("cancel-midway") << "cancel" << false << false;
    QTest::newRow("cancel-before-process-start")
        << "cancel-start" << false << false;
    QTest::newRow("failed-to-start") << "start-failure" << false << false;
    QTest::newRow("no-progress-output") << "silent" << false << true;
    QTest::newRow("listing-fails") << "list-failure" << false << false;
  }
  void archiveProgress() {
    QFETCH(QString, outcome);
    QFETCH(bool, scanImages);
    QFETCH(bool, success);
    const QString source = dir + "/input/progress.mock-archive";
    const QString output = dir + "/output";
    QVERIFY(QDir().mkpath(dir + "/input"));
    QVERIFY(put(source, "archive-progress-" + outcome.toUtf8()));
    OugaPreparation prep;
    QSignalSpy progress(&prep, &OugaPreparation::archiveProgress);
    QSignalSpy extracted(&prep, &OugaPreparation::archiveExtracted);
    QSignalSpy done(&prep, &OugaPreparation::finished);
    connect(&prep, &OugaPreparation::archiveProgress, &prep, [&](int percent) {
      if ((outcome == "cancel" && percent == 42) ||
          (outcome == "cancel-start" && percent == 0))
        prep.cancel();
      if (percent == 100) {
        QVERIFY(QFileInfo::exists(output + "/payload.bin"));
        QCOMPARE(read(output + "/payload.bin"), payloadBytes(false, 9));
        QCOMPARE(done.count(), 0);
      }
    });
    const QString tool = outcome == "start-failure"
        ? dir + "/missing-7z.exe" : QCoreApplication::applicationFilePath();
    prep.extractArchive(tool, source, output, scanImages);
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 5000);
    QCOMPARE(done[0][0].toBool(), success);
    QCOMPARE(extracted.count(), success ? 1 : 0);
    QList<int> values;
    for (const auto &args : progress)
      values << args[0].toInt();
    QList<int> expected;
    if (outcome == "cancel-start")
      expected = {0};
    else if (outcome == "cancel")
      expected = {0, 12, 42};
    else if (outcome == "silent")
      expected = {0, 100};
    else if (outcome != "start-failure" && outcome != "list-failure")
      expected = {0, 12, 42, 64, 80, 99};
    if (success && outcome != "silent")
      expected << 100;
    QCOMPARE(values, expected);
    if (outcome == "failed-marker")
      QVERIFY(done[0][1].toString().contains("FAILED"));
    QVERIFY(!prep.busy());
  }
  void payloadProgress_data() {
    QTest::addColumn<QString>("outcome");
    QTest::addColumn<bool>("success");
    for (const auto &name : {"success", "silent", "corrupt", "cancel", "cancel-start",
                             "failed-marker", "nonzero", "start-failure"})
      QTest::newRow(name) << QString(name) << (QString(name) == "success" || QString(name) == "silent");
  }
  void payloadProgress() {
    QFETCH(QString, outcome);
    QFETCH(bool, success);
    QVERIFY(QDir().mkpath(dir + "/input"));
    const QString source = dir + "/input/payload.bin";
    const QString output = dir + "/counter-" + outcome;
    QVERIFY(put(source, payloadContainer(payloadCounterPartition("boot", 8) +
                                        payloadCounterPartition("vendor", 2))));
    OugaPreparation prep;
    QSignalSpy progress(&prep, &OugaPreparation::payloadProgress);
    QSignalSpy started(&prep, &OugaPreparation::payloadPartitionStarted);
    QSignalSpy rows(&prep, &OugaPreparation::payloadPartitionFinished);
    QSignalSpy prepared(&prep, &OugaPreparation::prepared);
    QSignalSpy done(&prep, &OugaPreparation::finished);
    QSignalSpy logs(&prep, &OugaPreparation::log);
    connect(&prep, &OugaPreparation::payloadProgress, &prep, [&](int percent) {
      if (done.isEmpty() && ((outcome == "cancel" && percent == 30) ||
          (outcome == "cancel-start" && percent == 0))) prep.cancel();
      if (percent == 100 && done.isEmpty()) {
        QCOMPARE(rows.count(), 2);
        QCOMPARE(done.count(), 0);
        QCOMPARE(read(output + "/boot.img"), QByteArray(512, 'p'));
      }
    });
    prep.payload(outcome == "start-failure" ? dir + "/absent.exe"
                                            : QCoreApplication::applicationFilePath(), source, output);
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 10000);
    QCOMPARE(done[0][0].toBool(), success);
    QCOMPARE(prepared.count(), success ? 1 : 0);
    QList<int> values;
    for (const auto &args : progress) values << args[0].toInt();
    if (outcome == "cancel-start" || outcome == "start-failure")
      QCOMPARE(values, QList<int>{0});
    else if (outcome == "silent") QCOMPARE(values, (QList<int>{0,100}));
    else if (outcome == "cancel") QCOMPARE(values, (QList<int>{0,20,30}));
    else {
      QList<int> expected{0,20,30,50,60,99};
      if (success) expected << 100;
      QCOMPARE(values, expected);
    }
    if (success) {
      QCOMPARE(started.count(), 2);
      QCOMPARE(rows.count(), 2);
      for (const auto &row : rows) QVERIFY(row[1].toBool());
    } else {
      for (const auto &row : rows) QVERIFY(!row[1].toBool());
      QVERIFY(!values.contains(100));
    }
    if (outcome == "failed-marker") QVERIFY(done[0][1].toString().contains("FAILED"));
    if (outcome == "corrupt") QVERIFY(done[0][1].toString().contains("SHA-256"));
    for (const auto &row : logs) {
      QVERIFY(!row[0].toString().contains(QChar(0x1b)));
      QVERIFY(!row[0].toString().contains("ops/s"));
    }
    QVERIFY(!prep.busy());
    // A failed or cancelled terminal must release handles and reset all counters.
    progress.clear();
    prep.payload(QCoreApplication::applicationFilePath(), source, dir + "/retry");
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 2, 10000);
    QVERIFY(done[1][0].toBool());
    QCOMPARE(progress.first()[0].toInt(), 0);
    QCOMPARE(progress.last()[0].toInt(), 100);
  }
  void archiveProgressResetsBetweenRuns() {
    QVERIFY(QDir().mkpath(dir + "/input"));
    const QString source = dir + "/input/reuse.mock-archive";
    OugaPreparation prep;
    QSignalSpy progress(&prep, &OugaPreparation::archiveProgress);
    QSignalSpy done(&prep, &OugaPreparation::finished);
    for (int i = 0; i < 2; ++i) {
      QVERIFY(put(source, i == 0 ? "archive-progress-failed-marker"
                                : "archive-progress-success"));
      progress.clear();
      prep.extractArchive(QCoreApplication::applicationFilePath(), source,
                          dir + "/output-" + QString::number(i), false);
      QTRY_COMPARE_WITH_TIMEOUT(done.count(), i + 1, 5000);
      QCOMPARE(done[i][0].toBool(), i == 1);
      QList<int> values;
      for (const auto &args : progress)
        values << args[0].toInt();
      QList<int> expected{0, 12, 42, 64, 80, 99};
      if (i == 1)
        expected << 100;
      QCOMPARE(values, expected);
    }
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
      const QByteArray scenario = read(QString::fromLocal8Bit(argv[argc - 1]));
      if (scenario == "archive-progress-list-failure") {
        std::fprintf(stderr, "FAILED (injected listing error)\n");
        return 0;
      }
      // Percent-looking listing text must never become extraction progress.
      std::printf("87%% \nPath = payload.bin\nSize = 100\nAttributes = A\n\n");
      return 0;
    }
    if (command == "x" &&
        QString::fromLocal8Bit(argv[argc - 1]).endsWith(".mock-archive")) {
      QString output;
      bool progressOutput = false, ordinaryOutput = false;
      for (int i = 2; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg.startsWith("-o"))
          output = arg.mid(2);
        progressOutput |= arg == "-bsp1";
        ordinaryOutput |= arg == "-bso2";
      }
      if (output.isEmpty() || !progressOutput || !ordinaryOutput)
        return 2;
      const QByteArray scenario = read(QString::fromLocal8Bit(argv[argc - 1]));
      const auto writeChunk = [](FILE *stream, const QByteArray &bytes) {
        std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stream);
        std::fflush(stream);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      };
      if (scenario != "archive-progress-silent") {
        writeChunk(stdout, "\r  0%\b\b\b 1");
        writeChunk(stdout, "2");
        writeChunk(stdout, "% ");
        writeChunk(stdout, "\r4");
        writeChunk(stderr, "8% \n");
        writeChunk(stdout, "2%\b");
        writeChunk(stderr, "\r6");
        writeChunk(stdout, "1% \n");
        writeChunk(stderr, "4% \r");
        // Reject out-of-range, signed, decimal, filenames and embedded tokens.
        writeChunk(stdout, "101% \r1234% \r-9% \r1.5% \r41% \r42% "
                           "\r98%name\nprefix 97%\nfile96%.img\n");
        writeChunk(stdout, "filename " + QByteArray(8192, 'x') + "95% \n");
        writeChunk(stdout, "\r 80% \b\b\b\b\b 100%");
      }
      if (scenario == "archive-progress-failed-marker") {
        std::fprintf(stderr, "FAILED (injected extraction error)\n");
        return 0;
      }
      if (scenario == "archive-progress-nonzero") {
        std::fprintf(stderr, "Injected extraction error\n");
        return 9;
      }
      // Modern full Ouga packages can carry minor_version=9 without source ops.
      if (!put(output + "/payload.bin", payloadBytes(false, 9)))
        return 3;
      std::fprintf(stderr, "Everything is Ok\n");
      return 0;
    }
    if (command == "--out" && argc >= 6 &&
        QString::fromLocal8Bit(argv[argc - 1]).endsWith("payload.bin")) {
      // Assert the production invocation uses bounded CPU parallelism.
      if (std::strcmp(argv[3], "--workers") != 0 ||
          QString::fromLocal8Bit(argv[4]).toInt() != qBound(2, QThread::idealThreadCount(), 8))
        return 11;
      const QByteArray excluded = payloadContainer(payloadPartition("boot", 0) + payloadPartition("misc", 0));
      if (read(QString::fromLocal8Bit(argv[argc - 1])) == excluded) {
        const QString output = QString::fromLocal8Bit(argv[2]);
        return put(output + "/boot.img", QByteArray(512, 'p')) &&
               put(output + "/misc.img", QByteArray(512, output.endsWith("corrupt") ? 'x' : 'p')) ? 0 : 12;
      }
      // Only this executable's fixture format is accepted; no real tools.
      const QString source = QString::fromLocal8Bit(argv[argc - 1]);
      const QByteArray bytes = read(source);
      if (bytes == payloadContainer(payloadCounterPartition("boot", 8) +
                                    payloadCounterPartition("vendor", 2))) {
        const QString out = QDir::fromNativeSeparators(QString::fromLocal8Bit(argv[2]));
        const auto chunk = [](const char *s) {
          std::fputs(s, stdout); std::fflush(stdout);
          std::this_thread::sleep_for(std::chrono::milliseconds(60));
        };
        if (!out.endsWith("counter-silent")) {
          chunk("\x1b]0;fake boot 99%|#| 8/8 [title]\a\r");
          chunk("boot 25%|##| 2/"); chunk("8 [00:00 ops/s]\r\n");
          chunk("\x1b[35mvendor 50%|##| 1/2 [00:00 ops/s]\x1b[0m\r\n");
          chunk("unknown 100%|#| 1/1 [ignored]\r\n");
          chunk("boot 99%|#| 99/8 [ignored]\r\n");
          chunk("boot 99%|#| 8/9 [ignored]\r\n");
          chunk("boot 0%|#| 0/8 [ignored]\r\n");
          chunk("boot 50%|##| 4/8 [00:00 ops/s]\r\n");
          chunk("vendor 100%|##| 2/2 [00:00 ops/s]\r\n");
          chunk("boot 100%|##| 8/8 [00:00 ops/s]\r\n");
        }
        if (out.endsWith("counter-failed-marker")) { chunk("FAILED (injected)\n"); return 0; }
        if (out.endsWith("counter-nonzero")) return 9;
        if (!put(out + "/boot.img", QByteArray(512, out.endsWith("counter-corrupt") ? 'x' : 'p')) ||
            !put(out + "/vendor.img", QByteArray(512, 'p'))) return 8;
        return 0;
      }
      const bool sourceOperation = bytes == payloadBytes(true, 9);
      if (bytes != payloadBytes() && bytes != payloadBytes(false, 9) &&
          bytes != payloadBytes(false, 9, true) && !sourceOperation &&
          bytes != mixedPayloadBytes())
        return 5;
      bool diff = false;
      for (int i = 2; i < argc - 1; ++i)
        diff |= std::strcmp(argv[i], "--diff") == 0;
      if (diff != sourceOperation)
        return 7;
      const QString output = QDir::fromNativeSeparators(
          QString::fromLocal8Bit(argv[2]));
      const bool corrupt = output.contains("/corrupt-output/");
      if (!put(output + "/boot.img", QByteArray(512, corrupt ? 'x' : 'p')))
        return 6;
      std::printf("boot 100%%|########| 1/1 [00:00 ops/s]\r\n");
      std::fflush(stdout);
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
