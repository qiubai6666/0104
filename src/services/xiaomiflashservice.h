#ifndef XIAOMIFLASHSERVICE_H
#define XIAOMIFLASHSERVICE_H
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QVector>
#include <QElapsedTimer>

namespace Xiaomi {
enum class Mode { Wipe, KeepData, WipeAndLock };
struct Image { QString partition; qint64 bytes = 0; };
struct Package {
    QString directory, script;
    QByteArray scriptSha256;
    Mode mode = Mode::Wipe;
    QVector<Image> images;
    qint64 totalBytes = 0;
    bool progressPlanValid = false;
};
QString scriptName(Mode mode);
QString modeName(Mode mode);
bool inspectPackage(const QString &directory, Mode mode, Package *package, QString *error);
}

// Executes the selected Xiaomi batch file; only the known swapped COTA erase
// targets in KeepData mode are repaired in a runtime copy. Package parsing is
// otherwise used for progress estimation, not to replace vendor commands.
class XiaomiFlashService : public QObject {
    Q_OBJECT
public:
    explicit XiaomiFlashService(QObject *parent = nullptr);
    bool isBusy() const { return m_busy; }
    bool isChecking() const { return m_checking; }
    bool hasStartedScript() const { return m_scriptStarted; }
    void cancelCheck(); // Only the read-only preflight may be cancelled.
    void configure(const QString &fastbootPath);
    bool start(const Xiaomi::Package &package, QString *error);
signals:
    void log(const QString &text);
    void progress(int percent); // -1: indeterminate/fallback progress
    void warning(const QString &text);
    void progressInfo(const QString &text);
    void checkingChanged(bool checking);
    void finished(bool success, const QString &message);
private:
    friend class XiaomiTests;
    QProcess m_process, m_probe;
    QTimer m_mismatchTimer, m_statusTimer, m_probeTimer;
    bool m_checking = false, m_checkProduct = false, m_probeTimedOut = false;
    bool m_probeCancelled = false, m_scriptStarted = false;
    QString m_detectedSerial;
    QString m_fastboot;
    QByteArray m_pendingOutput;
    Xiaomi::Package m_package;
    bool m_busy = false, m_failed = false, m_mismatchPending = false;
    int m_item = -1, m_lastPercent = 0;
    qint64 m_completedBytes = 0;
    qint64 m_completedChunkBytes = 0;
    qint64 m_currentChunkExpectedBytes = 0;
    qint64 m_currentChunkReportedBytes = 0;
    qint64 m_currentItemTransferredBytes = 0;
    bool m_currentCommandFinished = false;
    QString m_transferRate = "0MB/s";
    QString m_runtimeScript;
    qint64 m_pendingSendingBytes = 0;
    QElapsedTimer m_elapsed;

    void readOutput();
    void handleLine(const QString &line);
    void completeProcess(int code, QProcess::ExitStatus status);
    bool scriptUnchanged(QString *error) const;
    void beginProbe(bool product);
    void completeProbe(int code, QProcess::ExitStatus status);
    void launchScript();
    bool prepareRuntimeScript(QString *error);
    void cleanupRuntimeScript();
    static bool isCommandEcho(const QString &line);
    void finish(bool success, const QString &message);
    void parseFallbackProgress(const QString &line);
    void updateProgress();
    bool activateItem(const QString &partition);
    bool ensureItemActive(const QString &partition);
    void completeChunk();
    void completeItem();
    void setCurrentTransferred(qint64 bytes);
    static qint64 sizeToBytes(const QString &value, const QString &unit);
    static QString formatTransferRate(double bytesPerSecond);
    void updateTransferRate(qint64 bytes, double seconds);
};
#endif
