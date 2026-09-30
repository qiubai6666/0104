#ifndef INTEGRITYCHECKER_H
#define INTEGRITYCHECKER_H

#include <QString>
#include <QStringList>
#include <QMap>

class IntegrityChecker
{
public:
    // 验证结果结构
    struct CheckResult {
        bool success;
        QString errorMessage;
        QStringList missingFiles;
        QStringList corruptedFiles;
    };
    
    // 验证程序完整性
    static CheckResult verifyIntegrity();
    
    // 验证嵌入的资源文件是否完整
    static CheckResult verifyEmbeddedResources();
    
    // 验证提取的资源文件是否完整
    static CheckResult verifyExtractedResources();
    
    // 计算文件的SHA256哈希值
    static QString calculateFileHash(const QString &filePath);
    
    // 计算嵌入资源的SHA256哈希值
    static QString calculateResourceHash(const QString &resourcePath);
    
    // 获取关键文件列表
    static QStringList getCriticalFiles();
    
    // 反调试检测
    static bool isDebuggerAttached();
    
private:
    IntegrityChecker() = default;
    
    // 验证单个文件
    static bool verifyFile(const QString &filePath);
};

#endif // INTEGRITYCHECKER_H
