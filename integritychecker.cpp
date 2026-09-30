#include "integritychecker.h"
#include "resourceextractor.h"
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QDebug>
#include <QDirIterator>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

// 关键文件列表（必须存在且完整）
QStringList IntegrityChecker::getCriticalFiles()
{
    return QStringList() 
        << "adb.exe"
        << "fastboot.exe"
        << "scrcpy.exe"
        << "scrcpy-server"
        << "payload.exe"
        << "AdbWinApi.dll"
        << "AdbWinUsbApi.dll"
        << "libusb-1.0.dll"
        << "SDL2.dll"
        << "avcodec-61.dll"
        << "avformat-61.dll"
        << "avutil-59.dll"
        << "swresample-5.dll";
}

QString IntegrityChecker::calculateFileHash(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(&file);
    file.close();
    
    return hash.result().toHex();
}

QString IntegrityChecker::calculateResourceHash(const QString &resourcePath)
{
    QFile file(resourcePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(file.readAll());
    file.close();
    
    return hash.result().toHex();
}

bool IntegrityChecker::verifyFile(const QString &filePath)
{
    QFileInfo fileInfo(filePath);
    
    // 检查文件是否存在
    if (!fileInfo.exists()) {
        return false;
    }
    
    // 检查文件大小是否合理（不为0）
    if (fileInfo.size() == 0) {
        return false;
    }
    
    // 尝试打开文件验证可读性
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    file.close();
    
    return true;
}

IntegrityChecker::CheckResult IntegrityChecker::verifyEmbeddedResources()
{
    CheckResult result;
    result.success = true;
    
    QStringList criticalFiles = getCriticalFiles();
    
    for (const QString &fileName : criticalFiles) {
        QString resourcePath = ":/qiubai/qiubai/" + fileName;
        QFile resourceFile(resourcePath);
        
        if (!resourceFile.exists() || !resourceFile.open(QIODevice::ReadOnly)) {
            result.missingFiles.append(fileName);
            result.success = false;
            qDebug() << "嵌入资源缺失:" << fileName;
        } else {
            // 检查资源是否可以正确读取
            QByteArray data = resourceFile.readAll();
            resourceFile.close();
            
            if (data.isEmpty()) {
                result.corruptedFiles.append(fileName);
                result.success = false;
                qDebug() << "嵌入资源损坏:" << fileName;
            }
        }
    }
    
    if (!result.success) {
        result.errorMessage = "程序资源文件不完整，可能已被篡改或损坏！";
    }
    
    return result;
}

IntegrityChecker::CheckResult IntegrityChecker::verifyExtractedResources()
{
    CheckResult result;
    result.success = true;
    
    QString resourcePath = ResourceExtractor::getResourcePath();
    QStringList criticalFiles = getCriticalFiles();
    
    for (const QString &fileName : criticalFiles) {
        QString filePath = resourcePath + "/" + fileName;
        
        if (!verifyFile(filePath)) {
            result.missingFiles.append(fileName);
            result.success = false;
            qDebug() << "提取的资源文件缺失或损坏:" << fileName;
        }
    }
    
    if (!result.success) {
        result.errorMessage = "资源文件提取不完整！";
    }
    
    return result;
}

IntegrityChecker::CheckResult IntegrityChecker::verifyIntegrity()
{
    CheckResult result;
    result.success = true;
    
    qDebug() << "开始程序完整性验证...";
    
    // 1. 反调试检测
    if (isDebuggerAttached()) {
        result.success = false;
        result.errorMessage = "检测到调试器，程序拒绝运行！";
        qDebug() << "警告：检测到调试器";
        return result;
    }
    
    // 2. 验证嵌入的资源
    CheckResult embeddedResult = verifyEmbeddedResources();
    if (!embeddedResult.success) {
        result.success = false;
        result.errorMessage = embeddedResult.errorMessage;
        result.missingFiles = embeddedResult.missingFiles;
        result.corruptedFiles = embeddedResult.corruptedFiles;
        return result;
    }
    
    qDebug() << "程序完整性验证通过";
    return result;
}

bool IntegrityChecker::isDebuggerAttached()
{
#ifdef Q_OS_WIN
    // Windows平台反调试检测
    return IsDebuggerPresent() != 0;
#else
    // 其他平台暂不实现
    return false;
#endif
}
