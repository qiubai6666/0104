#include <QtTest>
#include <QApplication>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QNetworkAccessManager>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QUuid>
#include <QSignalSpy>
#include <QFile>
#include <QTableWidget>
#include <QPushButton>
#include <QLabel>
#include <QGroupBox>
#include <QLineEdit>
#include <QScreen>
#include <QProcess>
#include <QThread>
#include <QScopeGuard>
#include <cstdio>
#include <algorithm>
#ifdef Q_OS_WIN
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#endif
#include "openlistservice.h"
#include "openlistinstaller.h"
#include "openlistwindow.h"
#include "resourceextractor.h"
#include "deviceoperationlease.h"
#include "shellcommand.h"

QString ResourceExtractor::getAdbPath() { return QStringLiteral("test-no-real-adb.exe"); }
QString ResourceExtractor::getFastbootPath() { return QStringLiteral("test-no-real-fastboot.exe"); }
QString ResourceExtractor::getResourcePath() { return QStringLiteral("test-no-real-tools"); }
QString ResourceExtractor::getNeilImagePath() { return {}; }

namespace {
QByteArray api(QJsonObject data, int code=200) { return QJsonDocument(QJsonObject{{"code",code},{"data",data}}).toJson(); }
QJsonObject entry(QString name,bool dir=false,qint64 size=3) {
    return {{"name",name},{"is_dir",dir},{"size",double(size)},{"modified","2026-10-07T08:00:00Z"}};
}
bool writeFile(const QString &path,const QByteArray &bytes) { QFile f(path); return f.open(QIODevice::WriteOnly) && f.write(bytes)==bytes.size(); }
// A hypothetical SFX directory outside the source tree; never write to this path.
QString sfxDirectory() {
    return QDir(QDir::rootPath()).filePath("OpenList-sfx-fixture-" + QUuid::createUuid().toString(QUuid::Id128));
}
class Reply final : public QNetworkReply {
public:
    QByteArray bytes; qint64 offset=0; bool done=false;
    Reply(const QNetworkRequest &r,QByteArray b,int status,QUrl redirect={},bool defer=false):bytes(b) {
        setRequest(r);setUrl(r.url());setAttribute(QNetworkRequest::HttpStatusCodeAttribute,status);
        if (!redirect.isEmpty()) setAttribute(QNetworkRequest::RedirectionTargetAttribute,redirect);
        open(QIODevice::ReadOnly);
        if (!defer) QTimer::singleShot(0,this,[this] { if(done)return; emit readyRead(); emit downloadProgress(bytes.size(),bytes.size()); done=true;setFinished(true); emit finished(); });
    }
    void setHeader(const QByteArray &name, const QByteArray &value) { setRawHeader(name, value); }
    void abort() override { if(done)return;done=true;setError(OperationCanceledError,"cancelled");setFinished(true);emit finished(); }
    qint64 bytesAvailable() const override { return bytes.size()-offset+QNetworkReply::bytesAvailable(); }
protected:
    qint64 readData(char *data,qint64 max) override {auto n=qMin(max,qint64(bytes.size())-offset);if(n<=0)return -1;memcpy(data,bytes.constData()+offset,size_t(n));offset+=n;return n;}
};
class Network final : public QNetworkAccessManager {
public:
    std::function<Reply*(const QNetworkRequest&,const QJsonObject&)> handler;
    QList<QNetworkRequest> requests;
protected:
    QNetworkReply *createRequest(Operation,const QNetworkRequest &r,QIODevice *body) override {
        requests.append(r); auto o=body?QJsonDocument::fromJson(body->readAll()).object():QJsonObject();
        return handler(r,o);
    }
};
class Runner final : public OugaCommandRunner {
public:
    struct Call { QString program; QStringList args; int timeout; };
    QList<Call> calls;
    bool active=false;
    void run(const QString &p,const QStringList&a,int t) override {calls.append({p,a,t});active=true;}
    bool running() const override {return active;}
    void respond(QString out,int code=0,bool normal=true){active=false;emit completed(code,normal,out);}
    void denyApkRoot() { respond("2000\n"); respond("permission denied",1); respond("permission denied",1); }
};
}
class OpenListTests final : public QObject {
    Q_OBJECT
private slots:
    void cleanup(){ QVERIFY(!DeviceOperationLease::owner()); }
    void applicationExitPolicy() {
        class ExitReceiver final : public QObject {
        public:
            bool quitReceived = false;
            bool event(QEvent *event) override {
                if (event->type() == QEvent::Quit) { quitReceived = true; return true; }
                return QObject::event(event);
            }
        } receiver;
        QWidget window;
        QObject flashOwner(&window);
        QVERIFY(DeviceOperationLease::acquire(&flashOwner));
        QVERIFY(DeviceOperationLease::blocksApplicationExit());
        window.show();
        QVERIFY(!window.close());
        QEvent blockedQuit(QEvent::Quit);
        QCoreApplication::sendEvent(&receiver, &blockedQuit);
        QVERIFY(!receiver.quitReceived);
        DeviceOperationLease::release(&flashOwner);

        Runner runner;
        OpenListInstaller installer(&window, &runner, "fake-adb", "fake-7z");
        QVERIFY(DeviceOperationLease::acquire(&installer));
        QVERIFY(!DeviceOperationLease::blocksApplicationExit());
        QVERIFY(window.close());
        QEvent allowedQuit(QEvent::Quit);
        QCoreApplication::sendEvent(&receiver, &allowedQuit);
        QVERIFY(receiver.quitReceived);
        // Exit exemption does not release the lock or permit concurrent flashing.
        QVERIFY(!DeviceOperationLease::acquire(&flashOwner));
        QCOMPARE(DeviceOperationLease::owner(), static_cast<QObject *>(&installer));
        DeviceOperationLease::release(&installer);
        QVERIFY(!DeviceOperationLease::blocksApplicationExit());
    }
    void deviceChannelTransientBusy() {
        Runner runner; OpenListInstaller installer(nullptr, &runner, "fake-adb", "fake-7z");
        bool idle = false;
        DeviceOperationLease::setIdleCheck([&] { return idle; });
        const auto resetIdle = qScopeGuard([] { DeviceOperationLease::setIdleCheck({}); });
        QSignalSpy devices(&installer, &OpenListInstaller::devicesReady);
        QVERIFY(installer.inspectDevices()); QVERIFY(installer.busy()); QVERIFY(runner.calls.isEmpty());
        idle = true;
        QTRY_COMPARE(runner.calls.size(), 1);
        runner.respond("phone\tdevice\n");
        QCOMPARE(devices.size(), 1); QVERIFY(!installer.busy());
        DeviceOperationLease::setIdleCheck({});
    }
    void mtRangeDownload() {
        QTemporaryDir dir; Network network;
        const QByteArray apk = QByteArray("PK\x03\x04",4) + QByteArray(9 * 1024 * 1024, 'm');
        network.handler = [&](const QNetworkRequest &req, const QJsonObject &) {
            if (req.url().host() == "mt.cc") return new Reply(req,
                "<a href=\"https://pan.mt2.cn/apk/26091198/clone-target28\">download</a>", 200);
            const auto range = QRegularExpression("^bytes=(\\d+)-(\\d+)$").match(QString::fromLatin1(req.rawHeader("Range")));
            const qint64 first = range.captured(1).toLongLong(), last = range.captured(2).toLongLong();
            auto *reply = new Reply(req, apk.mid(first, last-first+1), 206);
            reply->setHeader("Content-Range", QByteArray("bytes ") + QByteArray::number(first) + '-' + QByteArray::number(last) + '/' + QByteArray::number(apk.size()));
            return reply;
        };
        MtManagerService service(nullptr, &network, dir.path());
        QSignalSpy ready(&service, &MtManagerService::ready), failed(&service, &MtManagerService::failed);
        service.start(); QTRY_COMPARE(ready.size(), 1); QVERIFY(failed.isEmpty());
        QCOMPARE(network.requests.size(), 8); // Page, 1-byte probe, six simultaneous ranges.
        QFile file(ready[0][1].toString()); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), apk);
        QVERIFY(!service.active());
        QCOMPARE(QDir(dir.filePath(QStringLiteral("软件"))).entryList({"*.part"}, QDir::Files).size(), 0);
    }
    void directRangeFallback() {
        QTemporaryDir dir; Network network;
        const QByteArray payload(3 * 1024 * 1024, 'f');
        network.handler = [&](const QNetworkRequest &req, const QJsonObject &) {
            return new Reply(req, payload, 200); // Server ignores Range.
        };
        OpenListService service(nullptr, &network, dir.path());
        OpenList::Entry entry; entry.name = entry.relativePath = "fallback.apk"; entry.size = payload.size();
        QSignalSpy ready(&service, &OpenListService::ready), failed(&service, &OpenListService::failed);
        QVERIFY(service.downloadDirect(entry, QUrl("https://files.example.com/file")));
        QTRY_COMPARE(ready.size(), 1); QVERIFY(failed.isEmpty());
        QFile file(ready[0][1].toString()); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), payload);
        QVERIFY(network.requests.last().rawHeader("Range").isEmpty());
        QVERIFY(!service.downloadDirect(entry, QUrl("http://files.example.com/file")));
    }
    void mtRangeCancel() {
        QTemporaryDir dir; Network network; int ranges = 0;
        network.handler = [&](const QNetworkRequest &req, const QJsonObject &) {
            if (req.url().host() == "mt.cc") return new Reply(req,
                "<a href=\"https://pan.mt2.cn/apk/26091198/clone-target28\">download</a>", 200);
            if (req.rawHeader("Range") == "bytes=0-0") {
                auto *reply = new Reply(req, "P", 206);
                reply->setHeader("Content-Range", "bytes 0-0/9437184"); return reply;
            }
            ++ranges; return new Reply(req, {}, 206, {}, true);
        };
        MtManagerService service(nullptr, &network, dir.path());
        QSignalSpy ready(&service, &MtManagerService::ready), failed(&service, &MtManagerService::failed);
        service.start(); QTRY_COMPARE(ranges, 6); service.cancel(); service.cancel();
        QCOMPARE(failed.size(), 1); QVERIFY(ready.isEmpty()); QVERIFY(!service.active());
        QCOMPARE(QDir(dir.filePath(QStringLiteral("软件"))).entryList(QDir::Files).size(), 0);
    }
    void mtDownloadCompletion() {
        QTemporaryDir dir;
        Network network;
        const QByteArray apk = QByteArray("PK\x03\x04", 4) + QByteArray(1024, 'a');
        network.handler = [&](const QNetworkRequest &req, const QJsonObject &) {
            auto *reply = new Reply(req, req.url().host() == "mt.cc"
                ? QByteArray("<a href=\"https://pan.mt2.cn/apk/26091198/clone-target28\">download</a>") : apk, 200);
            if (req.url().host() != "mt.cc") reply->setHeader("Content-Length", QByteArray::number(apk.size()));
            return reply;
        };
        MtManagerService service(nullptr, &network, dir.path());
        QSignalSpy ready(&service, &MtManagerService::ready), failed(&service, &MtManagerService::failed);
        service.start();
        service.start(); // Duplicate trigger must not create another request.
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(failed.isEmpty()); QVERIFY(!service.active());
        QCOMPARE(network.requests.size(), 2);
        QFile file(ready.first()[1].toString()); QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), apk); file.close();
        service.start(); // Repeat download must not overwrite the first file.
        QTRY_COMPARE(ready.size(), 2);
        QVERIFY(ready[0][1].toString() != ready[1][1].toString());
        QVERIFY(QFileInfo::exists(ready[0][1].toString()));
        QCOMPARE(QDir(dir.filePath(QStringLiteral("软件"))).entryList({"*.part"}, QDir::Files).size(), 0);
    }
    void mtDownloadCancel() {
        QTemporaryDir dir;
        Network network;
        QPointer<Reply> download;
        network.handler = [&](const QNetworkRequest &req, const QJsonObject &) {
            if (req.url().host() == "mt.cc") return new Reply(req,
                "<a href=\"https://pan.mt2.cn/apk/26091198/clone-target28\">download</a>", 200);
            download = new Reply(req, {}, 200, {}, true);
            return download.data();
        };
        MtManagerService service(nullptr, &network, dir.path());
        QSignalSpy ready(&service, &MtManagerService::ready), failed(&service, &MtManagerService::failed);
        service.start(); QTRY_VERIFY(download);
        service.cancel(); service.cancel();
        QCOMPARE(failed.size(), 1); QVERIFY(ready.isEmpty()); QVERIFY(!service.active());
        QCOMPARE(QDir(dir.filePath(QStringLiteral("软件"))).entryList(QDir::Files).size(), 0);
    }
    void mtDownloadFailure_data() {
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<int>("size");
        QTest::newRow("invalid-apk") << QByteArray("not an apk") << 10;
        QTest::newRow("incomplete") << (QByteArray("PK\x03\x04",4) + "apk") << 100;
    }
    void mtDownloadFailure() {
        QFETCH(QByteArray, body); QFETCH(int, size);
        QTemporaryDir dir;
        Network network;
        network.handler = [&](const QNetworkRequest &req, const QJsonObject &) {
            if (req.url().host() == "mt.cc") return new Reply(req,
                "<a href=\"https://pan.mt2.cn/apk/26091198/clone-target28\">download</a>", 200);
            auto *reply = new Reply(req, body, 200, {}, true);
            reply->setHeader("Content-Length", QByteArray::number(size));
            QTimer::singleShot(0, reply, [reply, size] {
                emit reply->readyRead(); emit reply->downloadProgress(reply->bytes.size(), size);
                reply->done = true; emit reply->finished();
            });
            return reply;
        };
        MtManagerService service(nullptr, &network, dir.path());
        QSignalSpy ready(&service, &MtManagerService::ready), failed(&service, &MtManagerService::failed);
        service.start(); QTRY_COMPARE(failed.size(), 1);
        QVERIFY(!service.active()); QVERIFY(ready.isEmpty());
        QCOMPARE(QDir(dir.filePath(QStringLiteral("软件"))).entryList(QDir::Files).size(), 0);
    }
    void safety(){
        QVERIFY(OpenListService::https(QUrl("https://example.com/file"))); QVERIFY(!OpenListService::https(QUrl("http://example.com")));
        QVERIFY(!OpenListService::https(QUrl("https://user:secret@example.com")));
        for(QString s:QStringList{"..","CON.apk","x/y","a:b","a.","x\\y","NUL","a "}) QVERIFY(!OpenList::safeComponent(s));
        QVERIFY(OpenList::safeComponent(QStringLiteral("软件.apk")));
        auto t=OpenList::redact("https://server/x?sign=secret /www/wwwroot/secret token=abc"); QVERIFY(!t.contains("server"));QVERIFY(!t.contains("wwwroot"));QVERIFY(!t.contains("abc"));
        QCOMPARE(OpenListService::sha256({{"hash_info",QJsonObject{{"SHA-256",QString(64,'a')}}}}),QString(64,'a'));
    }
    void downloadRootResolution() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QString localData = dir.filePath(QStringLiteral("用户数据/QiuBai/Orange Tools"));
        const QString app = dir.filePath("OrangeToolsApp");
        QVERIFY(QDir().mkdir(app));
        QCOMPARE(OpenList::resolveDownloadRoot(app, localData), app + "/download");
        QCOMPARE(OpenList::resolveDownloadRoot(app + "/./", {}), app + "/download");

        const QString project = dir.filePath(QStringLiteral("中文项目"));
        const QString build = project + "/build/release";
        QVERIFY(QDir().mkpath(build));
        QVERIFY(writeFile(project + "/OrangeTools.pro", "# project marker"));
        QCOMPARE(OpenList::resolveDownloadRoot(build, localData), project + "/OrangeToolsApp/download");
        QCOMPARE(OpenList::resolveDownloadRoot(build, {}), project + "/OrangeToolsApp/download");

        const QString sfx = sfxDirectory();
        QVERIFY(!QFileInfo::exists(sfx));
        const QString persistentRoot = localData + "/download";
        QCOMPARE(OpenList::resolveDownloadRoot(sfx + "/instance-one", localData), persistentRoot);
        QCOMPARE(OpenList::resolveDownloadRoot(sfx + "/instance-two", localData), persistentRoot);
        QVERIFY(!QFileInfo::exists(localData)); // Resolving a location must not create it.
        QVERIFY(OpenList::resolveDownloadRoot(sfx, {}).isEmpty());
        QVERIFY(OpenList::resolveDownloadRoot(sfx, "relative-data").isEmpty());
        QVERIFY(OpenList::resolveDownloadRoot({}, localData).isEmpty());
        QVERIFY(OpenList::resolveDownloadRoot("relative-app", localData).isEmpty());
        QCOMPARE(OpenList::defaultDownloadRoot(), OpenList::resolveDownloadRoot(
            QCoreApplication::applicationDirPath(),
            QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)));
    }
    void sfxPersistentDownloads() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QString localData = dir.filePath(QStringLiteral("持久数据/QiuBai/Orange Tools"));
        const QString firstRoot = OpenList::resolveDownloadRoot(sfxDirectory(), localData);
        const QString secondRoot = OpenList::resolveDownloadRoot(sfxDirectory(), localData);
        QCOMPARE(firstRoot, localData + "/download"); QCOMPARE(secondRoot, firstRoot);
        const QByteArray bytes = QByteArray("PK\x03\x04", 4) + "fixture";
        Network network;
        network.handler = [&](const QNetworkRequest &request, const QJsonObject &) {
            if (request.url().path() == "/api/fs/get") return new Reply(request,
                api({{"raw_url", "https://files.example.com/package"}, {"size", bytes.size()}}), 200);
            return new Reply(request, bytes, 200);
        };
        OpenList::Entry entry;
        entry.name = "alpha.apk";
        entry.relativePath = QStringLiteral("中文 分类/") + entry.name;
        entry.remotePath = QStringLiteral("/A秋白工作室/") + entry.relativePath;
        entry.size = bytes.size();
        OpenListService first(nullptr, &network, firstRoot), second(nullptr, &network, secondRoot);
        QSignalSpy firstReady(&first, &OpenListService::ready), secondReady(&second, &OpenListService::ready);
        QVERIFY(first.download(entry)); QTRY_COMPARE(firstReady.size(), 1);
        const QString firstFile = firstReady[0][1].toString();
        QCOMPARE(firstFile, firstRoot + QStringLiteral("/软件/") + entry.relativePath);
        QVERIFY(second.download(entry)); QTRY_COMPARE(secondReady.size(), 1);
        const QString secondFile = secondReady[0][1].toString(); QVERIFY(secondFile != firstFile);
        QFile original(firstFile), downloaded(secondFile);
        QVERIFY(original.open(QIODevice::ReadOnly)); QVERIFY(downloaded.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), bytes); QCOMPARE(downloaded.readAll(), bytes);
        QVERIFY(QDir(QFileInfo(firstFile).absolutePath()).entryList({"*.part"}, QDir::Files).isEmpty());
    }
    void downloadRootUnavailable_data() {
        QTest::addColumn<bool>("sfx");
        QTest::newRow("portable-no-fallback-on-failure") << false;
        QTest::newRow("sfx-persistent-directory-blocked") << true;
    }
    void downloadRootUnavailable() {
        QFETCH(bool, sfx);
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QString localData = dir.filePath("local-data");
        const QString root = OpenList::resolveDownloadRoot(sfx ? sfxDirectory() : dir.filePath("OrangeToolsApp"), localData);
        QVERIFY(QDir().mkpath(QFileInfo(root).absolutePath()));
        QVERIFY(writeFile(root, "keep existing file")); // A file blocks the selected download directory.
        Network network;
        OpenListService service(nullptr, &network, root); QSignalSpy failed(&service, &OpenListService::failed);
        OpenList::Entry entry; entry.name = "alpha.apk"; entry.relativePath = entry.name; entry.size = 3;
        QVERIFY(!service.downloadDirect(entry, QUrl("https://files.example.com/package")));
        QCOMPARE(failed.size(), 1); QVERIFY(network.requests.isEmpty()); QVERIFY(!service.downloading());
        QFile blocker(root); QVERIFY(blocker.open(QIODevice::ReadOnly));
        QCOMPARE(blocker.readAll(), QByteArray("keep existing file"));
        if (!sfx) QVERIFY(!QFileInfo::exists(localData));
    }
    void recursiveListing(){
        Network network;int pages=0;
        network.handler=[&](const QNetworkRequest&r,const QJsonObject&b){QString p=b.value("path").toString();QJsonArray a;
            if(p==QStringLiteral("/A秋白工作室")){int page=b.value("page").toInt();pages++;if(page==1)a={entry("A.apk"),entry("sub",true),entry("readme.txt"),entry("../bad.apk")};else a={entry("B.APK")};return new Reply(r,api({{"content",a},{"total",101}}),200);}
            if(p.endsWith("/sub"))return new Reply(r,api({{"content",QJsonArray{entry("A.apk")}}, {"total",1}}),200);
            return new Reply(r,api({{"content",QJsonValue(QJsonValue::Null)},{"total",0}}),200);
        };
        OpenListService service(nullptr,&network,"unused");QSignalSpy spy(&service,&OpenListService::listingReady);service.refresh();QTRY_COMPARE(spy.size(),1);QCOMPARE(pages,2);
        const auto xs=qvariant_cast<QList<OpenList::Entry>>(spy[0][0]);
        QCOMPARE(xs.size(),5); QVERIFY(xs[1].directory);
        for (const auto &e : xs) QVERIFY(e.remotePath.startsWith(QStringLiteral("/A秋白工作室/")));
        QVERIFY(!spy[0][1].toString().isEmpty());
    }
    void toolsListingAllFormats() {
        Network network; int pages = 0;
        network.handler = [&](const QNetworkRequest &r,const QJsonObject &b) {
            const QString path=b.value("path").toString();
            if (path==QStringLiteral("/A秋白工作室")) {
                ++pages;
                const QJsonArray content=b.value("page").toInt()==1 ?
                    QJsonArray{entry("tool.SH"),entry("app.APK"),entry("module.zip"),entry("readme.txt"),entry("LICENSE"),entry(QStringLiteral("中文文件夹"),true),entry("../bad.sh")} :
                    QJsonArray{entry("image.png")};
                return new Reply(r,api({{"content",content},{"total",101}}),200);
            }
            return new Reply(r,api({{"content",QJsonArray{entry(QStringLiteral("测试脚本.sh")),entry("data.bin")}}, {"total",2}}),200);
        };
        OpenListService service(nullptr,&network,"unused"); QSignalSpy listed(&service,&OpenListService::listingReady);
        service.refresh(); QTRY_COMPARE(listed.size(),1); QCOMPARE(pages,2);
        const auto entries=qvariant_cast<QList<OpenList::Entry>>(listed[0][0]);
        QCOMPARE(entries.size(),9);
        for (const auto &item:entries) QVERIFY(!item.relativePath.contains(".."));
        QVERIFY(!listed[0][1].toString().isEmpty());
        QVERIFY(std::any_of(entries.cbegin(),entries.cend(),[](const OpenList::Entry &entry) { return entry.remotePath==QStringLiteral("/A秋白工作室/中文文件夹/测试脚本.sh"); }));
    }
    void toolsGenericDownload_data() {
        QTest::addColumn<QString>("name"); QTest::addColumn<QByteArray>("bytes");
        QTest::newRow("script") << QStringLiteral("中文脚本.SH") << QByteArray("#!/system/bin/sh\necho test\n");
        QTest::newRow("text") << QString("readme.txt") << QByteArray("readme");
        QTest::newRow("no-extension") << QString("LICENSE") << QByteArray("license");
        QTest::newRow("empty") << QString("empty.bin") << QByteArray();
    }
    void toolsGenericDownload() {
        QFETCH(QString,name); QFETCH(QByteArray,bytes); QTemporaryDir dir; Network network;
        network.handler=[&](const QNetworkRequest &req,const QJsonObject &) {
            if(req.url().path()=="/api/fs/get") return new Reply(req,api({{"raw_url","https://files.example.com/tool"},{"size",bytes.size()}}),200);
            return new Reply(req,bytes,200);
        };
        OpenList::Entry entry; entry.name=name; entry.relativePath=QStringLiteral("中文/")+name;
        entry.remotePath=QStringLiteral("/A秋白工作室/中文/")+name;
        OpenListService service(nullptr,&network,dir.path()); QSignalSpy ready(&service,&OpenListService::ready);
        const QString original=dir.filePath(QStringLiteral("软件/")+entry.relativePath);
        QVERIFY(QDir().mkpath(QFileInfo(original).absolutePath())); QVERIFY(writeFile(original,"existing"));
        for(int i=0;i<2;++i) {
            QVERIFY(service.download(entry)); QTRY_COMPARE(ready.size(),i+1);
            const QString file=ready[i][1].toString(); QVERIFY(file!=original); QVERIFY(!file.endsWith('.'));
            QFile data(file); QVERIFY(data.open(QIODevice::ReadOnly)); QCOMPARE(data.readAll(),bytes);
            if(i==1) QVERIFY(file!=ready[0][1].toString());
        }
        QFile previous(original); QVERIFY(previous.open(QIODevice::ReadOnly)); QCOMPARE(previous.readAll(),QByteArray("existing"));
        QCOMPARE(QDir(QFileInfo(original).absolutePath()).entryList({"*.part"},QDir::Files).size(),0);
    }
    void scriptExecution_data() {
        QTest::addColumn<int>("mode");
        const char *names[]={"shell-root","su-c","su-s","root-denied","disconnected","push-failed",
            "exit-nonzero","abnormal-exit","missing-marker","error-output-with-marker","remote-cleanup-failed",
            "local-cleanup-failed","root-lost","cleanup-error-output","repeat-execution-prohibited"};
        for (int i=0;i<15;++i) QTest::newRow(names[i]) << i;
    }
    void scriptExecution() {
        QFETCH(int,mode); QTemporaryDir dir;
        const QString file=dir.filePath(QStringLiteral("中文脚本's (1).SH")),old=dir.filePath("existing.sh");
        QVERIFY(writeFile(file,"#!/system/bin/sh\necho test\n")); QVERIFY(writeFile(old,"old"));
        Runner runner; OpenListInstaller installer(nullptr,&runner,"fake-adb","fake-7z");
        QSignalSpy done(&installer,&OpenListInstaller::completed),logs(&installer,&OpenListInstaller::log);
        QVERIFY(installer.install(file,"original")); QVERIFY(!installer.install(file,"other"));
        QCOMPARE(DeviceOperationLease::owner(),static_cast<QObject*>(&installer));
        runner.respond(mode==4 ? "other\tdevice" : "original\tdevice");
        QString remote,marker;
        if(mode!=4) {
            if(mode==1 || mode==2 || mode==3 || mode==14) runner.respond("2000\n");
            if(mode==2 || mode==3) runner.respond("permission denied",1);
            if(mode==3) runner.respond("permission denied",1);
            else {
                runner.respond("0\n");
                QCOMPARE(runner.calls.last().args.mid(0,4),QStringList({"-s","original","push",file}));
                remote=runner.calls.last().args.last(); QVERIFY(remote.endsWith(".sh"));
                QVERIFY(remote.startsWith("/data/local/tmp/orange-openlist-")); QVERIFY(!remote.contains(QFileInfo(file).fileName()));
                runner.respond(mode==5 ? "error: upload failed" : "1 file pushed",mode==5 ? 1 : 0);
                if(mode!=5) {
                    const QString command=runner.calls.last().args.last();
                    QVERIFY(command.contains("/system/bin/sh")); QVERIFY(command.contains("$(id -u)"));
                    QVERIFY(command.contains(remote)); QCOMPARE(runner.calls.last().timeout,0);
                    if(mode==1 || mode==14) QVERIFY(command.startsWith("su -c "));
                    else if(mode==2) QVERIFY(command.startsWith("su -s "));
                    else QVERIFY(!command.startsWith("su "));
                    const auto match=QRegularExpression("OL_SCRIPT_SUCCESS_[a-f0-9]{32}").match(command);
                    QVERIFY(match.hasMatch()); marker=match.captured();
                    QString output=QStringLiteral("中文执行完成\n")+marker+'\n';
                    if(mode==8) output="done\n";
                    if(mode==9 || mode==14) output="error: denied\n"+marker+'\n';
                    if(mode==12) output="ERROR: Root authorization lost\n";
                    const int count=runner.calls.size();
                    runner.respond(output,(mode==6 || mode==12 || mode==14) ? 1 : 0,mode!=7);
                    QCOMPARE(runner.calls.size(),count+1); // cleanup, never a second execution
                }
                QVERIFY(runner.calls.last().args.last().contains("rm -f "));
                QVERIFY(runner.calls.last().args.last().contains(remote));
                QVERIFY(!runner.calls.last().args.last().contains("/sdcard/"));
#ifdef Q_OS_WIN
                HANDLE locked=INVALID_HANDLE_VALUE;
                if(mode==11) {
                    locked=CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(file).utf16()),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
                    QVERIFY(locked!=INVALID_HANDLE_VALUE);
                }
#endif
                runner.respond(mode==10 ? "device disconnected" : mode==13 ? "permission denied" : "",mode==10 ? 1 : 0);
#ifdef Q_OS_WIN
                if(locked!=INVALID_HANDLE_VALUE) CloseHandle(locked);
#endif
            }
        }
        // mode 9：脚本输出 error 字样但 rc==0 且有成功标记——退出码与标记行
        // 优先于关键词（模块脚本常输出 "xxx not found, skip" 的正常检查信息）。
        const bool success=mode==0 || mode==1 || mode==2 || mode==9 || mode==10 || mode==11 || mode==13;
        QCOMPARE(done.size(),1); QCOMPARE(done[0][0].toBool(),success);
        QCOMPARE(QFileInfo::exists(file),!success || mode==11); QVERIFY(QFileInfo::exists(old));
        if(mode==11) QVERIFY(done[0][1].toString().contains(QStringLiteral("清理失败")));
        QVERIFY(!installer.busy()); QVERIFY(!DeviceOperationLease::owner());
        int executions=0; for(const auto &call:runner.calls) {
            if(call.args.join(' ').contains("/system/bin/sh")) ++executions;
            if(call.args.value(0)=="-s") QCOMPARE(call.args.value(1),QString("original"));
            QVERIFY(!call.args.contains("install")); QVERIFY(!call.args.join(' ').contains("/sdcard/"));
        }
        QCOMPARE(executions,mode==3 || mode==4 || mode==5 ? 0 : 1);
        for(const auto &row:logs) { QVERIFY(marker.isEmpty() || !row[0].toString().contains(marker)); QVERIFY(remote.isEmpty() || !row[0].toString().contains(remote)); }
    }
    void unsupportedInstallRejected() {
        QTemporaryDir dir; const QString file=dir.filePath("readme.txt"); QVERIFY(writeFile(file,"readme"));
        Runner runner; OpenListInstaller installer(nullptr,&runner,"fake-adb","fake-7z"); QSignalSpy done(&installer,&OpenListInstaller::completed);
        QVERIFY(!installer.install(file,"phone")); QCOMPARE(done.size(),1); QVERIFY(!done[0][0].toBool());
        QVERIFY(runner.calls.isEmpty()); QVERIFY(QFileInfo::exists(file)); QVERIFY(!DeviceOperationLease::owner());
    }
    void toolsSelection_data() {
        QTest::addColumn<int>("mode");
        QTest::newRow("tools-apk") << 0;
        QTest::newRow("tools-sh") << 1;
        QTest::newRow("tools-text-transfer") << 2;
        QTest::newRow("tools-text-install-rejected") << 3;
        QTest::newRow("tools-folder-filter-install") << 4;
        QTest::newRow("tools-folder-transfer-all") << 5;
    }
    void toolsSelection() {
        QFETCH(int,mode); QTemporaryDir dir; Network network; int downloads=0;
        network.handler=[&](const QNetworkRequest &req,const QJsonObject &) {
            if(req.url().path()=="/api/fs/list") return new Reply(req,api({{"content",QJsonArray{}},{"total",0}}),200);
            if(req.url().path()=="/api/fs/get") { ++downloads; return new Reply(req,api({{"raw_url","https://files.example.com/file"},{"size",3}}),200); }
            return new Reply(req,"abc",200);
        };
        OpenListService service(nullptr,&network,dir.path()); Runner runner;
        OpenListInstaller installer(nullptr,&runner,"fake-adb","fake-7z"); OpenListWindow window(nullptr,&service,&installer);
        QSignalSpy listed(&service,&OpenListService::listingReady); QTRY_COMPARE(listed.size(),1);
        auto *card=window.findChild<QGroupBox*>("openListFilesCard"); QVERIFY(card); QCOMPARE(card->title(),QStringLiteral("秋白工作室 · 资源列表"));
        OpenList::Entry folder; folder.directory=true; folder.name="folder"; folder.relativePath=folder.name; folder.remotePath=QStringLiteral("/A秋白工作室/folder");
        auto file=folder; file.directory=false; file.name=mode==0 ? "app.apk" : mode==1 || mode>=4 ? "script.sh" : "readme.txt";
        file.relativePath=mode>=4 ? "folder/"+file.name : file.name; file.remotePath=QStringLiteral("/A秋白工作室/")+file.relativePath;
        auto text=file; text.name="readme.txt"; text.relativePath=mode>=4 ? "folder/readme.txt" : text.name; text.remotePath=QStringLiteral("/A秋白工作室/")+text.relativePath;
        emit service.listingReady(mode>=4 ? QList<OpenList::Entry>{folder,file,text} : QList<OpenList::Entry>{file},{});
        auto *table=window.findChild<QTableWidget*>("openListFiles"); QVERIFY(table); table->selectRow(0);
        const bool transfer=mode==2 || mode==5;
        auto *button=window.findChild<QPushButton*>(transfer ? "openListTransferSelected" : "openListInstallSelected"); QVERIFY(button);
        button->click(); if(mode==3) { QVERIFY(!window.isBusy()); QVERIFY(runner.calls.isEmpty()); QCOMPARE(downloads,0); return; }
        QVERIFY(window.isBusy()); button->click(); emit table->cellDoubleClicked(0,0); QCOMPARE(runner.calls.size(),1);
        runner.respond("phone\tdevice");
        const int count=mode==5 ? 2 : 1;
        QTRY_COMPARE(downloads,count); QTRY_COMPARE(runner.calls.size(),2);
        for(int index=0;index<count;++index) {
            runner.respond("phone\tdevice");
            if(transfer) { runner.respond(""); QVERIFY(runner.calls.last().args.contains("push")); runner.respond("1 file pushed"); }
            else if(mode==0) { runner.denyApkRoot(); QVERIFY(runner.calls.last().args.contains("install")); runner.respond("Success\n"); }
            else {
                runner.respond("0\n"); QVERIFY(runner.calls.last().args.contains("push")); runner.respond("1 file pushed");
                const auto marker=QRegularExpression("OL_SCRIPT_SUCCESS_[a-f0-9]{32}").match(runner.calls.last().args.last()).captured();
                QVERIFY(!marker.isEmpty()); runner.respond(marker+'\n'); runner.respond("");
            }
            if(index+1<count) QTRY_VERIFY(installer.busy());
        }
        QTRY_VERIFY(!window.isBusy()); QVERIFY(table->selectedItems().isEmpty());
        QCOMPARE(QFileInfo::exists(dir.filePath(QStringLiteral("软件/")+file.relativePath)),transfer);
        QCOMPARE(QFileInfo::exists(dir.filePath(QStringLiteral("软件/")+text.relativePath)),transfer);
    }
    void listingFailureReported(){
        Network network;network.handler=[](const QNetworkRequest&r,const QJsonObject&){return new Reply(r,QByteArray("bad"),200);};
        OpenListService s(nullptr,&network,"unused");QSignalSpy spy(&s,&OpenListService::listingReady);s.refresh();QTRY_COMPARE(spy.size(),1);QVERIFY(!spy[0][1].toString().isEmpty());QCOMPARE(qvariant_cast<QList<OpenList::Entry>>(spy[0][0]).size(),0);
    }
    void download_data(){QTest::addColumn<int>("mode");for(int i=0;i<6;++i)QTest::newRow(qPrintable(QString::number(i)))<<i;}
    void download(){
        QFETCH(int,mode);QTemporaryDir dir;QVERIFY(dir.isValid());Network network;
        network.handler=[mode](const QNetworkRequest&r,const QJsonObject&body){
            if(r.url().path()=="/api/fs/get" && body.value("path").toString()!=QStringLiteral("/A秋白工作室/sub/ok.apk")) return new Reply(r,"bad path",400);
            if(r.url().path()=="/api/fs/get")return new Reply(r,api({{"raw_url",mode==1?"http://unsafe/file":"https://files.example.com/file"},{"size",3},{"hash_info",QJsonObject{{"sha256",mode==2?QString(64,'a'):QString(QCryptographicHash::hash("abc",QCryptographicHash::Sha256).toHex())}}}}),200);
            if(mode==3)return new Reply(r,{},302,QUrl("http://unsafe/file"));
            return new Reply(r,mode==4?"ab":"abc",200,{},mode==5);
        };
        OpenListService s(nullptr,&network,dir.path());OpenList::Entry e;e.name="ok.apk";e.relativePath="sub/ok.apk";e.remotePath=QStringLiteral("/A秋白工作室/sub/ok.apk");e.size=3;
        QSignalSpy ready(&s,&OpenListService::ready),failed(&s,&OpenListService::failed),logs(&s,&OpenListService::log);QVERIFY(s.download(e));QVERIFY(!s.download(e));
        if(mode==5){QTRY_COMPARE(network.requests.size(),2);s.cancel();}
        if(mode==0){QTRY_COMPARE(ready.size(),1);auto file=ready[0][1].toString();QVERIFY(QFileInfo::exists(file));QVERIFY(writeFile(file,"old"));QVERIFY(s.download(e));QTRY_COMPARE(ready.size(),2);QVERIFY(ready[1][1].toString()!=file);QFile original(file);QVERIFY(original.open(QIODevice::ReadOnly));QCOMPARE(original.readAll(),QByteArray("old"));}
        else{QTRY_COMPARE(failed.size(),1);QVERIFY(ready.isEmpty());}
        QDir d(dir.path()+QStringLiteral("/软件/sub"));QVERIFY(d.entryList({"*.part"},QDir::Files).isEmpty());
        for(const auto&r:network.requests){QVERIFY(r.rawHeader("Authorization").isEmpty());QVERIFY(r.rawHeader("Cookie").isEmpty());}
    }
    void parallelDownload() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QByteArray payload(17 * 1024 * 1024 + 123, 'P');
        Network network;
        network.handler = [&](const QNetworkRequest &r, const QJsonObject &body) {
            Q_UNUSED(body);
            if (r.url().path() == "/api/fs/get")
                return new Reply(r, api({{"raw_url", "https://files.example.com/large"},
                                         {"size", double(payload.size())}}), 200);
            const QByteArray range = r.rawHeader("Range");
            QRegularExpressionMatch match = QRegularExpression("^bytes=(\\d+)-(\\d+)$").match(QString::fromLatin1(range));
            if (!match.hasMatch()) return new Reply(r, {}, 200);
            const qint64 first = match.captured(1).toLongLong();
            const qint64 last = match.captured(2).toLongLong();
            auto *reply = new Reply(r, payload.mid(first, last - first + 1), 206);
            reply->setHeader("Content-Range", QByteArray("bytes ") + QByteArray::number(first) + '-' + QByteArray::number(last) + '/' + QByteArray::number(payload.size()));
            return reply;
        };
        OpenListService service(nullptr, &network, dir.path());
        OpenList::Entry entry; entry.name = "large.apk"; entry.relativePath = entry.name;
        entry.remotePath = QStringLiteral("/A秋白工作室/large.apk");
        QSignalSpy ready(&service, &OpenListService::ready);
        QSignalSpy failed(&service, &OpenListService::failed), logs(&service, &OpenListService::log);
        QVERIFY(service.download(entry));
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 5000);
        QCOMPARE(failed.size(), 0);
        for(const auto &line:logs) QVERIFY(!line[0].toString().contains(QStringLiteral("未提供发布方 SHA-256")));
        int ranged = 0;
        for (const auto &request : network.requests) if (!request.rawHeader("Range").isEmpty()) ++ranged;
        QCOMPARE(ranged, 6);
        QFile file(ready.first()[1].toString());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), payload);
    }
    void utf8ProcessOutput() {
        OpenListProcessRunner runner;
        QSignalSpy done(&runner,&OugaCommandRunner::completed), output(&runner,&OugaCommandRunner::output);
        const QString expected=QStringLiteral("中文路径/软件/春秋.apk\n📌 模块安装完成\nOL_INSTALL_SUCCESS\n");
        for(int attempt=0;attempt<2;++attempt) {
            done.clear(); output.clear();
            runner.run(QCoreApplication::applicationFilePath(),{"--utf8-fixture"});
            QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);
            QCOMPARE(done[0][0].toInt(),0); QVERIFY(done[0][1].toBool());
            QCOMPARE(done[0][2].toString(),expected);
            QString combined; for(const auto &chunk:output) combined+=chunk[0].toString();
            QCOMPARE(combined,expected); QVERIFY(output.size()>1);
        }
    }
    void conciseInstallerLog() {
        QTemporaryDir dir; const QString file=dir.filePath(QStringLiteral("中文模块.zip"));
        QVERIFY(writeFile(file,QByteArray("PK\x03\x04",4)));
        Runner r; OpenListInstaller s(nullptr,&r,"adb","7z");
        QSignalSpy logs(&s,&OpenListInstaller::log),done(&s,&OpenListInstaller::completed);
        QVERIFY(s.install(file,true,"phone")); r.respond("phone\tdevice");
        r.respond("Path = module.prop"); r.respond("Everything is Ok");
        r.respond("id=test\nname=Test"); r.respond("0"); r.respond("OL_KERNELSU=/data/adb/ksu/bin/ksud");
        emit r.output(file.left(4)); emit r.output(file.mid(4)+": 1 file pushed\n");
        r.respond("1 file pushed"); logs.clear();
        emit r.output(QStringLiteral("📌 中文模"));
        QCOMPARE(logs.size(),0);
        emit r.output(QStringLiteral("块安装完成\nArchive: /tmp/file\n  inflating: module.prop\nSuccess\nO"));
        emit r.output("L_INSTALL_SUCC"); emit r.output("ESS\n");
        emit r.output("ERROR: https://server.test/file?sign=secret\n");
        emit r.output(QStringLiteral("尾行没有换行"));
        r.respond("ERROR: test",1); r.respond("");
        QCOMPARE(logs[0][0].toString(),QStringLiteral("📌 中文模块安装完成"));
        QCOMPARE(logs[2][0].toString(),QStringLiteral("尾行没有换行"));
        QString all; for(const auto &line:logs) all+=line[0].toString();
        QVERIFY(all.contains("ERROR:")); QVERIFY(!all.contains("server.test")); QVERIFY(!all.contains("sign="));
        QVERIFY(!all.contains("OL_INSTALL")); QVERIFY(!all.contains("inflating:")); QVERIFY(!all.contains("Archive:"));
        QVERIFY(!all.contains("1 file pushed")); QVERIFY(!all.contains(QStringLiteral("已获得 Shell Root")));
        r.respond("phone\tdevice"); r.respond(""); r.respond("1 file pushed");
        QCOMPARE(done.size(),1); QVERIFY(!done[0][0].toBool());
    }
    void resultsAndModuleFormat(){
        QVERIFY(OpenListInstaller::success(0,true,"Success\n",true));QVERIFY(!OpenListInstaller::success(0,true,"Success\nFailure [bad]",true));QVERIFY(!OpenListInstaller::success(0,false,"Success",true));QVERIFY(!OpenListInstaller::success(0,true,"",true));
        // 非 APK 阶段以退出码为准：输出中的 not found/error 等关键词来自
        // 模块脚本"可选组件不存在则跳过"的正常分支，不再否决 rc==0 的结果。
        QVERIFY(OpenListInstaller::success(0,true,"Done\nERROR: bad",false));
        QVERIFY(!OpenListInstaller::success(1,true,"Done",false));
        QVERIFY(!OpenListInstaller::success(0,false,"Done",false));
        QVERIFY(OpenListInstaller::validModuleListing("Path = module.prop\nEncrypted = -"));QVERIFY(!OpenListInstaller::validModuleListing("Path = nested/module.prop"));QVERIFY(!OpenListInstaller::validModuleListing("Path = module.prop\nEncrypted = +"));QVERIFY(!OpenListInstaller::validModuleListing("Path = module.prop\nPath = ../bad"));
        QVERIFY(OpenListInstaller::validModuleProperties("id=test.module\nname=Test"));QVERIFY(!OpenListInstaller::validModuleProperties("id=../test\nname=Test"));
        QCOMPARE(OpenListInstaller::authorizedDevices("List of devices attached\na\tdevice\nb\tunauthorized\nc\toffline\nd\tdevice\n"),QStringList({"a","d"}));
    }
    void apkInstall_data(){QTest::addColumn<bool>("ok");QTest::newRow("success")<<true;QTest::newRow("failure")<<false;}
    void apkInstall(){QFETCH(bool,ok);QTemporaryDir dir;QString f=dir.filePath("a.apk"),other=dir.filePath("old.apk");QVERIFY(writeFile(f,"a"));QVERIFY(writeFile(other,"old"));Runner runner;OpenListInstaller s(nullptr,&runner,"fake-adb","fake-7z");QSignalSpy done(&s,&OpenListInstaller::completed);QVERIFY(s.install(f,false,"phone"));QVERIFY(DeviceOperationLease::owner()==&s);runner.respond("phone\tdevice\n");runner.denyApkRoot();QCOMPARE(runner.calls.last().args,QStringList({"-s","phone","install","-r",f}));runner.respond(ok?"Success\n":"Success\nFailure [bad]",0);if(!ok){runner.respond("phone\tdevice");runner.respond("");runner.respond("1 file pushed");}QCOMPARE(done.size(),1);QCOMPARE(done[0][0].toBool(),ok);QCOMPARE(QFileInfo::exists(f),!ok);QVERIFY(QFileInfo::exists(other));}
    void disconnectAndRootRefusal(){QTemporaryDir dir;QString f=dir.filePath("m.zip");QVERIFY(writeFile(f,QByteArray("PK\x03\x04",4)));Runner r;OpenListInstaller s(nullptr,&r,"adb","7z");QSignalSpy done(&s,&OpenListInstaller::completed);s.install(f,true,"original");r.respond("other\tdevice");QCOMPARE(done.size(),1);QVERIFY(QFileInfo::exists(f));s.install(f,true,"original");r.respond("original\tdevice");r.respond("Path = module.prop\nEncrypted = -");r.respond("Everything is Ok");r.respond("id=test\nname=Test");r.respond("permission denied",1);r.respond("original\tdevice");r.respond("");r.respond("1 file pushed");QCOMPARE(done.size(),2);QVERIFY(QFileInfo::exists(f));}
    void moduleChoiceAndCleanup(){QTemporaryDir dir;QString f=dir.filePath("m.zip");QVERIFY(writeFile(f,QByteArray("PK\x03\x04",4)));Runner r;OpenListInstaller s(nullptr,&r,"adb","7z");QSignalSpy choice(&s,&OpenListInstaller::rootChoiceRequired),done(&s,&OpenListInstaller::completed);s.install(f,true,"phone");r.respond("phone\tdevice");r.respond("Path = module.prop\nEncrypted = -");r.respond("Everything is Ok");r.respond("id=test\nname=Test");r.respond("0\n");r.respond("OL_MAGISK=/data/adb/magisk/magisk\nOL_KERNELSU=/data/adb/ksu/bin/ksud\n");QCOMPARE(choice.size(),1);s.chooseRoot("KernelSU");QCOMPARE(r.calls.last().args.value(2),QString("push"));QString remote=r.calls.last().args.last();QVERIFY(remote.startsWith("/data/local/tmp/orange-openlist-"));r.respond("1 file pushed");QVERIFY(r.calls.last().args.last().contains("ksud"));r.respond("Done\nOL_INSTALL_SUCCESS\n");QVERIFY(r.calls.last().args.last().contains("rm -f"));r.respond("device disconnected",1);QCOMPARE(done.size(),1);QVERIFY(done[0][0].toBool());QVERIFY(!QFileInfo::exists(f));}
    void rootApkInstall_data() {
        QTest::addColumn<int>("mode");
        QTest::newRow("root-shell") << 0;
        QTest::newRow("su-c") << 1;
        QTest::newRow("su-s-only") << 2;
        QTest::newRow("su-s-retry") << 3;
        QTest::newRow("failure-transferred") << 4;
        QTest::newRow("unknown-not-retried") << 5;
        QTest::newRow("push-failed") << 6;
        QTest::newRow("cleanup-failed-install-still-success") << 7;
        QTest::newRow("success-keyword-with-failure") << 8;
        QTest::newRow("su-c-unknown-not-retried") << 9;
        QTest::newRow("su-c-crashed-not-retried") << 10;
    }
    void rootApkInstall() {
        QFETCH(int,mode);
        QTemporaryDir dir; const QString file=dir.filePath(QStringLiteral("中文应用's.apk"));
        QVERIFY(writeFile(file,"apk"));
        Runner r; OpenListInstaller s(nullptr,&r,"adb","7z");
        QSignalSpy done(&s,&OpenListInstaller::completed),logs(&s,&OpenListInstaller::log);
        QVERIFY(s.install(file,false,"original")); r.respond("original\tdevice");
        QCOMPARE(r.calls.last().args,QStringList({"-s","original","shell","id -u"}));
        if (mode==1 || mode==2 || mode==3 || mode==9 || mode==10) {
            r.respond("2000"); QVERIFY(r.calls.last().args.last().startsWith("su -c "));
            if (mode==2) { r.respond("permission denied",1); QVERIFY(r.calls.last().args.last().startsWith("su -s ")); }
        }
        r.respond("0\n");
        QCOMPARE(r.calls.last().args.mid(0,4),QStringList({"-s","original","push",file}));
        const QString remote=r.calls.last().args.last();
        QVERIFY(remote.startsWith("/data/local/tmp/orange-openlist-")); QVERIFY(remote.endsWith(".apk"));
        QVERIFY(DeviceOperationLease::owner()==&s); QVERIFY(s.busy());
        r.respond(mode==6 ? "error: disconnected" : "1 file pushed",mode==6 ? 1 : 0);
        if (mode!=6) {
            QVERIFY(r.calls.last().args.last().contains("pm install -r"));
            QCOMPARE(r.calls.last().timeout,0);
            if (mode==0) QVERIFY(!r.calls.last().args.last().contains("su "));
            if (mode==1 || mode==3 || mode==9 || mode==10) QVERIFY(r.calls.last().args.last().startsWith("su -c "));
            if (mode==2) QVERIFY(r.calls.last().args.last().startsWith("su -s "));
            if (mode==3) {
                r.respond("Failure [INSTALL_FAILED_USER_RESTRICTED]",1);
                QVERIFY(r.calls.last().args.last().startsWith("su -s "));
            }
            r.respond(mode==4 ? "Failure [bad]" : mode==5 || mode==9 ? "" : mode==8 ? "Success\nFailure [bad]" : "Success\n",mode==4 ? 1 : 0,mode!=10);
        }
        QVERIFY(r.calls.last().args.last().contains("rm -f"));
        QVERIFY(r.calls.last().args.last().contains(ShellCommand::quote(remote)));
        QCOMPARE(done.size(),0); QVERIFY(DeviceOperationLease::owner()==&s);
        r.respond(mode==7 ? "device offline" : "",mode==7 ? 1 : 0);
        const bool ok=mode<=3 || mode==7;
        if (!ok) {
            QCOMPARE(r.calls.last().args,QStringList({"devices"}));
            r.respond("original\tdevice"); r.respond("");
            QCOMPARE(r.calls.last().args.mid(0,4),QStringList({"-s","original","push",file}));
            QVERIFY(r.calls.last().args.last().startsWith("/sdcard/"));
            r.respond("1 file pushed");
        }
        for(const auto &line:logs) QVERIFY(!line[0].toString().contains(QStringLiteral("已获得 Shell Root 权限")));
        QCOMPARE(done.size(),1); QCOMPARE(done[0][0].toBool(),ok);
        QCOMPARE(QFileInfo::exists(file),!ok);
        QVERIFY(!s.busy()); QVERIFY(!DeviceOperationLease::owner());
        for (const auto &call:r.calls) {
            if(call.args==QStringList({"devices"})) continue;
            QCOMPARE(call.args.mid(0,2),QStringList({"-s","original"}));
        }
    }
    void failedInstallTransfer_data() {
        QTest::addColumn<int>("mode");
        QTest::newRow("transferred") << 0;
        QTest::newRow("disconnected-no-switch") << 1;
        QTest::newRow("unauthorized") << 2;
        QTest::newRow("destination-unwritable") << 3;
        QTest::newRow("push-failed") << 4;
        QTest::newRow("push-crashed") << 5;
        QTest::newRow("push-keyword-with-zero-exit") << 6;
        QTest::newRow("partial-cleanup-failed") << 7;
    }
    void failedInstallTransfer() {
        QFETCH(int,mode);
        QTemporaryDir dir;
        const QString file = dir.filePath(QStringLiteral("中文应用's (1).apk"));
        const QString existing = dir.filePath("old.apk");
        QVERIFY(writeFile(file,"apk")); QVERIFY(writeFile(existing,"old"));
        Runner r; OpenListInstaller s(nullptr,&r,"adb","7z");
        QSignalSpy done(&s,&OpenListInstaller::completed);
        QVERIFY(s.install(file,false,"original"));
        r.respond("original\tdevice"); r.denyApkRoot(); r.respond("Failure [INSTALL_FAILED_USER_RESTRICTED]",1);
        QVERIFY(s.busy()); QCOMPARE(done.size(),0);
        QCOMPARE(DeviceOperationLease::owner(),static_cast<QObject*>(&s));
        QVERIFY(!s.install(file,false,"other"));
        QCOMPARE(r.calls.last().args,QStringList({"devices"}));
        r.respond(mode==1 ? "other\tdevice" : mode==2 ? "original\tunauthorized" : "original\tdevice");
        if (mode!=1 && mode!=2) {
            QCOMPARE(r.calls.last().args.mid(0,3),QStringList({"-s","original","shell"}));
            QVERIFY(r.calls.last().args.last().contains("/sdcard/"));
            QVERIFY(r.calls.last().args.last().startsWith("set -C;"));
            r.respond(mode==3 ? "permission denied" : "",mode==3 ? 1 : 0);
            if (mode!=3) {
                QCOMPARE(r.calls.last().args.mid(0,4),QStringList({"-s","original","push",file}));
                const QString remote=r.calls.last().args.last();
                QVERIFY(remote.startsWith("/sdcard/"));
                QVERIFY(remote.endsWith(".apk"));
                QVERIFY(remote.contains(QFileInfo(file).completeBaseName()));
                QCOMPARE(remote.split('/').size(),3);
                QVERIFY(DeviceOperationLease::owner()==&s);
                // mode 6：push 输出 error 字样但退出码 0——按退出码判定推送成功，
                // 不再触发关键词误判后的降级清理。
                r.respond(mode==0 || mode==6 ? "1 file pushed" : "error: no space",mode==4 || mode==7 ? 1 : 0,mode!=5);
                if(mode!=0 && mode!=6) {
                    QVERIFY(r.calls.last().args.last().contains("rm -f"));
                    QVERIFY(!r.calls.last().args.last().contains("rmdir"));
                    r.respond(mode==7 ? "device offline" : "",mode==7 ? 1 : 0);
                }
            }
        }
        QCOMPARE(done.size(),1); QVERIFY(!done[0][0].toBool());
        const QString result=done[0][1].toString();
        QVERIFY(result.contains(QStringLiteral("安装包保留在")));
        // Successful fallback push is intentionally silent after log simplification.
        QVERIFY(!result.contains(QStringLiteral("已推送到手机")));
        if(mode==7) QVERIFY(result.contains(QStringLiteral("不完整文件清理失败")));
        QVERIFY(QFileInfo::exists(file)); QVERIFY(QFileInfo::exists(existing));
        QVERIFY(!s.busy()); QVERIFY(!DeviceOperationLease::owner());
    }
    void directTransfer_data() {
        QTest::addColumn<int>("mode");
        QTest::newRow("success") << 0;
        QTest::newRow("disconnected") << 1;
        QTest::newRow("unauthorized") << 2;
        QTest::newRow("storage-denied") << 3;
        QTest::newRow("push-failed") << 4;
        QTest::newRow("abnormal-exit") << 5;
        QTest::newRow("cleanup-failed") << 6;
    }
    void directTransfer() {
        QFETCH(int,mode); QTemporaryDir dir;
        const QString file=dir.filePath(QStringLiteral("中文 ' 模块.zip"));
        const QString old=dir.filePath("old.zip");
        QVERIFY(writeFile(file,"zip")); QVERIFY(writeFile(old,"old"));
        Runner r; OpenListInstaller s(nullptr,&r,"adb","7z");
        QSignalSpy done(&s,&OpenListInstaller::completed);
        QVERIFY(s.transfer(file,"original")); QVERIFY(DeviceOperationLease::owner()==&s);
        QVERIFY(!s.transfer(file,"other")); QCOMPARE(r.calls.size(),1);
        r.respond(mode==1 ? "other\tdevice" : mode==2 ? "original\tunauthorized" : "original\tdevice");
        if (mode!=1 && mode!=2) {
            QCOMPARE(r.calls.last().args.mid(0,3),QStringList({"-s","original","shell"}));
            QVERIFY(r.calls.last().args.last().startsWith("set -C;"));
            r.respond(mode==3 ? "permission denied" : "",mode==3 ? 1 : 0);
            if (mode!=3) {
                QCOMPARE(r.calls.last().args.mid(0,4),QStringList({"-s","original","push",file}));
                const QString remote=r.calls.last().args.last();
                QVERIFY(remote.startsWith("/sdcard/")); QVERIFY(remote.endsWith(".zip"));
                QCOMPARE(remote.split('/').size(),3);
                r.respond(mode==0 ? "1 file pushed" : "error: no space",mode==0 ? 0 : 1,mode!=5);
                if (mode!=0) {
                    QVERIFY(r.calls.last().args.last().contains("rm -f"));
                    QVERIFY(r.calls.last().args.last().contains(ShellCommand::quote(remote)));
                    r.respond(mode==6 ? "device offline" : "",mode==6 ? 1 : 0);
                }
            }
        }
        QCOMPARE(done.size(),1); QCOMPARE(done[0][0].toBool(),mode==0);
        QVERIFY(done[0][1].toString().contains(QStringLiteral("下载文件保留在")));
        QVERIFY(QFileInfo::exists(file)); QVERIFY(QFileInfo::exists(old));
        QVERIFY(!DeviceOperationLease::owner()); QVERIFY(!s.busy());
        for (const auto &call : r.calls) {
            QVERIFY(!call.args.contains("install")); QVERIFY(!call.args.join(' ').contains("pm install"));
        }
        // A subsequent APK installation returns to normal cleanup; a ZIP is not an APK.
        const QString apk=dir.filePath("next.apk"); QVERIFY(writeFile(apk,"apk"));
        QVERIFY(s.install(apk,"original"));
        r.respond("original\tdevice"); r.denyApkRoot(); r.respond("Success");
        QCOMPARE(done.size(),2); QVERIFY(done[1][0].toBool()); QVERIFY(!QFileInfo::exists(apk));
        QVERIFY(QFileInfo::exists(file));
    }
    void transferPreflightRejectsInvalidTargetOrFile() {
        QTemporaryDir dir; const QString file=dir.filePath("a.apk"); QVERIFY(writeFile(file,"apk"));
        Runner r; OpenListInstaller s(nullptr,&r,"adb","7z"); QSignalSpy done(&s,&OpenListInstaller::completed);
        QVERIFY(!s.transfer(file,{})); QVERIFY(!s.transfer(file,"bad serial"));
        QObject other; QVERIFY(DeviceOperationLease::acquire(&other));
        QVERIFY(!s.transfer(file,"phone")); QCOMPARE(DeviceOperationLease::owner(),&other);
        DeviceOperationLease::release(&other);
        QVERIFY(!s.transfer(dir.filePath("missing.apk"),"phone"));
        QVERIFY(!DeviceOperationLease::owner()); QCOMPARE(r.calls.size(),0);
        QCOMPARE(done.size(),4); QVERIFY(QFileInfo::exists(file));
    }
    void cleanupFailure() {
        QTemporaryDir dir; QString f=dir.filePath("a.apk"); QVERIFY(writeFile(f,"a"));
        Runner r; OpenListInstaller s(nullptr,&r,"adb","7z"); QSignalSpy done(&s,&OpenListInstaller::completed);
        s.install(f,false,"p"); r.respond("p\tdevice"); r.denyApkRoot();
#ifdef Q_OS_WIN
        const auto handle=CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(f).utf16()),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
        QVERIFY(handle!=INVALID_HANDLE_VALUE);
        r.respond("Success\n"); CloseHandle(handle);
        QVERIFY(QFileInfo::exists(f));
        QVERIFY(done[0][1].toString().contains(QStringLiteral("清理失败")));
