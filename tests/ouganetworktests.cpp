#include "ouganetworktests.h"
#include "ougaromservice.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUuid>
#include <QtTest>
#include <functional>
#ifndef Q_MOC_RUN
namespace {
QByteArray digest(const QByteArray &b) {
  return QCryptographicHash::hash(b, QCryptographicHash::Sha256);
}
void put(const QString &p, const QByteArray &b) {
  QFile f(p);
  if (!f.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
      f.write(b) != b.size())
    qFatal("Fixture write failed");
}
QByteArray read(const QString &p) {
  QFile f(p);
  return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
struct Request {
  QByteArray method, target, body;
  QMap<QByteArray, QByteArray> headers;
};
class Server : public QTcpServer {
public:
  QList<Request> requests;
  std::function<void(QTcpSocket *, const Request &)> handler;
  Server() {
    if (!listen(QHostAddress::LocalHost))
      qFatal("Loopback server failed");
    connect(this, &QTcpServer::newConnection, this, [this] {
      while (hasPendingConnections()) {
        auto s = nextPendingConnection();
        connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
        connect(s, &QTcpSocket::readyRead, s, [this, s] {
          if (s->property("handled").toBool())
            return;
          QByteArray buffer =
              s->property("buffer").toByteArray() + s->readAll();
          s->setProperty("buffer", buffer);
          int end = buffer.indexOf("\r\n\r\n");
          if (end < 0)
            return;
          const auto lines = buffer.left(end).split('\n');
          Request r;
          auto first = lines[0].trimmed().split(' ');
          r.method = first.value(0);
          r.target = first.value(1);
          for (int i = 1; i < lines.size(); ++i) {
            int n = lines[i].indexOf(':');
            if (n > 0)
              r.headers[lines[i].left(n).trimmed().toLower()] =
                  lines[i].mid(n + 1).trimmed();
          }
          int length = r.headers.value("content-length").toInt();
          if (buffer.size() - end - 4 < length)
            return;
          r.body = buffer.mid(end + 4, length);
          s->setProperty("handled", true);
          requests << r;
          if (handler)
            handler(s, r);
          else
            reply(s, "{}", 200, {{"Content-Type", "application/json"}});
        });
      }
    });
  }
  QUrl url(const QString &p = "/file") const {
    return QUrl("http://127.0.0.1:" + QString::number(serverPort()) + p);
  }
  static void reply(QTcpSocket *s, const QByteArray &body, int status = 200,
                    QMap<QByteArray, QByteArray> headers = {}) {
    if (!headers.contains("Content-Length"))
      headers["Content-Length"] = QByteArray::number(body.size());
    QByteArray out = "HTTP/1.1 " + QByteArray::number(status) +
                     " test\r\nConnection: close\r\n";
    for (auto i = headers.begin(); i != headers.end(); ++i)
      out += i.key() + ": " + i.value() + "\r\n";
    s->write(out + "\r\n" + body);
    s->disconnectFromHost();
  }
};
} // namespace
#endif
void OugaNetworkTests::seed(const QString &file, const QUrl &url,
                            const QByteArray &validator) {
  put(file + ".part", data.left(4));
  put(file + ".resume.json",
      QJsonDocument(
          QJsonObject{
              {"urlHash", QString::fromLatin1(digest(url.toEncoded()).toHex())},
              {"validator", QString::fromLatin1(validator)},
              {"total", QString::number(data.size())}})
          .toJson());
}
void OugaNetworkTests::initTestCase() {
  base = qEnvironmentVariable("ORANGE_TEST_ARTIFACTS");
  QVERIFY2(!base.isEmpty(), "Set isolated TEMP artifact path");
  QVERIFY(QDir().mkpath(base));
}
void OugaNetworkTests::init() {
  dir = base + "/" + QTest::currentTestFunction() + "-" +
        QUuid::createUuid().toString(QUuid::Id128);
  QVERIFY(QDir().mkpath(dir));
}
void OugaNetworkTests::endpointContract_data() {
  QTest::addColumn<QString>("endpoint");
  for (auto p : {"/series", "/devices", "/versions", "/download-link"})
    QTest::newRow(p) << QString(p);
}
void OugaNetworkTests::endpointContract() {
  QFETCH(QString, endpoint);
  Server server;
  server.handler = [](QTcpSocket *s, const Request &) {
    Server::reply(
        s,
        R"({"Items":["A",{"Name":"B"}],"Links":["https://example.invalid/rom"],"RequestHeaders":{"Authorization":"secret"}})");
  };
  OugaRomService service;
  service.setBaseUrl(server.url("/api"));
  QSignalSpy done(&service, &OugaRomService::finished),
      response(&service, &OugaRomService::response);
  QJsonObject p{{"packageType", "afterSales"}, {"device", "中文 空格"}};
  service.request(endpoint, p);
  QTRY_COMPARE(done.count(), 1);
  QVERIFY(done[0][0].toBool());
  QCOMPARE(response.count(), 1);
  auto r = server.requests[0];
  QVERIFY(r.target.startsWith("/api" + endpoint.toUtf8()));
  QCOMPARE(r.method, endpoint == "/download-link" ? QByteArray("POST")
                                                  : QByteArray("GET"));
  if (endpoint == "/download-link")
    QCOMPARE(QJsonDocument::fromJson(r.body).object(), p);
  else
    QVERIFY(r.target.contains("packageType=afterSales"));
  auto obj = response[0][1].toJsonObject();
  QCOMPARE(OugaRomService::items(obj), QStringList({"A", "B"}));
  QVERIFY(OugaRomService::field(obj, "requestHeaders")
              .toObject()
              .contains("Authorization"));
}
void OugaNetworkTests::defaultOffline() {
  OugaRomService service;
  QSignalSpy done(&service, &OugaRomService::finished);
  service.request("/series", {});
  QCOMPARE(done.count(), 1);
  QVERIFY(!done[0][0].toBool());
  QVERIFY(!service.busy());
  QVERIFY(!OugaRomService::safeUrl(QUrl("http://example.invalid/")));
  QVERIFY(!OugaRomService::safeUrl(QUrl("https://user:pass@example.invalid/")));
}
void OugaNetworkTests::downloadIntegrity_data() {
  QTest::addColumn<int>("kind");
  for (int i = 0; i < 7; ++i)
    QTest::newRow(qPrintable(QString::number(i))) << i;
}
void OugaNetworkTests::downloadIntegrity() {
  QFETCH(int, kind);
  Server server;
  server.handler = [this, kind](QTcpSocket *s, const Request &) {
    QMap<QByteArray, QByteArray> h{{"ETag", "\"v1\""}};
    if (kind == 3)
      h["Content-Length"] = "99";
    if (kind == 4)
      h["Content-Encoding"] = "unsupported";
    Server::reply(s, data, kind == 5 ? 403 : 200, h);
  };
  OugaRomService service;
  QSignalSpy done(&service, &OugaRomService::finished),
      files(&service, &OugaRomService::downloaded),
      logs(&service, &OugaRomService::log);
  QString file = dir + "/rom.zip";
  service.download(server.url("/file?token=SECRET"), file,
                   {{"Authorization", "SECRET"}},
                   kind == 1 ? digest("wrong") : digest(data),
                   kind == 2   ? data.size() + 1
                   : kind == 6 ? -2
                               : data.size());
  QTRY_COMPARE(done.count(), 1);
  QCOMPARE(done[0][0].toBool(), kind == 0);
  QCOMPARE(files.count(), kind == 0 ? 1 : 0);
  QCOMPARE(QFileInfo::exists(file), kind == 0);
  if (kind == 0)
    QCOMPARE(read(file), data);
  QVERIFY(!done[0][1].toString().contains("SECRET"));
  for (const auto &row : logs)
    QVERIFY(!row[0].toString().contains("SECRET"));
  QVERIFY(!service.busy());
}
void OugaNetworkTests::resumeValidation_data() {
  QTest::addColumn<int>("kind");
  for (int i = 0; i < 6; ++i)
    QTest::newRow(qPrintable(QString::number(i))) << i;
}
void OugaNetworkTests::resumeValidation() {
  QFETCH(int, kind);
  Server server;
  server.handler = [this, kind](QTcpSocket *s, const Request &) {
    QMap<QByteArray, QByteArray> h{
        {"ETag", kind == 2 ? "\"different\"" : "\"v1\""},
        {"Content-Range", kind == 3   ? "bytes 5-15/16"
                          : kind == 4 ? "bytes 4-15/17"
                                      : "bytes 4-15/16"}};
    Server::reply(s, data.mid(4), kind == 1 ? 200 : 206, h);
  };
  QString file = dir + "/rom.zip";
  seed(file, server.url(), kind == 5 ? QByteArray() : QByteArray("\"v1\""));
  OugaRomService service;
  QSignalSpy done(&service, &OugaRomService::finished);
  service.download(server.url(), file, {}, digest(data), data.size());
  QTRY_COMPARE(done.count(), 1);
  QCOMPARE(done[0][0].toBool(), kind == 0);
  if (kind == 0) {
    QCOMPARE(read(file), data);
    QCOMPARE(server.requests[0].headers.value("range"), QByteArray("bytes=4-"));
    QCOMPARE(server.requests[0].headers.value("if-range"),
             QByteArray("\"v1\""));
  } else {
    QVERIFY(!QFileInfo::exists(file));
    QCOMPARE(read(file + ".part"), data.left(4));
  }
}
void OugaNetworkTests::redirectStripsCredentials() {
  Server first, second;
  first.handler = [&](QTcpSocket *s, const Request &) {
    Server::reply(s, {}, 302, {{"Location", second.url().toEncoded()}});
  };
  second.handler = [this](QTcpSocket *s, const Request &) {
    Server::reply(s, data);
  };
  OugaRomService service;
  QSignalSpy done(&service, &OugaRomService::finished);
  QString file = dir + "/rom";
  service.download(first.url(), file,
                   {{"Authorization", "Bearer SECRET"},
                    {"Cookie", "SECRET"},
                    {"X-Custom-Token", "SECRET"}},
                   digest(data));
  QTRY_COMPARE(done.count(), 1);
  QVERIFY2(done[0][0].toBool(), qPrintable(done[0][1].toString()));
  QVERIFY(first.requests[0].headers.contains("authorization"));
  for (auto n : {"authorization", "cookie", "x-custom-token"})
    QVERIFY(!second.requests[0].headers.contains(n));
  QCOMPARE(read(file), data);
}
void OugaNetworkTests::invalidRedirect() {
  Server server;
  server.handler = [](QTcpSocket *s, const Request &) {
    Server::reply(s, {}, 302,
                  {{"Location", "http://not-local.invalid/SECRET"}});
  };
  OugaRomService service;
  QSignalSpy done(&service, &OugaRomService::finished);
  service.download(server.url(), dir + "/rom");
  QTRY_COMPARE(done.count(), 1);
  QVERIFY(!done[0][0].toBool());
  QVERIFY(!done[0][1].toString().contains("SECRET"));
  QCOMPARE(server.requests.size(), 1);
}
void OugaNetworkTests::cancellationPreservesPartial() {
  Server server;
  server.handler = [this](QTcpSocket *s, const Request &) {
    s->write("HTTP/1.1 200 OK\r\nContent-Length: 16\r\nETag: \"v1\"\r\n\r\n" +
             data.left(4));
  };
  OugaRomService service;
  QSignalSpy done(&service, &OugaRomService::finished);
  connect(&service, &OugaRomService::progress, &service, [&](qint64 n, qint64) {
    if (n > 0)
      service.cancel();
  });
  QString file = dir + "/rom";
  service.download(server.url(), file);
  QTRY_COMPARE(done.count(), 1);
  QVERIFY(!done[0][0].toBool());
  QCOMPARE(read(file + ".part"), data.left(4));
  QVERIFY(!QFileInfo::exists(file));
  QVERIFY(QFileInfo::exists(file + ".resume.json"));
}
void OugaNetworkTests::interruptedThenResume() {
  Server server;
  int attempt = 0;
  server.handler = [&](QTcpSocket *s, const Request &) {
    if (attempt++ == 0)
      Server::reply(s, data.left(4), 200,
                    {{"ETag", "\"v1\""}, {"Content-Length", "16"}});
    else
      Server::reply(s, data.mid(4), 206,
                    {{"ETag", "\"v1\""}, {"Content-Range", "bytes 4-15/16"}});
  };
  OugaRomService service;
  QSignalSpy done(&service, &OugaRomService::finished);
  QString file = dir + "/rom";
  service.download(server.url(), file, {}, digest(data));
  QTRY_COMPARE(done.count(), 1);
  QVERIFY(!done[0][0].toBool());
  QCOMPARE(read(file + ".part"), data.left(4));
  service.download(server.url(), file, {}, digest(data));
  QTRY_COMPARE(done.count(), 2);
  QVERIFY2(done[1][0].toBool(), qPrintable(done[1][1].toString()));
  QCOMPARE(read(file), data);
}
QTEST_GUILESS_MAIN(OugaNetworkTests)
