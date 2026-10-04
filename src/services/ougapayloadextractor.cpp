#include "ougapayloadextractor.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QtEndian>
#include <algorithm>
#include <thread>
#include <vector>
#include <bzlib.h>
#include <lzma.h>
namespace {
quint16 le16(const QByteArray &b, qsizetype at) {
  return qFromLittleEndian<quint16>(
      reinterpret_cast<const uchar *>(b.constData() + at));
}
quint32 le32(const QByteArray &b, qsizetype at) {
  return qFromLittleEndian<quint32>(
      reinterpret_cast<const uchar *>(b.constData() + at));
}
quint64 le64(const QByteArray &b, qsizetype at) {
  return qFromLittleEndian<quint64>(
      reinterpret_cast<const uchar *>(b.constData() + at));
}
QByteArray readAt(QFile &f, quint64 offset, qint64 size) {
  if (!f.seek(qint64(offset)))
    return {};
  return f.read(size);
}
// One decoded operation must fit a bounded buffer; AOSP chunks are <= 2 MiB.
constexpr quint64 maxOperationBytes = 256ull * 1024 * 1024;
constexpr quint64 xzMemoryLimit = 256ull * 1024 * 1024;
bool xz(const QByteArray &in, QByteArray *out) {
  uint64_t limit = xzMemoryLimit;
  size_t inPos = 0, outPos = 0;
  const lzma_ret r = lzma_stream_buffer_decode(
      &limit, 0, nullptr, reinterpret_cast<const uint8_t *>(in.constData()),
      &inPos, size_t(in.size()), reinterpret_cast<uint8_t *>(out->data()),
      &outPos, size_t(out->size()));
  return r == LZMA_OK && outPos == size_t(out->size());
}
bool bz2(const QByteArray &in, QByteArray *out) {
  unsigned int length = unsigned(out->size());
  const int r = BZ2_bzBuffToBuffDecompress(
      out->data(), &length, const_cast<char *>(in.constData()),
      unsigned(in.size()), 0, 0);
  return r == BZ_OK && length == unsigned(out->size());
}
} // namespace

OugaPayloadExtractor::Zip OugaPayloadExtractor::locate(const QString &file,
                                                       quint64 *offset,
                                                       quint64 *size) {
  QFile f(file);
  if (!f.open(QIODevice::ReadOnly) || f.read(2) != "PK")
    return Zip::NotZip;
  const quint64 total = quint64(f.size());
  // End of central directory: 22 bytes plus a comment of at most 65535.
  const quint64 tailSize = qMin<quint64>(total, 22 + 65535);
  const QByteArray tail = readAt(f, total - tailSize, qint64(tailSize));
  qsizetype eocd = -1;
  for (qsizetype i = tail.size() - 22; i >= 0; --i)
    if (le32(tail, i) == 0x06054b50 &&
        quint64(i) + 22 + le16(tail, i + 20) == tailSize) {
      eocd = i;
      break;
    }
  if (eocd < 0)
    return Zip::Other;
  quint64 entries = le16(tail, eocd + 10), cdSize = le32(tail, eocd + 12),
          cdOffset = le32(tail, eocd + 16);
  if (entries == 0xffff || cdSize == 0xffffffff || cdOffset == 0xffffffff) {
    if (eocd < 20 || le32(tail, eocd - 20) != 0x07064b50)
      return Zip::Other;
    const quint64 record = le64(tail, eocd - 20 + 8);
    const QByteArray z = readAt(f, record, 56);
    if (z.size() != 56 || le32(z, 0) != 0x06064b50)
      return Zip::Other;
    entries = le64(z, 32);
    cdSize = le64(z, 40);
    cdOffset = le64(z, 48);
  }
  if (cdOffset > total || cdSize > total - cdOffset ||
      cdSize > 64ull * 1024 * 1024)
    return Zip::Other;
  const QByteArray cd = readAt(f, cdOffset, qint64(cdSize));
  if (quint64(cd.size()) != cdSize)
    return Zip::Other;
  int matches = 0;
  quint16 flags = 0, method = 0;
  quint64 compressed = 0, uncompressed = 0, local = 0;
  qsizetype p = 0;
  for (quint64 n = 0; n < entries; ++n) {
    if (cd.size() - p < 46 || le32(cd, p) != 0x02014b50)
      return Zip::Other;
    const quint16 nameLength = le16(cd, p + 28), extraLength = le16(cd, p + 30),
                  commentLength = le16(cd, p + 32);
    const qsizetype next = p + 46 + nameLength + extraLength + commentLength;
    if (next > cd.size())
      return Zip::Other;
    const QString name =
        QString::fromUtf8(cd.mid(p + 46, nameLength)).replace('\\', '/');
    if (name.section('/', -1).compare("payload.bin", Qt::CaseInsensitive) ==
        0) {
      ++matches;
      flags = le16(cd, p + 8);
      method = le16(cd, p + 10);
      compressed = le32(cd, p + 20);
      uncompressed = le32(cd, p + 24);
      local = le32(cd, p + 42);
      // ZIP64 extended information replaces only the saturated fields.
      qsizetype x = p + 46 + nameLength;
      const qsizetype extraEnd = x + extraLength;
      while (extraEnd - x >= 4) {
        const quint16 id = le16(cd, x), length = le16(cd, x + 2);
        if (x + 4 + length > extraEnd)
          return Zip::Other;
        if (id == 0x0001) {
          qsizetype v = x + 4;
          auto take = [&](quint64 *field) {
            if (*field != 0xffffffff)
              return true;
            if (v + 8 > x + 4 + length)
              return false;
            *field = le64(cd, v);
            v += 8;
            return true;
          };
          if (!take(&uncompressed) || !take(&compressed) || !take(&local))
            return Zip::Other;
        }
        x += 4 + length;
      }
    }
    p = next;
  }
  // Same rule as the 7z path: exactly one payload.bin, never a guess.
  if (matches != 1 || (flags & 0x0001) || method != 0 ||
      compressed != uncompressed)
    return Zip::Other;
  const QByteArray header = readAt(f, local, 30);
  if (header.size() != 30 || le32(header, 0) != 0x04034b50 ||
      le16(header, 8) != 0)
    return Zip::Other;
  const quint64 data = local + 30 + le16(header, 26) + le16(header, 28);
  if (data > total || uncompressed > total - data ||
      readAt(f, data, 4) != "CrAU")
    return Zip::Other;
  *offset = data;
  *size = uncompressed;
  return Zip::Stored;
}

