#include "ougaflashtypes.h"
#include <QRegularExpression>
namespace Ouga {
QString baseName(const QString &name) {
  QString n = name.trimmed().toLower();
  if (n.endsWith(".img") || n.endsWith(".bin"))
    n.chop(4);
  if (n.endsWith("_a") || n.endsWith("_b"))
    n.chop(2);
  return n;
}
bool safeName(const QString &name) {
  return QRegularExpression("^[a-zA-Z0-9][a-zA-Z0-9_-]{0,63}$")
      .match(name)
      .hasMatch();
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
  Device d;
  d.serial = serial;
  for (QString line : out.split('\n')) {
    line = line.trimmed();
    line.remove(QRegularExpression("^\\((?:bootloader|fastboot)\\)\\s*"));
    auto m = QRegularExpression("^(partition-(?:size|type)|is-logical|has-slot)"
                                ":([^:\\s]+):\\s*(.*)$")
                 .match(line);
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
      auto v = QRegularExpression("^([a-zA-Z0-9_-]+):\\s*(\\S.*)$").match(line);
      if (v.hasMatch())
        d.variables[v.captured(1).toLower()] = v.captured(2).trimmed();
    }
  }
  d.product = d.variables.value("product");
  d.slot = d.variables.value("current-slot").trimmed().toLower();
  if (d.slot.startsWith('_'))
    d.slot.remove(0, 1);
  QString mode = d.variables.value("is-userspace").toLower(),
          unlock = d.variables.value("unlocked").toLower();
  d.modeKnown = mode == "yes" || mode == "no";
  d.userspace = mode == "yes";
  d.unlockKnown = unlock == "yes" || unlock == "no";
  d.unlocked = unlock == "yes";
  bool lk = false, xbl = false;
  for (const QString &n : d.partitions) {
    lk |= baseName(n) == "lk";
    xbl |= baseName(n) == "xbl";
  }
  if (lk != xbl)
    d.platform = lk ? Platform::MediaTek : Platform::Qualcomm;
  return d;
}
bool commandSucceeded(int code, bool normal, const QString &out) {
  return normal && code == 0 &&
         !QRegularExpression("\\b(?:FAILED|FAILURE|ERROR)\\b",
                             QRegularExpression::CaseInsensitiveOption)
              .match(out)
              .hasMatch();
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
