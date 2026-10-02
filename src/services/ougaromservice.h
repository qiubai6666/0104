#ifndef OUGAROMSERVICE_H
#define OUGAROMSERVICE_H
#include <QFile>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QUrl>
class OugaRomService : public QObject {
  Q_OBJECT
public:
  explicit OugaRomService(QObject *parent = nullptr);
  void setBaseUrl(const QUrl &url) { m_base = url; }
  bool busy() const { return m_reply != nullptr || m_verifying; }
  void request(const QString &endpoint, const QJsonObject &parameters);
  void download(const QUrl &url, const QString &destination,
                const QMap<QByteArray, QByteArray> &headers = {},
                const QByteArray &sha256 = {}, qint64 length = -1);
  void cancel();
  static QJsonValue field(const QJsonObject &object, const QString &name);
  static QStringList items(const QJsonObject &object);
  static bool safeUrl(const QUrl &url);
signals:
  void response(const QString &endpoint, const QJsonObject &object);
  void downloaded(const QString &file);
  void progress(qint64 bytes, qint64 total);
  void finished(bool success, const QString &message);
  void log(const QString &message);

private:
  QNetworkAccessManager m_network;
  QNetworkReply *m_reply = nullptr;
  QUrl m_base, m_original, m_url;
  QString m_destination, m_error;
  QFile m_file;
  QMap<QByteArray, QByteArray> m_headers;
  QByteArray m_expectedHash, m_validator;
  qint64 m_offset = 0, m_total = -1, m_expectedLength = -1;
  int m_redirects = 0;
  bool m_headersOk = false, m_cancelled = false, m_verifying = false;
  void startDownload(const QUrl &url);
  bool validateHeaders();
  void consume();
  void completeDownload();
  void fail(const QString &error);
};
#endif
