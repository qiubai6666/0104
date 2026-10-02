#include "ougaromservice.h"
#include "ougapackage.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUrlQuery>
#include <QtConcurrent>
OugaRomService::OugaRomService(QObject *parent) : QObject(parent) {}
bool OugaRomService::safeUrl(const QUrl &u) {
  return u.isValid() && !u.host().isEmpty() && u.userInfo().isEmpty() &&
         (u.scheme() == "https" ||
          (u.scheme() == "http" &&
           (u.host() == "127.0.0.1" || u.host() == "localhost" ||
            u.host() == "::1")));
}
QJsonValue OugaRomService::field(const QJsonObject &o, const QString &n) {
  for (auto i = o.begin(); i != o.end(); ++i)
    if (i.key().compare(n, Qt::CaseInsensitive) == 0)
      return i.value();
  return {};
}
QStringList OugaRomService::items(const QJsonObject &o) {
  QStringList items;
  for (const auto &v : field(o, "items").toArray()) {
    QString n =
        v.isString() ? v.toString() : field(v.toObject(), "name").toString();
    if (!n.trimmed().isEmpty() && !items.contains(n))
      items << n;
  }
  return items;
}
void OugaRomService::request(const QString &endpoint,
                             const QJsonObject &parameters) {
  if (busy())
    return;
  if (!safeUrl(m_base) ||
      !QStringList{"/series", "/devices", "/versions", "/download-link"}
           .contains(endpoint)) {
    emit finished(false,
                  "请配置自己的有效 HTTPS ROM 服务；默认不连接任何作者服务");
    return;
  }
  QUrl url = m_base;
  url.setPath(url.path().remove(QRegularExpression("/$")) + endpoint);
  QNetworkRequest req;
  req.setTransferTimeout(30000);
  req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                   QNetworkRequest::ManualRedirectPolicy);
  if (endpoint != "/download-link") {
    QUrlQuery q;
    for (auto i = parameters.begin(); i != parameters.end(); ++i)
      q.addQueryItem(i.key(), i.value().toString());
    url.setQuery(q);
    req.setUrl(url);
    m_reply = m_network.get(req);
  } else {
    req.setUrl(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    m_reply = m_network.post(
        req, QJsonDocument(parameters).toJson(QJsonDocument::Compact));
  }
  connect(m_reply, &QNetworkReply::downloadProgress, this,
          [this](qint64 n, qint64) {
            if (n > 4 * 1024 * 1024 && m_reply)
              m_reply->abort();
          });
  connect(m_reply, &QNetworkReply::finished, this, [this, endpoint] {
    auto r = m_reply;
    m_reply = nullptr;
    int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QByteArray body = r->readAll();
    bool ok = r->error() == QNetworkReply::NoError && status == 200 &&
              body.size() <= 4 * 1024 * 1024;
    r->deleteLater();
    QJsonParseError e;
    auto doc = QJsonDocument::fromJson(body, &e);
    if (!ok || e.error != QJsonParseError::NoError || !doc.isObject()) {
      emit finished(false, "ROM 服务响应无效，HTTP " + QString::number(status) +
                               "（不输出 URL、凭证或响应正文）");
      return;
    }
    emit response(endpoint, doc.object());
    emit finished(true, "ROM 服务请求完成");
  });
}
void OugaRomService::download(const QUrl &url, const QString &destination,
                              const QMap<QByteArray, QByteArray> &headers,
                              const QByteArray &sha256, qint64 length) {
  if (busy())
    return;
  if (length < -1 || !safeUrl(url) || QFileInfo::exists(destination) ||
      !QFileInfo(destination).dir().exists() ||
      (!sha256.isEmpty() && sha256.size() != 32)) {
    emit finished(false, "下载 URL/校验值/目标无效；目标文件不得已存在");
    return;
  }
  m_original = url;
  m_url = url;
  m_destination = destination;
  m_headers.clear();
  m_expectedHash = sha256;
  m_expectedLength = length;
  m_offset = 0;
  m_total = -1;
  m_redirects = 0;
  m_error.clear();
  m_cancelled = false;
  m_validator.clear();
  for (auto i = headers.begin(); i != headers.end(); ++i) {
    QByteArray n = i.key().toLower();
    if (!QRegularExpression("^[A-Za-z0-9!#$%&'*+.^_`|~-]+$")
             .match(QString::fromLatin1(n))
             .hasMatch() ||
        i.value().contains('\r') || i.value().contains('\n')) {
      emit finished(false, "无效请求头");
      return;
    }
    if (n == "host" || n == "range" || n == "if-range" ||
        n == "content-length" || n == "accept-encoding" || n == "connection" ||
        n == "transfer-encoding")
      continue;
    m_headers[i.key()] = i.value();
  }
  QString part = destination + ".part";
  QFileInfo info(part);
  if (info.isSymLink() || info.isJunction()) {
    emit finished(false, "断点文件不能为链接");
    return;
  }
  if (info.exists() && info.size() > 0) {
    QFile meta(destination + ".resume.json");
    if (!meta.open(QIODevice::ReadOnly)) {
      emit finished(false, "已有断点缺少验证元数据；请选择新的保存名称");
      return;
    }
    auto o = QJsonDocument::fromJson(meta.readAll()).object();
    QString key = QString::fromLatin1(
        QCryptographicHash::hash(url.toEncoded(), QCryptographicHash::Sha256)
            .toHex());
    m_validator = o["validator"].toString().toUtf8();
    if (o["urlHash"].toString() != key || m_validator.isEmpty()) {
      emit finished(false, "断点 URL/验证条件不符；保留旧文件，请选新名称");
      return;
    }
    m_offset = info.size();
    m_total = o["total"].toString().toLongLong();
    if (m_total <= m_offset) {
      emit finished(false, "断点长度无效；请保留排错并另选输出名称");
      return;
    }
  }
  m_file.setFileName(part);
  if (!m_file.open(QIODevice::WriteOnly |
                   (info.exists() ? QIODevice::Append : QIODevice::NewOnly))) {
    emit finished(false, "不能打开下载断点文件");
    return;
  }
  startDownload(url);
}
void OugaRomService::startDownload(const QUrl &url) {
  m_url = url;
  m_headersOk = false;
  QNetworkRequest req(url);
  req.setTransferTimeout(60000);
  req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                   QNetworkRequest::ManualRedirectPolicy);
  req.setRawHeader("Accept-Encoding", "identity");
  for (auto i = m_headers.begin(); i != m_headers.end(); ++i)
    req.setRawHeader(i.key(), i.value());
  if (m_offset) {
    req.setRawHeader("Range", "bytes=" + QByteArray::number(m_offset) + "-");
    req.setRawHeader("If-Range", m_validator);
  }
  m_reply = m_network.get(req);
  connect(m_reply, &QNetworkReply::readyRead, this, &OugaRomService::consume);
  connect(m_reply, &QNetworkReply::finished, this,
          &OugaRomService::completeDownload);
}
bool OugaRomService::validateHeaders() {
  if (m_headersOk)
    return true;
  int status =
      m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  if (status >= 300 && status < 400)
    return false;
  QByteArray encoding = m_reply->rawHeader("Content-Encoding");
  if (!encoding.isEmpty() && encoding != "identity") {
    fail("不接受带内容编码的 ROM 响应");
    return false;
  }
  bool lenOk = false;
  qint64 length = m_reply->rawHeader("Content-Length").toLongLong(&lenOk);
  if (!lenOk)
    length = -1;
  if (m_offset) {
    auto m =
        QRegularExpression("^bytes ([0-9]+)-([0-9]+)/([0-9]+)$")
            .match(QString::fromLatin1(m_reply->rawHeader("Content-Range")));
    if (status != 206 || !m.hasMatch() ||
        m.captured(1).toLongLong() != m_offset ||
        m.captured(3).toLongLong() != m_total ||
        m.captured(2).toLongLong() != m_total - 1 ||
        (length >= 0 && length != m_total - m_offset)) {
      fail("无效 Range 响应；未拼接任何响应内容，保留断点");
      return false;
    }
    QByteArray actual = m_reply->rawHeader("ETag");
    if (!m_validator.startsWith('"'))
      actual = m_reply->rawHeader("Last-Modified");
    if (actual != m_validator) {
      fail("断点实体校验条件已变化，拒绝拼接");
      return false;
    }
  } else {
    if (status != 200) {
      fail("下载 HTTP 状态无效：" + QString::number(status));
      return false;
    }
    m_total = length;
    if (m_total < 0 && m_expectedHash.isEmpty() && m_expectedLength < 0) {
      fail("响应无长度或摘要，无法验证完整下载");
      return false;
    }
    QByteArray etag = m_reply->rawHeader("ETag");
    m_validator = !etag.isEmpty() && !etag.startsWith("W/")
                      ? etag
                      : m_reply->rawHeader("Last-Modified");
  }
  if (m_expectedLength >= 0 && m_total >= 0 && m_total != m_expectedLength) {
    fail("服务声明长度与预期不符");
    return false;
  }
  QJsonObject meta;
  meta["urlHash"] =
      QString::fromLatin1(QCryptographicHash::hash(m_original.toEncoded(),
                                                   QCryptographicHash::Sha256)
                              .toHex());
  meta["validator"] = QString::fromLatin1(m_validator);
  meta["total"] = QString::number(m_total);
  QSaveFile f(m_destination + ".resume.json");
  const QByteArray metadata = QJsonDocument(meta).toJson();
  if (!f.open(QIODevice::WriteOnly) || f.write(metadata) != metadata.size() ||
      !f.commit()) {
    fail("无法保存断点验证信息");
    return false;
  }
  m_headersOk = true;
  return true;
}
void OugaRomService::fail(const QString &e) {
  if (m_error.isEmpty())
    m_error = e;
  if (m_reply)
    m_reply->abort();
}
void OugaRomService::consume() {
  if (!m_reply || !m_error.isEmpty() || !validateHeaders())
    return;
  QByteArray data = m_reply->readAll();
  if ((m_total >= 0 && data.size() > m_total - m_file.size()) ||
      (m_expectedLength >= 0 &&
       data.size() > m_expectedLength - m_file.size()) ||
      m_file.write(data) != data.size()) {
    fail("下载超过声明长度或写盘失败");
    return;
  }
  emit progress(m_file.size(), m_total);
}
void OugaRomService::completeDownload() {
  auto r = m_reply;
  int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  if (status >= 300 && status < 400 && m_error.isEmpty() && !m_cancelled) {
    QUrl to = r->url().resolved(
        r->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl());
    if (++m_redirects > 5 || !safeUrl(to)) {
      fail("重定向不安全或次数超限");
    } else {
      if (to.scheme() != m_url.scheme() || to.host() != m_url.host() ||
          to.port() != m_url.port())
        m_headers.clear();
      m_reply = nullptr;
      r->deleteLater();
      startDownload(to);
      return;
    }
  }
  if (m_error.isEmpty() && !m_cancelled) {
    consume();
    if (!m_headersOk && m_error.isEmpty())
      m_error = "下载未收到有效响应头";
  }
  m_reply = nullptr;
  if (!m_file.flush() && m_error.isEmpty())
    m_error = "下载写盘刷新失败";
  m_file.close();
  bool ok =
      r->error() == QNetworkReply::NoError && m_error.isEmpty() && !m_cancelled;
  r->deleteLater();
  qint64 size = QFileInfo(m_destination + ".part").size();
  if (ok && ((m_total >= 0 && size != m_total) ||
             (m_expectedLength >= 0 && size != m_expectedLength))) {
    ok = false;
    m_error = "下载长度不匹配";
  }
  if (!ok) {
    emit finished(false, m_error.isEmpty()
                             ? "下载中断/取消；保留断点，未作为完整包使用"
                             : m_error);
    return;
  }
  m_verifying = true;
  QString part = m_destination + ".part";
  QByteArray hash = m_expectedHash;
  auto w = new QFutureWatcher<QString>(this);
  connect(w, &QFutureWatcher<QString>::finished, this, [this, w, part] {
    QString e = w->result();
    w->deleteLater();
    m_verifying = false;
    if (m_cancelled || !e.isEmpty()) {
      emit finished(false, e.isEmpty() ? "已取消，保留下载文件" : e);
      return;
    }
    if (!QFile::rename(part, m_destination)) {
      emit finished(false, "无法完成下载文件命名，保留 .part");
      return;
    }
    emit downloaded(m_destination);
    emit finished(true, "下载及完整性条件验证完成；不会自动刷写");
  });
  w->setFuture(QtConcurrent::run([part, hash] {
    QString e;
    QByteArray actual = OugaPackage::digest(part, &e);
    if (!e.isEmpty())
      return e;
    return !hash.isEmpty() && actual != hash ? QString("下载 SHA-256 不匹配")
                                             : QString();
  }));
}
void OugaRomService::cancel() {
  m_cancelled = true;
  if (m_reply)
    m_reply->abort();
}
