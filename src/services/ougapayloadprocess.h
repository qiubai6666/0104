#ifndef OUGAPAYLOADPROCESS_H
#define OUGAPAYLOADPROCESS_H
#include <QObject>
#include <QProcess>
#include <memory>

// The bundled dumper only publishes operation counters to a terminal. Keep its
// extraction implementation unchanged and capture a private, invisible terminal.
class OugaPayloadProcess : public QObject {
  Q_OBJECT
public:
  explicit OugaPayloadProcess(QObject *parent = nullptr);
  ~OugaPayloadProcess() override;
  void start(const QString &tool, const QStringList &args, const QString &cwd);
  void cancel();
signals:
  void output(const QByteArray &bytes);
  void finished(int exitCode, bool normalExit, const QString &error);
private:
  struct State;
  std::unique_ptr<State> d;
  void drain();
  void closeTerminal();
};
#endif
