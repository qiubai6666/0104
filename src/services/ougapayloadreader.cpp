#include "ougapayloadreader.h"
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <QThreadStorage>
#include <QtGlobal>
#include <algorithm>
#include <cstring>
#include <limits>

namespace {
bool fail(QString *error, const QString &message) {
  if (error)
    *error = message;
  return false;
}

bool validRange(quint64 offset, quint64 length, quint64 total) {
  return offset <= total && length <= total - offset;
}
}

OugaLocalFileReader::OugaLocalFileReader(const QString &file, QString *error)
    : m_file(QFileInfo(file).absoluteFilePath()) {
  const QFileInfo info(m_file);
  if (!info.isFile() || !info.isReadable()) {
    fail(error, "Payload 源文件不可读：" + file);
    return;
  }
  if (info.size() < 0) {
    fail(error, "Payload 源文件大小无效：" + file);
    return;
  }
  m_size = quint64(info.size());
  m_valid = true;
}

QByteArray OugaLocalFileReader::read(quint64 offset, quint64 length,
                                     QString *error) const {
  if (!m_valid || !validRange(offset, length, m_size)) {
    if (error)
      *error = "Payload 本地读取范围越界";
    return {};
  }
  if (!length)
    return {};
  if (length > quint64(std::numeric_limits<qint64>::max()) ||
      length > quint64(std::numeric_limits<qsizetype>::max())) {
    if (error)
      *error = "Payload 读取块过大";
    return {};
  }
  QFile file(m_file);
  if (!file.open(QIODevice::ReadOnly) || !file.seek(qint64(offset))) {
    if (error)
      *error = "Payload 本地读取失败：" + m_file;
    return {};
  }
  const QByteArray data = file.read(qint64(length));
  if (data.size() != qsizetype(length)) {
    if (error)
      *error = "Payload 本地读取被截断：" + m_file;
    return {};
  }
  return data;
}

OugaHttpRangeReader::OugaHttpRangeReader(const QUrl &url,
                                         const std::atomic_bool *cancel)
    : m_url(url), m_cancel(cancel) {}

QSharedPointer<OugaHttpRangeReader>
OugaHttpRangeReader::create(const QUrl &url, const std::atomic_bool *cancel,
                           QString *error) {
  if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() ||
      (url.scheme() != "https" &&
       !(url.scheme() == "http" &&
         (url.host() == "127.0.0.1" || url.host() == "localhost" ||
          url.host() == "::1")))) {
    fail(error, "无效或不安全的远程 Payload 地址");
    return {};
  }
  auto reader = QSharedPointer<OugaHttpRangeReader>(
      new OugaHttpRangeReader(url, cancel));
  if (!reader->probe(error))
    return {};
  return reader;
}

