#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <cstring>
#include "downloadfilewriter.h"

// Produces bytes on demand rather than storing a complete test response.
class GeneratedReply : public QNetworkReply
{
public:
    GeneratedReply()
    {
        setUrl(QUrl("https://example.test/download"));
        open(QIODevice::ReadOnly);
    }
    void feed(qint64 size, bool notify = true)
    {
        available += size;
        if (notify) emit readyRead();
    }
    void complete()
    {
        setFinished(true);
        emit finished();
    }
    void abort() override
    {
        setError(OperationCanceledError, "cancelled test transfer");
        setFinished(true);
        emit finished();
    }
    qint64 bytesAvailable() const override { return available + QNetworkReply::bytesAvailable(); }
    qint64 largestRead = 0;
protected:
    qint64 readData(char *data, qint64 maxSize) override
    {
        largestRead = qMax(largestRead, maxSize);
        const qint64 count = qMin(maxSize, available);
        if (count == 0) return isFinished() ? -1 : 0;
        std::memset(data, 'x', static_cast<size_t>(count));
        available -= count;
        return count;
    }
private:
    qint64 available = 0;
};

class ConfigDownloadTests : public QObject
{
    Q_OBJECT
private slots:
    void streamsLargeFileAndCommitsAtomically();
    void networkFailurePreservesExistingFile();
    void destructionRemovesTemporaryFile();
    void timeout_data();
    void timeout();
    void emptyResponseRejected();
    void finalBytesAreDrained();
    void invalidDestinationRejected();
private:
    static QByteArray contents(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return {};
        return file.readAll();
    }
    static void oldFile(const QString &path)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("keep old file"), qint64(13));
    }
};

void ConfigDownloadTests::streamsLargeFileAndCommitsAtomically()
{
    QTemporaryDir dir(QDir::tempPath() + "/download-large-XXXXXX");
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("tool.exe");
    oldFile(path);
    GeneratedReply reply;
    DownloadFileWriter writer(path);
    QVERIFY(writer.isOpen());
    writer.attach(&reply);
    const qint64 size = 96 * 1024 * 1024;
    for (qint64 offset = 0; offset < size; offset += 64 * 1024) {
        reply.feed(64 * 1024);
        QCOMPARE(writer.bytesWritten(), offset + 64 * 1024);
    }
    QCOMPARE(contents(path), QByteArray("keep old file"));
    QVERIFY(reply.largestRead <= 64 * 1024);
    QCOMPARE(reply.readBufferSize(), qint64(64 * 1024));
    reply.complete();
    QVERIFY2(writer.finish().isEmpty(), qPrintable(writer.errorString()));
    QCOMPARE(QFileInfo(path).size(), size);
    QCOMPARE(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden).size(), 1);
}

void ConfigDownloadTests::networkFailurePreservesExistingFile()
{
    QTemporaryDir dir(QDir::tempPath() + "/download-failure-XXXXXX");
    const QString path = dir.filePath("tool.exe");
    oldFile(path);
    GeneratedReply reply;
    DownloadFileWriter writer(path);
    QVERIFY(writer.isOpen());
    writer.attach(&reply);
    reply.feed(65536);
    reply.abort();
    QVERIFY(!writer.finish().isEmpty());
    QCOMPARE(contents(path), QByteArray("keep old file"));
    QCOMPARE(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden).size(), 1);
}

void ConfigDownloadTests::destructionRemovesTemporaryFile()
{
    QTemporaryDir dir(QDir::tempPath() + "/download-destroy-XXXXXX");
    const QString path = dir.filePath("tool.zip");
    oldFile(path);
    auto *reply = new GeneratedReply;
    auto *writer = new DownloadFileWriter(path);
    writer->attach(reply);
    reply->feed(65536);
    delete reply; // The reply owns the writer and its still-open QSaveFile.
    QCOMPARE(contents(path), QByteArray("keep old file"));
    QCOMPARE(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden).size(), 1);
}

void ConfigDownloadTests::timeout_data()
{
    QTest::addColumn<bool>("firstByte");
    QTest::newRow("first-byte") << true;
    QTest::newRow("stalled-transfer") << false;
}

void ConfigDownloadTests::timeout()
{
    QFETCH(bool, firstByte);
    QTemporaryDir dir(QDir::tempPath() + "/download-timeout-XXXXXX");
    const QString path = dir.filePath("tool.exe");
    oldFile(path);
    GeneratedReply reply;
    DownloadFileWriter writer(path);
    writer.attach(&reply, 20);
    if (!firstByte) reply.feed(64);
    QTRY_VERIFY_WITH_TIMEOUT(writer.timedOut(), 1000);
    QVERIFY(!writer.finish().isEmpty());
    QVERIFY(writer.errorString().contains(firstByte ? "首字节" : "停滞"));
    QCOMPARE(contents(path), QByteArray("keep old file"));
    QCOMPARE(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden).size(), 1);
}

void ConfigDownloadTests::emptyResponseRejected()
{
    QTemporaryDir dir(QDir::tempPath() + "/download-empty-XXXXXX");
    const QString path = dir.filePath("tool.exe");
    GeneratedReply reply;
    DownloadFileWriter writer(path);
    writer.attach(&reply);
    reply.complete();
    QVERIFY(writer.finish().contains("为空"));
    QVERIFY(!QFile::exists(path));
}

void ConfigDownloadTests::finalBytesAreDrained()
{
    QTemporaryDir dir(QDir::tempPath() + "/download-final-XXXXXX");
    const QString path = dir.filePath("tool.exe");
    GeneratedReply reply;
    DownloadFileWriter writer(path);
    writer.attach(&reply);
    reply.feed(42, false);
    reply.complete();
    QVERIFY(writer.finish().isEmpty());
    QCOMPARE(contents(path), QByteArray(42, 'x'));
}

void ConfigDownloadTests::invalidDestinationRejected()
{
    QTemporaryDir dir(QDir::tempPath() + "/download-invalid-XXXXXX");
    DownloadFileWriter writer(dir.filePath("missing/tool.exe"));
    QVERIFY(!writer.isOpen());
    QVERIFY(!writer.errorString().isEmpty());
}

QTEST_GUILESS_MAIN(ConfigDownloadTests)
#include "configdownloadtests.moc"
