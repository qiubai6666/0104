#include "resourceextractor.h"
#include "version.h"
#include <QFile>
#include <QDir>
#include <QStandardPaths>
#include <QDebug>
#include <QDirIterator>

QString ResourceExtractor::getResourcePath()
{
    // 直接使用AppData/Local/qiubai目录
    QString appDataPath = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return appDataPath + "/" + RESOURCE_FOLDER;
}

bool ResourceExtractor::extractFile(const QString &resourcePath, const QString &outputPath)
{
    QFile resourceFile(resourcePath);
    if (!resourceFile.open(QIODevice::ReadOnly)) {
        qDebug() << "无法打开资源文件:" << resourcePath;
        return false;
    }

    QFile outputFile(outputPath);
    if (!outputFile.open(QIODevice::WriteOnly)) {
        qDebug() << "无法创建输出文件:" << outputPath;
        resourceFile.close();
        return false;
    }

    const QByteArray data = resourceFile.readAll();
    const bool success = resourceFile.error() == QFileDevice::NoError &&
                         outputFile.write(data) == data.size() && outputFile.flush();
    outputFile.close();
    resourceFile.close();

    return success;
}

bool ResourceExtractor::extractResources()
{
    return extractResources(getResourcePath(), getNeilImagePath());
}

bool ResourceExtractor::extractResources(const QString &targetPath, const QString &neilImagePath)
{
    QDir targetDir(targetPath);

    // 如果目录已存在，先删除
    if (targetDir.exists()) {
        qDebug() << "资源目录已存在，删除旧目录:" << targetPath;
        if (!targetDir.removeRecursively()) {
            qDebug() << "警告：无法完全删除旧目录，尝试继续...";
        }
    }

    // 创建目录
    QDir dir;
    if (!dir.mkpath(targetPath)) {
        qDebug() << "无法创建目录:" << targetPath;
        return false;
    }

    qDebug() << "开始提取资源到:" << targetPath;

    // 遍历所有qiubai资源
    const QDir resourceRoot(":/qiubai/qiubai");
    QDirIterator it(resourceRoot.path(), QDirIterator::Subdirectories);
    int successCount = 0;
    int totalCount = 0;

    while (it.hasNext()) {
        QString resourcePath = it.next();
        QFileInfo fileInfo(resourcePath);

        if (fileInfo.isFile()) {
            totalCount++;
            QString fileName = fileInfo.fileName();
            QString outputPath;

            // Neil.jpg 单独提取到 AppData/Local 目录
            if (fileName == "Neil.jpg") {
                outputPath = neilImagePath;
            } else {
                outputPath = targetDir.filePath(resourceRoot.relativeFilePath(resourcePath));
            }

            // 保留 bin/ 内布局，完整 platform-tools 不与旧版同名文件混用。
            if (dir.mkpath(QFileInfo(outputPath).absolutePath()) &&
                extractFile(resourcePath, outputPath)) {
                successCount++;
                qDebug() << "提取成功:" << fileName;
            } else {
                qDebug() << "提取失败:" << fileName;
            }
        }
    }

    qDebug() << "资源提取完成:" << successCount << "/" << totalCount;
    return totalCount > 0 && successCount == totalCount;
}

QString ResourceExtractor::getAdbPath()
{
    return getResourcePath() + "/adb.exe";
}

QString ResourceExtractor::getFastbootPath()
{
    return getResourcePath() + "/fastboot.exe";
}

QString ResourceExtractor::getNeilImagePath()
{
    // Neil.jpg 独立存放在 AppData/Local 目录（与 qiubai 同级）
    QString appDataPath = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return appDataPath + "/Neil.jpg";
}
