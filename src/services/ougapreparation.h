#ifndef OUGAPREPARATION_H
#define OUGAPREPARATION_H
#include "ougapackage.h"
#include "ougapayloadprocess.h"
#include <atomic>
#include <QFutureWatcher>
#include <QObject>
#include <QProcess>
#include <functional>
class OugaPreparation : public QObject {
  Q_OBJECT
public:
  explicit OugaPreparation(QObject *parent = nullptr);
  bool busy() const { return m_busy; }
  void scan(const QString &directory);
  void payload(const QString &tool, const QString &file, const QString &output,
               const QStringList &selected = {},
               const QString &oldDirectory = {});
  void listPayload(const QString &tool, const QString &file);
  void extractArchive(const QString &tool, const QString &file,
                      const QString &output, bool scanImages = true);
  void makeSuper(const QString &tool, const QString &directory,
                 const QString &output);
  void discoverAdb(const QString &adb);
  void readCurrentArb(const QString &adb, const QString &serial,
                      const QString &output);
  void cancel();
signals:
  void prepared(const QVector<Ouga::Partition> &images,
                const QString &directory);
  void archiveExtracted(const QString &directory);
  void archiveProgress(int percent);
  void payloadProgress(int percent);
  void payloadPartitionStarted(const QString &name);
  void payloadPartitionFinished(const QString &name, bool success);
  void payloadListed(const QStringList &partitions);
  void adbDevicesFound(const QStringList &serials);
  void arbRead(const QString &file, quint32 index);
  void log(const QString &text);
  void finished(bool success, const QString &message);
  void busyChanged(bool busy);

private:
  bool m_busy = false, m_cancel = false, m_lease = false;
  std::atomic_bool m_abort{false}; // read by in-process Payload workers
  QProcess m_process;
  OugaPayloadProcess m_payloadProcess;
  QMap<QString, quint64> m_payloadOperations, m_payloadDone;
  QMap<QString, int> m_payloadRows;
  QByteArray m_payloadLine;
  enum TerminalState { Text, Escape, Csi, Osc, OscEscape };
  TerminalState m_terminalState = Text;
  bool m_payloadActive = false, m_quietOutput = false;
  int m_lastPayloadProgress = -1;
  void consumePayloadOutput(const QByteArray &bytes);
  void parsePayloadCounter();
  void startPayloadRow(const QString &name);
  void nativePayload(const QString &file, const OugaPayloadLayout &layout,
                     const QVector<OugaPayloadEntry> &entries,
                     const QString &output, const QStringList &selected);
  QString m_output;
  struct ArchiveProgressState {
    enum Phase { LeadingSpace, Digits, Percent, Ignore };
    Phase phase = LeadingSpace;
    int percent = 0, digits = 0;
  };
  ArchiveProgressState m_archiveProgressStreams[2];
  bool m_reportArchiveProgress = false;
  int m_lastArchiveProgress = -1;
  std::function<void(bool, const QString &)> m_callback;
  bool begin();
  void end(bool ok, const QString &message);
  bool newOutput(const QString &source, const QString &output, QString *error);
  void run(const QString &tool, const QStringList &args, const QString &cwd,
           std::function<void(bool, const QString &)> done,
           bool reportArchiveProgress = false, bool quietOutput = false);
  void consumeOutput(const QByteArray &bytes, bool standardError,
                     bool final = false);
  void publishArchiveProgress(int percent);
  void work(std::function<QString()> job, std::function<void()> done);
};
#endif
