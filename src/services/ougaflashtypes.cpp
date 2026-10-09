#include "ougaflashtypes.h"
#include <QRegularExpression>
#include <climits>
namespace Ouga {
QString baseName(const QString &name) {
  QString n = name.trimmed().toLower();
  if (n.endsWith(".sparse"))
    n.chop(7);
  else if (n.endsWith(".img") || n.endsWith(".iso") || n.endsWith(".bin") ||
           n.endsWith(".raw"))
    n.chop(4);
  if (n.endsWith("_a") || n.endsWith("_b"))
    n.chop(2);
  return n;
}
bool safeName(const QString &name) {
  static const QRegularExpression pattern("^[a-zA-Z0-9][a-zA-Z0-9_-]{0,63}$");
  return pattern.match(name).hasMatch();
}
bool logicalName(const QString &name) {
  static const QSet<QString> names = {
      "system",     "odm",         "vendor",      "product",
      "system_ext", "system_dlkm", "vendor_dlkm", "odm_dlkm",
      "my_bigball", "my_carrier",  "my_company",  "my_engineering",
      "my_heytap",  "my_manifest", "my_preload",  "my_product",
      "my_region",  "my_stock"};
  return names.contains(baseName(name));
}
bool blockedImageName(const QString &name) {
  const QString n = baseName(name);
  return n == "frp" || n == "misc" || n == "userdata" || n == "metadata" ||
         n == "super_empty" || n == "payload" || n.contains("gpt") ||
         n.startsWith("prog_");
}
QStringList criticalImages(Platform platform) {
  if (platform != Platform::Qualcomm && platform != Platform::MediaTek)
    return {};
  // Repair and after-sales Bootloader writes use the same reference order.
  QStringList names = {"boot", "dtbo", "init_boot"};
  if (platform == Platform::MediaTek)
    names << "lk";
  else
    names << "modem" << "recovery";
  names << "vbmeta" << "vbmeta_system" << "vbmeta_vendor" << "vendor_boot";
  return names;
}
QStringList criticalExtractionImages(Platform platform) {
  // Reference shortcut selection/log order is not its repair write order.
  if (platform == Platform::Qualcomm)
    return {"boot", "recovery", "dtbo", "modem", "vbmeta", "vendor_boot",
            "init_boot", "vbmeta_system", "vbmeta_vendor"};
  if (platform == Platform::MediaTek)
    return {"boot", "init_boot", "dtbo", "lk", "vbmeta", "vendor_boot",
            "vbmeta_system", "vbmeta_vendor"};
  return {};
}
QString afterSalesSlot(const Device &device, QString *error) {
  if (error)
    error->clear();
  long double sizes[2] = {0, 0};
  for (const QString &name : device.partitions) {
    if (!logicalName(name) ||
        (!name.endsWith("_a") && !name.endsWith("_b")))
      continue;
    sizes[name.endsWith("_a") ? 0 : 1] += device.sizes.value(name);
  }
  return sizes[0] >= sizes[1] ? "a" : "b";
}
bool needsAdditionalImages(FlashMode mode) {
  return mode == FlashMode::BothSlots || mode == FlashMode::Force ||
         mode == FlashMode::OnlyFastbootd;
}
bool startsInFastbootd(FlashMode mode) {
  return mode != FlashMode::RepairFastbootd &&
         mode != FlashMode::AfterSalesBootloader;
}
QString Device::targetPartition(const QString &name,
                                const QString &requestedSlot,
                                QString *error) const {
  if (error)
    error->clear();
  const auto fail = [error](const QString &message) {
    if (error)
      *error = message;
    return QString();
  };
  const QString n = baseName(name);
  if (!safeName(n) || (requestedSlot != "a" && requestedSlot != "b"))
    return fail("分区名称或目标槽位无效：" + name);
  // ResolveOujiaTargetPartitionNameAsync prefers the requested suffixed
  // partition, then falls back to the plain name; has-slot is not a veto.
  const QString target = n + "_" + requestedSlot;
  if (partitions.contains(target))
    return target;
  if (partitions.contains(n))
    return n;
  return fail("实际目标不存在：" + target);
}
bool Device::isLogical(const QString &name) const {
  const QString n = baseName(name);
  return logicalName(n) || logical.contains(n) || logical.contains(n + "_a") ||
         logical.contains(n + "_b");
}
namespace {
QMap<QString, QString> layoutProperties(const Device &device) {
  QMap<QString, QString> properties;
  for (auto it = device.variables.cbegin(); it != device.variables.cend(); ++it)
    if (it.key().startsWith("has-slot:"))
      properties.insert(it.key(), it.value());
  return properties;
}
} // namespace
bool Device::sameLayout(const Device &other) const {
  return partitions == other.partitions && sizes == other.sizes &&
         logical == other.logical &&
         layoutProperties(*this) == layoutProperties(other);
}
bool Device::sameSnapshot(const Device &other) const {
  return serial == other.serial && product == other.product &&
         slot == other.slot && platform == other.platform &&
         userspace == other.userspace && modeKnown == other.modeKnown &&
         unlocked == other.unlocked && unlockKnown == other.unlockKnown &&
         variables.value("serialno") == other.variables.value("serialno") &&
         sameLayout(other);
}
QString sizeText(qint64 b) {
  double n = double(b);
  QStringList u = {"B", "KiB", "MiB", "GiB", "TiB"};
  int i = 0;
  while (n >= 1024 && i < 4) {
    n /= 1024;
    ++i;
  }
  return QString::number(n, 'f', i ? 2 : 0) + " " + u[i];
}
QString platformText(Platform p) {
  return p == Platform::Qualcomm   ? QStringLiteral("高通")
         : p == Platform::MediaTek ? QStringLiteral("联发科")
                                   : QStringLiteral("未知");
}
Device parseDevice(const QString &serial, const QString &out) {
  static const QRegularExpression prefix("^\\((?:bootloader|fastboot)\\)\\s*");
  static const QRegularExpression mapping(
      "^(partition-(?:size|type)|is-logical|has-slot):([^:\\s]+):\\s*(.*)$");
  static const QRegularExpression variable("^([a-zA-Z0-9_-]+):\\s*(\\S.*)$");
  Device d;
  d.serial = serial;
  for (QString line : out.split('\n')) {
    line = line.trimmed();
    line.remove(prefix);
    auto m = mapping.match(line);
    if (m.hasMatch()) {
      QString kind = m.captured(1), n = m.captured(2).toLower(),
              v = m.captured(3).trimmed().toLower();
      d.variables[kind + ":" + n] = v;
      if (kind.startsWith("partition-"))
        d.partitions.insert(n);
      if (kind == "partition-size") {
        bool ok = false;
        quint64 b = v.toULongLong(&ok, 16);
        if (ok)
          d.sizes[n] = b;
      }
      if (kind == "is-logical" && v == "yes")
        d.logical.insert(n);
    } else {
      auto v = variable.match(line);
      if (v.hasMatch())
        d.variables[v.captured(1).toLower()] = v.captured(2).trimmed();
    }
  }
  d.product = d.variables.value("product");
  d.slot = d.variables.value("current-slot").trimmed().toLower();
  while (d.slot.startsWith('_'))
    d.slot.remove(0, 1);
  QString mode = d.variables.value("is-userspace").toLower(),
          unlock = d.variables.value("unlocked").toLower();
  const auto yes = [](const QString &value) { return value == "yes" || value == "true" || value == "1"; };
  const auto no = [](const QString &value) { return value == "no" || value == "false" || value == "0"; };
  d.modeKnown = yes(mode) || no(mode);
  d.userspace = yes(mode);
  d.unlockKnown = yes(unlock) || no(unlock);
  d.unlocked = yes(unlock);
  bool lk = false;
  for (const QString &n : d.partitions) {
    lk |= baseName(n) == "lk";
  }
  if (!out.trimmed().isEmpty())
    d.platform = lk ? Platform::MediaTek : Platform::Qualcomm;
  return d;
}
bool transportFailure(const QString &out) {
  if (out.trimmed().isEmpty()) return true;
  for (const QString text : {"Write to device failed", "no link", "Too many links", "USB Error",
                             "device disconnected", "device not found", "status read failed",
                             "failed to read", "failed to write", "< waiting for"})
    if (out.contains(text, Qt::CaseInsensitive)) return true;
  return false;
}
bool commandSucceeded(int code, bool normal, const QString &out) {
  static const QRegularExpression failure(
      "\\b(?:FAILED|FAILURE|ERROR)\\b",
      QRegularExpression::CaseInsensitiveOption);
  return normal && code == 0 && !failure.match(out).hasMatch();
}
bool partitionMissing(const QString &out) {
  if (QRegularExpression("transport|timeout|timed out|disconnect|write "
                         "failed|read failed|I/O|too many links",
                         QRegularExpression::CaseInsensitiveOption)
          .match(out)
          .hasMatch())
    return false;
  return QRegularExpression(
             "(?:remote[^\\n]*(?:partition[^\\n]*(?:not found|does not "
             "exist|doesn't "
             "exist)|no such partition)|partition does not exist)",
             QRegularExpression::CaseInsensitiveOption)
      .match(out)
      .hasMatch();
}
QString planText(const Plan &p) {
  QString t = p.summary + "\n" + p.warnings.join('\n') + "\n";
  int i = 0;
  for (const Step &s : p.steps) {
    if (s.kind == Step::PostActions) continue;
    t += QString("%1. [%2] %3")
             .arg(++i)
             .arg(s.userspace ? "FastbootD" : "Fastboot", s.title);
    if (!s.image.isEmpty())
      t += "\n    " + s.image + " → " + s.target + "  " + sizeText(s.bytes) +
           "\n    SHA256 " + s.sha256.toHex();
    if (!s.arguments.isEmpty())
      t += "\n    fastboot -s " + p.device.serial + " " + s.arguments.join(' ');
    t += '\n';
  }
  return t;
}
} // namespace Ouga
