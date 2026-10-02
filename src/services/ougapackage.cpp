#include "ougapackage.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <QtEndian>
#include <algorithm>
#include <climits>
#include <cmath>
#include <limits>
namespace {
bool fail(QString *error, const QString &text) {
  if (error)
    *error = text;
  return false;
}
quint32 u32(const QByteArray &b, int n) {
  return qFromLittleEndian<quint32>(
      reinterpret_cast<const uchar *>(b.constData() + n));
}
quint64 u64(const QByteArray &b, int n) {
  return qFromLittleEndian<quint64>(
      reinterpret_cast<const uchar *>(b.constData() + n));
}
quint16 u16(const QByteArray &b, int n) {
  return qFromLittleEndian<quint16>(
      reinterpret_cast<const uchar *>(b.constData() + n));
}
quint64 number(const QJsonValue &v, bool *ok) {
  if (!v.isString()) {
    const double n = v.toDouble(-1);
    // JSON numbers above 2^53 cannot preserve byte-accurate capacities; use a
    // decimal string instead.
    *ok = v.isDouble() && std::isfinite(n) && n >= 0 &&
          n <= 9007199254740991.0 && std::floor(n) == n;
    return *ok ? quint64(n) : 0;
  }
  QString s = v.toString();
  s.remove(',');
  s.remove('_');
  return s.toULongLong(ok, s.startsWith("0x") ? 16 : 10);
}
bool forbidden(const QString &n) {
  QString b = Ouga::baseName(n);
  return b == "frp" || b == "misc" || b == "userdata" || b == "metadata" ||
         b == "super_empty" || b == "payload" || b.contains("gpt") ||
         b.startsWith("prog_");
}
struct Field {
  int n = 0, wire = 0;
  quint64 value = 0;
  QByteArray data;
};
bool varint(const QByteArray &b, qsizetype &p, quint64 &v) {
  v = 0;
  for (int i = 0; i < 10 && p < b.size(); ++i) {
    quint8 c = quint8(b[p++]);
    if (i == 9 && c > 1)
      return false;
    v |= quint64(c & 127) << (7 * i);
    if (!(c & 128))
      return true;
  }
  return false;
}
bool proto(const QByteArray &b, QVector<Field> *fields) {
  qsizetype p = 0;
  while (p < b.size()) {
    quint64 tag = 0;
    if (!varint(b, p, tag) || !(tag >> 3) || tag >> 3 > 536870911)
      return false;
    Field f;
    f.n = int(tag >> 3);
    f.wire = int(tag & 7);
    if (f.wire == 0) {
      if (!varint(b, p, f.value))
        return false;
    } else if (f.wire == 2) {
      quint64 n;
      if (!varint(b, p, n) || n > quint64(b.size() - p))
        return false;
      f.data = b.mid(p, qsizetype(n));
      p += qsizetype(n);
    } else if (f.wire == 1 || f.wire == 5) {
      int n = f.wire == 1 ? 8 : 4;
      if (b.size() - p < n)
        return false;
      p += n;
    } else
      return false;
    fields->append(f);
  }
  return true;
}
} // namespace
bool OugaPackage::inside(const QString &root, const QString &file) {
  QString r = QFileInfo(root).canonicalFilePath(),
          f = QFileInfo(file).canonicalFilePath();
  return !r.isEmpty() && !f.isEmpty() &&
         f.startsWith(r + '/', Qt::CaseInsensitive);
}
QByteArray OugaPackage::digest(const QString &file, QString *error) {
  QFile f(file);
  if (!f.open(QIODevice::ReadOnly)) {
    fail(error, "无法读取镜像：" + file);
    return {};
  }
  QCryptographicHash h(QCryptographicHash::Sha256);
  if (!h.addData(&f)) {
    fail(error, "读取镜像中途失败：" + file);
    return {};
  }
  return h.result();
}
qint64 OugaPackage::expandedSize(const QString &file, QString *error) {
  QFile f(file);
  if (!f.open(QIODevice::ReadOnly) || f.size() <= 0) {
    fail(error, "镜像为空或不可读：" + file);
    return -1;
  }
  const QByteArray b = f.read(28);
  if (b.size() < 4 || u32(b, 0) != 0xed26ff3a)
    return f.size();
  if (b.size() != 28 || u16(b, 4) != 1 || u16(b, 8) < 28 || u16(b, 10) < 12 ||
      u32(b, 12) == 0 || u32(b, 12) % 4) {
    fail(error, "稀疏镜像头无效：" + file);
    return -1;
  }
  const quint64 size = quint64(u32(b, 12)) * u32(b, 16);
  if (size == 0 || size > quint64(std::numeric_limits<qint64>::max())) {
    fail(error, "稀疏镜像展开大小溢出：" + file);
    return -1;
  }
  // 验证 chunk 范围，不将损坏的 sparse 镜像送给 fastboot。
  if (!f.seek(u16(b, 8)))
    return -1;
  quint64 blocks = 0;
  const quint32 count = u32(b, 20);
  if (count > 10000000) {
    fail(error, "稀疏镜像 chunk 数量异常");
    return -1;
  }
  for (quint32 i = 0; i < count; ++i) {
    const auto h = f.read(u16(b, 10));
    if (h.size() != u16(b, 10)) {
      fail(error, "稀疏镜像被截断");
      return -1;
    }
    const quint16 type = u16(h, 0);
    const quint32 n = u32(h, 4), total = u32(h, 8);
    quint64 data = 0;
    if (type == 0xcac1)
      data = quint64(n) * u32(b, 12);
    else if (type == 0xcac2 || type == 0xcac4)
      data = 4;
    else if (type != 0xcac3) {
      fail(error, "不支持的 sparse chunk");
      return -1;
    }
    if (quint64(total) != quint64(u16(b, 10)) + data ||
        data > quint64(f.size() - f.pos()) || (type == 0xcac4 && n)) {
      fail(error, "稀疏镜像 chunk 范围无效");
      return -1;
    }
    if (type != 0xcac4)
      blocks += n;
    if (!f.seek(f.pos() + qint64(data)))
      return -1;
  }
  if (blocks != u32(b, 16) || f.pos() != f.size()) {
    fail(error, "稀疏镜像 block 数量不一致");
    return -1;
  }
  return qint64(size);
}

