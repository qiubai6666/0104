#include "ougapayloadextractor.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QScopeGuard>
#include <QThread>
#include <QThreadPool>
#include <QtConcurrentRun>
#include <QtEndian>
#include <algorithm>
#include <bzlib.h>
#include <lzma.h>
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <bcrypt.h>
#endif
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
// One decoded operation must fit a bounded buffer; AOSP chunks are <= 2 MiB.
constexpr quint64 maxOperationBytes = 256ull * 1024 * 1024;
constexpr quint64 xzMemoryLimit = 256ull * 1024 * 1024;
// Reinitializing one stream reuses liblzma's dictionary/decoder allocations.
// Keep checksum, input-consumption and exact output-length validation.
class XzDecoder {
public:
  ~XzDecoder() { lzma_end(&m_stream); }
  bool decode(const QByteArray &in, QByteArray *out) {
    if (lzma_stream_decoder(&m_stream, xzMemoryLimit, 0) != LZMA_OK)
      return false;
    m_stream.next_in = reinterpret_cast<const uint8_t *>(in.constData());
    m_stream.avail_in = size_t(in.size());
    m_stream.next_out = reinterpret_cast<uint8_t *>(out->data());
    m_stream.avail_out = size_t(out->size());
    const lzma_ret result = lzma_code(&m_stream, LZMA_FINISH);
    return result == LZMA_STREAM_END && m_stream.avail_in == 0 &&
           m_stream.avail_out == 0;
  }
private:
  lzma_stream m_stream = LZMA_STREAM_INIT;
};

// VioletToolBox's Type.Zstd (14), without falling back to payload.exe.
// A worker reuses its context, and the manifest's bounded destination length
// controls allocation. Reject oversized windows, dictionaries, extra frames
// or trailing bytes instead of silently accepting a partial decode.
class ZstdDecoder {
public:
  ZstdDecoder() : m_context(ZSTD_createDCtx()) {}
  ~ZstdDecoder() { ZSTD_freeDCtx(m_context); }
  bool decode(const QByteArray &in, QByteArray *out) {
    if (!m_context)
      return false;
    ZSTD_FrameHeader header{};
    if (ZSTD_getFrameHeader(&header, in.constData(), size_t(in.size())) != 0 ||
        header.frameType != ZSTD_frame || header.dictID != 0 ||
        header.windowSize > maxOperationBytes ||
        (header.frameContentSize != ZSTD_CONTENTSIZE_UNKNOWN &&
         header.frameContentSize != quint64(out->size())))
      return false;
    const size_t frame =
        ZSTD_findFrameCompressedSize(in.constData(), size_t(in.size()));
    if (ZSTD_isError(frame) || frame != size_t(in.size()))
      return false;
    const size_t decoded = ZSTD_decompressDCtx(
        m_context, out->data(), size_t(out->size()),
        in.constData(), size_t(in.size()));
    return !ZSTD_isError(decoded) && decoded == size_t(out->size());
  }
private:
  ZSTD_DCtx *m_context = nullptr;
};

// Windows CNG provides the same SHA-256 with CPU acceleration where available.
// Each worker owns a reusable hash. Any provider/API failure falls back to Qt;
// a failed hash is never accepted and never reused in a partially-fed state.
class OperationHash {
public:
#ifdef Q_OS_WIN
  OperationHash() {
    if (BCryptOpenAlgorithmProvider(&m_algorithm, BCRYPT_SHA256_ALGORITHM,
                                     nullptr, 0) < 0)
      return;
    DWORD length = 0, copied = 0;
    if (BCryptGetProperty(m_algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&length), sizeof(length),
                          &copied, 0) < 0 || !length || length > 65536)
      return;
    m_object.resize(qsizetype(length));
    if (BCryptCreateHash(m_algorithm, &m_hash,
                         reinterpret_cast<PUCHAR>(m_object.data()), length,
                         nullptr, 0, BCRYPT_HASH_REUSABLE_FLAG) < 0)
      m_hash = nullptr;
  }
  ~OperationHash() {
    if (m_hash)
      BCryptDestroyHash(m_hash);
    if (m_algorithm)
      BCryptCloseAlgorithmProvider(m_algorithm, 0);
  }
