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
#include "downloadfilewriter.h"
#include <QMap>
#include <QTimer>

class ConfigWindow : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigWindow(QWidget *parent = nullptr);
    ~ConfigWindow();

    void setPosition(int mainMenuX, int mainMenuY, int mainMenuHeight);
    // 启动预热：异步预下载 download_config.txt，避免首次打开配置窗口时阻塞。
    static void preloadConfigFile();

private slots:
    void onButtonClicked();
    void onDownloadFinished(QNetworkReply *reply);
    void onFileDownloadFinished(QNetworkReply *reply);
    void onDownloadProgress(qint64 bytesReceived, qint64 bytesTotal);

private:
    friend class DeviceOperationTests;
    // download_config.txt 的一行数据，只在读取文件时解析分隔符。
    struct DownloadConfig {
        QString fileType;  // exe 或 zip
        QString fileName;
        QString url;
    };
    enum { DownloadOptionCount = 6 };

    void setupUI();
    void openNDM();
    void openWebLink();
    void openGeekFlashTool();
    void downloadConfig();
    void loadConfigFile();
    void refreshConfigButtonLabels();
    void downloadFile(int id, bool retry = false);
    void restoreDownloadButton(int id);
    void extractZipAndOpen(const QString &zipPath, const QString &exeName);
    void openExecutable(const QString &exePath);
    static bool isSafeFileName(const QString &fileName);
    QString findFileRecursively(const QString &dirPath, const QString &fileName);
    void onExtractFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onExtractError(QProcess::ProcessError error);

    QVBoxLayout *mainLayout;
    QVector<QPushButton*> buttons;
    QNetworkAccessManager *networkManager;
    QNetworkAccessManager *fileDownloadManager;
    QMap<int, DownloadConfig> configData;  // 配置编号 -> 下载描述
    QMap<QNetworkReply*, int> downloadingFiles;  // reply -> id
    QMap<int, QString> originalButtonTexts;  // id -> 原始按钮文本
    QMap<QNetworkReply*, DownloadFileWriter*> downloadFiles;  // reply -> 同目录临时文件
    QMap<QNetworkReply*, QString> downloadPaths;  // reply -> 正式目标路径
    QMap<QNetworkReply*, DownloadFileWriter*> configDownloadFiles;  // 配置文件临时文件
    QMap<int, QString> activeDownloadPaths;  // 含等待重试的任务，防止目标冲突
    QMap<int, int> retryCount;  // id -> 重试次数
    QProcess *extractProcess;
    QString pendingExeName;
};

#endif // CONFIGWINDOW_H
