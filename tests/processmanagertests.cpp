#include "processmanager.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QPointer>
#include <QTimer>
#include <QtTest>
#include <cstdio>

#ifdef Q_OS_WIN
#include <windows.h>

// 仅用于本测试启动的 helper；持有句柄后不再按 PID 查找进程。
class HelperHandle
{
public:
    explicit HelperHandle(qint64 pid)
        : handle(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE,
                             FALSE, static_cast<DWORD>(pid))) {}
    ~HelperHandle()
    {
        if (handle) {
            if (WaitForSingleObject(handle, 0) == WAIT_TIMEOUT) {
                TerminateProcess(handle, 0);
                WaitForSingleObject(handle, 3000);
            }
            CloseHandle(handle);
        }
    }
    HANDLE handle;
};
#endif

// 仅运行测试自身的副本，不调用实际的 ADB/Fastboot、不连接设备或访问网络。
class ProcessManagerTests : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();
    void sameNameUnmanagedProcessSurvives_data();
    void sameNameUnmanagedProcessSurvives();
    void cleanupWithoutOwnedProcessesIsHarmless();
    void destroyedOwnerIsIgnored();
    void naturallyFinishedProcessIsReleased();
    void reusableProcessIsStoppedOnEveryRun();
    void startingProcessIsStopped();
    void shutdownDoesNotRunCompletionHandlers();
    void failedStartReturnsNull();
    void newConsoleUsesItsOwnStandardHandles();
    void interactiveConsoleDoesNotBlockDeviceOperations();
    void applicationQuitStopsOwnedProcesses();
    void sharedDescendantIsNotStopped();

private:
    QString helperPath(const QString &name) const;
    bool waitForReady(QProcess *process);
};

QString ProcessManagerTests::helperPath(const QString &name) const
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(name);
}

bool ProcessManagerTests::waitForReady(QProcess *process)
{
    QByteArray output;
    for (int i = 0; i < 30; ++i) {
        output += process->readAllStandardOutput();
        if (output.contains("helper-ready")) {
            return true;
        }
        if (!process->waitForReadyRead(100) && process->state() == QProcess::NotRunning) {
            break;
        }
    }
    return false;
}

void ProcessManagerTests::initTestCase()
{
    const QString executable = QCoreApplication::applicationFilePath();
    for (const QString &name : {QString("adb.exe"), QString("fastboot.exe"),
                                QString("cmd.exe"), QString("NDM.exe")}) {
        // 不覆盖已有文件；测试副本位于专用构建目录，任务结束后统一安全清理。
        if (QFile::exists(helperPath(name))) {
            QFile source(executable);
            QFile copy(helperPath(name));
            QVERIFY(source.open(QIODevice::ReadOnly));
            QVERIFY(copy.open(QIODevice::ReadOnly));
            QCOMPARE(QCryptographicHash::hash(copy.readAll(), QCryptographicHash::Sha256),
                     QCryptographicHash::hash(source.readAll(), QCryptographicHash::Sha256));
        } else {
            QVERIFY2(QFile::copy(executable, helperPath(name)), qPrintable(helperPath(name)));
        }
    }
}