bool OugaPackage::inspect(const QString &name, const QString &file,
                          Ouga::Partition *p, QString *error) {
  if (!Ouga::safeName(name) || forbidden(name))
    return fail(error, "禁止或无法确定用途的分区：" + name);
  p->name = name.toLower();
  p->path = QFileInfo(file).canonicalFilePath();
  p->bytes = QFileInfo(file).size();
  p->expandedBytes = expandedSize(file, error);
  if (p->expandedBytes <= 0)
    return false;
  if (Ouga::baseName(name) == "super" &&
      !superContents(file, &p->merged, error))
    return false;
  p->sha256 = digest(file, error);
  return p->sha256.size() == 32;
}
QVector<Ouga::Partition> OugaPackage::scan(const QString &directory,
                                           QString *error) {
  if (error)
    error->clear();
  QVector<Ouga::Partition> result;
  QDir root(directory);
  if (!root.exists()) {
    fail(error, "目录不存在");
    return {};
  }
  QStringList dirs = {root.absolutePath()};
  for (const QString &n : {"images", "IMAGES", "RADIO"}) {
    QString d = root.filePath(n);
    if (QDir(d).exists() && !dirs.contains(d, Qt::CaseInsensitive))
      dirs << d;
  }
  QMap<QString, QString> files;
  QSet<QString> usedFiles;
  auto add = [&](const QString &name, const QString &file) {
    if (forbidden(name))
      return true;
    if (!Ouga::safeName(name) || !inside(directory, file))
      return fail(error, "映射名称或路径越界：" + name + " / " + file);
    QString n = Ouga::baseName(name), f = QFileInfo(file).canonicalFilePath();
    if (files.contains(n) && files[n] != f)
      return fail(error, "多个来源映射到 " + n + "，请在独立目录消除歧义");
    files[n] = f;
    usedFiles.insert(f);
    return true;
  };
  QSet<QString> xmlLabels;
  for (const QString &d : dirs)
    for (const QFileInfo &xml :
         QDir(d).entryInfoList({"rawprogram*.xml"}, QDir::Files)) {
      if (xml.fileName().contains("BLANK_GPT", Qt::CaseInsensitive) ||
          xml.fileName().contains("WIPE", Qt::CaseInsensitive))
        continue;
      QFile f(xml.absoluteFilePath());
      if (!f.open(QIODevice::ReadOnly)) {
        fail(error, "rawprogram 不可读");
        return {};
      }
      QXmlStreamReader r(&f);
      while (!r.atEnd()) {
        r.readNext();
        if (r.isDTD()) {
          fail(error, "禁止 rawprogram DTD");
          return {};
        }
        if (!r.isStartElement() || r.name() != QStringLiteral("program"))
          continue;
        auto a = r.attributes();
        QString n = a.value("label").toString().toLower(),
                rel = a.value("filename").toString();
        if (rel.isEmpty() || forbidden(n))
          continue;
        bool ok = true;
        QString off = a.value("file_sector_offset").toString();
        if (!off.isEmpty() && off.toULongLong(&ok) != 0) {
          fail(error, "不支持 rawprogram 偏移/多段镜像：" + n);
          return {};
        }
        if (!ok || xmlLabels.contains(n)) {
          fail(error, "重复 rawprogram 分区/多段拼接：" + n);
          return {};
        }
        xmlLabels.insert(n);
        QString file = QDir(d).filePath(rel);
        if (!inside(directory, file) || !QFileInfo(file).isFile()) {
          fail(error, "rawprogram 文件缺失/越界：" + rel);
          return {};
        }
        QString len = a.value("num_partition_sectors").toString(),
                sector = a.value("SECTOR_SIZE_IN_BYTES").toString();
        if (!len.isEmpty()) {
          bool k1 = false, k2 = false;
          quint64 count = len.toULongLong(&k1), unit = sector.toULongLong(&k2);
          qint64 size = expandedSize(file, error);
          if (!k1 || !k2 || !unit || count > quint64(LLONG_MAX) / unit ||
              size <= 0 || count * unit != quint64(size)) {
            fail(error,
                 "rawprogram 范围不等于完整镜像（不支持偏移/填充/分段）：" + n);
            return {};
          }
        }
        if (!add(n, file))
          return {};
      }
      if (r.hasError()) {
        fail(error, "rawprogram XML 损坏");
        return {};
      }
    }
  for (const QString &d : dirs) {
    for (const QFileInfo &f :
         QDir(d).entryInfoList({"*.img"}, QDir::Files, QDir::Name)) {
      if (usedFiles.contains(f.canonicalFilePath()))
        continue;
      if (!add(f.completeBaseName().toLower(), f.absoluteFilePath()))
        return {};
    }
    // SMT's eight named after-sales directories: never arbitrarily pick a
    // candidate.
    for (const QString &n :
         {"my_bigball", "my_carrier", "my_company", "my_heytap", "my_manifest",
          "my_preload", "my_region", "my_stock"}) {
      QDir sub(QDir(d).filePath(n));
      if (!sub.exists())
        continue;
      auto candidates = sub.entryInfoList({"*.img"}, QDir::Files, QDir::Name);
      if (candidates.size() != 1) {
        fail(error, "售后子目录存在零个或多个候选：" + sub.path());
        return {};
      }
      if (!add(n, candidates[0].absoluteFilePath()))
        return {};
    }
  }
  for (auto it = files.cbegin(); it != files.cend(); ++it) {
    Ouga::Partition p;
    if (!inspect(it.key(), it.value(), &p, error))
      return {};
    result << p;
  }
  if (result.isEmpty())
    fail(error,
         "没有可确定用途的镜像；请提取 Payload 或选择 IMAGES/RADIO 的父目录");
  return result;
}
QString OugaPackage::findPayload(const QString &directory) {
  QString p = QDir(directory).filePath("payload.bin");
  return QFileInfo(p).isFile() ? p : QString();
}
QStringList OugaPackage::repairNames(Ouga::Platform p) {
  QStringList n = {"boot",        "init_boot",     "dtbo",         "vbmeta",
                   "vendor_boot", "vbmeta_system", "vbmeta_vendor"};
  if (p == Ouga::Platform::MediaTek)
    n << "lk";
  else
    n << "modem" << "recovery";
  return n;
}
QStringList OugaPackage::parsePayloadList(const QString &out) {
  QStringList names;
  for (const QString &l : out.split('\n')) {
    auto m =
        QRegularExpression("^\\s*([a-zA-Z0-9_]+)\\s*[: ]\\s*[0-9]+").match(l);
    if (m.hasMatch() && !names.contains(m.captured(1)))
      names << m.captured(1);
  }
  return names;
}
bool OugaPackage::readArb(const QString &file, quint32 *index, QString *error) {
  QFile f(file);
  if (!f.open(QIODevice::ReadOnly))
    return fail(error, "无法读取 xbl_config：" + file);
  QByteArray h = f.read(64);
  if (h.size() != 64 ||
      h.left(4) != QByteArray("\x7f"
                              "ELF",
                              4) ||
      uchar(h[4]) != 2 || uchar(h[5]) != 1)
    return fail(error, "ARB 检测仅支持小端 ELF64 xbl_config");
  quint64 off = u64(h, 32);
  quint16 entry = u16(h, 54), count = u16(h, 56);
  if (entry < 56 || !count || off > quint64(f.size()) ||
      quint64(entry) * count > quint64(f.size()) - off)
    return fail(error, "ELF 程序头越界");
  QByteArray segment;
  for (int i = count - 1; i >= 0; --i) {
    f.seek(qint64(off + quint64(i) * entry));
    const auto p = f.read(56);
    if (p.size() != 56)
      return fail(error, "ELF 程序头截断");
    if (u32(p, 0) != 0 || u64(p, 32) == 0)
      continue;
    quint64 pos = u64(p, 8), size = u64(p, 32);
    if (pos > quint64(f.size()) || size > quint64(f.size()) - pos ||
        size > 16 * 1024 * 1024)
      return fail(error, "HASH 段越界或过大");
    f.seek(qint64(pos));
    segment = f.read(qint64(size));
    break;
  }
  if (segment.isEmpty())
    return fail(error, "未找到 ELF HASH 段，不能推测 ARB");
  for (int i = 0; i < 4096 && i + 36 <= segment.size(); i += 4) {
    quint32 v = u32(segment, i), common = u32(segment, i + 4),
            qti = u32(segment, i + 8), oem = u32(segment, i + 12),
            hash = u32(segment, i + 16);
    quint64 pos = quint64(i) + 36 + common + qti;
    if (v < 1 || v > 10 || common > 4096 || oem < 12 || oem > 16384 ||
        hash > 16384 || pos + oem > quint64(segment.size()))
      continue;
    *index = u32(segment, int(pos) + 8);
    return true;
  }
  return fail(error, "未找到有效 OEM Metadata，不能确认 ARB");
}