#endif
  QByteArray hash(const QByteArray &data) {
#ifdef Q_OS_WIN
    if (m_hash) {
      QByteArray result(32, Qt::Uninitialized);
      if (BCryptHashData(m_hash,
                         reinterpret_cast<PUCHAR>(const_cast<char *>(data.constData())),
                         ULONG(data.size()), 0) >= 0 &&
          BCryptFinishHash(m_hash, reinterpret_cast<PUCHAR>(result.data()),
                            ULONG(result.size()), 0) >= 0)
        return result;
      BCryptDestroyHash(m_hash);
      m_hash = nullptr;
    }
#endif
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
  }
  // Verify the bytes actually written, not the manifest's claimed digest. Use
  // bounded reads and CNG acceleration; a failed CNG stream restarts from byte
  // zero with Qt rather than accepting a partial hash. No extra image copy.
  QByteArray fileHash(const QString &path, quint64 expectedSize,
                      const std::atomic_bool &cancel) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || quint64(file.size()) != expectedSize)
      return {};
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    for (;;) {
      if (!file.seek(0)) return {};
      QCryptographicHash fallback(QCryptographicHash::Sha256);
      quint64 consumed = 0;
      bool retry = false;
      while (consumed < expectedSize) {
        if (cancel) return {};
        const qint64 count = file.read(buffer.data(),
            qint64(qMin<quint64>(quint64(buffer.size()), expectedSize - consumed)));
        if (count <= 0) return {};
#ifdef Q_OS_WIN
        if (m_hash) {
          if (BCryptHashData(m_hash, reinterpret_cast<PUCHAR>(buffer.data()),
                            ULONG(count), 0) < 0) {
            BCryptDestroyHash(m_hash);
            m_hash = nullptr;
            retry = true;
            break;
          }
        } else
#endif
          fallback.addData(QByteArrayView(buffer.constData(), qsizetype(count)));
        consumed += quint64(count);
      }
      if (retry) continue;
      if (cancel || quint64(file.size()) != expectedSize) return {};
#ifdef Q_OS_WIN
      if (m_hash) {
        QByteArray result(32, Qt::Uninitialized);
        if (BCryptFinishHash(m_hash, reinterpret_cast<PUCHAR>(result.data()),
                            ULONG(result.size()), 0) >= 0)
          return result;
        BCryptDestroyHash(m_hash);
        m_hash = nullptr;
        continue;
      }
#endif
      return fallback.result();
    }
  }
private:
#ifdef Q_OS_WIN
  BCRYPT_ALG_HANDLE m_algorithm = nullptr;
  BCRYPT_HASH_HANDLE m_hash = nullptr;
  QByteArray m_object;
#endif
};
bool bz2(const QByteArray &in, QByteArray *out) {
  unsigned int length = unsigned(out->size());
  const int r = BZ2_bzBuffToBuffDecompress(
      out->data(), &length, const_cast<char *>(in.constData()),
      unsigned(in.size()), 0, 0);
  return r == BZ_OK && length == unsigned(out->size());
}
} // namespace

OugaPayloadExtractor::Zip OugaPayloadExtractor::locate(
    const QString &file, quint64 *offset, quint64 *size, QString *error) {
  QString localError;
  OugaLocalFileReader reader(file, &localError);
  if (!reader.valid()) {
    if (error && !localError.isEmpty())
      *error = localError;
    return Zip::Other;
  }
  return locate(reader, offset, size, error);
}

