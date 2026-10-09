#ifndef OUGAFLASHSERVICE_H
#define OUGAFLASHSERVICE_H
#include "ougacommandrunner.h"
#include "ougaflashtypes.h"
#include <QElapsedTimer>
#include <QFile>
#include <QTimer>
#include <functional>
class OugaFlashService : public QObject {
  Q_OBJECT
public:
  struct Timing {
    int pollMs = 1000, modeTimeoutMs = 120000, stableWindowMs = 30000,
        stableSamples = 3, waitScale = 1, readTimeoutMs = 10000;
  };
  explicit OugaFlashService(OugaCommandRunner *runner,
                            QObject *parent = nullptr);
  void configure(const QString &fastboot, const QString &logDirectory);
  void setTiming(const Timing &timing) { m_timing = timing; }
  bool busy() const { return m_busy; }
  void discover();
  void probe(const QString &serial, bool fullPackage = true);
  void readPartitionTable(const Ouga::Device &device);
  void prepareMode(const QString &serial,
                   bool userspace); // explicit preparation, never flash
  void execute(const Ouga::Plan &plan);
  void setLogContext(const QString &source, const QString &interfaceLog);
  void setPostActions(bool clearData, bool autoReboot);
  void requestStop();
signals:
  void devicesFound(const QStringList &serials);
  void deviceReady(const Ouga::Device &device);
  void log(const QString &text);
  void progress(int completedWrites, int totalWrites, const QString &stage);
  void transferProgress(qint64 bytes, qint64 total);
  void stepStarted(const QString &title);
  void stepFinished(bool success);
  void coolingProgress(int remainingSeconds);
  void nativeLog(const QString &text, bool error);
  void finished(bool success, const QString &message);
  void busyChanged(bool busy);

private:
  OugaCommandRunner *m_runner;
  Timing m_timing;
  QString m_tool, m_logBase, m_serial, m_streamedOutput, m_commandFlashTarget;
  QString m_source, m_interfacePrefix, m_nativeOutput;
  QStringList m_lines, m_failedTargets;
  Ouga::Plan m_plan;
  Ouga::Device m_expected;
  int m_index = 0, m_completed = 0, m_stable = 0, m_successful = 0,
      m_stepLine = -1;
  bool m_busy = false, m_stop = false, m_expectedMode = true, m_executing = false,
       m_phaseFailed = false, m_clearData = false, m_autoReboot = false,
       m_clearDataStarted = false, m_connectionReady = false, m_fullPackage = true,
       m_summaryLogged = false, m_channelOutputRecorded = false;
  bool m_eraseUserdata = true, m_eraseMetadata = true, m_wipe = true, m_misc = true;
  qint64 m_transferCommitted = 0, m_transferStepBytes = 0, m_transferImageBytes = 0;
  qint64 m_transferAccumulated = 0, m_chunkExpected = 0, m_chunkReported = 0,
         m_transferReported = 0;
  QString m_transferPending;
  bool m_transferFailed = false;
  QElapsedTimer m_modeClock, m_stableClock;
  QTimer m_wait;
  quint64 m_generation = 0;
  std::function<void(int, bool, const QString &)> m_callback;
  bool acquire();
  void finish(bool success, const QString &message);
  void writeLog(const QString &text);
  void logPartitionSummary();
  void command(const QStringList &args,
               std::function<void(int, bool, const QString &)> callback,
               bool bound = true, int timeoutMs = -1);
  void probeStable(bool userspace,
                   std::function<void(const Ouga::Device &)> ready,
                   std::function<void()> failed = {});
  void pollStable(std::function<void(const Ouga::Device &)> ready,
                  std::function<void()> failed);
  void queryDevice(std::function<void(int, bool, const QString &)> callback);
  void queryMode(std::function<void(bool)> ready);
  void queryMissing(QStringList keys, QString text,
                    std::function<void(int, bool, const QString &)> callback);
  bool compatible(const Ouga::Device &device, bool initial,
                  QString *error) const;
  void next();
  void clearData();
  void cleanCowPartitions();
  void deleteCowPartitions(const QStringList &names, int index);
  void completeCowCleanup(bool success);
  void skipSalePostActions();
  void skipSaleModem(const QString &message);
  void validateAndResume();
  void updateTransferOutput(const QString &text);
  void beginStep(const QString &title);
  void endStep(bool success);
  void appendNative(const QString &text, bool error);
  bool saveLog();
  void completeStep(const Ouga::Step &step, bool success, const QString &output);
};
#endif
