#include <QApplication>
#include <QDebug>
#include <QImageReader>
#include <QLibrary>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSslSocket>
#include <QSvgRenderer>
#include <QVBoxLayout>
#include <QWidget>
#include <cstdio>

// 只验证部署包的插件、绘制和 TLS
// 能力，不显示窗口、不联网、不提取资源或操作设备。
int main(int argc, char *argv[]) {
  qInstallMessageHandler(
      [](QtMsgType, const QMessageLogContext &, const QString &message) {
        std::fprintf(stderr, "%s\n", message.toUtf8().constData());
      });
  QApplication application(argc, argv);
  if (application.platformName() != QStringLiteral("windows")) {
    qCritical() << "Windows platform plugin was not loaded";
    return 1;
  }
  QWidget panel;
  QVBoxLayout layout(&panel);
  layout.addWidget(new QPushButton(QStringLiteral("Orange Tools"), &panel));
  panel.resize(300, 164);
  panel.ensurePolished();
  if (panel.grab().isNull()) {
    qCritical() << "Widget rendering failed";
    return 2;
  }
  if (application.arguments().size() != 2) {
    qCritical() << "Pass the path of qiubai/icon.png";
    return 3;
  }
  QImageReader reader(application.arguments().at(1));
  if (reader.read().isNull()) {
    qCritical() << "PNG decoding failed:" << reader.errorString();
    return 4;
  }
  if (!QSslSocket::supportsSsl() ||
      QSslSocket::activeBackend() != QStringLiteral("schannel") ||
      !QSslSocket::isProtocolSupported(QSsl::TlsV1_2)) {
    qCritical() << "Schannel TLS backend is unavailable:"
                << QSslSocket::availableBackends();
    return 5;
  }
  QLibrary concurrent(application.applicationDirPath() + "/Qt6Concurrent");
  if (!concurrent.load()) {
    qCritical() << "Deployed Qt Concurrent library cannot be loaded:"
                << concurrent.errorString();
    return 6;
  }
  for (const QString &name :
       {QStringLiteral("folder"), QStringLiteral("shield"),
        QStringLiteral("power")}) {
    QSvgRenderer renderer(QStringLiteral(":/ouga/") + name +
                          QStringLiteral(".svg"));
    if (!renderer.isValid()) {
      qCritical() << "Ouga SVG resource cannot be loaded:" << name;
      return 7;
    }
    QImage icon(32, 32, QImage::Format_ARGB32_Premultiplied);
    icon.fill(Qt::transparent);
    QPainter painter(&icon);
    renderer.render(&painter);
    painter.end();
    bool visible = false;
    for (int y = 0; y < icon.height(); ++y)
      for (int x = 0; x < icon.width(); ++x)
        visible |= qAlpha(icon.pixel(x, y)) != 0;
    if (!visible) {
      qCritical() << "Ouga SVG resource rendered blank:" << name;
      return 8;
    }
  }
  qInfo() << "Deployment smoke passed: Windows plugin, widget rendering, PNG, "
             "Schannel TLS 1.2, Qt Concurrent, Ouga SVG icons";
  qInfo() << "Plugin paths:" << application.libraryPaths();
  qInfo() << "TLS backends:" << QSslSocket::availableBackends();
  return 0;
}
