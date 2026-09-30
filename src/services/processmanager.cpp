#include "processmanager.h"
#include "resourceextractor.h"
#include <QProcess>
#include <QDebug>
#include <QDir>
#include <QTextStream>
#include <QFile>
#include <QThread>
#include <QFileInfo>
#include <QSet>

#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
#endif

QString ProcessManager::getPIDFilePath()
{
    QString qiubaiPath = ResourceExtractor::getResourcePath();
    return qiubaiPath + "/PID.txt";
}

void ProcessManager::recordPID(qint64 pid)
{
    QString pidFile = getPIDFilePath();
    
    QFile file(pidFile);
    if (file.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream out(&file);
        out << pid << "\n";
        file.close();
        qDebug() << "记录进程PID:" << pid;
    } else {
        qDebug() << "无法打开PID文件进行写入:" << pidFile;
    }
}

void ProcessManager::recordProcess(qint64 pid, const QString &processName)
{
    QString pidFile = getPIDFilePath();
    
    QFile file(pidFile);
    if (file.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream out(&file);
        out << pid << "|" << processName << "\n";
        file.close();
        qDebug() << "记录进程 PID:" << pid << "进程名:" << processName;
    } else {
        qDebug() << "无法打开PID文件进行写入:" << pidFile;
    }
}

void ProcessManager::killAllRecordedProcesses()
{
    QString pidFile = getPIDFilePath();
    
    QFile file(pidFile);
    if (!file.exists()) {
        qDebug() << "PID文件不存在:" << pidFile;
        return;
    }
    
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qDebug() << "无法打开PID文件进行读取:" << pidFile;
        return;
    }
    
    QTextStream in(&file);
    QList<qint64> pids;
    QStringList processNames;
    
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (!line.isEmpty()) {
            // 支持两种格式：纯PID 或 PID|进程名
            if (line.contains('|')) {
                QStringList parts = line.split('|');
                if (parts.size() >= 2) {
                    bool ok;
                    qint64 pid = parts[0].toLongLong(&ok);
                    if (ok && pid > 0) {
                        pids.append(pid);
                        processNames.append(parts[1]);
                    }
                }
            } else {
                bool ok;
                qint64 pid = line.toLongLong(&ok);
                if (ok && pid > 0) {
                    pids.append(pid);
                    processNames.append("");
                }
            }
        }
    }
    
    file.close();
    
    qDebug() << "读取到" << pids.size() << "个进程";
    
    if (pids.isEmpty()) {
        qDebug() << "没有需要结束的进程";
        return;
    }
    
    qDebug() << "开始结束进程...";
    
#ifdef Q_OS_WIN
    // 使用Windows API结束进程
    for (qint64 pid : pids) {
        HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
        if (hProcess != NULL) {
            if (TerminateProcess(hProcess, 0)) {
                qDebug() << "成功结束进程 PID:" << pid;
            } else {
                qDebug() << "结束进程失败 PID:" << pid;
            }
            CloseHandle(hProcess);
        } else {
            qDebug() << "无法打开进程 PID:" << pid;
        }
    }
#else
    // 非Windows系统使用kill命令
    for (qint64 pid : pids) {
        QProcess::execute("kill", QStringList() << "-9" << QString::number(pid));
    }
#endif
    
    qDebug() << "进程结束完成";
    
    // 按进程名结束（作为补充）
#ifdef Q_OS_WIN
    QStringList uniqueProcessNames;
    for (const QString &name : processNames) {
        if (!name.isEmpty() && !uniqueProcessNames.contains(name, Qt::CaseInsensitive)) {
            uniqueProcessNames.append(name);
        }
    }
    
    if (!uniqueProcessNames.isEmpty()) {
        qDebug() << "按进程名结束:" << uniqueProcessNames;
        
        for (const QString &processName : uniqueProcessNames) {
            HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (hSnapshot != INVALID_HANDLE_VALUE) {
                PROCESSENTRY32W pe32;
                pe32.dwSize = sizeof(PROCESSENTRY32W);
                
                if (Process32FirstW(hSnapshot, &pe32)) {
                    do {
                        QString exeName = QString::fromWCharArray(pe32.szExeFile);
                        if (exeName.compare(processName, Qt::CaseInsensitive) == 0) {
                            HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, pe32.th32ProcessID);
                            if (hProcess != NULL) {
                                TerminateProcess(hProcess, 0);
                                CloseHandle(hProcess);
                            }
                        }
                    } while (Process32NextW(hSnapshot, &pe32));
                }
                CloseHandle(hSnapshot);
            }
        }
    }
#endif
}

void ProcessManager::killProcessByName(const QString &processName)
{
    qDebug() << "结束进程:" << processName;
    
#ifdef Q_OS_WIN
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe32;
        pe32.dwSize = sizeof(PROCESSENTRY32W);
        
        if (Process32FirstW(hSnapshot, &pe32)) {
            do {
                QString exeName = QString::fromWCharArray(pe32.szExeFile);
                if (exeName.compare(processName, Qt::CaseInsensitive) == 0) {
                    qDebug() << "找到进程:" << processName << "PID:" << pe32.th32ProcessID;
                    
                    HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, pe32.th32ProcessID);
                    if (hProcess != NULL) {
                        if (TerminateProcess(hProcess, 0)) {
                            qDebug() << "成功结束进程:" << processName;
                        }
                        CloseHandle(hProcess);
                    }
                }
            } while (Process32NextW(hSnapshot, &pe32));
        }
        CloseHandle(hSnapshot);
    }
#else
    // 非Windows系统使用killall命令
    QProcess::execute("killall", QStringList() << processName);
#endif
}

void ProcessManager::clearPIDFile()
{
    QString pidFile = getPIDFilePath();
    
    if (QFile::exists(pidFile)) {
        if (QFile::remove(pidFile)) {
            qDebug() << "已清空PID文件";
        } else {
            qDebug() << "无法删除PID文件:" << pidFile;
        }
    }
}