bool OugaPackage::payloadManifest(const QString &file,
                                  QVector<OugaPayloadEntry> *entries,
                                  bool *delta, QString *error) {
  entries->clear();
  *delta = false;
  QFile f(file);
  if (!f.open(QIODevice::ReadOnly))
    return fail(error, "Payload 不可读");
  QByteArray h = f.read(24);
  if (h.size() != 24 || h.left(4) != "CrAU" ||
      qFromBigEndian<quint64>(
          reinterpret_cast<const uchar *>(h.constData() + 4)) != 2)
    return fail(error, "仅支持有效的 v2 Payload");
  quint64 n = qFromBigEndian<quint64>(
      reinterpret_cast<const uchar *>(h.constData() + 12));
  quint32 sig = qFromBigEndian<quint32>(
      reinterpret_cast<const uchar *>(h.constData() + 20));
  if (!n || n > 64 * 1024 * 1024 || n + sig > quint64(f.size() - 24))
    return fail(error, "Payload manifest 越界");
  QVector<Field> fields;
  if (!proto(f.read(qint64(n)), &fields))
    return fail(error, "Payload protobuf 损坏");
  QSet<QString> names;
  for (const Field &v : fields) {
    if (v.n == 12 && v.value)
      *delta = true;
    if (v.n != 13 || v.wire != 2)
      continue;
    QVector<Field> part;
    if (!proto(v.data, &part))
      return fail(error, "Payload partition 损坏");
    OugaPayloadEntry e;
    for (const Field &p : part) {
      if (p.n == 1)
        e.name = QString::fromUtf8(p.data);
      if (p.n == 6 || p.n == 7) {
        QVector<Field> info;
        if (!proto(p.data, &info))
          return fail(error, "Payload image info 损坏");
        for (const Field &i : info) {
          if (i.n == 1) {
            if (p.n == 6)
              e.oldSize = i.value;
            else
              e.size = i.value;
          }
          if (i.n == 2) {
            if (p.n == 6)
              e.oldHash = i.data;
            else
              e.hash = i.data;
          }
        }
        if (p.n == 6)
          *delta = true;
      }
    }
    if (!Ouga::safeName(e.name) || names.contains(e.name) || !e.size ||
        e.hash.size() != 32)
      return fail(error, "Payload 分区名/大小/校验值无效");
    names.insert(e.name);
    entries->append(e);
  }
  return !entries->isEmpty() || fail(error, "Payload 无分区");
}
bool OugaPackage::safeArchiveListing(const QString &listing, QString *error) {
  QMap<QString, bool> entries;
  for (QString block : listing.split(QRegularExpression("\r?\n\r?\n"))) {
    QString entry;
    bool directory = false;
    for (QString line : block.split('\n')) {
      if (line.endsWith('\r'))
        line.chop(1);
      if (line.startsWith("Symbolic Link =") ||
          line.startsWith("Hard Link =") || line.startsWith("Reparse") ||
          QRegularExpression("^Attributes =.*(?:[lL]|[rR]eparse)")
              .match(line)
              .hasMatch() ||
          QRegularExpression("^Mode = [lbcps]",
                             QRegularExpression::CaseInsensitiveOption)
              .match(line)
              .hasMatch())
        return fail(error, "压缩包含链接/重解析点或特殊文件");
      if (line == "Folder = +" || line.startsWith("Attributes = D") ||
          line.startsWith("Mode = d"))
        directory = true;
      if (!line.startsWith("Path = "))
        continue;
      if (!entry.isEmpty())
        return fail(error, "压缩包目录记录不明确");
      entry = line.mid(7);
      entry.replace('\\', '/');
      const auto parts = entry.split('/');
      if (entry.isEmpty() || entry.startsWith('/') || entry.contains(':') ||
          parts.contains("..") || parts.contains(".") || parts.contains("") ||
          QRegularExpression("[\\x00-\\x1f<>\"|?*]").match(entry).hasMatch())
        return fail(error, "压缩包路径越界或不适用于 Windows：" + entry);
      for (const QString &c : parts) {
        if (c.endsWith('.') || c.endsWith(' ') ||
            QRegularExpression(
                "^(?:CON|PRN|AUX|NUL|COM[0-9¹²³]|LPT[0-9¹²³])(?:\\.|$)",
                QRegularExpression::CaseInsensitiveOption)
                .match(c)
                .hasMatch())
          return fail(error, "压缩包包含 Windows 特殊路径");
      }
    }
    if (entry.isEmpty())
      continue;
    const QString key = entry.toCaseFolded();
    if (entries.contains(key))
      return fail(error, "压缩包包含重复/大小写冲突路径：" + entry);
    entries[key] = directory;
  }
  for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
    const QStringList parts = it.key().split('/');
    QString parent;
    for (int i = 0; i + 1 < parts.size(); ++i) {
      parent += (parent.isEmpty() ? "" : "/") + parts[i];
      if (entries.contains(parent) && !entries[parent])
        return fail(error, "压缩包文件与目录冲突：" + parent);
    }
  }
  return !entries.isEmpty() || fail(error, "无法验证压缩包目录");
}

