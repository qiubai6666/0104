#ifndef DOWNLOADFILEWRITER_H
#define DOWNLOADFILEWRITER_H

#include <QNetworkReply>
#include <QSaveFile>
#include <QTimer>

// A bounded-memory download sink. QSaveFile never falls back to overwriting the
// destination directly: incomplete transfers leave any existing file intact.
class DownloadFileWriter : public QObject
{
public:
    explicit DownloadFileWriter(const QString &path, QObject *parent = nullptr)
        : QObject(parent), file(path)
    {
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly)) failure = file.errorString();
    }

    bool isOpen() const { return file.isOpen(); }
    QString errorString() const { return failure; }
    qint64 bytesWritten() const { return written; }
    bool timedOut() const { return timeout; }

    void attach(QNetworkReply *networkReply, int timeoutMs = 15000)
    {
        reply = networkReply;
        setParent(reply);
        reply->setReadBufferSize(64 * 1024);
        watchdog.setSingleShot(true);
        watchdog.setInterval(timeoutMs);
        connect(&watchdog, &QTimer::timeout, this, [this] {
            timeout = true;
            fail(written == 0 ? QStringLiteral("等待下载首字节超时")
                              : QStringLiteral("下载传输停滞超时"));
        });
        connect(reply, &QNetworkReply::readyRead, this, [this] { drain(); });
        watchdog.start();
    }

    QString finish()
    {
        watchdog.stop();
        if (failure.isEmpty() && reply->error() == QNetworkReply::NoError) drain();
        watchdog.stop();
        if (failure.isEmpty() && reply->error() != QNetworkReply::NoError)
            failure = reply->errorString();
        if (failure.isEmpty() && written == 0) failure = QStringLiteral("下载的文件为空");
        if (!failure.isEmpty()) {
            file.cancelWriting();
            // commit() closes and discards a cancelled QSaveFile immediately.
            if (file.isOpen()) file.commit();
        } else if (!file.commit()) {
            failure = file.errorString();
        }
        return failure;
    }

private:
    void fail(const QString &message)
    {
        if (!failure.isEmpty()) return;
        failure = message;
        file.cancelWriting();
        reply->abort();
    }

    void drain()
    {
        while (failure.isEmpty() && reply->bytesAvailable() > 0) {
            const QByteArray chunk = reply->read(64 * 1024);
            if (chunk.isEmpty()) break;
            if (file.write(chunk) != chunk.size()) {
                fail(file.errorString());
                return;
            }
            written += chunk.size();
            watchdog.start();
        }
    }

    QSaveFile file;
    QTimer watchdog;
    QNetworkReply *reply = nullptr;
    QString failure;
    qint64 written = 0;
    bool timeout = false;
};

#endif
