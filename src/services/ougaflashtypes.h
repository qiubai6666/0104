#ifndef OUGAFLASHTYPES_H
#define OUGAFLASHTYPES_H
#include <QByteArray>
#include <QMap>
#include <QMetaType>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
namespace Ouga {
enum class PackageMode { Full, AfterSales };
enum class FlashMode {
  Normal,
  BothSlots,
  Force,
  OnlyFastbootd,
  RepairFastbootd,
  AfterSalesBootloader,
  AfterSalesFastbootd
};
enum class Platform { Unknown, Qualcomm, MediaTek };
struct Partition {
  QString name, path;
  qint64 bytes = 0, expandedBytes = 0;
  bool selected = true;
  QByteArray sha256;
  QSet<QString> merged;
};
struct Options {
  PackageMode packageMode = PackageMode::Full;
  FlashMode mode = FlashMode::Normal;
  bool clearData = true, autoReboot = true, validateTable = true;
  QString targetSlot, currentXblConfig;
  Platform packagePlatform = Platform::Unknown;
  bool checkArb = false, arbVerified = false, arbDowngrade = false;
  bool formatToolsReady = false;
  bool afterSuper =
      false; // Internal checkpoint continuation, not a user setting.
};
struct Device {
  QString serial, product, slot;
  Platform platform = Platform::Unknown;
  bool userspace = false, unlocked = false;
  bool modeKnown = false, unlockKnown = false;
  QMap<QString, QString> variables;
  QMap<QString, quint64> sizes;
  QSet<QString> partitions, logical;
};
struct Step {
  enum Kind { Command, ModeSwitch, Wait, Checkpoint };
  Kind kind = Command;
  QString title;
  QStringList arguments;
  QString target, image;
  QByteArray sha256;
  qint64 bytes = 0;
  int waitMs = 0;
  bool userspace = true, allowMissing = false;
};
struct Plan {
  Device device;
  Options options;
  QVector<Partition> images;
  QVector<Step> steps;
  QStringList warnings;
  QString summary;
  qint64 totalBytes = 0;
  int partitionCount = 0, flashCount = 0;
};
QString baseName(const QString &name);
bool safeName(const QString &name);
bool logicalName(const QString &name);
QString sizeText(qint64 bytes);
Device parseDevice(const QString &serial, const QString &output);
bool commandSucceeded(int code, bool normalExit, const QString &output);
bool partitionMissing(const QString &output);
QString planText(const Plan &plan);
QString platformText(Platform platform);
} // namespace Ouga
Q_DECLARE_METATYPE(Ouga::Device)
Q_DECLARE_METATYPE(Ouga::Plan)
Q_DECLARE_METATYPE(QVector<Ouga::Partition>)
#endif
