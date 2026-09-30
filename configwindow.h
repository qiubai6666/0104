#ifndef CONFIGWINDOW_H
#define CONFIGWINDOW_H

#include <QWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <QVector>
#include <QMessageBox>
#include <QProcess>
#include <QCoreApplication>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QFile>
#include <QMap>
#include <QTimer>

class ConfigWindow : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigWindow(QWidget *parent = nullptr);
    ~ConfigWindow();
    
    void setPosition(int mainMenuX, int mainMenuY, int mainMenuHeight);

private slots:
    void onButtonClicked();
    void onDownloadFinished(QNetworkReply *reply);
    void onFileDownloadFinished(QNetworkReply *reply);
    void onDownloadProgress(qint64 bytesReceived, qint64 bytesTotal);
    void onDownloadTimeout();  // 超时处理

private:
    void setupUI();
    void openNDM();
    void openWebLink();
    void openGeekFlashTool();
    void downloadConfig();
    void loadConfigFile();
    void downloadFile(int id);
    void extractZipAndOpen(const QString &zipPath, const QString &exeName);
    void openExecutable(const QString &exePath);
    QString findFileRecursively(const QString &dirPath, const QString &fileName);
    void onExtractFinished(int exitCode, QProcess::ExitStatus exitStatus);
    
    QVBoxLayout *mainLayout;
    QVector<QPushButton*> buttons;
    QNetworkAccessManager *networkManager;
    QNetworkAccessManager *fileDownloadManager;
    QMap<int, QString> configData;  // id -> 文件类型|文件名|URL
    QMap<QNetworkReply*, int> downloadingFiles;  // reply -> id
    QMap<int, QString> originalButtonTexts;  // id -> 原始按钮文本
    QMap<QNetworkReply*, QByteArray> downloadBuffers;  // reply -> 下载缓冲区
    QMap<QNetworkReply*, QTimer*> downloadTimeoutTimers;  // 超时定时器
    QMap<QNetworkReply*, qint64> lastReceivedBytes;  // 上次接收字节数
    QMap<int, int> retryCount;  // id -> 重试次数
    QProcess *extractProcess;
    QString pendingExeName;
};

#endif // CONFIGWINDOW_H