bool OugaPackage::lpmakeArguments(const QString &directory,
                                  const QString &output, QStringList *args,
                                  QSet<QString> *merged, QString *error) {
  args->clear();
  merged->clear();
  QStringList defs;
  for (const QString &d : {directory, QDir(directory).filePath("META")})
    for (const QFileInfo &f :
         QDir(d).entryInfoList({"super_def*.json"}, QDir::Files))
      defs << f.absoluteFilePath();
  if (defs.size() != 1)
    return fail(error, "请选择仅含一个匹配机型 super_def 的目录（不猜测候选）");
  QFile f(defs[0]);
  if (!f.open(QIODevice::ReadOnly) || f.size() > 4 * 1024 * 1024)
    return fail(error, "super_def 不可读或过大");
  QJsonParseError pe;
  auto doc = QJsonDocument::fromJson(f.readAll(), &pe);
  if (pe.error != QJsonParseError::NoError || !doc.isObject())
    return fail(error, "super_def JSON 无效");
  auto root = doc.object();
  auto devices = root["block_devices"].toArray();
  if (devices.size() != 1)
    return fail(error, "不支持多物理设备 Super");
  auto bd = devices[0].toObject();
  bool ok = false;
  quint64 capacity = number(bd["size"], &ok);
  if (!ok || !capacity || capacity > quint64(LLONG_MAX))
    return fail(error, "Super 容量无效");
  QString dev = bd["name"].toString("super");
  if (dev != "super")
    return fail(error, "不支持非 super 物理设备名");
  auto integer = [&](const char *key, quint64 fallback) {
    if (!bd.contains(key))
      return fallback;
    bool valid = false;
    quint64 value = number(bd[key], &valid);
    return valid ? value : quint64(0);
  };
  quint64 block = integer("block_size", 4096),
          align = integer("alignment", 1048576),
          offset = integer("alignment_offset", 0);
  if (!block || block % 512 || !align || align % block || offset >= align ||
      offset % block || capacity % block)
    return fail(error, "Super 对齐参数无效");
  quint32 metadata = 65536, metadataSlots = 2;
  auto meta = root["super_meta"].toObject();
  QString metaPath = meta["path"].toString();
  if (!metaPath.isEmpty()) {
    QString file = QDir(directory).filePath(metaPath);
    if (!inside(directory, file))
      file = QDir(QFileInfo(defs[0]).absolutePath()).filePath(metaPath);
    if (!inside(directory, file))
      return fail(error, "super_meta 路径缺失/越界");
    QFile g(file);
    if (!g.open(QIODevice::ReadOnly) || !g.seek(4096))
      return fail(error, "super_meta 不可读");
    auto h = g.read(52);
    if (h.size() != 52 || u32(h, 0) != 0x616c4467 || u32(h, 4) != 52)
      return fail(error, "super_meta geometry 无效（暂不接受稀疏 metadata）");
    QByteArray checksum = h.mid(8, 32);
    h.replace(8, 32, QByteArray(32, 0));
    if (QCryptographicHash::hash(h, QCryptographicHash::Sha256) != checksum)
      return fail(error, "super_meta geometry 校验失败");
    metadata = u32(h, 40);
    metadataSlots = u32(h, 44);
    if (u32(h, 48) != block)
      return fail(error, "super_meta 与定义 block_size 冲突");
  }
  if (metadata < 512 || metadata > 1024 * 1024 || metadata % 512 ||
      metadataSlots < 1 || metadataSlots > 3)
    return fail(error, "Super metadata 参数无效");
  *args = {"--metadata-size",
           QString::number(metadata),
           "--metadata-slots",
           QString::number(metadataSlots),
           "--block-size",
           QString::number(block),
           "--super-name",
           dev,
           "--device",
           dev + ":" + QString::number(capacity) + ":" +
               QString::number(align) + ":" + QString::number(offset),
           "--sparse",
           "--output",
           output};
  QMap<QString, quint64> groups, used;
  quint64 groupTotal = 0;
  for (const QJsonValue &v : root["groups"].toArray()) {
    auto g = v.toObject();
    QString n = g["name"].toString();
    quint64 size = number(g["maximum_size"], &ok);
    if (!ok || !size || !Ouga::safeName(n) || groups.contains(n) ||
        size > capacity - groupTotal)
      return fail(error, "Super 分组无效/总容量超限");
    groups[n] = size;
    groupTotal += size;
    *args << "--group" << n + ":" + QString::number(size);
  }
  quint64 total = 12288 + 2 * quint64(metadata) * metadataSlots;
  QSet<QString> names;
  for (const QJsonValue &v : root["partitions"].toArray()) {
    auto d = v.toObject();
    auto dynamic = d["is_dynamic"];
    if (dynamic.isBool() && !dynamic.toBool())
      continue;
    if (dynamic.isDouble() && dynamic.toInt() == 0)
      continue;
    if (dynamic.isString() &&
        (dynamic.toString() == "false" || dynamic.toString() == "0"))
      continue;
    QString n = d["name"].toString(),
            g = d["group_name"].toString().isEmpty()
                    ? d["group"].toString()
                    : d["group_name"].toString(),
            rel = d["path"].toString();
    quint64 declared = number(d["size"], &ok);
    if (!ok || !declared || declared > quint64(LLONG_MAX) - block ||
        !Ouga::safeName(n) || names.contains(n) || !groups.contains(g))
      return fail(error, "Super 分区定义冲突：" + n);
    QStringList candidates;
    if (!rel.isEmpty()) {
      for (const QString &dir :
           {directory, QFileInfo(defs[0]).absolutePath()}) {
        QString file = QDir(dir).filePath(rel);
        if (inside(directory, file) && QFileInfo(file).isFile() &&
            !candidates.contains(QFileInfo(file).canonicalFilePath()))
          candidates << QFileInfo(file).canonicalFilePath();
      }
    } else
      for (const QString &dir :
           {directory, QDir(directory).filePath("IMAGES")}) {
        QString file = QDir(dir).filePath(n + ".img");
        if (inside(directory, file) && QFileInfo(file).isFile())
          candidates << QFileInfo(file).canonicalFilePath();
      }
    candidates.removeDuplicates();
    if (candidates.size() != 1)
      return fail(error, "Super 镜像缺失/歧义/越界：" + n);
    qint64 expanded = expandedSize(candidates[0], error);
    if (expanded <= 0 || quint64(expanded) > declared)
      return fail(error, "Super 镜像超过定义容量：" + n);
    quint64 allocation = ((declared + block - 1) / block) * block;
    if (allocation > groups[g] - used[g])
      return fail(error, "Super 分组容量不足：" + g);
    used[g] += allocation;
    total = ((total + align - 1) / align) * align;
    if (total > capacity || allocation > capacity - total)
      return fail(error, "Super 总容量不足（含对齐及 metadata）");
    total += allocation;
    names.insert(n);
    merged->insert(Ouga::baseName(n));
    *args << "--partition"
          << n + ":readonly:" + QString::number(allocation) + ":" + g
          << "--image" << n + "=" + candidates[0];
  }
  return !names.isEmpty() || fail(error, "Super 定义没有动态分区");
}
bool OugaPackage::toRaw(const QString &source, const QString &destination,
                        quint64 allocated, QString *error) {
  qint64 expanded = expandedSize(source, error);
  if (expanded <= 0 || quint64(expanded) > allocated ||
      allocated > quint64(LLONG_MAX))
    return fail(error, "镜像对齐容量无效");
  QFile in(source), out(destination);
  if (!in.open(QIODevice::ReadOnly) ||
      !out.open(QIODevice::WriteOnly | QIODevice::NewOnly))
    return fail(error, "无法创建独立 raw 镜像副本");
  auto h = in.read(28);
  auto copy = [&](quint64 n) {
    while (n) {
      QByteArray b = in.read(qint64(qMin<quint64>(n, 1024 * 1024)));
      if (b.isEmpty() || out.write(b) != b.size())
        return false;
      n -= quint64(b.size());
    }
    return true;
  };
  if (h.size() < 4 || u32(h, 0) != 0xed26ff3a) {
    in.seek(0);
    if (!copy(quint64(in.size())))
      return fail(error, "复制 raw 镜像失败");
  } else {
    if (!in.seek(u16(h, 8)))
      return false;
    for (quint32 i = 0; i < u32(h, 20); ++i) {
      auto c = in.read(u16(h, 10));
      quint64 n = quint64(u32(c, 4)) * u32(h, 12);
      switch (u16(c, 0)) {
      case 0xcac1:
        if (!copy(n))
          return fail(error, "展开 sparse RAW 失败");
        break;
      case 0xcac2: {
        QByteArray word = in.read(4), buffer(1024 * 1024, 0);
        for (int j = 0; j < buffer.size(); j += 4)
          std::copy(word.begin(), word.end(), buffer.begin() + j);
        while (n) {
          qint64 len = qint64(qMin<quint64>(n, buffer.size()));
          if (out.write(buffer.constData(), len) != len)
            return fail(error, "展开 sparse FILL 失败");
          n -= quint64(len);
        }
        break;
      }
      case 0xcac3:
        if (!out.seek(out.pos() + qint64(n)))
          return fail(error, "展开 sparse hole 失败");
        break;
      case 0xcac4:
        in.read(4);
        break;
      default:
        return fail(error, "未知 sparse chunk");
      }
    }
  }
  if (!out.resize(qint64(allocated)) || !out.flush())
    return fail(error, "镜像副本对齐失败/磁盘空间不足");
  return true;
}

