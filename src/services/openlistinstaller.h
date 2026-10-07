#ifndef OPENLISTINSTALLER_H
#define OPENLISTINSTALLER_H
#include "ougacommandrunner.h"
#include <QPointer>
#include <QMap>
#include <QStringList>
#include <QTimer>

// Managed process adapter; deadlines apply only to preflight/read-only commands.
class OpenListProcessRunner final : public OugaCommandRunner {
    Q_OBJECT
public:
    explicit OpenListProcessRunner(QObject *parent = nullptr);
    void run(const QString &, const QStringList &, int timeoutMs = 0) override;
    bool running() const override { return m_running; }
private:
    QProcess *m_process;
    QTimer m_deadline, m_watchdog;
    QString m_output;
    bool m_running = false, m_truncated = false;
    void finish(int, bool);
};

class OpenListInstaller final : public QObject {
    Q_OBJECT
public:
    explicit OpenListInstaller(QObject *parent = nullptr, OugaCommandRunner *runner = nullptr,
                               const QString &adb = QString(), const QString &sevenZip = QString());
    bool inspectDevices();
    bool install(const QString &file, bool module, const QString &serial);
    void chooseRoot(const QString &manager);
    bool busy() const { return m_stage != Idle; }
    static QStringList authorizedDevices(const QString &output);
    static bool success(int code, bool normal, const QString &output, bool apk);
    static bool validModuleListing(const QString &output);
    static bool validModuleProperties(const QString &output);
    static bool cleanupDownloadedFile(const QString &file);
signals:
    void devicesReady(const QStringList &serials);
    void rootChoiceRequired(const QStringList &managers);
    void completed(bool success, const QString &message);
    void log(const QString &text);
private:
    enum Stage { Idle, Discover, Verify, ZipList, ZipTest, ZipProperties, RootAccess,
                 Managers, RootChoice, Push, ModuleInstall, RemoteCleanup, ApkInstall };
    Stage m_stage = Idle;
    OugaCommandRunner *m_runner;
    QString m_adb, m_sevenZip, m_file, m_serial, m_remote, m_selectedManager;
    QMap<QString,QString> m_managers;
    bool m_module = false, m_installSuccess = false, m_remoteTouched = false;
    QString m_result;
    void command(Stage, const QString &program, const QStringList &, int timeout = 0);
    void adb(Stage, const QStringList &, int timeout = 0);
    void onCompleted(int, bool, const QString &);
    void end(bool, const QString &);
    void cleanRemote(bool, const QString &);
};
#endif

