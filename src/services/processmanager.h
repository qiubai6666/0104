#ifndef PROCESSMANAGER_H
#define PROCESSMANAGER_H

#include <QProcess>
#include <QString>
#include <QStringList>

// 只管理通过本服务创建并持有的 QProcess，不读取 PID 文件或按名称枚举进程。
// 不接管派生进程（尤其是可能被其他工具共享的 ADB Server）。
class ProcessManager
{
public:
    // 创建可复用的受管进程；生命周期仍由调用方指定的 QObject 父对象管理。
    static QProcess *createProcess(QObject *parent, bool newConsole = false);

    // 启动外部工具并持续持有进程对象，结束后自动释放；启动失败返回 nullptr。
    // Windows 下 newConsole 用于直接打开独立 CMD 窗口，而不是通过 start 脱离管理。
    static QProcess *startProcess(const QString &program, const QStringList &arguments,
                                  const QString &workingDirectory, bool newConsole = false);

    // 仅结束仍存活的受管进程；可重复调用，不凭历史 PID 查找系统进程。
    static void stopAllProcesses();

private:
    ProcessManager() = default;
};

#endif // PROCESSMANAGER_H