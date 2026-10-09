#ifndef OUGAFLASHSERVICE_H
#define OUGAFLASHSERVICE_H
#include "ougacommandrunner.h"
#include "ougaflashtypes.h"
#include <QElapsedTimer>
#include <QFile>
#include <QTimer>
#include <functional>
#include <optional>
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
  bool needsFlashCompletionDelay() const { return !m_workflowCompleted || m_stop; }
  void discover(const QString &preferredSerial = {}, std::optional<bool> userspace = {}, bool repair = false);
  void probe(const QString &serial, bool fullPackage = true,
             std::optional<bool> requiredMode = {}, bool readSlot = false, bool repair = false);
  void readPartitionTable(const Ouga::Device &device);
  void resolveFullSlot(const Ouga::Device &device, bool queryAgain);
  void prepareMode(const QString &serial,
                   bool userspace); // explicit preparation, never flash
  void execute(const Ouga::Plan &plan);
  void setLogContext(const QString &source, const QString &interfaceLog);
  void beginFlashSession(Ouga::FlashMode mode, const QString &source);
  void completeFlashSession(bool stopped, bool cancelled);
  void saveFlashSession(const QString &interfaceLog);
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
  void deviceWaitStarted(const QString &title, int remainingSeconds);
  void deviceWaitUpdated(const QString &result, int state);
  void nativeLog(const QString &text, bool error);
  void executionFinishing();
  void resetProgress();
  void finished(bool success, const QString &message);
  void busyChanged(bool busy);

private:
  OugaCommandRunner *m_runner;
  Timing m_timing;
  QString m_tool, m_logBase, m_serial, m_streamedOutput, m_commandFlashTarget;
  QString m_source, m_interfacePrefix, m_nativeOutput;
  QString m_confirmedSlot;
  bool m_recordCommand = true;
  QStringList m_lines, m_failedTargets;
  Ouga::Plan m_plan;
  Ouga::Device m_expected;
  int m_index = 0, m_completed = 0, m_stable = 0, m_successful = 0,
      m_stepLine = -1, m_deviceWaitLine = -1;
  bool m_busy = false, m_stop = false, m_executing = false,
       m_phaseFailed = false, m_clearData = false, m_autoReboot = false,
       m_clearDataStarted = false, m_connectionReady = false, m_fullPackage = true,
       m_summaryLogged = false, m_channelOutputRecorded = false,
       m_logSession = false, m_completionLogged = false;
  bool m_workflowCompleted = false;
  bool m_eraseUserdata = true, m_eraseMetadata = true, m_wipe = true, m_misc = true;
  qint64 m_transferCommitted = 0, m_transferStepBytes = 0, m_transferImageBytes = 0;
  qint64 m_transferAccumulated = 0, m_chunkExpected = 0, m_chunkReported = 0,
         m_transferReported = 0;
  QString m_transferPending;
  bool m_transferFailed = false;
  QElapsedTimer m_modeClock, m_stableClock;
  QTimer m_wait;
  QTimer m_countdown;
  QString m_deviceWaitTitle;
  std::optional<bool> m_requiredMode;
  quint64 m_generation = 0;
  std::function<void(int, bool, const QString &)> m_callback;
  bool acquire();
  void finish(bool success, const QString &message);
  void writeLog(const QString &text);
  void logPartitionSummary();
  void command(const QStringList &args,
               std::function<void(int, bool, const QString &)> callback,
               bool bound = true, int timeoutMs = -1, bool recordNative = true);
  void waitDevice(std::optional<bool> userspace, std::function<void()> ready,
                  std::function<void()> failed = {}, bool showSerial = false,
                  const QString &title = {});
  void pollDevice(std::function<void()> ready, std::function<void()> failed, bool showSerial);
  void probeStable(std::optional<bool> userspace,
                   std::function<void(const Ouga::Device &)> ready,
                   std::function<void()> failed = {}, bool waitFirst = true,
                   const QString &title = "等待设备连接稳定...");
  void pollStable(std::function<void(const Ouga::Device &)> ready,
                  std::function<void()> failed);
  void queryDevice(std::function<void(const Ouga::Device &)> ready, bool includeMode);
  void queryUnlock(std::function<void(std::optional<bool>)> ready);
  void queryCurrentSlot(std::function<void(const QString &)> ready, bool useConfirmedSlot);
  void queryMode(std::function<void(bool)> ready);
  void startDeviceWaitLog(const QString &title, int timeoutMs);
  void updateDeviceWaitLog(const QString &result, int state);
  void next();
  void clearData();
  void cleanCowPartitions();
  void deleteCowPartitions(const QStringList &names, int index);
  void completeCowCleanup(bool success);
  void skipSalePostActions();
  void skipSaleModem(const QString &message);
  void updateTransferOutput(const QString &text);
  void beginStep(const QString &title);
  void endStep(bool success);
  void appendNative(const QString &text, bool error);
  bool saveLog(const QString &interfaceLog = {});
  void completeStep(const Ouga::Step &step, bool success, const QString &output);
};
#endif
