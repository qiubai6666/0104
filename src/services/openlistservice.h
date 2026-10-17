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
#include <QVector>
#include <QJsonObject>

namespace OpenList {
struct Entry {
    QString name, relativePath, remotePath, modified;
    qint64 size = -1;
    bool directory = false;
};
QString redact(QString text);
bool safeComponent(const QString &name);
bool safeDirectory(const QString &path, bool create);
QString resolveDownloadRoot(const QString &applicationDirectory, const QString &localDataDirectory);
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
    // directory 非空时直接保存到该目录（文件名扁平化，不保留云端子目录）；
    // 为空时沿用默认下载根的“软件”目录并保留相对路径。
    bool download(const OpenList::Entry &entry, const QString &directory = QString());
    // Direct HTTPS downloads share the same range, validation and cleanup engine.
    bool downloadDirect(const OpenList::Entry &entry, const QUrl &url);
    void cancel();
    bool downloading() const { return m_active; }
    static bool https(const QUrl &url);
    static QString sha256(const QJsonObject &metadata);
signals:
    void listingReady(const QList<OpenList::Entry> &entries, const QString &error);
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
    void pump(quint64 generation);
    QNetworkRequest request(const QUrl &url) const;
    struct Segment {
        QPointer<QNetworkReply> reply;
        qint64 start = 0;
        qint64 end = -1;
        qint64 received = 0;
        bool finished = false;
    };
    void getFile(const QUrl &url, int redirects, quint64 generation);
    void startParallel(const QUrl &url, quint64 generation);
    void fallbackToSingleStream(const QUrl &url, quint64 generation);
    bool consume(QNetworkReply *reply);
    bool consumeSegment(int index, QNetworkReply *reply);
    void fail(const QString &reason);
    bool prepareFile();
    QString m_root, m_path, m_part;
    QString m_targetDirectory;
    QNetworkAccessManager *m_network;
    QPointer<QNetworkReply> m_reply;
    QList<QPointer<QNetworkReply>> m_lists;
    QVector<Segment> m_segments;
    qint64 m_parallelReceived = 0;
    bool m_parallel = false;
    bool m_parallelFallback = false;
    Listing m_listing;
    quint64 m_listGeneration = 0, m_downloadGeneration = 0;
    bool m_active = false;
    OpenList::Entry m_entry;
    QFile m_file;
    QByteArray m_expectedHash;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
    QElapsedTimer m_clock;
};
#endif