OugaPayloadExtractor::Zip OugaPayloadExtractor::locate(
    const OugaRandomAccessReader &reader, quint64 *offset, quint64 *size,
    QString *error) {
  if (!offset || !size)
    return Zip::Other;
  *offset = 0;
  *size = 0;
  bool readFailed = false;
  auto readAt = [&](quint64 position, quint64 length) {
    if (length > quint64(std::numeric_limits<qsizetype>::max())) {
      readFailed = true;
      return QByteArray();
    }
    QString readError;
    const QByteArray data = reader.read(position, length, &readError);
    if (data.size() != qsizetype(length)) {
      readFailed = true;
      if (error && !readError.isEmpty())
        *error = readError;
      return QByteArray();
    }
    return data;
  };
  const quint64 total = reader.size();
  const QByteArray prefix = readAt(0, qMin<quint64>(2, total));
  if (readFailed)
    return Zip::Other;
  if (prefix != "PK")
    return Zip::NotZip;
  if (total < 22)
    return Zip::Other;

  // End of central directory: 22 bytes plus a comment of at most 65535.
  const quint64 tailSize = qMin<quint64>(total, 22 + 65535);
  const QByteArray tail = readAt(total - tailSize, tailSize);
  if (readFailed)
    return Zip::Other;
  qsizetype eocd = -1;
  if (tail.size() >= 22) {
    for (qsizetype i = tail.size() - 22; i >= 0; --i) {
      if (le32(tail, i) == 0x06054b50 &&
          quint64(i) + 22 + le16(tail, i + 20) == tailSize) {
        eocd = i;
        break;
      }
    }
  }
  if (eocd < 0)
    return Zip::Other;

  if (le16(tail, eocd + 4) != 0 || le16(tail, eocd + 6) != 0 ||
      le16(tail, eocd + 8) != le16(tail, eocd + 10))
    return Zip::Other; // Multi-volume ZIP is not a contiguous Range source.
  quint64 entries = le16(tail, eocd + 10), cdSize = le32(tail, eocd + 12),
          cdOffset = le32(tail, eocd + 16);
  if (entries == 0xffff || cdSize == 0xffffffff || cdOffset == 0xffffffff) {
    if (eocd < 20 || le32(tail, eocd - 20) != 0x07064b50 ||
        le32(tail, eocd - 16) != 0 || le32(tail, eocd - 4) != 1)
      return Zip::Other;
    const quint64 record = le64(tail, eocd - 20 + 8);
    const quint64 locator = total - tailSize + quint64(eocd) - 20;
    if (record > locator || locator - record < 56)
      return Zip::Other;
    const QByteArray z = readAt(record, 56);
    if (readFailed || z.size() != 56 || le32(z, 0) != 0x06064b50 ||
        le64(z, 4) < 44 || le64(z, 4) > locator - record - 12 ||
        le32(z, 16) != 0 || le32(z, 20) != 0 ||
        le64(z, 24) != le64(z, 32))
      return Zip::Other;
    entries = le64(z, 32);
    cdSize = le64(z, 40);
    cdOffset = le64(z, 48);
  }
  const quint64 directoryEnd = total - tailSize + quint64(eocd);
  if (cdOffset > directoryEnd || cdSize > directoryEnd - cdOffset ||
      cdSize > 64ull * 1024 * 1024 ||
      entries > (cdSize / 46) + 1)
    return Zip::Other;
  const QByteArray cd = readAt(cdOffset, cdSize);
  if (readFailed || quint64(cd.size()) != cdSize)
    return Zip::Other;

  int matches = 0;
  quint16 flags = 0, method = 0;
  quint64 compressed = 0, uncompressed = 0, local = 0;
  QByteArray payloadName;
  qsizetype p = 0;
  for (quint64 n = 0; n < entries; ++n) {
    if (cd.size() - p < 46 || le32(cd, p) != 0x02014b50)
      return Zip::Other;
    const quint16 nameLength = le16(cd, p + 28), extraLength = le16(cd, p + 30),
                  commentLength = le16(cd, p + 32);
    const qsizetype next = p + 46 + nameLength + extraLength + commentLength;
    if (next < p || next > cd.size())
      return Zip::Other;
    const QString name =
        QString::fromUtf8(cd.mid(p + 46, nameLength)).replace('\\', '/');
    if (name.section('/', -1).compare("payload.bin", Qt::CaseInsensitive) ==
        0) {
      ++matches;
      payloadName = cd.mid(p + 46, nameLength);
      if (le16(cd, p + 34) != 0)
        return Zip::Other;
      flags = le16(cd, p + 8);
      method = le16(cd, p + 10);
      compressed = le32(cd, p + 20);
      uncompressed = le32(cd, p + 24);
      local = le32(cd, p + 42);
      // ZIP64 extended information replaces only the saturated fields, in
      // central-directory order: uncompressed, compressed, local offset.
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

  // Exactly one unencrypted STORED payload.bin is required. Compressed or
  // ambiguous remote ZIPs must never fall back to downloading the full ZIP.
  if (p != cd.size() || matches != 1 || (flags & 0x0041) || method != 0 ||
      compressed != uncompressed || !uncompressed)
    return Zip::Other;
  if (local > cdOffset || cdOffset - local < 30)
    return Zip::Other;
  const QByteArray header = readAt(local, 30);
  if (readFailed || header.size() != 30 || le32(header, 0) != 0x04034b50 ||
      le16(header, 6) != flags || le16(header, 8) != 0)
    return Zip::Other;
  const quint64 nameLength = le16(header, 26), extraLength = le16(header, 28);
  if (nameLength + extraLength > cdOffset - local - 30)
    return Zip::Other;
  if (readAt(local + 30, nameLength) != payloadName || readFailed)
    return Zip::Other;
  const quint64 data = local + 30 + nameLength + extraLength;
  if (data > cdOffset || uncompressed > cdOffset - data || uncompressed < 4)
    return Zip::Other;
  const QByteArray magic = readAt(data, 4);
  if (readFailed || magic != "CrAU")
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
    case 14: // ZSTD (VioletToolBox / AOSP full-image operation)
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
    default: // Source/delta operations stay with payload.exe
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
QString OugaPayloadExtractor::extract(
    const QString &file, const OugaPayloadLayout &layout,
    const QVector<OugaPayloadEntry> &entries, const QString &output, int workers,
    const std::atomic_bool &cancel, const Callbacks &callbacks) {
  QString error;
  auto local = QSharedPointer<OugaLocalFileReader>::create(file, &error);
  if (!local->valid())
    return error.isEmpty() ? "Payload 不可读" : error;
  QSharedPointer<OugaRandomAccessReader> reader = local;
  return extract(reader, layout, entries, output, workers, cancel, callbacks);
}

QString OugaPayloadExtractor::extract(
    const QSharedPointer<OugaRandomAccessReader> &reader,
    const OugaPayloadLayout &layout,
    const QVector<OugaPayloadEntry> &entries, const QString &output, int workers,
    const std::atomic_bool &cancel, const Callbacks &callbacks) {
  if (reader.isNull())
    return "Payload 读取器不可用";
  quint64 total = 0;
  for (const auto &entry : entries) {
    if (!supported(entry, layout))
      return "Payload 分区包含应用内提取不支持的操作：" + entry.name;
    if (entry.size > std::numeric_limits<quint64>::max() - total)
      return "Payload 总大小溢出";
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
  QThreadPool pool;
  pool.setMaxThreadCount(workers);
  pool.setThreadPriority(QThread::LowPriority);
  pool.setExpiryTimeout(-1);
  const auto localReader =
      dynamic_cast<const OugaLocalFileReader *>(reader.data());
  for (const auto &entry : entries) {
    if (cancel)
      return "准备已取消；保留已完成的镜像";
    const QString target = QDir(output).filePath(entry.name + ".img");
    const QString partial = target + ".partial";
    if (QFileInfo::exists(target) || QFileInfo::exists(partial))
      return "Payload 输出已存在，拒绝覆盖：" + entry.name;
    if (callbacks.started)
      callbacks.started(entry.name);
    bool ownsPartial = false;
    const auto cleanup = qScopeGuard([&] {
      // Never remove a pre-existing path, including when NewOnly lost a race.
      if (ownsPartial)
        QFile::remove(partial);
    });
    {
      QFile out(partial);
      if (!out.open(QIODevice::ReadWrite | QIODevice::NewOnly))
        return "无法创建 Payload 输出：" + partial;
      ownsPartial = true;
      if (!out.resize(qint64(entry.size)))
        return "无法预分配 Payload 输出：" + partial;
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
    auto worker = [&, reader, localReader] {
      // Local files keep one positioned handle per worker. Remote reads go
      // through the bounded Range reader and never materialize the archive.
      QFile in(localReader ? localReader->file() : QString()), out(partial);
      if ((localReader && !in.open(QIODevice::ReadOnly)) ||
          !out.open(QIODevice::ReadWrite | QIODevice::ExistingOnly |
                    QIODevice::Unbuffered)) {
        fail("无法打开 Payload 读写句柄：" + entry.name);
        return;
      }
      QByteArray data, raw;
      XzDecoder xzDecoder;
      ZstdDecoder zstdDecoder;
      OperationHash hasher;
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
        data.resize(qsizetype(op.dataLength));
        QString readError;
        bool readOk = false;
        if (localReader) {
          readOk = in.seek(qint64(layout.dataOffset + op.dataOffset)) &&
                   in.read(data.data(), data.size()) == data.size();
        } else {
          data = reader->read(layout.dataOffset + op.dataOffset,
                              op.dataLength, &readError);
          readOk = data.size() == qsizetype(op.dataLength);
        }
        if (!readOk) {
          fail(readError.isEmpty() ? "Payload 数据读取失败：" + entry.name
                                   : readError + "：" + entry.name);
          return;
        }
        if (hasher.hash(data) != op.dataHash) {
          fail("operation data sha256 校验失败：" + entry.name);
          return;
        }
        const QByteArray *decoded = &data;
        if (op.type != 0) {
          raw.resize(qsizetype(bytes));
          const bool decodedOk = op.type == 8 ? xzDecoder.decode(data, &raw)
              : op.type == 14 ? zstdDecoder.decode(data, &raw)
                              : bz2(data, &raw);
          if (!decodedOk) {
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
    QVector<QFuture<void>> jobs;
    jobs.reserve(count);
    for (int n = 0; n < count; ++n)
      jobs << QtConcurrent::run(&pool, worker);
    for (auto &job : jobs)
      job.waitForFinished();
    if (!failed && cancel)
      error = "准备已取消；保留已完成的镜像";
    if (!error.isEmpty() || cancel)
      return error;
    // Hash after all writer handles have closed, before publishing .img or OK.
    OperationHash imageHasher;
    const QByteArray actual = imageHasher.fileHash(partial, entry.size, cancel);
    if (cancel)
      return "准备已取消；保留已完成的镜像";
    if (actual.size() != 32 || actual != entry.hash)
      return "Payload 镜像长度/SHA-256 校验失败：" + entry.name;
    if (!QFile::rename(partial, target))
      return "Payload 输出重命名失败：" + entry.name;
    ownsPartial = false;
    if (callbacks.finished)
      callbacks.finished(entry.name);
  }
  return {};
}
