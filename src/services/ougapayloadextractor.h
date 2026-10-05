#ifndef OUGAPAYLOADEXTRACTOR_H
#define OUGAPAYLOADEXTRACTOR_H
#include "ougapackage.h"
#include <atomic>
#include <functional>

// In-process extractor for full AOSP payloads, following VioletToolBox's
// PayloadProcessing: partitions in order, operations of one partition in
// parallel, each operation's data_sha256_hash verified before decoding.
// A STORED payload.bin is read in place from the ZIP, without a copy.
class OugaPayloadExtractor {
public:
  enum class Zip { NotZip, Stored, Other };
  // Locate a single STORED payload.bin. Other: ZIP that needs 7z (compressed,
  // encrypted, ambiguous or malformed); the existing safe extraction is used.
  static Zip locate(const QString &file, quint64 *offset, quint64 *size,
                    QString *error = nullptr);
  static Zip locate(const OugaRandomAccessReader &reader, quint64 *offset,
                    quint64 *size, QString *error = nullptr);
  // REPLACE / BZ / XZ / ZSTD / ZERO / DISCARD only. Every data operation is
  // hash-verified, and the destination extents
  // tile the partition exactly; anything else is left to payload.exe.
  static bool supported(const OugaPayloadEntry &entry,
                        const OugaPayloadLayout &layout);
  // Selected entries (all when empty) can be extracted in-process.
  static bool supported(const QVector<OugaPayloadEntry> &entries,
                        const QStringList &selected,
                        const OugaPayloadLayout &layout);
  struct Callbacks {
    std::function<void(const QString &)> started;
    std::function<void(const QString &)> finished;
    std::function<void(quint64 done, quint64 total)> progress;
  };
  static QString extract(const QString &file, const OugaPayloadLayout &layout,
                         const QVector<OugaPayloadEntry> &entries,
                         const QString &output, int workers,
                         const std::atomic_bool &cancel,
                         const Callbacks &callbacks);
  static QString extract(
      const QSharedPointer<OugaRandomAccessReader> &reader,
      const OugaPayloadLayout &layout,
      const QVector<OugaPayloadEntry> &entries,
      const QString &output, int workers, const std::atomic_bool &cancel,
      const Callbacks &callbacks);
};
#endif