QByteArray OugaHttpRangeReader::requestRange(quint64 offset, quint64 length,
                                             quint64 *reportedTotal,
                                             QString *error) const {
  if (m_cancel && m_cancel->load()) {
    fail(error, "远程 Payload 读取已取消");
    return {};
  }
  if (!length || length > quint64(std::numeric_limits<qsizetype>::max()) ||
      offset > quint64(std::numeric_limits<qint64>::max()) ||
      length - 1 > quint64(std::numeric_limits<qint64>::max()) - offset) {
    fail(error, "远程 Range 范围无效");
    return {};
  }
  const quint64 end = offset + length - 1;
  // Each worker retains its own manager/connection pool; no cross-thread
  // QObject access, and consecutive ranges can reuse TCP/TLS connections.
  static QThreadStorage<QNetworkAccessManager *> managers;
  if (!managers.hasLocalData())
    managers.setLocalData(new QNetworkAccessManager);
  auto network = managers.localData();
  QNetworkRequest request(m_url);
  request.setRawHeader("Range", "bytes=" + QByteArray::number(offset) +
                                   "-" + QByteArray::number(end));
  request.setRawHeader("Accept-Encoding", "identity");
  if (!m_validator.isEmpty())
    request.setRawHeader("If-Range", m_validator);
  request.setTransferTimeout(30000);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                      QNetworkRequest::ManualRedirectPolicy);
  QNetworkReply *reply = network->get(request);
  reply->setReadBufferSize(qint64(qMin<quint64>(length + 1, 64 * 1024)));
  QEventLoop loop;
  QTimer timeout, cancellation;
  timeout.setSingleShot(true);
  timeout.setInterval(30000);
  cancellation.setInterval(50);
  QString failure;
  QByteArray body;
  quint64 total = 0;
  bool headersValid = false;
  // Reject an ignored Range at headers, not after buffering a multi-GB 200
  // response. Never accept more bytes than requested, even without Length.
  const auto validateHeaders = [&] {
    if (headersValid || !failure.isEmpty())
      return;
    const int status = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (!status)
      return;
    if (status != 206) {
      failure = QString("远程服务器不支持按需 Range（HTTP %1），不会下载完整包或跟随重定向").arg(status);
    } else {
      const QByteArray encoding = reply->rawHeader("Content-Encoding").trimmed().toLower();
      const auto match = QRegularExpression("^bytes ([0-9]+)-([0-9]+)/([0-9]+)$")
          .match(QString::fromLatin1(reply->rawHeader("Content-Range")).trimmed());
      bool okStart = false, okEnd = false, okTotal = false, okLength = false;
      const quint64 actualStart = match.captured(1).toULongLong(&okStart);
      const quint64 actualEnd = match.captured(2).toULongLong(&okEnd);
      total = match.captured(3).toULongLong(&okTotal);
      const QByteArray contentLength = reply->rawHeader("Content-Length");
      const quint64 declaredLength = contentLength.toULongLong(&okLength);
      if ((!encoding.isEmpty() && encoding != "identity") ||
          !match.hasMatch() || !okStart || !okEnd || !okTotal ||
          actualStart != offset || actualEnd != end || total <= end ||
          total > quint64(std::numeric_limits<qint64>::max()) ||
          (m_size && total != m_size) ||
          (!contentLength.isEmpty() && (!okLength || declaredLength != length)))
        failure = "远程服务器返回了无效或变化的 Content-Range，拒绝拼接响应";
      const QByteArray etag = reply->rawHeader("ETag");
      const QByteArray modified = reply->rawHeader("Last-Modified");
      if (failure.isEmpty() &&
          ((!m_etag.isEmpty() && etag != m_etag) ||
           (!m_modified.isEmpty() && modified != m_modified)))
        failure = "远程 Payload 在提取期间发生变化，拒绝混合不同版本";
    }
    if (!failure.isEmpty())
      reply->abort();
    else
      headersValid = true;
  };
  const auto drain = [&] {
    validateHeaders();
    if (!headersValid || !failure.isEmpty())
      return;
    const quint64 remaining = length - quint64(body.size());
    body += reply->read(qint64(remaining));
    if (reply->bytesAvailable() > 0) {
      failure = "远程 Range 响应超过请求长度，已中止";
      reply->abort();
    }
  };
  QObject::connect(reply, &QNetworkReply::metaDataChanged, &loop, validateHeaders);
  QObject::connect(reply, &QNetworkReply::readyRead, &loop, drain);
  QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
  QObject::connect(&timeout, &QTimer::timeout, &loop, [&] {
    failure = "远程 Range 请求超时";
    reply->abort();
  });
  QObject::connect(&cancellation, &QTimer::timeout, &loop, [&] {
    if (m_cancel && m_cancel->load()) {
      failure = "远程 Payload 读取已取消";
      reply->abort();
    }
  });
  timeout.start();
  cancellation.start();
  if (!reply->isFinished())
    loop.exec();
  timeout.stop();
  cancellation.stop();
  drain();
  if (m_cancel && m_cancel->load())
    failure = "远程 Payload 读取已取消";
  if (failure.isEmpty() && reply->error() != QNetworkReply::NoError)
    // Do not expose URLs/query credentials from QNetworkReply::errorString().
    failure = QString("远程 Range 请求失败（网络错误 %1）").arg(int(reply->error()));
  if (failure.isEmpty() && (!headersValid || quint64(body.size()) != length))
    failure = "远程 Range 响应长度不符，拒绝截断数据";
  if (failure.isEmpty() && !m_size) {
    // Probe runs before publication; these values are immutable during reads.
    m_etag = reply->rawHeader("ETag");
    m_modified = reply->rawHeader("Last-Modified");
    m_validator = !m_etag.isEmpty() && !m_etag.startsWith("W/")
        ? m_etag : m_modified;
  }
  delete reply;
  if (!failure.isEmpty()) {
    fail(error, failure);
    return {};
  }
  if (reportedTotal)
    *reportedTotal = total;
  return body;
}

bool OugaHttpRangeReader::probe(QString *error) {
  quint64 total = 0;
  if (requestRange(0, 1, &total, error).size() != 1 || !total)
    return false;
  m_size = total;
  return true;
}

void OugaHttpRangeReader::cacheBlock(quint64 offset,
                                     const QByteArray &data) const {
  m_cache.remove(offset);
  m_cacheOrder.removeAll(offset);
  m_cache.insert(offset, data);
  m_cacheOrder.append(offset);
  while (m_cacheOrder.size() > maxCachedBlocks) {
    const quint64 old = m_cacheOrder.takeFirst();
    m_cache.remove(old);
  }
}

QByteArray OugaHttpRangeReader::read(quint64 offset, quint64 length,
                                     QString *error) const {
  if (!validRange(offset, length, m_size)) {
    fail(error, "远程 Payload 读取范围越界");
    return {};
  }
  if (!length)
    return {};
  if (length > quint64(std::numeric_limits<qsizetype>::max())) {
    fail(error, "远程 Payload 读取块过大");
    return {};
  }
  // Operation-sized reads are exact, parallel requests. Only small metadata
  // reads use bounded read-ahead; never hold the cache lock during network I/O.
  if (length >= cacheBlockSize) {
    quint64 total = 0;
    return requestRange(offset, length, &total, error);
  }
  QByteArray output;
  output.resize(qsizetype(length));
  quint64 position = offset;
  qsizetype written = 0;
  while (position < offset + length) {
    if (m_cancel && m_cancel->load()) {
      fail(error, "远程 Payload 读取已取消");
      return {};
    }
    const quint64 block = (position / cacheBlockSize) * cacheBlockSize;
    const quint64 blockLength = qMin(cacheBlockSize, m_size - block);
    QByteArray data;
    {
      QMutexLocker lock(&m_mutex);
      data = m_cache.value(block);
      if (!data.isEmpty()) {
        m_cacheOrder.removeAll(block);
        m_cacheOrder.append(block);
      }
    }
    if (data.size() != qsizetype(blockLength)) {
      quint64 total = 0;
      data = requestRange(block, blockLength, &total, error);
      if (data.size() != qsizetype(blockLength))
        return {};
      QMutexLocker lock(&m_mutex);
      cacheBlock(block, data);
    }
    const quint64 begin = position - block;
    const quint64 take = qMin(blockLength - begin, offset + length - position);
    std::memcpy(output.data() + written, data.constData() + begin, size_t(take));
    written += qsizetype(take);
    position += take;
  }
  return output;
}
