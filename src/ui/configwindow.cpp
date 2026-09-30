#include "configwindow.h"
#include "processmanager.h"
#include "resourceextractor.h"
#include "uihelper.h"
#include <QApplication>
#include <QScreen>
#include <QGuiApplication>
#include <QFile>
#include <QNetworkRequest>
#include <QUrl>
#include <QDesktopServices>
#include <QFileInfo>
#include <QDir>
#include <QEventLoop>
#include <QTimer>

ConfigWindow::ConfigWindow(QWidget *parent)
    : QWidget(parent)
    , extractProcess(nullptr)
{
    // 设置窗口标志：无边框、置顶
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground);

    // 设置窗口标题
    setWindowTitle("配置");

    // 创建网络管理器
    networkManager = new QNetworkAccessManager(this);
    connect(networkManager, &QNetworkAccessManager::finished, this, &ConfigWindow::onDownloadFinished);

    // 创建文件下载管理器
    fileDownloadManager = new QNetworkAccessManager(this);
    connect(fileDownloadManager, &QNetworkAccessManager::finished, this, &ConfigWindow::onFileDownloadFinished);

    // 先检查配置文件是否存在
    QString qiubaiPath = ResourceExtractor::getResourcePath();
    QString configPath = qiubaiPath + "/download_config.txt";

    if (!QFile::exists(configPath)) {
        // 配置文件不存在，先下载
        qDebug() << "配置文件不存在，开始下载...";
        downloadConfig();

        // 等待下载完成（最多3秒）
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        connect(networkManager, &QNetworkAccessManager::finished, &loop, &QEventLoop::quit);
        connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timer.start(3000);
        loop.exec();
    }

    setupUI();
}

ConfigWindow::~ConfigWindow()
{
    if (extractProcess) {
        extractProcess->kill();
        extractProcess->deleteLater();
    }
}

void ConfigWindow::setupUI()
{
    mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(0);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    // 先加载配置文件
    loadConfigFile();

    QStringList buttonTexts;

    // 前 DownloadOptionCount 个按钮来自配置，其余按钮保持固定。
    for (int id = 1; id <= DownloadOptionCount; ++id) {
        QString displayName = QString("配置选项%1").arg(id);
        if (configData.contains(id)) {
            const QString fullName = configData.value(id).fileName;
            const int dotIndex = fullName.lastIndexOf('.');
            displayName = (dotIndex > 0) ? fullName.left(dotIndex) : fullName;
        }
        buttonTexts.append(displayName);
        originalButtonTexts[id] = displayName;
    }

    // 后3个按钮固定
    buttonTexts.append("GeekFlashTool");
    buttonTexts.append("123网盘");
    buttonTexts.append("打开NDM");

    buttons = UIHelper::createMenuButtons(this, mainLayout, buttonTexts);
    for (QPushButton *button : buttons) {
        connect(button, &QPushButton::clicked, this, &ConfigWindow::onButtonClicked);
    }

    setStyleSheet(
        "ConfigWindow {"
        "   background-color: rgba(195, 219, 228, 245);"
        "   border-radius: 10px;"
        "}"
    );

    // 调整窗口大小
    adjustSize();
}

void ConfigWindow::setPosition(int mainMenuX, int mainMenuY, int mainMenuHeight)
{
    Q_UNUSED(mainMenuHeight);

    // 计算位置：主菜单左侧，顶部对齐，无间隔
    int x = mainMenuX - width();  // 紧贴主菜单左侧
    int y = mainMenuY;  // 顶部对齐

    move(x, y);
}

void ConfigWindow::onButtonClicked()
{
    QPushButton *btn = qobject_cast<QPushButton*>(sender());
    if (!btn) return;

    int index = buttons.indexOf(btn);

    // 根据按钮索引执行不同的配置操作
    if (index >= 0 && index < DownloadOptionCount) {
        // 配置选项1-6：下载对应的文件
        downloadFile(index + 1);  // id从1开始
    } else if (index == DownloadOptionCount) {
        openGeekFlashTool();
    } else if (index == DownloadOptionCount + 1) {
        openWebLink();
    } else if (index == DownloadOptionCount + 2) {
        openNDM();
    }
}

