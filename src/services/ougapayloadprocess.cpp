#include "ougapayloadprocess.h"
#include <QFuture>
#include <QTimer>
#include <QtConcurrent>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <vector>
#endif

struct OugaPayloadProcess::State {
  QProcess process;
  QTimer poll;
  int exitCode = -1;
  bool normalExit = false, closing = false;
  QString error;
#ifdef Q_OS_WIN
  using CreateConsole = HRESULT (WINAPI *)(COORD, HANDLE, HANDLE, DWORD, HANDLE *);
  using CloseConsole = void (WINAPI *)(HANDLE);
  HANDLE console = nullptr, input = nullptr, output = nullptr;
  STARTUPINFOEXW startup{};
  std::vector<unsigned char> attributes;
  CloseConsole close = nullptr;
  QFuture<void> closed;
  void release() {
    if (input) CloseHandle(input);
    if (output) CloseHandle(output);
    if (startup.lpAttributeList)
      DeleteProcThreadAttributeList(startup.lpAttributeList);
    input = output = nullptr;
    startup = {};
    attributes.clear();
  }
#endif
};
OugaPayloadProcess::OugaPayloadProcess(QObject *parent)
    : QObject(parent), d(new State) {
  d->poll.setInterval(20);
  connect(&d->poll, &QTimer::timeout, this, &OugaPayloadProcess::drain);
  connect(&d->process, &QProcess::readyReadStandardOutput, this, [this] {
    emit output(d->process.readAllStandardOutput());
  });
  connect(&d->process, &QProcess::readyReadStandardError, this, [this] {
    emit output(d->process.readAllStandardError());
  });
  connect(&d->process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
          this, [this](int code, QProcess::ExitStatus status) {
    d->exitCode = code;
    d->normalExit = status == QProcess::NormalExit;
    emit output(d->process.readAllStandardOutput());
    emit output(d->process.readAllStandardError());
    closeTerminal();
  });
  connect(&d->process, &QProcess::errorOccurred, this,
          [this](QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) {
      d->error = d->process.errorString();
      closeTerminal();
    }
  });
}
OugaPayloadProcess::~OugaPayloadProcess() {
  // Normal users cannot close a busy preparation window. Also safely reap only
  // this preparation child if the owner is destroyed during application teardown.
  d->poll.stop();
  disconnect(&d->process, nullptr, this, nullptr);
  if (d->process.state() != QProcess::NotRunning) {
    d->process.kill();
    d->process.waitForFinished();
  }
#ifdef Q_OS_WIN
  if (d->console) {
    const auto console = d->console;
    const auto close = d->close;
    d->console = nullptr;
    d->closed = QtConcurrent::run([console, close] { close(console); });
  }
  // ClosePseudoConsole may block while emitting its final frame. Drain its pipe
  // on this thread while the closer runs, rather than deadlocking on shutdown.
  while (d->closed.isRunning()) {
    DWORD available = 0, read = 0;
    char buffer[8192];
    if (d->output && PeekNamedPipe(d->output, nullptr, 0, nullptr, &available, nullptr)
        && available)
      ReadFile(d->output, buffer, qMin<DWORD>(available, sizeof(buffer)), &read, nullptr);
    else
      QThread::msleep(1);
  }
  d->release();
#endif
}
void OugaPayloadProcess::start(const QString &tool, const QStringList &args,
                               const QString &cwd) {
  d->exitCode = -1;
  d->normalExit = d->closing = false;
  d->error.clear();
#ifdef Q_OS_WIN
  const auto kernel = GetModuleHandleW(L"kernel32.dll");
  const auto create = reinterpret_cast<State::CreateConsole>(
      GetProcAddress(kernel, "CreatePseudoConsole"));
  d->close = reinterpret_cast<State::CloseConsole>(
      GetProcAddress(kernel, "ClosePseudoConsole"));
  HANDLE inputRead = nullptr, outputWrite = nullptr;
  bool ok = create && d->close &&
      CreatePipe(&inputRead, &d->input, nullptr, 0) &&
      CreatePipe(&d->output, &outputWrite, nullptr, 65536);
  if (ok)
    ok = SUCCEEDED(create({200, 100}, inputRead, outputWrite, 0, &d->console));
  if (inputRead) CloseHandle(inputRead);
  if (outputWrite) CloseHandle(outputWrite);
  SIZE_T size = 0;
  if (ok) {
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    d->attributes.resize(size);
    auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(d->attributes.data());
    ok = InitializeProcThreadAttributeList(list, 1, 0, &size);
    if (ok) {
      d->startup.lpAttributeList = list;
      // PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE (older MinGW headers omit it).
      ok = UpdateProcThreadAttribute(list, 0, 0x00020016, d->console,
                                     sizeof(HANDLE), nullptr, nullptr);
    }
  }
  if (!ok) {
    d->error = QString("无法创建 Payload 进度终端（Windows 错误 %1）").arg(GetLastError());
    QTimer::singleShot(0, this, &OugaPayloadProcess::closeTerminal);
    return;
  }
  d->process.setCreateProcessArgumentsModifier([this](QProcess::CreateProcessArguments *a) {
    d->startup.StartupInfo = *a->startupInfo;
    d->startup.StartupInfo.cb = sizeof(STARTUPINFOEXW);
    d->startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    // NULL handles select the pseudoconsole, not Qt's redirected pipes or the
    // parent's handles. Do not change the application's global standard handles.
    d->startup.StartupInfo.hStdInput = nullptr;
    d->startup.StartupInfo.hStdOutput = nullptr;
    d->startup.StartupInfo.hStdError = nullptr;
    a->startupInfo = &d->startup.StartupInfo;
    a->flags = (a->flags & ~(CREATE_NO_WINDOW | DETACHED_PROCESS | CREATE_NEW_CONSOLE))
               | EXTENDED_STARTUPINFO_PRESENT;
    a->inheritHandles = false;
  });
#endif
  auto environment = QProcessEnvironment::systemEnvironment();
  environment.insert("TERM", "xterm-256color");
  environment.insert("PYTHONIOENCODING", "utf-8");
  d->process.setProcessEnvironment(environment);
  d->process.setWorkingDirectory(cwd);
  d->poll.start();
  d->process.start(tool, args);
}
void OugaPayloadProcess::cancel() {
  // This class is exclusively for local package extraction, never flashing.
  if (d->process.state() != QProcess::NotRunning)
    d->process.kill();
}
void OugaPayloadProcess::closeTerminal() {
  if (d->closing) return;
  d->closing = true;
#ifdef Q_OS_WIN
  if (d->console) {
    const auto console = d->console;
    const auto close = d->close;
    d->console = nullptr;
    d->closed = QtConcurrent::run([console, close] { close(console); });
  }
#endif
  d->poll.start();
  drain();
}
void OugaPayloadProcess::drain() {
#ifdef Q_OS_WIN
  DWORD available = 0, count = 0;
  char bytes[8192];
  // A per-tick limit keeps UI cancellation responsive under excessive output.
  for (int n = 0; n < 128 && d->output; ++n) {
    if (!PeekNamedPipe(d->output, nullptr, 0, nullptr, &available, nullptr) || !available)
      break;
    if (!ReadFile(d->output, bytes, qMin<DWORD>(available, sizeof(bytes)), &count, nullptr)
        || !count) break;
    emit output(QByteArray(bytes, count));
  }
  if (!d->closing || d->closed.isRunning()) return;
  if (d->output && PeekNamedPipe(d->output, nullptr, 0, nullptr, &available, nullptr)
      && available) return;
  d->process.setCreateProcessArgumentsModifier({});
  d->release();
#else
  if (!d->closing) return;
#endif
  d->poll.stop();
  emit finished(d->exitCode, d->normalExit, d->error);
}