bool OugaPackage::superContents(const QString &file, QSet<QString> *names,
                                QString *error) {
  // Read a bounded expanded prefix; never materialize a multi-gigabyte image in
  // memory.
  names->clear();
  const qint64 expanded = expandedSize(file, error);
  if (expanded <= 0)
    return false;
  QFile f(file);
  if (!f.open(QIODevice::ReadOnly))
    return fail(error, "Super 不可读");
  QByteArray h = f.read(28), prefix;
  const quint64 limit = 4 * 1024 * 1024;
  if (h.size() < 28 || u32(h, 0) != 0xed26ff3a) {
    f.seek(0);
    prefix = f.read(qint64(limit));
  } else {
    f.seek(u16(h, 8));
    for (quint32 i = 0; i < u32(h, 20) && quint64(prefix.size()) < limit; ++i) {
      auto c = f.read(u16(h, 10));
      if (c.size() != u16(h, 10))
        return fail(error, "Super sparse chunk 截断");
      quint64 n = quint64(u32(c, 4)) * u32(h, 12),
              take = qMin(n, limit - quint64(prefix.size()));
      switch (u16(c, 0)) {
      case 0xcac1:
        prefix += f.read(qint64(take));
        f.seek(f.pos() + qint64(n - take));
        break;
      case 0xcac2: {
        auto word = f.read(4);
        if (word.size() != 4)
          return fail(error, "Super fill 截断");
        QByteArray data(int(take), 0);
        for (int j = 0; j < data.size(); ++j)
          data[j] = word[j % 4];
        prefix += data;
        break;
      }
      case 0xcac3:
        prefix += QByteArray(int(take), 0);
        break;
      case 0xcac4:
        f.read(4);
        break;
      default:
        return fail(error, "Super sparse 类型无效");
      }
    }
  }
  if (prefix.size() < 12368)
    return fail(error, "Super metadata 截断");
  QByteArray geometry = prefix.mid(4096, 52);
  if (u32(geometry, 0) != 0x616c4467 || u32(geometry, 4) != 52)
    return fail(error, "Super geometry 无效");
  QByteArray ghash = geometry.mid(8, 32);
  geometry.replace(8, 32, QByteArray(32, 0));
  if (QCryptographicHash::hash(geometry, QCryptographicHash::Sha256) != ghash)
    return fail(error, "Super geometry 校验失败");
  quint32 max = u32(geometry, 40);
  const quint32 copies = u32(geometry, 44), block = u32(geometry, 48);
  if (max < 512 || max > 1024 * 1024 || max % 512 || !copies || copies > 3 ||
      block < 512 || block % 512)
    return fail(error, "Super metadata 大小不支持");
  QByteArray meta = prefix.mid(12288, int(max));
  if (meta.size() < 128 || u32(meta, 0) != 0x414c5030 || u16(meta, 4) != 10)
    return fail(error, "Super metadata header 无效");
  quint32 hs = u32(meta, 8), ts = u32(meta, 44);
  if (u16(meta, 6) > 2 || hs != (u16(meta, 6) >= 2 ? 256u : 128u) ||
      hs > quint32(meta.size()) || ts > quint32(meta.size()) - hs)
    return fail(error, "Super metadata 越界");
  QByteArray header = meta.left(int(hs)), hh = header.mid(12, 32);
  header.replace(12, 32, QByteArray(32, 0));
  QByteArray tables = meta.mid(int(hs), int(ts));
  if (QCryptographicHash::hash(header, QCryptographicHash::Sha256) != hh ||
      QCryptographicHash::hash(tables, QCryptographicHash::Sha256) !=
          meta.mid(48, 32))
    return fail(error, "Super metadata SHA-256 不符");
  // AOSP liblp metadata_format.h: descriptors at 80/92/104/116; packed entries.
  struct Table {
    quint32 offset, count, size;
  };
  QVector<Table> descriptors;
  const quint32 sizes[] = {52, 24, 48, 64};
  quint64 tableEnd = 0;
  for (int index = 0; index < 4; ++index) {
    int at = 80 + index * 12;
    Table table{u32(meta, at), u32(meta, at + 4), u32(meta, at + 8)};
    if (table.size != sizes[index] || table.offset > ts ||
        quint64(table.count) * table.size > ts - table.offset)
      return fail(error, "Super metadata 表越界");
    descriptors << table;
  }
  auto ordered = descriptors;
  std::sort(ordered.begin(), ordered.end(),
            [](const Table &a, const Table &b) { return a.offset < b.offset; });
  for (const auto &table : ordered) {
    if (!table.count)
      continue;
    if (table.offset != tableEnd)
      return fail(error, "Super metadata 表重叠或不连续");
    tableEnd += quint64(table.count) * table.size;
  }
  if (tableEnd != ts || descriptors[3].count != 1 || !descriptors[2].count)
    return fail(error, "Super 不支持多物理设备/无分组布局");
  auto entry = [&](int table, quint32 index) {
    const auto &d = descriptors[table];
    return tables.mid(d.offset + index * d.size, d.size);
  };
  const QByteArray device = entry(3, 0);
  const quint64 firstSector = u64(device, 0), physical = u64(device, 16);
  const quint64 reserved = 12288 + 2 * quint64(max) * copies;
  if (device.mid(24, 36).split('\0').first() != "super" ||
      u32(device, 60) != 0 || physical % 512 || physical < quint64(expanded) ||
      firstSector < (reserved + 511) / 512 || firstSector >= physical / 512)
    return fail(error, "Super 物理设备边界或布局无效");
  QVector<quint64> groupUsage(descriptors[2].count, 0);
  QSet<QString> groupNames, partitionNames;
  QVector<QPair<quint64, quint64>> ranges;
  for (quint32 i = 0; i < descriptors[2].count; ++i) {
    const auto group = entry(2, i);
    const QString name =
        QString::fromLatin1(group.left(36).split('\0').first());
    if (!Ouga::safeName(name) || groupNames.contains(name) ||
        u32(group, 36) != 0)
      return fail(error, "Super 分组重复/不支持的分组属性");
    groupNames.insert(name);
  }
  for (quint32 i = 0; i < descriptors[0].count; ++i) {
    const auto part = entry(0, i);
    const QString name = QString::fromLatin1(part.left(36).split('\0').first());
    const quint32 first = u32(part, 40), count = u32(part, 44),
                  group = u32(part, 48);
    const quint32 flags = u32(part, 36);
    if (!Ouga::safeName(name) || partitionNames.contains(name) ||
        (flags & ~13u) || group >= descriptors[2].count ||
        first > descriptors[1].count || count > descriptors[1].count - first)
      return fail(error, "Super 分区重复、属性不支持或 extent 越界");
    partitionNames.insert(name);
    quint64 bytes = 0;
    for (quint32 j = 0; j < count; ++j) {
      const auto extent = entry(1, first + j);
      const quint64 sectors = u64(extent, 0), sector = u64(extent, 12);
      const quint32 type = u32(extent, 8), source = u32(extent, 20);
      if (!sectors || sectors > (quint64(LLONG_MAX) - bytes) / 512 ||
          type > 1 || source != 0)
        return fail(error, "Super extent 类型或长度无效");
      if (type == 0) {
        if (sector < firstSector || sector > physical / 512 ||
            sectors > physical / 512 - sector ||
            sector > quint64(expanded) / 512 ||
            sectors > quint64(expanded) / 512 - sector)
          return fail(error, "Super extent 超出完整镜像/设备容量");
        ranges.append({sector, sector + sectors});
      } else if (sector != 0)
        return fail(error, "Super ZERO extent 非零地址");
      bytes += sectors * 512;
    }
    if (bytes % block || bytes > quint64(LLONG_MAX) - groupUsage[group])
      return fail(error, "Super 逻辑容量未对齐或溢出");
    groupUsage[group] += bytes;
    if (bytes && !(flags & 8))
      names->insert(Ouga::baseName(name));
  }
  std::sort(ranges.begin(), ranges.end());
  for (int i = 1; i < ranges.size(); ++i)
    if (ranges[i].first < ranges[i - 1].second)
      return fail(error, "Super 物理 extent 重叠");
  for (quint32 i = 0; i < descriptors[2].count; ++i) {
    const quint64 maximum = u64(entry(2, i), 40);
    if (maximum && groupUsage[i] > maximum)
      return fail(error, "Super 分组容量超限");
  }
  return !names->isEmpty() || fail(error, "Super 不含有效数据分区");
}