bool OugaPayloadExtractor::supported(const OugaPayloadEntry &entry,
                                     const OugaPayloadLayout &layout) {
  const quint64 blockSize = layout.blockSize;
  if (layout.dataOffset > layout.end)
    return false;
  const quint64 available = layout.end - layout.dataOffset;
  if (entry.requiresOldImage || !blockSize || entry.size % blockSize ||
      entry.ops.isEmpty())
    return false;
  const quint64 blocks = entry.size / blockSize;
  QVector<OugaPayloadExtent> extents;
  for (const auto &op : entry.ops) {
    if (op.hasSource || op.destination.isEmpty())
      return false;
    quint64 bytes = 0;
    for (const auto &e : op.destination) {
      if (!e.blocks || e.start > blocks || e.blocks > blocks - e.start)
        return false;
      bytes += e.blocks * blockSize;
      extents << e;
    }
    if (bytes > maxOperationBytes)
      return false;
    switch (op.type) {
    case 0: // REPLACE
    case 1: // REPLACE_BZ
    case 8: // REPLACE_XZ
      if (!op.hasDataOffset || !op.dataLength || op.dataHash.size() != 32 ||
          op.dataLength > maxOperationBytes ||
          op.dataOffset > available ||
          op.dataLength > available - op.dataOffset ||
          (op.type == 0 && op.dataLength != bytes))
        return false;
      break;
    case 6: // ZERO
    case 7: // DISCARD
      if (op.dataLength)
        return false;
      break;
    default: // REPLACE_ZSTD and every source operation stay with payload.exe
      return false;
    }
  }
  // Destination extents tile the image exactly once: no gap, no overlap.
  std::sort(extents.begin(), extents.end(),
            [](const OugaPayloadExtent &a, const OugaPayloadExtent &b) {
              return a.start < b.start;
            });
  quint64 next = 0;
  for (const auto &e : extents) {
    if (e.start != next)
      return false;
    next += e.blocks;
  }
  return next == blocks;
}
bool OugaPayloadExtractor::supported(const QVector<OugaPayloadEntry> &entries,
                                     const QStringList &selected,
                                     const OugaPayloadLayout &layout) {
  int count = 0;
  for (const auto &entry : entries)
    if (selected.isEmpty() || selected.contains(entry.name)) {
      if (!supported(entry, layout))
        return false;
      ++count;
    }
  return count > 0;
}
QString OugaPayloadExtractor::extract(const QString &file,
                                      const OugaPayloadLayout &layout,
                                      const QVector<OugaPayloadEntry> &entries,
                                      const QString &output, int workers,
                                      const std::atomic_bool &cancel,
                                      const Callbacks &callbacks) {
  quint64 total = 0;
  for (const auto &entry : entries) {
    if (!supported(entry, layout))
      return "Payload 分区包含应用内提取不支持的操作：" + entry.name;
    total += entry.size;
  }
  std::atomic<quint64> done{0};
  auto report = [&](quint64 bytes) {
    const quint64 value = done.fetch_add(bytes) + bytes;
    if (callbacks.progress)
      callbacks.progress(qMin(value, total), total);
  };
  if (callbacks.progress)
    callbacks.progress(0, total);
  workers = qMax(1, workers);
  for (const auto &entry : entries) {
    if (cancel)
      return "准备已取消；保留已完成的镜像";
    const QString target = QDir(output).filePath(entry.name + ".img");
    const QString partial = target + ".partial";
    if (QFileInfo::exists(target) || QFileInfo::exists(partial))
      return "Payload 输出已存在，拒绝覆盖：" + entry.name;
    if (callbacks.started)
      callbacks.started(entry.name);
    {
      // Preallocate; regions not written later (ZERO/DISCARD) read as zero.
      QFile out(partial);
      if (!out.open(QIODevice::ReadWrite | QIODevice::NewOnly) ||
          !out.resize(qint64(entry.size)))
        return "无法创建 Payload 输出：" + partial;
    }
    std::atomic<qsizetype> next{0};
    std::atomic_bool failed{false};
    QMutex errorLock;
    QString error;
    auto fail = [&](const QString &text) {
      QMutexLocker lock(&errorLock);
      if (error.isEmpty())
        error = text;
      failed = true;
    };
    auto worker = [&] {
      // Each thread owns its handles: positioned reads/writes never contend.
      QFile in(file), out(partial);
      if (!in.open(QIODevice::ReadOnly) ||
          !out.open(QIODevice::ReadWrite | QIODevice::ExistingOnly |
                    QIODevice::Unbuffered)) {
        fail("无法打开 Payload 读写句柄：" + entry.name);
        return;
      }
      QByteArray raw;
      for (qsizetype i = next++; i < entry.ops.size() && !failed && !cancel;
           i = next++) {
        const OugaPayloadOperation &op = entry.ops[i];
        quint64 bytes = 0;
        for (const auto &e : op.destination)
          bytes += e.blocks * layout.blockSize;
        if (op.type == 6 || op.type == 7) {
          report(bytes);
          continue;
        }
        const QByteArray data =
            readAt(in, layout.dataOffset + op.dataOffset, qint64(op.dataLength));
        if (quint64(data.size()) != op.dataLength) {
          fail("Payload 数据读取失败：" + entry.name);
          return;
        }
        if (QCryptographicHash::hash(data, QCryptographicHash::Sha256) !=
            op.dataHash) {
          fail("operation data sha256 校验失败：" + entry.name);
          return;
        }
        const QByteArray *decoded = &data;
        if (op.type != 0) {
          raw.resize(qsizetype(bytes));
          if (!(op.type == 8 ? xz(data, &raw) : bz2(data, &raw))) {
            fail("Payload 数据解压失败：" + entry.name);
            return;
          }
          decoded = &raw;
        }
        qsizetype at = 0;
        for (const auto &e : op.destination) {
          const qint64 length = qint64(e.blocks * layout.blockSize);
          if (!out.seek(qint64(e.start * layout.blockSize)) ||
              out.write(decoded->constData() + at, length) != length) {
            fail("Payload 输出写入失败：" + entry.name);
            return;
          }
          at += length;
          report(quint64(length));
        }
      }
    };
    const int count = int(qMin<qsizetype>(workers, entry.ops.size()));
    std::vector<std::thread> threads;
    threads.reserve(size_t(count));
    for (int n = 0; n < count; ++n)
      threads.emplace_back(worker);
    for (auto &thread : threads)
      thread.join();
    if (!failed && cancel)
      error = "准备已取消；保留已完成的镜像";
    if (!error.isEmpty() || cancel) {
      QFile::remove(partial);
      return error;
    }
    if (!QFile::rename(partial, target)) {
      QFile::remove(partial);
      return "Payload 输出重命名失败：" + entry.name;
    }
    if (callbacks.finished)
      callbacks.finished(entry.name);
  }
  return {};
}
