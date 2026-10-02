#ifndef SHELLCOMMAND_H
#define SHELLCOMMAND_H

#include <QString>

// Android adb shell 及 su -c 会各解析一次命令，两个层级都需要引用。
namespace ShellCommand {
inline QString quote(const QString &argument)
{
    QString escaped = argument;
    escaped.replace("'", "'\"'\"'");
    return "'" + escaped + "'";
}

inline QString asRoot(const QString &command, const QString &option = "-c")
{
    return "su " + option + " " + quote(command);
}
}

#endif // SHELLCOMMAND_H