#else
        r.respond("Success\n");
#endif
        QCOMPARE(done.size(),1); QVERIFY(done[0][0].toBool());
    }
    void linkedDirectoryRejected() {
#ifdef Q_OS_WIN
        QTemporaryDir dir; QString target=dir.filePath("target"),link=dir.filePath("link"); QVERIFY(QDir().mkdir(target));
        if (!CreateSymbolicLinkW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(link).utf16()),reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(target).utf16()),SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2)) QSKIP("Windows symbolic-link permission unavailable");
        QVERIFY(!OpenList::safeDirectory(link,false)); QVERIFY(!OpenList::safeDirectory(link+"/sub",true));
        QVERIFY(!QFileInfo::exists(target+"/sub"));
        const QString root = OpenList::resolveDownloadRoot(sfxDirectory(), link);
        Network network;
        OpenListService service(nullptr, &network, root);
        OpenList::Entry entry; entry.name = "alpha.apk"; entry.relativePath = entry.name; entry.size = 3;
        QVERIFY(!service.downloadDirect(entry, QUrl("https://files.example.com/package")));
        QVERIFY(network.requests.isEmpty()); QVERIFY(!QFileInfo::exists(target + "/download"));
        QVERIFY(RemoveDirectoryW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(link).utf16())));
#endif
    }
    void realZipValidation() {
        const QString tool=qEnvironmentVariable("OPENLIST_7ZIP"); if(tool.isEmpty()) QSKIP("Set OPENLIST_7ZIP for bundled-tool integration test");
        QTemporaryDir dir; QVERIFY(writeFile(dir.filePath("module.prop"),"id=test\nname=Test"));
        QProcess p; p.setWorkingDirectory(dir.path()); p.setProcessChannelMode(QProcess::MergedChannels);
        p.start(tool,{"a","-tzip","fixture.zip","module.prop"}); QVERIFY(p.waitForFinished(10000)); QCOMPARE(p.exitCode(),0);
        p.start(tool,{"l","-slt","-ba","-pOrangeOpenListNoPassword","--",dir.filePath("fixture.zip")}); QVERIFY(p.waitForFinished(10000)); QCOMPARE(p.exitCode(),0);
        QVERIFY(OpenListInstaller::validModuleListing(QString::fromLocal8Bit(p.readAll())));
        p.start(tool,{"t","-bd","-pOrangeOpenListNoPassword","--",dir.filePath("fixture.zip")}); QVERIFY(p.waitForFinished(10000)); QCOMPARE(p.exitCode(),0);
        p.start(tool,{"e","-so","-pOrangeOpenListNoPassword","--",dir.filePath("fixture.zip"),"module.prop"}); QVERIFY(p.waitForFinished(10000)); QCOMPARE(p.exitCode(),0);
        QVERIFY(OpenListInstaller::validModuleProperties(QString::fromLocal8Bit(p.readAll())));
    }
    void deviceDiscoveryFailures() {
        Runner r; OpenListInstaller s(nullptr,&r,"fake-adb","fake-7z");
        QSignalSpy done(&s,&OpenListInstaller::completed), devices(&s,&OpenListInstaller::devicesReady);
        QVERIFY(s.inspectDevices()); r.respond("a\tunauthorized\nb\toffline\n");
        QVERIFY(s.inspectDevices()); r.respond("",-1,false);
        QCOMPARE(done.size(),2); QVERIFY(devices.isEmpty());
        QObject other; QVERIFY(DeviceOperationLease::acquire(&other));
        QVERIFY(!s.inspectDevices()); DeviceOperationLease::release(&other);
    }
    void moduleFailure_data() {
        QTest::addColumn<int>("stage");
        QTest::newRow("invalid-zip")<<0; QTest::newRow("invalid-module")<<1;
        QTest::newRow("push-failed")<<2; QTest::newRow("installer-failed")<<3;
        QTest::newRow("unknown-result")<<4; QTest::newRow("crashed")<<5;
    }
    void moduleFailure() {
        QFETCH(int,stage); QTemporaryDir dir; const QString file=dir.filePath("m.zip");
        QVERIFY(writeFile(file,stage==0 ? QByteArray("ordinary") : QByteArray("PK\x03\x04",4)));
        Runner r; OpenListInstaller s(nullptr,&r,"fake-adb","fake-7z"); QSignalSpy done(&s,&OpenListInstaller::completed);
        s.install(file,true,"phone");
        if(stage>0) {
            r.respond("phone\tdevice"); r.respond(stage==1 ? "Path = nested/module.prop" : "Path = module.prop");
            if(stage>1) {
                r.respond("Everything is Ok"); r.respond("id=test\nname=Test"); r.respond("0\n");
                r.respond("OL_APATCH=/data/adb/ap/bin/apd\n");
                r.respond(stage==2 ? "error: disconnected" : "1 file pushed",stage==2 ? 1 : 0);
                if(stage>2) r.respond(stage==3 ? "OL_INSTALL_SUCCESS\nERROR: denied" : stage==4 ? "" : "OL_INSTALL_SUCCESS",stage==3 ? 1 : 0,stage!=5);
                r.respond("");
                r.respond("phone\tdevice"); r.respond(""); r.respond("1 file pushed");
            }
        }
        QCOMPARE(done.size(),1); QVERIFY(!done[0][0].toBool()); QVERIFY(QFileInfo::exists(file));
    }
    void outstandingListingLifetime() {
        Network n; n.handler=[](const QNetworkRequest&r,const QJsonObject&) {return new Reply(r,{},200,{},true);};
        { OpenListService s(nullptr,&n,"unused"); s.refresh(); s.refresh(); }
        QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    }
    void liveGuestListing() {
        if (!qEnvironmentVariableIsSet("OPENLIST_LIVE_READ")) QSKIP("Read-only cloud integration is opt-in");
        OpenListService s; QSignalSpy ready(&s,&OpenListService::listingReady); s.refresh();
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(),1,90000);
        QVERIFY2(ready[0][1].toString().isEmpty(),qPrintable(ready[0][1].toString()));
        for (const auto &e : qvariant_cast<QList<OpenList::Entry>>(ready[0][0]))
            QVERIFY(!e.relativePath.startsWith('/'));
    }
    void batchQueue_data() {
        QTest::addColumn<int>("mode");
        QTest::newRow("success") << 0;
        QTest::newRow("install-failure") << 1;
        QTest::newRow("download-failure") << 2;
        QTest::newRow("cancel") << 3;
        QTest::newRow("disconnect") << 4;
        QTest::newRow("no-device") << 5;
        QTest::newRow("mixed-apk-module") << 6;
        QTest::newRow("transfer-selected") << 7;
    }
    void batchQueue() {
        QFETCH(int,mode);
        QTemporaryDir dir; QVERIFY(dir.isValid()); Network network;
        int downloads = 0;
        network.handler=[&](const QNetworkRequest &r,const QJsonObject &body) {
            if (r.url().path()=="/api/fs/list") return new Reply(r,api({{"content",QJsonArray{}},{"total",0}}),200);
            if (r.url().path()=="/api/fs/get") {
                ++downloads;
                if (mode==2 && downloads==1) return new Reply(r,"bad",500);
                return new Reply(r,api({{"raw_url","https://files.example.com/"+body.value("path").toString().section('/',-1)},{"size",body.value("path").toString().endsWith(".zip") ? 4 : 3}}),200);
            }
            return new Reply(r,r.url().path().endsWith(".zip") ? QByteArray("PK\x03\x04",4) : QByteArray("abc"),200,{},mode==3 && downloads==2);
        };
        OpenListService service(nullptr,&network,dir.path()); Runner runner;
        OpenListInstaller installer(nullptr,&runner,"fake-adb","fake-7z");
        OpenListWindow window(nullptr,&service,&installer);
        QSignalSpy listed(&service,&OpenListService::listingReady);
        QTRY_COMPARE(listed.size(),1);
        OpenList::Entry first; first.name="first.apk"; first.relativePath=first.name; first.remotePath=QStringLiteral("/A秋白工作室/")+first.name;
        auto second=first; second.name="second.apk"; second.relativePath=second.name; second.remotePath=QStringLiteral("/A秋白工作室/")+second.name;
        OpenList::Entry folder; folder.name="folder"; folder.relativePath=folder.name; folder.remotePath=QStringLiteral("/A秋白工作室/folder"); folder.directory=true;
        if (mode==6) { second.name="module.zip"; second.relativePath=second.name; second.remotePath=QStringLiteral("/A秋白工作室/")+second.name; }
        emit service.listingReady(mode==6 ? QList<OpenList::Entry>{first,folder,second} : QList<OpenList::Entry>{first,second,folder},{});
        auto *table=window.findChild<QTableWidget*>("openListFiles"); QVERIFY(table);
        QCOMPARE(table->selectionMode(),QAbstractItemView::ExtendedSelection);
        table->selectAll();
        auto *button=window.findChild<QPushButton*>(mode==7 ? "openListTransferSelected" : "openListInstallSelected"); QVERIFY(button);
        button->click(); QVERIFY(window.isBusy()); QCOMPARE(runner.calls.size(),1);
        button->click(); emit table->cellDoubleClicked(0,0); QCOMPARE(runner.calls.size(),1);
        if (mode==5) {
            runner.respond("phone\tunauthorized"); QTRY_VERIFY(!window.isBusy()); QCOMPARE(downloads,0); return;
        }
        runner.respond("phone\tdevice");
        QTRY_COMPARE(downloads,2);
        const QString firstFile=dir.filePath(QStringLiteral("软件/first.apk"));
        const QString secondFile=dir.filePath(mode==6 ? QStringLiteral("软件/module.zip") : QStringLiteral("软件/second.apk"));
        if (mode==3) {
            auto *cancel=window.findChild<QPushButton*>();
            for (auto *candidate : window.findChildren<QPushButton*>()) if (candidate->text()==QStringLiteral("取消下载")) cancel=candidate;
            QVERIFY(cancel->isEnabled()); cancel->click(); QTRY_VERIFY(!window.isBusy());
            QCOMPARE(runner.calls.size(),1); QVERIFY(QFileInfo::exists(firstFile)); QVERIFY(!QFileInfo::exists(secondFile));
            QVERIFY(QDir(dir.filePath(QStringLiteral("软件"))).entryList({"*.part"},QDir::Files).isEmpty()); return;
        }
        QTRY_COMPARE(runner.calls.size(),2);
        QVERIFY(QFileInfo::exists(secondFile));
        if (mode!=2) QVERIFY(QFileInfo::exists(firstFile));
        const int installations=mode==2 ? 1 : 2;
        for (int i=0;i<installations;++i) {
            if (mode==4) {
                runner.respond("other-phone\tdevice");
            } else {
                runner.respond("phone\tdevice");
                if (mode==7) {
                    runner.respond("");
                    QCOMPARE(runner.calls.last().args.mid(0,3),QStringList({"-s","phone","push"}));
                    QVERIFY(runner.calls.last().args.last().startsWith("/sdcard/"));
                    runner.respond("1 file pushed");
                } else if (mode==6 && i==1) {
                    QVERIFY(DeviceOperationLease::owner()==&installer);
                    runner.respond("Path = module.prop"); runner.respond("Everything is Ok");
                    runner.respond("id=test\nname=Test"); runner.respond("0\n");
                    runner.respond("OL_APATCH=/data/adb/ap/bin/apd\n");
                    QCOMPARE(runner.calls.last().args.mid(0,3),QStringList({"-s","phone","push"}));
                    runner.respond("1 file pushed"); runner.respond("OL_INSTALL_SUCCESS"); runner.respond("");
                } else {
                    runner.denyApkRoot();
                    QCOMPARE(runner.calls.last().args.mid(0,4),QStringList({"-s","phone","install","-r"}));
                    runner.respond(mode==1 && i==0 ? "Failure [INSTALL_FAILED_USER_RESTRICTED]" : "Success",mode==1 && i==0 ? 1 : 0);
                    if(mode==1 && i==0) { runner.respond("phone\tdevice"); runner.respond(""); runner.respond("1 file pushed"); }
                }
            }
            if (i+1<installations) QTRY_VERIFY(runner.active);
        }
        QTRY_VERIFY(!window.isBusy()); QVERIFY(button->isEnabled());
        QCOMPARE(QFileInfo::exists(firstFile),mode==1 || mode==4 || mode==7);
        QCOMPARE(QFileInfo::exists(secondFile),mode==4 || mode==7);
    }
    void folderSelectionQueuesChildren_data() {
        QTest::addColumn<bool>("transferOnly");
        QTest::newRow("install-folder") << false;
        QTest::newRow("transfer-folder") << true;
    }
    void folderSelectionQueuesChildren() {
        QFETCH(bool,transferOnly);
        QTemporaryDir dir; QVERIFY(dir.isValid()); Network network;
        int downloads = 0;
        network.handler = [&](const QNetworkRequest &r, const QJsonObject &body) {
            if (r.url().path() == "/api/fs/list") return new Reply(r, api({{"content", QJsonArray{}}, {"total", 0}}), 200);
            if (r.url().path() == "/api/fs/get") {
                ++downloads;
                if (body.value("path").toString() != QStringLiteral("/A秋白工作室/folder/child.apk")) return new Reply(r, "bad path", 400);
                return new Reply(r, api({{"raw_url", "https://files.example.com/child.apk"}, {"size", 3}}), 200);
            }
            return new Reply(r, QByteArray("abc"), 200);
        };
        OpenListService service(nullptr, &network, dir.path()); Runner runner;
        OpenListInstaller installer(nullptr, &runner, "fake-adb", "fake-7z");
        OpenListWindow window(nullptr, &service, &installer);
        QSignalSpy listed(&service, &OpenListService::listingReady);
        QTRY_COMPARE(listed.size(), 1);
        OpenList::Entry folder; folder.name = "folder"; folder.relativePath = folder.name;
        folder.remotePath = QStringLiteral("/A秋白工作室/folder"); folder.directory = true;
        OpenList::Entry child; child.name = "child.apk"; child.relativePath = "folder/child.apk";
        child.remotePath = QStringLiteral("/A秋白工作室/folder/child.apk");
        emit service.listingReady({folder, child}, {});
        auto *table = window.findChild<QTableWidget*>("openListFiles"); QVERIFY(table);
        QCOMPARE(table->rowCount(), 1);
        table->selectRow(0);
        auto *button = window.findChild<QPushButton*>(transferOnly ? "openListTransferSelected" : "openListInstallSelected"); QVERIFY(button);
        button->click(); QVERIFY(window.isBusy()); QCOMPARE(runner.calls.size(), 1);
        runner.respond("phone\tdevice");
        QTRY_COMPARE(downloads, 1);
        QTRY_COMPARE(runner.calls.size(), 2);
        runner.respond("phone\tdevice");
        QTRY_COMPARE(runner.calls.size(), 3);
        if (transferOnly) {
            runner.respond("");
            QCOMPARE(runner.calls.last().args.mid(0,3),QStringList({"-s","phone","push"}));
            runner.respond("1 file pushed");
        } else {
            runner.denyApkRoot();
            QCOMPARE(runner.calls.last().args.mid(0, 4), QStringList({"-s", "phone", "install", "-r"}));
            runner.respond("Success");
        }
        QTRY_VERIFY(!window.isBusy());
        QVERIFY(table->selectedItems().isEmpty());
        QCOMPARE(QFileInfo::exists(dir.filePath(QStringLiteral("软件/folder/child.apk"))),transferOnly);
    }
    void folderNavigationAndWindow() {
        QWidget launcher;
        OpenListWindow w(&launcher);
        QVERIFY(!w.parentWidget());
        QCOMPARE(w.minimumSize(), QSize(qRound(866 * 0.9), qRound(729 * 0.9)));
        w.show(); QTest::qWait(50);
        QCOMPARE(w.size(), w.minimumSize());
        QVERIFY((w.frameGeometry().center() - launcher.screen()->availableGeometry().center()).manhattanLength() <= 2);
        #ifdef Q_OS_WIN
        const HWND handle = reinterpret_cast<HWND>(w.winId());
        QVERIFY(GetWindow(handle, GW_OWNER) == nullptr);
        QVERIFY((GetWindowLongPtrW(handle, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) == 0);
        #endif
        w.showMinimized(); QTRY_VERIFY(w.isMinimized());
        #ifdef Q_OS_WIN
        QTRY_VERIFY(IsIconic(handle));
        #endif
        w.showNormal();
        auto *service = w.findChild<OpenListService*>();
        auto *table = w.findChild<QTableWidget*>("openListFiles");
        auto *up = w.findChild<QPushButton*>("openListUp");
        OpenList::Entry dir; dir.name = QStringLiteral("中文分类"); dir.relativePath = dir.name; dir.directory = true;
        OpenList::Entry child; child.name = QStringLiteral("嵌套"); child.relativePath = dir.name + "/" + child.name; child.directory = true;
        OpenList::Entry file; file.name = "same.apk"; file.relativePath = child.relativePath + "/same.apk";
        OpenList::Entry empty; empty.name = QStringLiteral("空目录"); empty.relativePath = empty.name; empty.directory = true;
        emit service->listingReady({dir, child, file, empty}, {});
        QCOMPARE(table->rowCount(), 2); QVERIFY(!up->isEnabled());
        emit table->cellDoubleClicked(0, 0); QCOMPARE(table->rowCount(), 1); QVERIFY(up->isEnabled()); QVERIFY(!w.isBusy());
        emit table->cellDoubleClicked(0, 0); QCOMPARE(table->rowCount(), 1); QCOMPARE(table->item(0, 0)->text(), file.name);
        up->click(); up->click(); QCOMPARE(table->rowCount(), 2);
        emit table->cellDoubleClicked(1, 0); QCOMPARE(table->rowCount(), 0); QVERIFY(!w.isBusy());
        up->click();
        auto *search = table->parentWidget()->findChild<QLineEdit*>(); QVERIFY(search);
        search->setText("same.apk"); QCOMPARE(table->rowCount(), 1);
        QSignalSpy failed(service, &OpenListService::failed);
        QVERIFY(!service->download(dir)); QVERIFY(failed.isEmpty());
        w.close();
    }
    void uiSnapshot(){OpenListWindow w;w.show();QTest::qWait(30);auto *files=w.findChild<QTableWidget*>("openListFiles");QVERIFY(files);for(auto*label:w.findChildren<QLabel*>())QVERIFY(!label->text().contains("pan.xn--ucy"));OpenList::Entry e; e.name=QStringLiteral("很长的中文软件名称用于检查缩放和省略显示.apk"); e.relativePath=e.name; e.size=12345678;
        auto *service=w.findChild<OpenListService*>(); QVERIFY(service);
        auto tool=e; tool.name=QStringLiteral("中文工具脚本.sh"); tool.relativePath=tool.name;
        auto text=tool; text.name="README.txt"; text.relativePath=text.name;
        auto module=tool; module.name="module.zip"; module.relativePath=module.name;
        emit service->listingReady({e,tool,text,module},{});
        QTest::qWait(20); auto img=w.grab();if (!qEnvironmentVariable("OPENLIST_SCREENSHOT").isEmpty()) QVERIFY(img.save(qEnvironmentVariable("OPENLIST_SCREENSHOT")));w.close();}
};
int main(int argc,char **argv) {
    if(argc==2 && QByteArray(argv[1])=="--utf8-fixture") {
#ifdef Q_OS_WIN
        _setmode(_fileno(stdout),_O_BINARY);
#endif
        const QByteArray bytes=QStringLiteral("中文路径/软件/春秋.apk\n📌 模块安装完成\nOL_INSTALL_SUCCESS\n").toUtf8();
        for(char byte:bytes) { std::fwrite(&byte,1,1,stdout); std::fflush(stdout); QThread::msleep(3); }
        return 0;
    }
    QApplication app(argc,argv); OpenListTests tests; return QTest::qExec(&tests,argc,argv);
}
#include "openlisttests.moc"
