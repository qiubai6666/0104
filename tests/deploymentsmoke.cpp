#include <QApplication>
#include <QDebug>
#include <QImageReader>
#include <QPixmap>
#include <QPushButton>
#include <QSslSocket>
#include <QVBoxLayout>
#include <QWidget>
#include <cstdio>

// 只验证部署包的插件、绘制和 TLS 能力，不显示窗口、不联网、不提取资源或操作设备。
int main(int argc, char *argv[])
{
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
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
    if (!QSslSocket::supportsSsl()
        || QSslSocket::activeBackend() != QStringLiteral("schannel")
        || !QSslSocket::isProtocolSupported(QSsl::TlsV1_2)) {
        qCritical() << "Schannel TLS backend is unavailable:" << QSslSocket::availableBackends();
        return 5;
    }
    qInfo() << "Deployment smoke passed: Windows plugin, widget rendering, PNG, Schannel TLS 1.2";
    qInfo() << "Plugin paths:" << application.libraryPaths();
    qInfo() << "TLS backends:" << QSslSocket::availableBackends();
    return 0;
}