void ConfigWindow::openGeekFlashTool()
{
    // 使用默认浏览器打开GeekFlashTool链接
    QUrl url("https://syxz.lanzoue.com/b0mbywcli");
    bool success = QDesktopServices::openUrl(url);

    if (!success) {
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误", "无法打开链接！", this);
    }
}

void ConfigWindow::openWebLink()
{
    // 使用默认浏览器打开网盘链接
    QUrl url("https://www.123865.com/s/Rvc4jv-LhZPA");
    bool success = QDesktopServices::openUrl(url);

    if (!success) {
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误", "无法打开链接！", this);
    }
}

void ConfigWindow::openNDM()
{
    QString qiubaiPath = ResourceExtractor::getResourcePath();
    QString ndmPath = qiubaiPath + "/NDM.exe";

    // 检查文件是否存在
    if (!QFile::exists(ndmPath)) {
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误", "找不到 NDM.exe 文件！", this);
        return;
    }

    // 使用Shell执行（允许UAC提示）
    qint64 pid = 0;
    bool success = QProcess::startDetached(ndmPath, QStringList(), qiubaiPath, &pid);

    if (success && pid > 0) {
        ProcessManager::recordProcess(pid, "NDM.exe");
        qDebug() << "NDM已启动，PID:" << pid;
    } else {
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误", "无法启动 NDM.exe！", this);
    }
}

void ConfigWindow::downloadConfig()
{
    QString qiubaiPath = ResourceExtractor::getResourcePath();
    QString configPath = qiubaiPath + "/download_config.txt";

    // 检查文件是否已存在
    if (QFile::exists(configPath)) {
        qDebug() << "配置文件已存在，跳过下载:" << configPath;
        return;
    }

    // 文件不存在，开始下载
    qDebug() << "配置文件不存在，开始下载...";
    QUrl url("https://gitee.com/qiuabi19/orange-box/raw/master/download_config.txt");
    QNetworkRequest request(url);

    // 设置请求头
    request.setHeader(QNetworkRequest::UserAgentHeader, "Mozilla/5.0");

    networkManager->get(request);
}

void ConfigWindow::loadConfigFile()
{
    QString qiubaiPath = ResourceExtractor::getResourcePath();
    QString configPath = qiubaiPath + "/download_config.txt";

    QFile file(configPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qDebug() << "无法打开配置文件:" << configPath;
        return;
    }

    QTextStream in(&file);
    in.setEncoding(QStringConverter::Utf8);

    // 跳过第一行标题
    if (!in.atEnd()) {
        in.readLine();
    }

    // 读取数据行
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.isEmpty()) continue;

        QStringList parts = line.split('|');
        if (parts.size() >= 4) {
            int id = parts[0].toInt();
            QString fileType = parts[1];
            QString fileName = parts[2];
            QString url = parts[3];
            configData[id] = DownloadConfig{fileType, fileName, url};
        }
    }

    file.close();
    qDebug() << "配置文件加载完成，共" << configData.size() << "项";
}

