#ifndef PROCESSMANAGER_H
#define PROCESSMANAGER_H

#include <QString>
#include <QList>
#include <QFile>
#include <QTextStream>
#include <QCoreApplication>

// 记录外部工具 PID，并在显式退出时清理进程。
// Windows 按名称终止进程会影响本程序之外的同名实例。
class ProcessManager
{
public:
    // 记录进程PID
    static void recordPID(qint64 pid);

    // 记录进程PID和名称
    static void recordProcess(qint64 pid, const QString &processName);

    // 结束所有记录的进程
    static void killAllRecordedProcesses();

    // 按进程名结束进程
    static void killProcessByName(const QString &processName);

    // 清空PID记录
    static void clearPIDFile();

    // 获取PID文件路径
    static QString getPIDFilePath();

private:
    ProcessManager() = default;
};

#endif // PROCESSMANAGER_H
