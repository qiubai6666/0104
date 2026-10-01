#include "processmanager.h"

#include <QCoreApplication>
#include <QDebug>
#include <QList>
#include <QPointer>
#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
QList<QPointer<QProcess>> &ownedProcesses()
{
    static QList<QPointer<QProcess>> processes;
    return processes;
}

void discardDestroyedProcesses()
{
    auto &processes = ownedProcesses();
    processes.erase(std::remove_if(processes.begin(), processes.end(),
                                   [](const QPointer<QProcess> &process) { return process.isNull(); }),
                    processes.end());
}
}

QProcess *ProcessManager::createProcess(QObject *parent, bool newConsole)
{
    discardDestroyedProcesses();
    QProcess *process = new QProcess(parent);
#ifdef Q_OS_WIN
    if (newConsole) {
        // 让 CMD 本身成为受管进程，并使用新控制台的标准输入/输出。
        process->setProcessChannelMode(QProcess::ForwardedChannels);
        process->setInputChannelMode(QProcess::ForwardedInputChannel);
        process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
            args->flags &= ~CREATE_NO_WINDOW;
            args->flags |= CREATE_NEW_CONSOLE;
            args->startupInfo->dwFlags &= ~(STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW);
        });
    }
#else
    Q_UNUSED(newConsole);
#endif
    ownedProcesses().append(process);
    return process;
}

QProcess *ProcessManager::startProcess(const QString &program, const QStringList &arguments,
                                      const QString &workingDirectory, bool newConsole)
{
    QProcess *process = createProcess(QCoreApplication::instance(), newConsole);
    process->setWorkingDirectory(workingDirectory);

    QObject::connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                     process, &QObject::deleteLater);
    process->start(program, arguments);
    if (!process->waitForStarted(3000)) {
        qWarning() << "启动受管进程失败:" << program << process->errorString();
        delete process;
        return nullptr;
    }
    return process;
}

void ProcessManager::stopAllProcesses()
{
    discardDestroyedProcesses();
    const auto processes = ownedProcesses();
    QList<bool> signalsWereBlocked;

    // 先屏蔽所有完成回调，避免退出时多步骤操作继续启动下一条命令。
    for (const auto &process : processes) {
        signalsWereBlocked.append(process ? process->blockSignals(true) : false);
    }
    for (const auto &process : processes) {
        if (!process) {
            continue;
        }
        if (process->state() == QProcess::Starting) {
            process->waitForStarted(1000);
        }
        if (process->state() != QProcess::NotRunning) {
            // QProcess 在 Windows 持有实际进程句柄，不会因 PID 复用结束其他实例。
            process->kill();
        }
    }
    for (int i = 0; i < processes.size(); ++i) {
        const auto &process = processes.at(i);
        if (!process) {
            continue;
        }
        if (process->state() != QProcess::NotRunning && !process->waitForFinished(1000)) {
            qWarning() << "等待受管进程退出超时:" << process->program();
        }
        process->blockSignals(signalsWereBlocked.at(i));
    }
    // 不枚举或终止派生进程，也不向共享 ADB Server 发送 kill-server。
}