void ConfigWindow::downloadFile(int id)
{
    if (!configData.contains(id)) {
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误", "配置信息不存在！", this);
        return;
    }

    const DownloadConfig entry = configData.value(id);
    const QString fileType = entry.fileType;
    const QString fileName = entry.fileName;
    const QString url = entry.url;

    QString qiubaiPath = ResourceExtractor::getResourcePath();

    // 根据文件类型确定实际保存的文件名
    QString actualFileName = fileName;
    if (fileType == "zip") {
        // zip类型：如果文件名是.exe，改为.zip
        if (actualFileName.endsWith(".exe", Qt::CaseInsensitive)) {
            actualFileName = actualFileName.left(actualFileName.length() - 4) + ".zip";
        } else if (!actualFileName.endsWith(".zip", Qt::CaseInsensitive)) {
            actualFileName += ".zip";
        }
    }

    QString filePath = qiubaiPath + "/" + actualFileName;

    // 如果是exe且文件已存在，直接打开
    if (fileType == "exe" && QFile::exists(filePath)) {
        openExecutable(filePath);
        return;
    }

    // 如果是zip，检查zip文件或解压后的exe是否存在
    if (fileType == "zip") {
        // 先检查zip文件是否存在
        if (QFile::exists(filePath)) {
            // zip存在，检查是否已解压
            QString exeName = fileName;  // 原始文件名（.exe）
            QString exePath = qiubaiPath + "/" + exeName;
            if (QFile::exists(exePath)) {
                openExecutable(exePath);
                return;
            } else {
                // zip存在但未解压，解压并打开
                extractZipAndOpen(filePath, exeName);
                return;
            }
        }
    }

    // 文件不存在，开始下载
    qDebug() << "开始下载文件:" << fileName << "从" << url;

    // 更新按钮文本为进度
    if (id >= 1 && id <= DownloadOptionCount) {
        buttons[id - 1]->setText("0%");
        buttons[id - 1]->setEnabled(false);
    }

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, "Mozilla/5.0");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    // 禁用HTTP/2，使用HTTP/1.1更稳定
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);

    // 设置缓存策略
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferNetwork);

    // 设置超时（0表示无超时限制）
    request.setTransferTimeout(0);

    QNetworkReply *reply = fileDownloadManager->get(request);
    downloadingFiles[reply] = id;
    downloadBuffers[reply] = QByteArray();  // 初始化缓冲区

    // 不设置缓冲区大小限制，让Qt自动管理
    reply->setReadBufferSize(0);

    // 连接下载进度信号
    connect(reply, &QNetworkReply::downloadProgress, this, &ConfigWindow::onDownloadProgress);

    // 连接readyRead信号，及时读取数据到缓冲区
    connect(reply, &QNetworkReply::readyRead, this, [this, reply]() {
        if (downloadBuffers.contains(reply)) {
            downloadBuffers[reply].append(reply->readAll());
        }
    });

    // 连接错误信号
    connect(reply, &QNetworkReply::errorOccurred, this, [this, reply, id](QNetworkReply::NetworkError code) {
        qDebug() << "下载错误:" << code << reply->errorString();
        // 清理超时定时器
        if (downloadTimeoutTimers.contains(reply)) {
            QTimer *timer = downloadTimeoutTimers.take(reply);
            timer->stop();
            timer->deleteLater();
        }
        lastReceivedBytes.remove(reply);

        if (id >= 1 && id <= DownloadOptionCount) {
            buttons[id - 1]->setText("下载失败");
            QTimer::singleShot(2000, this, [this, id]() {
                if (originalButtonTexts.contains(id)) {
                    buttons[id - 1]->setText(originalButtonTexts[id]);
                    buttons[id - 1]->setEnabled(true);
                }
            });
        }
    });

    // 创建超时定时器（15秒无数据则超时）
    QTimer *timeoutTimer = new QTimer(this);
    timeoutTimer->setSingleShot(false);
    timeoutTimer->setInterval(15000);  // 15秒检测一次
    connect(timeoutTimer, &QTimer::timeout, this, &ConfigWindow::onDownloadTimeout);
    downloadTimeoutTimers[reply] = timeoutTimer;
    lastReceivedBytes[reply] = 0;
    timeoutTimer->start();
}

void ConfigWindow::onDownloadProgress(qint64 bytesReceived, qint64 bytesTotal)
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply || !downloadingFiles.contains(reply)) {
        return;
    }

    // 更新最后接收的字节数
    lastReceivedBytes[reply] = bytesReceived;

    int id = downloadingFiles[reply];

    if (bytesTotal > 0) {
        int progress = (bytesReceived * 100) / bytesTotal;

        // 更新按钮文本显示进度
        if (id >= 1 && id <= DownloadOptionCount) {
            buttons[id - 1]->setText(QString("%1%").arg(progress));
        }
    } else {
        // 如果服务器没有返回总大小，显示已下载的字节数
        if (id >= 1 && id <= DownloadOptionCount) {
            double mb = bytesReceived / (1024.0 * 1024.0);
            buttons[id - 1]->setText(QString("%1MB").arg(mb, 0, 'f', 1));
        }
    }
}

