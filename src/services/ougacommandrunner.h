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
signals:
  void output(const QString &text);
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
  static QString formatToolsError(const QString &fastbootPath);
  static QString bundledToolPath(const QString &resourceDirectory,
                                 const QString &key,
                                 const QString &fallback = {});

private:
  QProcess m_process;
  QTimer m_watchdog, m_readDeadline;
  QString m_output;
  bool m_done = true;
  void finish(int code, bool normal);
};
#endif