void ProcessManagerTests::cleanup()
{
    ProcessManager::stopAllProcesses();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void ProcessManagerTests::sameNameUnmanagedProcessSurvives_data()
{
    QTest::addColumn<QString>("name");
    QTest::newRow("adb") << QString("adb.exe");
    QTest::newRow("fastboot") << QString("fastboot.exe");
    QTest::newRow("cmd") << QString("cmd.exe");
    QTest::newRow("external-tool") << QString("NDM.exe");
}

void ProcessManagerTests::sameNameUnmanagedProcessSurvives()
{
    QFETCH(QString, name);
    QProcess unrelated;
    unrelated.start(helperPath(name), {"--process-helper"});
    QVERIFY(unrelated.waitForStarted());
    QVERIFY(waitForReady(&unrelated));

    QProcess *owned = ProcessManager::startProcess(helperPath(name), {"--process-helper"},
                                                   QCoreApplication::applicationDirPath());
    QVERIFY(owned);
    QVERIFY(waitForReady(owned));
    ProcessManager::stopAllProcesses();
    QCOMPARE(owned->state(), QProcess::NotRunning);
    QCOMPARE(unrelated.state(), QProcess::Running);
    // 不仅检查 QProcess 缓存，确认外部实例仍真正存活。
    QVERIFY(!unrelated.waitForFinished(100));
    unrelated.kill();
    QVERIFY(unrelated.waitForFinished());
}

void ProcessManagerTests::cleanupWithoutOwnedProcessesIsHarmless()
{
    QProcess unrelated;
    unrelated.start(helperPath("adb.exe"), {"--process-helper"});
    QVERIFY(unrelated.waitForStarted());
    QVERIFY(waitForReady(&unrelated));
    ProcessManager::stopAllProcesses();
    ProcessManager::stopAllProcesses();
    QVERIFY(!unrelated.waitForFinished(100));
    unrelated.kill();
    QVERIFY(unrelated.waitForFinished());
}

void ProcessManagerTests::destroyedOwnerIsIgnored()
{
    QPointer<QProcess> process;
    {
        QObject owner;
        process = ProcessManager::createProcess(&owner);
        process->start(helperPath("NDM.exe"), {"--exit-helper"});
        QVERIFY(process->waitForStarted());
        QVERIFY(process->waitForFinished());
    }
    QVERIFY(process.isNull());
    ProcessManager::stopAllProcesses();
}

void ProcessManagerTests::naturallyFinishedProcessIsReleased()
{
    QPointer<QProcess> process = ProcessManager::startProcess(helperPath("NDM.exe"),
                                    {"--exit-helper"}, QCoreApplication::applicationDirPath());
    QVERIFY(process);
    if (process->state() != QProcess::NotRunning) {
        QVERIFY(process->waitForFinished());
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(process.isNull());
    ProcessManager::stopAllProcesses();
}

void ProcessManagerTests::reusableProcessIsStoppedOnEveryRun()
{
    QObject owner;
    QProcess *process = ProcessManager::createProcess(&owner);
    for (int i = 0; i < 2; ++i) {
        process->start(helperPath("fastboot.exe"), {"--process-helper"});
        QVERIFY(process->waitForStarted());
        QVERIFY(waitForReady(process));
        ProcessManager::stopAllProcesses();
        QCOMPARE(process->state(), QProcess::NotRunning);
        QVERIFY(!process->signalsBlocked());
    }
}

void ProcessManagerTests::startingProcessIsStopped()
{
    QObject owner;
    QProcess *process = ProcessManager::createProcess(&owner);
    process->start(helperPath("adb.exe"), {"--process-helper"});
    ProcessManager::stopAllProcesses();
    QCOMPARE(process->state(), QProcess::NotRunning);
}

void ProcessManagerTests::shutdownDoesNotRunCompletionHandlers()
{
    QObject owner;
    QProcess *first = ProcessManager::createProcess(&owner);
    QProcess *next = ProcessManager::createProcess(&owner);
    bool continued = false;
    connect(first, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), &owner,
            [this, next, &continued]() {
        continued = true;
        next->start(helperPath("adb.exe"), {"--process-helper"});
    });
    first->start(helperPath("adb.exe"), {"--process-helper"});
    QVERIFY(first->waitForStarted());
    QVERIFY(waitForReady(first));
    ProcessManager::stopAllProcesses();
    QVERIFY(!continued);
    QCOMPARE(next->state(), QProcess::NotRunning);
}

void ProcessManagerTests::failedStartReturnsNull()
{
    QVERIFY(!ProcessManager::startProcess(helperPath("does-not-exist.exe"), {},
                                          QCoreApplication::applicationDirPath()));
    ProcessManager::stopAllProcesses();
}

void ProcessManagerTests::newConsoleUsesItsOwnStandardHandles()
{
#ifdef Q_OS_WIN
    // 检查 CMD 的创建参数而不实际弹出可见控制台。
    QObject owner;
    QProcess *process = ProcessManager::createProcess(&owner, true);
    auto modifier = process->createProcessArgumentsModifier();
    QVERIFY(modifier);
    STARTUPINFOW startupInfo = {};
    startupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    QProcess::CreateProcessArguments arguments = {};
    arguments.flags = CREATE_NO_WINDOW;
    arguments.startupInfo = &startupInfo;
    modifier(&arguments);
    QVERIFY(arguments.flags & CREATE_NEW_CONSOLE);
    QVERIFY(!(arguments.flags & CREATE_NO_WINDOW));
    QVERIFY(!(startupInfo.dwFlags & STARTF_USESTDHANDLES));
    QVERIFY(!(startupInfo.dwFlags & STARTF_USESHOWWINDOW));
    QCOMPARE(process->processChannelMode(), QProcess::ForwardedChannels);
    QCOMPARE(process->inputChannelMode(), QProcess::ForwardedInputChannel);
#else
    QSKIP("Windows console creation flags");
#endif
}

void ProcessManagerTests::interactiveConsoleDoesNotBlockDeviceOperations()
{
    // 用户打开的 CMD 不应让“执行重启”等操作误判设备通道占用。
    QObject owner;
    QVERIFY(ProcessManager::createProcess(&owner, true)->property("orangeNonBlockingTool").toBool());
    QProcess *process = ProcessManager::createProcess(&owner);
    process->start(helperPath("adb.exe"), {"--process-helper"});
    QVERIFY(process->waitForStarted());
    QVERIFY(waitForReady(process));
    QVERIFY(ProcessManager::hasActiveDeviceProcesses());
    process->setProperty("orangeNonBlockingTool", true);
    QVERIFY(!ProcessManager::hasActiveDeviceProcesses());
    ProcessManager::stopAllProcesses();
}

void ProcessManagerTests::applicationQuitStopsOwnedProcesses()
{
#ifdef Q_OS_WIN
    QProcess host;
    host.start(QCoreApplication::applicationFilePath(), {"--quit-host"});
    QVERIFY(host.waitForStarted());
    QVERIFY(waitForReady(&host));
    host.setReadChannel(QProcess::StandardError);
    if (host.bytesAvailable() == 0) {
        QVERIFY(host.waitForReadyRead(3000));
    }
    const QByteArray output = host.readAllStandardError().trimmed();
    bool ok = false;
    const qint64 pid = output.toLongLong(&ok);
    QVERIFY2(ok, output.constData());
    HelperHandle child(pid);
    QVERIFY(child.handle);
    host.write("quit\n");
    host.closeWriteChannel();
    QVERIFY(host.waitForFinished());
    QCOMPARE(host.exitCode(), 0);
    QCOMPARE(WaitForSingleObject(child.handle, 3000), DWORD(WAIT_OBJECT_0));
#else
    QSKIP("Windows process handle lifetime");
#endif
}

void ProcessManagerTests::sharedDescendantIsNotStopped()
{
#ifdef Q_OS_WIN
    QProcess *owned = ProcessManager::startProcess(helperPath("NDM.exe"), {"--spawn-helper"},
                                                   QCoreApplication::applicationDirPath());
    QVERIFY(owned);
    QVERIFY(waitForReady(owned));
    owned->setReadChannel(QProcess::StandardError);
    if (owned->bytesAvailable() == 0) {
        QVERIFY(owned->waitForReadyRead(3000));
    }
    const QByteArray output = owned->readAllStandardError().trimmed();
    bool ok = false;
    const qint64 pid = output.toLongLong(&ok);
    QVERIFY2(ok, output.constData());
    HelperHandle service(pid);
    QVERIFY(service.handle);
    ProcessManager::stopAllProcesses();
    QCOMPARE(owned->state(), QProcess::NotRunning);
    // 模拟 ADB Server 的派生服务不会因客户端所属应用退出而被强杀。
    QCOMPARE(WaitForSingleObject(service.handle, 100), DWORD(WAIT_TIMEOUT));
#else
    QSKIP("Windows shared descendant lifetime");
#endif
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    if (arguments.contains("--exit-helper")) {
        return 0;
    }
    if (arguments.contains("--process-helper")) {
        std::puts("helper-ready");
        std::fflush(stdout);
        return application.exec();
    }
    if (arguments.contains("--quit-host") || arguments.contains("--spawn-helper")) {
        const bool quitHost = arguments.contains("--quit-host");
        // 派生服务故意不注册，用于验证不会越过受管实例边界。
        QProcess service;
        QProcess *child = nullptr;
        const QString helper = QDir(application.applicationDirPath()).filePath("adb.exe");
        if (quitHost) {
            child = ProcessManager::startProcess(helper, {"--process-helper"},
                                                 application.applicationDirPath());
        } else {
            service.start(helper, {"--process-helper"});
            if (service.waitForStarted()) {
                child = &service;
            }
        }
        if (!child) {
            return 1;
        }
        std::fprintf(stderr, "%lld\n", static_cast<long long>(child->processId()));
        std::fflush(stderr);
        std::puts("helper-ready");
        std::fflush(stdout);
        if (quitHost) {
            QObject::connect(&application, &QCoreApplication::aboutToQuit,
                             &application, &ProcessManager::stopAllProcesses);
            // 测试端取得子进程句柄后才关闭输入，消除 PID 记录/退出时序的竞争。
            std::getchar();
            QTimer::singleShot(0, &application, &QCoreApplication::quit);
        }
        return application.exec();
    }
    ProcessManagerTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "processmanagertests.moc"