void ConfigWindow::onFileDownloadFinished(QNetworkReply *reply)
{
    // 清理超时定时器
    if (downloadTimeoutTimers.contains(reply)) {
        QTimer *timer = downloadTimeoutTimers.take(reply);
        timer->stop();
        timer->deleteLater();
    }
    lastReceivedBytes.remove(reply);

    if (!downloadingFiles.contains(reply)) {
        reply->deleteLater();
        return;
    }

    int id = downloadingFiles[reply];
    downloadingFiles.remove(reply);

    // 清理重试计数
    retryCount.remove(id);

    // 恢复按钮文本和状态
    if (id >= 1 && id <= DownloadOptionCount && originalButtonTexts.contains(id)) {
        buttons[id - 1]->setText(originalButtonTexts[id]);
        buttons[id - 1]->setEnabled(true);
    }

    if (reply->error() != QNetworkReply::NoError) {
        qDebug() << "下载失败 - 错误代码:" << reply->error() << "错误信息:" << reply->errorString();
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "下载失败",
            QString("文件下载失败：%1").arg(reply->errorString()), this);
        downloadBuffers.remove(reply);  // 清理缓冲区
        reply->deleteLater();
        return;
    }

    // 从缓冲区获取数据，并读取剩余数据
    QByteArray data;
    if (downloadBuffers.contains(reply)) {
        data = downloadBuffers[reply];
        downloadBuffers.remove(reply);
    }
    data.append(reply->readAll());  // 读取可能剩余的数据

    qDebug() << "下载完成，数据大小:" << data.size() << "字节";

    if (data.isEmpty()) {
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误", "下载的文件为空！", this);
        reply->deleteLater();
        return;
    }

    const DownloadConfig entry = configData.value(id);
    const QString fileType = entry.fileType;
    const QString fileName = entry.fileName;

    QString qiubaiPath = ResourceExtractor::getResourcePath();

    // 根据文件类型确定实际保存的文件名
    QString actualFileName = fileName;
    if (fileType == "zip") {
        // zip类型：如果文件名是.exe，改为.zip
        if (actualFileName.endsWith(".exe", Qt::CaseInsensitive)) {
            actualFileName = actualFileName.left(actualFileName.length() - 4) + ".zip";
        } else if (!actualFileName.endsWith(".zip", Qt::CaseInsensitive)) {
            actualFileName += ".zip";
        }
    }

    QString filePath = qiubaiPath + "/" + actualFileName;

    // 保存文件
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误",
            QString("无法保存文件：%1").arg(file.errorString()), this);
        reply->deleteLater();
        return;
    }

    qint64 written = file.write(data);
    file.close();

    qDebug() << "文件保存完成:" << filePath << "写入字节:" << written;

    // 根据文件类型处理
    if (fileType == "exe") {
        openExecutable(filePath);
    } else if (fileType == "zip") {
        // 使用原始文件名（.exe）作为解压后要打开的文件
        extractZipAndOpen(filePath, fileName);
    }

    reply->deleteLater();
}

void ConfigWindow::extractZipAndOpen(const QString &zipPath, const QString &exeName)
{
    QString qiubaiPath = ResourceExtractor::getResourcePath();

    // 保存待打开的exe文件名
    pendingExeName = exeName;

    // 使用PowerShell解压zip文件
    QString psCommand = QString("Expand-Archive -Path '%1' -DestinationPath '%2' -Force")
                            .arg(zipPath, qiubaiPath);

    if (extractProcess) {
        extractProcess->deleteLater();
    }
    extractProcess = new QProcess(this);

    connect(extractProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &ConfigWindow::onExtractFinished);

    extractProcess->start("powershell.exe", QStringList() << "-Command" << psCommand);
}

void ConfigWindow::onExtractFinished(int exitCode, QProcess::ExitStatus)
{
    disconnect(extractProcess, nullptr, this, nullptr);

    QString qiubaiPath = ResourceExtractor::getResourcePath();

    if (exitCode != 0) {
        QString errorMsg = QString::fromLocal8Bit(extractProcess->readAllStandardError());
        qDebug() << "解压失败:" << errorMsg;
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误",
            QString("解压失败：%1").arg(errorMsg), this);
        return;
    }

    qDebug() << "解压完成";

    // 递归搜索qiubai目录下的exe文件
    QString foundPath = findFileRecursively(qiubaiPath, pendingExeName);

    if (!foundPath.isEmpty()) {
        qDebug() << "找到文件:" << foundPath;
        openExecutable(foundPath);
    } else {
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误",
            QString("解压后找不到文件：%1").arg(pendingExeName), this);
    }
}

QString ConfigWindow::findFileRecursively(const QString &dirPath, const QString &fileName)
{
    QDir dir(dirPath);

    // 先在当前目录查找
    if (dir.exists(fileName)) {
        return dir.absoluteFilePath(fileName);
    }

    // 递归搜索子目录
    QFileInfoList subdirs = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &subdir : subdirs) {
        QString found = findFileRecursively(subdir.absoluteFilePath(), fileName);
        if (!found.isEmpty()) {
            return found;
        }
    }

    return QString();  // 未找到
}

