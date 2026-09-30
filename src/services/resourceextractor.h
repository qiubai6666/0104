#ifndef RESOURCEEXTRACTOR_H
#define RESOURCEEXTRACTOR_H

#include <QString>

// 把编译进 exe 的工具资源解包到用户数据目录，并统一提供路径。
class ResourceExtractor
{
public:
    // 提取所有资源到AppData目录
    static bool extractResources();

    // 获取资源目录路径
    static QString getResourcePath();

    // 获取adb.exe路径
    static QString getAdbPath();

    // 获取fastboot.exe路径
    static QString getFastbootPath();

    // 获取Neil.jpg路径（独立于qiubai目录）
    static QString getNeilImagePath();

private:
    ResourceExtractor() = default;
    static bool extractFile(const QString &resourcePath, const QString &outputPath);
};

#endif // RESOURCEEXTRACTOR_H
