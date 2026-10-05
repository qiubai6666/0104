#ifndef OUGAPAYLOADREADER_H
#define OUGAPAYLOADREADER_H
#include <QByteArray>
#include <QMutex>
#include <QSharedPointer>
#include <QUrl>
#include <QMap>
#include <QList>
#include <QString>
#include <atomic>
#include <limits>

// A bounded random-access source. Remote sources are never materialized as a
// complete archive; callers request only the byte ranges needed by the
// Payload manifest or selected operations.
class OugaRandomAccessReader {
public:
  virtual ~OugaRandomAccessReader() = default;
  virtual quint64 size() const = 0;
  virtual QByteArray read(quint64 offset, quint64 length,
                          QString *error = nullptr) const = 0;
  virtual bool isRemote() const = 0;
};

class OugaLocalFileReader final : public OugaRandomAccessReader {
public:
  explicit OugaLocalFileReader(const QString &file, QString *error = nullptr);
  quint64 size() const override { return m_size; }
  QByteArray read(quint64 offset, quint64 length,
                  QString *error = nullptr) const override;
  bool isRemote() const override { return false; }
  bool valid() const { return m_valid; }
  QString file() const { return m_file; }

private:
  QString m_file;
  quint64 m_size = 0;
  bool m_valid = false;
};

class OugaHttpRangeReader final : public OugaRandomAccessReader {
public:
  // The probe is deliberately performed before returning. A server that
  // replies 200 or omits a valid Content-Range is not a random-access source.
  static QSharedPointer<OugaHttpRangeReader>
  create(const QUrl &url, const std::atomic_bool *cancel,
         QString *error = nullptr);

  quint64 size() const override { return m_size; }
  QByteArray read(quint64 offset, quint64 length,
                  QString *error = nullptr) const override;
  bool isRemote() const override { return true; }
  QUrl url() const { return m_url; }

private:
  explicit OugaHttpRangeReader(const QUrl &url,
                               const std::atomic_bool *cancel);
  bool probe(QString *error);
  QByteArray requestRange(quint64 offset, quint64 length,
                          quint64 *reportedTotal, QString *error) const;
  void cacheBlock(quint64 offset, const QByteArray &data) const;

  QUrl m_url;
  const std::atomic_bool *m_cancel = nullptr;
  quint64 m_size = 0;
  mutable QByteArray m_etag, m_modified, m_validator;
  mutable QMutex m_mutex;
  mutable QMap<quint64, QByteArray> m_cache;
  mutable QList<quint64> m_cacheOrder;
  static constexpr quint64 cacheBlockSize = 64ull * 1024;
  static constexpr int maxCachedBlocks = 8;
};

#endif