void ConfigWindow::openExecutable(const QString &exePath)
{
    if (!QFile::exists(exePath)) {
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误",
            QString("找不到文件：%1").arg(exePath), this);
        return;
    }

    // 使用文件所在目录作为工作目录
    QFileInfo fileInfo(exePath);
    QString workingDir = fileInfo.absolutePath();
    QString fileName = fileInfo.fileName();

    // 显示启动提示（非阻塞）
    qDebug() << "正在启动程序:" << exePath;

    // 使用Shell执行（允许UAC提示）
    qint64 pid = 0;
    bool success = QProcess::startDetached(exePath, QStringList(), workingDir, &pid);

    if (success && pid > 0) {
        ProcessManager::recordProcess(pid, fileName);
        qDebug() << "程序已启动，PID:" << pid << "名称:" << fileName;
    } else {
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误",
            QString("无法启动程序：%1").arg(fileName), this);
    }
}

void ConfigWindow::onDownloadTimeout()
{
    QTimer *timer = qobject_cast<QTimer*>(sender());
    if (!timer) return;

    // 找到对应的 reply
    QNetworkReply *reply = nullptr;
    for (auto it = downloadTimeoutTimers.begin(); it != downloadTimeoutTimers.end(); ++it) {
        if (it.value() == timer) {
            reply = it.key();
            break;
        }
    }

    if (!reply || !downloadingFiles.contains(reply)) {
        timer->stop();
        return;
    }

    int id = downloadingFiles[reply];
    qint64 lastBytes = lastReceivedBytes.value(reply, 0);
    qint64 currentBytes = downloadBuffers.value(reply, QByteArray()).size();

    // 检查是否有新数据
    if (currentBytes == lastBytes && currentBytes > 0) {
        // 没有新数据，可能卡住了
        int retry = retryCount.value(id, 0);

        if (retry < 3) {
            // 重试
            qDebug() << "下载超时，正在重试... 第" << (retry + 1) << "次";

            // 清理当前下载
            timer->stop();
            downloadTimeoutTimers.remove(reply);
            lastReceivedBytes.remove(reply);
            downloadBuffers.remove(reply);
            downloadingFiles.remove(reply);

            // 保存重试次数
            retryCount[id] = retry + 1;

            reply->abort();
            reply->deleteLater();
            timer->deleteLater();

            // 重新下载
            if (id >= 1 && id <= DownloadOptionCount) {
                buttons[id - 1]->setText("重试中...");
                buttons[id - 1]->setEnabled(false);
            }

            QTimer::singleShot(1000, this, [this, id]() {
                downloadFile(id);
            });
        } else {
            // 超过重试次数
            qDebug() << "下载失败，已重试3次";

            timer->stop();
            downloadTimeoutTimers.remove(reply);
            lastReceivedBytes.remove(reply);
            retryCount.remove(id);
            downloadBuffers.remove(reply);
            downloadingFiles.remove(reply);

            reply->abort();
            reply->deleteLater();
            timer->deleteLater();

            if (id >= 1 && id <= DownloadOptionCount && originalButtonTexts.contains(id)) {
                buttons[id - 1]->setText(originalButtonTexts[id]);
                buttons[id - 1]->setEnabled(true);
            }

            UIHelper::showCenteredMessageBox(QMessageBox::Warning, "下载失败",
                "下载超时，请检查网络连接后重试", this);
        }
    }

    // 更新记录
    lastReceivedBytes[reply] = currentBytes;
}

void ConfigWindow::onDownloadFinished(QNetworkReply *reply)
{
    if (reply->error() == QNetworkReply::NoError) {
        // 下载成功，保存文件
        QByteArray data = reply->readAll();

        QString qiubaiPath = ResourceExtractor::getResourcePath();
        QString configPath = qiubaiPath + "/download_config.txt";

        QFile file(configPath);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(data);
            file.close();
            qDebug() << "配置文件下载成功:" << configPath;

            // 重新加载配置文件
            loadConfigFile();
        } else {
            qDebug() << "无法保存配置文件:" << file.errorString();
        }
    } else {
        // 下载失败
        qDebug() << "配置文件下载失败:" << reply->errorString();
    }

    reply->deleteLater();
}
