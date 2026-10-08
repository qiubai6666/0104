#ifndef MTMANAGERSERVICE_H
#define MTMANAGERSERVICE_H
#include <QObject>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QFile>
#include <QElapsedTimer>

class OpenListService;
class MtManagerService final : public QObject {
    Q_OBJECT
public:
    explicit MtManagerService(QObject *parent = nullptr, QNetworkAccessManager *network = nullptr,
                              const QString &downloadRoot = QString());
    ~MtManagerService() override;
    bool active() const { return m_active; }
    void start();
    void cancel();
signals:
    void versionReady(const QString &version);
    void progress(qint64 received, qint64 total, double bytesPerSecond);
    void ready(const QString &version, const QString &file);
    void failed(const QString &message);
    void log(const QString &message);
private:
    void fetchPage();
    void parsePage(QNetworkReply *reply);
    void startDownload(const QUrl &url, const QString &version);
    void finishFailure(const QString &message);
    bool useParallel(QNetworkReply *reply);
    static bool validDownloadUrl(const QUrl &url);
    QString uniqueApkPath(const QString &version) const;
    QNetworkAccessManager *m_network;
    OpenListService *m_downloader;
    QString m_root;
    QPointer<QNetworkReply> m_reply;
    QFile m_file;
    QString m_part, m_path, m_version;
    bool m_active = false;
    qint64 m_received = 0, m_total = -1;
    QElapsedTimer m_clock;
};
#endif
