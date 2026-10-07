#include <QtTest>
#include <QApplication>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QNetworkAccessManager>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QFile>
#include <QTableWidget>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QScreen>
#include <QProcess>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
#include "openlistservice.h"
#include "openlistinstaller.h"
#include "openlistwindow.h"
#include "resourceextractor.h"
#include "deviceoperationlease.h"

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
class Reply final : public QNetworkReply {
public:
    QByteArray bytes; qint64 offset=0; bool done=false;
    Reply(const QNetworkRequest &r,QByteArray b,int status,QUrl redirect={},bool defer=false):bytes(b) {
        setRequest(r);setUrl(r.url());setAttribute(QNetworkRequest::HttpStatusCodeAttribute,status);
        if (!redirect.isEmpty()) setAttribute(QNetworkRequest::RedirectionTargetAttribute,redirect);
        open(QIODevice::ReadOnly);
        if (!defer) QTimer::singleShot(0,this,[this] { if(done)return; emit readyRead(); emit downloadProgress(bytes.size(),bytes.size()); done=true;setFinished(true); emit finished(); });
    }
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
};
}
class OpenListTests final : public QObject {
    Q_OBJECT
private slots:
    void cleanup(){ QVERIFY(!DeviceOperationLease::owner()); }
    void safety(){
        QVERIFY(OpenListService::https(QUrl("https://example.com/file"))); QVERIFY(!OpenListService::https(QUrl("http://example.com")));
        QVERIFY(!OpenListService::https(QUrl("https://user:secret@example.com")));
        for(QString s:QStringList{"..","CON.apk","x/y","a:b","a.","x\\y","NUL","a "}) QVERIFY(!OpenList::safeComponent(s));
        QVERIFY(OpenList::safeComponent(QStringLiteral("软件.apk")));
        auto t=OpenList::redact("https://server/x?sign=secret /www/wwwroot/secret token=abc"); QVERIFY(!t.contains("server"));QVERIFY(!t.contains("wwwroot"));QVERIFY(!t.contains("abc"));
        QCOMPARE(OpenListService::sha256({{"hash_info",QJsonObject{{"SHA-256",QString(64,'a')}}}}),QString(64,'a'));
    }
    void recursiveListing(){
        Network network;int appPages=0;
        network.handler=[&](const QNetworkRequest&r,const QJsonObject&b){QString p=b.value("path").toString();QJsonArray a;
            if(p==QStringLiteral("/APK")){int page=b.value("page").toInt();appPages++;if(page==1)a={entry("A.apk"),entry("sub",true),entry("readme.txt"),entry("../bad.apk")};else a={entry("B.APK")};return new Reply(r,api({{"content",a},{"total",101}}),200);}
            if(p.endsWith("/sub"))return new Reply(r,api({{"content",QJsonArray{entry("A.apk")}}, {"total",1}}),200);
            return new Reply(r,api({{"content",QJsonValue(QJsonValue::Null)},{"total",0}}),200);
        };
        OpenListService service(nullptr,&network,"unused");QSignalSpy spy(&service,&OpenListService::listingReady);service.refresh();QTRY_COMPARE(spy.size(),2);QCOMPARE(appPages,2);
        for(const auto&args:spy){bool module=args[0].toBool();auto xs=qvariant_cast<QList<OpenList::Entry>>(args[1]);if(module)QCOMPARE(xs.size(),0);else{QCOMPARE(xs.size(),4); QVERIFY(xs[1].directory); for (const auto &e : xs) QVERIFY(e.remotePath.startsWith("/APK/"));QVERIFY(!args[2].toString().isEmpty());}}
    }
    void categoryFailureIndependent(){
        Network network;network.handler=[](const QNetworkRequest&r,const QJsonObject&b){return new Reply(r,b.value("path").toString()==QStringLiteral("/模块")?QByteArray("bad"):api({{"content",QJsonArray{entry("ok.apk")}},{"total",1}}),200);};
        OpenListService s(nullptr,&network,"unused");QSignalSpy spy(&s,&OpenListService::listingReady);s.refresh();QTRY_COMPARE(spy.size(),2);for(auto a:spy)if(a[0].toBool())QVERIFY(!a[2].toString().isEmpty());else QCOMPARE(qvariant_cast<QList<OpenList::Entry>>(a[1]).size(),1);
    }
    void download_data(){QTest::addColumn<int>("mode");for(int i=0;i<6;++i)QTest::newRow(qPrintable(QString::number(i)))<<i;}
    void download(){
        QFETCH(int,mode);QTemporaryDir dir;QVERIFY(dir.isValid());Network network;
        network.handler=[mode](const QNetworkRequest&r,const QJsonObject&body){
            if(r.url().path()=="/api/fs/get" && body.value("path").toString()!=QStringLiteral("/APK/sub/ok.apk")) return new Reply(r,"bad path",400);
            if(r.url().path()=="/api/fs/get")return new Reply(r,api({{"raw_url",mode==1?"http://unsafe/file":"https://files.example.com/file"},{"size",3},{"hash_info",QJsonObject{{"sha256",mode==2?QString(64,'a'):QString(QCryptographicHash::hash("abc",QCryptographicHash::Sha256).toHex())}}}}),200);
            if(mode==3)return new Reply(r,{},302,QUrl("http://unsafe/file"));
            return new Reply(r,mode==4?"ab":"abc",200,{},mode==5);
        };
        OpenListService s(nullptr,&network,dir.path());OpenList::Entry e;e.name="ok.apk";e.relativePath="sub/ok.apk";e.remotePath=QStringLiteral("/APK/sub/ok.apk");e.size=3;
        QSignalSpy ready(&s,&OpenListService::ready),failed(&s,&OpenListService::failed);QVERIFY(s.download(e));QVERIFY(!s.download(e));
        if(mode==5){QTRY_COMPARE(network.requests.size(),2);s.cancel();}
        if(mode==0){QTRY_COMPARE(ready.size(),1);auto file=ready[0][1].toString();QVERIFY(QFileInfo::exists(file));QVERIFY(writeFile(file,"old"));QVERIFY(s.download(e));QTRY_COMPARE(ready.size(),2);QVERIFY(ready[1][1].toString()!=file);QFile original(file);QVERIFY(original.open(QIODevice::ReadOnly));QCOMPARE(original.readAll(),QByteArray("old"));}
        else{QTRY_COMPARE(failed.size(),1);QVERIFY(ready.isEmpty());}
        QDir d(dir.path()+QStringLiteral("/软件/sub"));QVERIFY(d.entryList({"*.part"},QDir::Files).isEmpty());
        for(const auto&r:network.requests){QVERIFY(r.rawHeader("Authorization").isEmpty());QVERIFY(r.rawHeader("Cookie").isEmpty());}
    }
    void resultsAndModuleFormat(){
        QVERIFY(OpenListInstaller::success(0,true,"Success\n",true));QVERIFY(!OpenListInstaller::success(0,true,"Success\nFailure [bad]",true));QVERIFY(!OpenListInstaller::success(0,false,"Success",true));QVERIFY(!OpenListInstaller::success(0,true,"",true));
        QVERIFY(!OpenListInstaller::success(0,true,"Done\nERROR: bad",false));
        QVERIFY(OpenListInstaller::validModuleListing("Path = module.prop\nEncrypted = -"));QVERIFY(!OpenListInstaller::validModuleListing("Path = nested/module.prop"));QVERIFY(!OpenListInstaller::validModuleListing("Path = module.prop\nEncrypted = +"));QVERIFY(!OpenListInstaller::validModuleListing("Path = module.prop\nPath = ../bad"));
        QVERIFY(OpenListInstaller::validModuleProperties("id=test.module\nname=Test"));QVERIFY(!OpenListInstaller::validModuleProperties("id=../test\nname=Test"));
        QCOMPARE(OpenListInstaller::authorizedDevices("List of devices attached\na\tdevice\nb\tunauthorized\nc\toffline\nd\tdevice\n"),QStringList({"a","d"}));
    }
    void apkInstall_data(){QTest::addColumn<bool>("ok");QTest::newRow("success")<<true;QTest::newRow("failure")<<false;}
    void apkInstall(){QFETCH(bool,ok);QTemporaryDir dir;QString f=dir.filePath("a.apk"),other=dir.filePath("old.apk");QVERIFY(writeFile(f,"a"));QVERIFY(writeFile(other,"old"));Runner runner;OpenListInstaller s(nullptr,&runner,"fake-adb","fake-7z");QSignalSpy done(&s,&OpenListInstaller::completed);QVERIFY(s.install(f,false,"phone"));QVERIFY(DeviceOperationLease::owner()==&s);runner.respond("phone\tdevice\n");QCOMPARE(runner.calls.last().args,QStringList({"-s","phone","install","-r",f}));runner.respond(ok?"Success\n":"Success\nFailure [bad]",0);QCOMPARE(done.size(),1);QCOMPARE(done[0][0].toBool(),ok);QCOMPARE(QFileInfo::exists(f),!ok);QVERIFY(QFileInfo::exists(other));}
    void disconnectAndRootRefusal(){QTemporaryDir dir;QString f=dir.filePath("m.zip");QVERIFY(writeFile(f,QByteArray("PK\x03\x04",4)));Runner r;OpenListInstaller s(nullptr,&r,"adb","7z");QSignalSpy done(&s,&OpenListInstaller::completed);s.install(f,true,"original");r.respond("other\tdevice");QCOMPARE(done.size(),1);QVERIFY(QFileInfo::exists(f));s.install(f,true,"original");r.respond("original\tdevice");r.respond("Path = module.prop\nEncrypted = -");r.respond("Everything is Ok");r.respond("id=test\nname=Test");r.respond("permission denied",1);QCOMPARE(done.size(),2);QVERIFY(QFileInfo::exists(f));}
    void moduleChoiceAndCleanup(){QTemporaryDir dir;QString f=dir.filePath("m.zip");QVERIFY(writeFile(f,QByteArray("PK\x03\x04",4)));Runner r;OpenListInstaller s(nullptr,&r,"adb","7z");QSignalSpy choice(&s,&OpenListInstaller::rootChoiceRequired),done(&s,&OpenListInstaller::completed);s.install(f,true,"phone");r.respond("phone\tdevice");r.respond("Path = module.prop\nEncrypted = -");r.respond("Everything is Ok");r.respond("id=test\nname=Test");r.respond("0\n");r.respond("OL_MAGISK=/data/adb/magisk/magisk\nOL_KERNELSU=/data/adb/ksu/bin/ksud\n");QCOMPARE(choice.size(),1);s.chooseRoot("KernelSU");QCOMPARE(r.calls.last().args.value(2),QString("push"));QString remote=r.calls.last().args.last();QVERIFY(remote.startsWith("/data/local/tmp/orange-openlist-"));r.respond("1 file pushed");QVERIFY(r.calls.last().args.last().contains("ksud"));r.respond("Done\nOL_INSTALL_SUCCESS\n");QVERIFY(r.calls.last().args.last().contains("rm -f"));r.respond("device disconnected",1);QCOMPARE(done.size(),1);QVERIFY(done[0][0].toBool());QVERIFY(!QFileInfo::exists(f));}
    void cleanupFailure() {
        QTemporaryDir dir; QString f=dir.filePath("a.apk"); QVERIFY(writeFile(f,"a"));
        Runner r; OpenListInstaller s(nullptr,&r,"adb","7z"); QSignalSpy done(&s,&OpenListInstaller::completed);
        s.install(f,false,"p"); r.respond("p\tdevice");
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
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(),2,90000);
        for (const auto &result : ready) {
            QVERIFY2(result[2].toString().isEmpty(),qPrintable(result[2].toString()));
            const bool module=result[0].toBool();
            for (const auto &e : qvariant_cast<QList<OpenList::Entry>>(result[1])) {
                QVERIFY(e.directory || e.relativePath.endsWith(module ? ".zip" : ".apk",Qt::CaseInsensitive));
                QVERIFY(!e.relativePath.startsWith('/')); QCOMPARE(e.module,module);
            }
        }
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
        auto *table = w.findChild<QTableWidget*>("openListApps");
        auto *up = w.findChild<QPushButton*>("openListAppsUp");
        OpenList::Entry dir; dir.name = QStringLiteral("中文分类"); dir.relativePath = dir.name; dir.directory = true;
        OpenList::Entry child; child.name = QStringLiteral("嵌套"); child.relativePath = dir.name + "/" + child.name; child.directory = true;
        OpenList::Entry file; file.name = "same.apk"; file.relativePath = child.relativePath + "/same.apk";
        OpenList::Entry empty; empty.name = QStringLiteral("空目录"); empty.relativePath = empty.name; empty.directory = true;
        emit service->listingReady(false, {dir, child, file, empty}, {});
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
        auto *modules = w.findChild<QTableWidget*>("openListModules");
        dir.module = true; file.module = true; file.name = "module.zip"; file.relativePath = dir.name + "/module.zip";
        emit service->listingReady(true, {dir, file}, {});
        QCOMPARE(modules->rowCount(), 1);
        emit modules->cellDoubleClicked(0, 0); QCOMPARE(modules->rowCount(), 1);
        QCOMPARE(modules->item(0, 0)->text(), file.name);
        QCOMPARE(table->item(0, 0)->text(), QString("same.apk")); QVERIFY(!w.isBusy());
        w.close();
    }
    void uiSnapshot(){OpenListWindow w;w.show();QTest::qWait(30);auto *apps=w.findChild<QTableWidget*>("openListApps");auto *modules=w.findChild<QTableWidget*>("openListModules");QVERIFY(apps);QVERIFY(modules);QVERIFY(apps->mapTo(&w,QPoint()).x()<modules->mapTo(&w,QPoint()).x());for(auto*label:w.findChildren<QLabel*>())QVERIFY(!label->text().contains("pan.xn--ucy"));OpenList::Entry e; e.name=QStringLiteral("很长的中文软件名称用于检查缩放和省略显示.apk"); e.relativePath=e.name; e.size=12345678;
        auto *service=w.findChild<OpenListService*>(); QVERIFY(service);
        emit service->listingReady(false,{e},{}); emit service->listingReady(true,{},{});
        QTest::qWait(20); auto img=w.grab();if (!qEnvironmentVariable("OPENLIST_SCREENSHOT").isEmpty()) QVERIFY(img.save(qEnvironmentVariable("OPENLIST_SCREENSHOT")));w.close();}
};
QTEST_MAIN(OpenListTests)
#include "openlisttests.moc"
