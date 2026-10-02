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
        stableSamples = 3, waitScale = 1, readTimeoutMs = 5000;
  };
  explicit OugaFlashService(OugaCommandRunner *runner,
                            QObject *parent = nullptr);
  void configure(const QString &fastboot, const QString &logDirectory);
  void setTiming(const Timing &timing) { m_timing = timing; }
  bool busy() const { return m_busy; }
  bool paused() const { return m_paused; }
  void discover();
  void probe(const QString &serial);
  void prepareMode(const QString &serial,
                   bool userspace); // explicit preparation, never flash
  void execute(const Ouga::Plan &confirmedPlan);
  void requestStop();
  void confirmCheckpoint(bool proceed);
signals:
  void devicesFound(const QStringList &serials);
  void deviceReady(const Ouga::Device &device);
  void log(const QString &text);
  void progress(int completedWrites, int totalWrites, const QString &stage);
  void checkpoint(const Ouga::Plan &updatedPlan);
  void finished(bool success, const QString &message);
  void busyChanged(bool busy);

private:
  OugaCommandRunner *m_runner;
  Timing m_timing;
  QString m_tool, m_logBase, m_logDir, m_serial, m_lastSignature;
  QFile m_log;
  Ouga::Plan m_plan;
  Ouga::Device m_expected;
  int m_index = 0, m_completed = 0, m_stable = 0, m_checkpointCount = 0;
  bool m_busy = false, m_stop = false, m_paused = false, m_expectedMode = true;
  QElapsedTimer m_modeClock, m_stableClock;
  QTimer m_wait;
  quint64 m_generation = 0;
  std::function<void(int, bool, const QString &)> m_callback;
  bool acquire();
  void finish(bool success, const QString &message);
  void writeLog(const QString &text);
  void command(const QStringList &args,
               std::function<void(int, bool, const QString &)> callback,
               bool bound = true);
  void probeStable(bool userspace,
                   std::function<void(const Ouga::Device &)> ready);
  void pollStable(std::function<void(const Ouga::Device &)> ready);
  void queryDevice(std::function<void(int, bool, const QString &)> callback);
  void queryMissing(QStringList keys, QString text,
                    std::function<void(int, bool, const QString &)> callback);
  bool compatible(const Ouga::Device &device, bool initial,
                  QString *error) const;
  void next();
  void verifyImages(std::function<void()> ready);
  bool savePlan(const Ouga::Plan &plan);
};
#endif
