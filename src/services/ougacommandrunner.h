#ifndef OUGACOMMANDRUNNER_H
#define OUGACOMMANDRUNNER_H
#include <QObject>
#include <QProcess>
#include <QTimer>
class OugaCommandRunner : public QObject {
  Q_OBJECT
public:
  using QObject::QObject;
  virtual void run(const QString &program, const QStringList &arguments,
                   int readTimeoutMs = 0) = 0;
  virtual bool running() const = 0;
  virtual void runWithPolicy(const QString &program, const QStringList &arguments,
                            int timeoutMs, bool acceptOkay) {
    Q_UNUSED(acceptOkay);
    run(program, arguments, timeoutMs);
  }
signals:
  void output(const QString &text);
  void nativeOutputLine(const QString &text, bool standardError);
  void completed(int exitCode, bool normalExit, const QString &output);
  void stalled();
};
class OugaProcessRunner : public OugaCommandRunner {
  Q_OBJECT
public:
  explicit OugaProcessRunner(QObject *parent = nullptr);
  void run(const QString &program, const QStringList &arguments,
           int readTimeoutMs = 0) override;
  bool running() const override;
  void runWithPolicy(const QString &program, const QStringList &arguments,
                     int timeoutMs, bool acceptOkay) override;
  static QString formatToolsError(const QString &fastbootPath);
  static QString bundledToolPath(const QString &resourceDirectory,
                                 const QString &key,
                                 const QString &fallback = {});
  // Explicit roots keep discovery independent of the selected ROM directory.
  static QString resolveToolPath(const QString &resourceDirectory,
                                 const QString &applicationDirectory,
                                 const QString &key,
                                 const QString &configured = {},
                                 const QString &fallback = {});

private:
  QProcess m_process;
  QTimer m_watchdog, m_readDeadline;
  QString m_output;
  QString m_stdoutPending, m_stderrPending;
  bool m_done = true;
  bool m_acceptOkay = false, m_acknowledged = false, m_timedOut = false;
  bool m_controlled = false;
  void start(const QString &program, const QStringList &arguments, int timeoutMs,
             bool controlled, bool acceptOkay);
  void finish(int code, bool normal);
  void captureOutput(bool standardError, bool flush = false);
};
#endif
