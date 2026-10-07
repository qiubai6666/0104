#ifndef OPENLISTSERVICE_H
#define OPENLISTSERVICE_H
#include <QObject>
#include <QNetworkRequest>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QFile>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QQueue>
#include <QSet>
#include <QJsonObject>

namespace OpenList {
struct Entry {
    QString name, relativePath, remotePath, modified;
    qint64 size = -1;
    bool module = false;
    bool directory = false;
};
QString redact(QString text);
bool safeComponent(const QString &name);
bool safeDirectory(const QString &path, bool create);
QString defaultDownloadRoot();
}
Q_DECLARE_METATYPE(OpenList::Entry)
Q_DECLARE_METATYPE(QList<OpenList::Entry>)

// Read-only visitor API. No credentials or administrator tokens are stored.
class OpenListService : public QObject {
    Q_OBJECT
public:
    explicit OpenListService(QObject *parent = nullptr, QNetworkAccessManager *network = nullptr,
                             const QString &downloadRoot = QString());
    ~OpenListService() override;
    void refresh();
    bool download(const OpenList::Entry &entry);
    void cancel();
    bool downloading() const { return m_active; }
    static bool https(const QUrl &url);
    static QString sha256(const QJsonObject &metadata);
signals:
    void listingReady(bool module, const QList<OpenList::Entry> &entries, const QString &error);
    void progress(qint64 received, qint64 total, double bytesPerSecond);
    void ready(const OpenList::Entry &entry, const QString &file);
    void failed(const QString &message);
    void log(const QString &message);
private:
    struct Page { QString path; int page = 1; int depth = 0; };
    struct Listing {
        QQueue<Page> pending;
        QSet<QString> visited;
        QList<OpenList::Entry> entries;
        QString error;
        int requests = 0;
    };
    void pump(bool module, quint64 generation);
    QNetworkRequest request(const QUrl &url) const;
    void getFile(const QUrl &url, int redirects, quint64 generation);
    bool consume(QNetworkReply *reply);
    void fail(const QString &reason);
    bool prepareFile();
    QString m_root, m_path, m_part;
    QNetworkAccessManager *m_network;
    QPointer<QNetworkReply> m_reply;
    QList<QPointer<QNetworkReply>> m_lists;
    Listing m_listing[2];
    quint64 m_listGeneration = 0, m_downloadGeneration = 0;
    bool m_active = false;
    OpenList::Entry m_entry;
    QFile m_file;
    QByteArray m_expectedHash;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
    QElapsedTimer m_clock;
};
#endif


