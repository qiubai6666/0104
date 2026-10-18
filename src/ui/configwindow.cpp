#include "deviceoperationlease.h"
#include "configwindow.h"
#include "processmanager.h"
#include "resourceextractor.h"
#include "uihelper.h"
#include <QApplication>
#include <QScreen>
#include <QGuiApplication>
#include <QFile>
#include <QSaveFile>
#include <QNetworkRequest>
#include <QUrl>
#include <QDesktopServices>
#include <QFileInfo>
#include <QDir>
#include <QTimer>

namespace {
QString configFileUrl()
{
    return QStringLiteral("https://gitee.com/qiuabi19/orange-box/raw/master/download_config.txt");
}
}  // namespace

bool ConfigWindow::isSafeFileName(const QString &fileName)
{
    if (fileName.isEmpty() || fileName.contains("..") || fileName.endsWith('.')
        || fileName.endsWith(' ') || QDir::isAbsolutePath(fileName)) return false;
    const QString forbidden = QStringLiteral("<>:\"/\\|?*");
    for (const QChar ch : fileName) {
        if (ch.unicode() < 32 || forbidden.contains(ch)) return false;
    }
    // Windows device names remain reserved even with an extension.
    const QString base = fileName.section('.', 0, 0).toUpper();
    if (QStringList{"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"}.contains(base)) return false;
    if (base.size() == 4 && (base.startsWith("COM") || base.startsWith("LPT"))
        && QStringLiteral("123456789¹²³").contains(base.at(3))) return false;
    return QFileInfo(fileName).fileName() == fileName;
}
ConfigWindow::ConfigWindow(QWidget *parent)
    : QWidget(parent)
    , extractProcess(nullptr)
{
    if (!parentWidget()) {
        // 独立窗口：无边框、置顶；嵌入控制中心面板时保持普通子控件。
        setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        setAttribute(Qt::WA_TranslucentBackground);
    }

    // 设置窗口标题
    setWindowTitle("配置");

    // 创建网络管理器
    networkManager = new QNetworkAccessManager(this);
    connect(networkManager, &QNetworkAccessManager::finished, this, &ConfigWindow::onDownloadFinished);

    // 创建文件下载管理器
    fileDownloadManager = new QNetworkAccessManager(this);
    // Finish QNAM's reply bookkeeping before opening a nested message-box loop.
    connect(fileDownloadManager, &QNetworkAccessManager::finished, this,
            &ConfigWindow::onFileDownloadFinished, Qt::QueuedConnection);

    // 先检查配置文件是否存在
    QString qiubaiPath = ResourceExtractor::getResourcePath();
    QString configPath = qiubaiPath + "/download_config.txt";

    // 配置文件缺失时触发异步下载（通常启动预热已提前下载好），
    // 窗口立即显示占位文字，配置到达后自动刷新按钮，不再阻塞 UI。
    if (!QFile::exists(configPath)) {
        qDebug() << "配置文件不存在，开始下载...";
        downloadConfig();
    }

    setupUI();
}

ConfigWindow::~ConfigWindow()
{
    // Abort while writers still exist; suppress UI callbacks during destruction.
    for (QNetworkAccessManager *manager : {networkManager, fileDownloadManager}) {
        disconnect(manager, nullptr, this, nullptr);
        const auto replies = manager->findChildren<QNetworkReply *>();
        for (QNetworkReply *reply : replies) {
            disconnect(reply, nullptr, this, nullptr);
            reply->abort();
        }
    }
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

    buttons = UIHelper::createDrawerButtons(this, mainLayout,
        QStringLiteral("配置与工具"), QStringLiteral(""), buttonTexts,
        6, QStringLiteral("下载资源"), QStringLiteral("外部工具"),
        parentWidget() == nullptr,
        QStringList{"hard-drive", "smartphone", "cpu", "shield", "wrench", "zap",
                    "wrench", "cloud", "external"},
        buttonTexts.size() - 1);
    for (QPushButton *button : buttons) {
        connect(button, &QPushButton::clicked, this, &ConfigWindow::onButtonClicked);
    }

}

void ConfigWindow::setPosition(int mainMenuX, int mainMenuY, int mainMenuHeight)
{
    UIHelper::positionDrawer(this, QRect(mainMenuX, mainMenuY, UIHelper::MainMenuWidth, mainMenuHeight));
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
    if (DeviceOperationLease::busyFor(this)) { UIHelper::showCenteredMessageBox(QMessageBox::Warning, "设备通道占用", "请等待设备操作完成后再启动外部工具。", this); return; }
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
    if (DeviceOperationLease::busyFor(this)) { UIHelper::showCenteredMessageBox(QMessageBox::Warning, "设备通道占用", "请等待设备操作完成后再启动外部工具。", this); return; }
    QString qiubaiPath = ResourceExtractor::getResourcePath();
    QString ndmPath = qiubaiPath + "/NDM.exe";

    // 检查文件是否存在
    if (!QFile::exists(ndmPath)) {
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误", "找不到 NDM.exe 文件！", this);
        return;
    }

    QProcess *process = ProcessManager::startProcess(ndmPath, QStringList(), qiubaiPath, false, true);

    if (process) {
        qDebug() << "NDM已启动，PID:" << process->processId();
    } else {
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误", "无法启动 NDM.exe！", this);
    }
}

void ConfigWindow::downloadConfig()
{
    const QString configPath = ResourceExtractor::getResourcePath() + "/download_config.txt";
    if (QFile::exists(configPath) || !configDownloadFiles.isEmpty()) return;
    auto *writer = new DownloadFileWriter(configPath, this);
    if (!writer->isOpen()) {
        qWarning() << "无法创建配置临时文件:" << writer->errorString();
        delete writer;
        return;
    }
    QNetworkRequest request(configFileUrl());
    request.setHeader(QNetworkRequest::UserAgentHeader, "Mozilla/5.0");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    QNetworkReply *reply = networkManager->get(request);
    configDownloadFiles[reply] = writer;
    writer->attach(reply);
}

void ConfigWindow::loadConfigFile()
{
    QString qiubaiPath = ResourceExtractor::getResourcePath();
    QString configPath = qiubaiPath + "/download_config.txt";

    configData.clear();
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
        if (parts.size() == 4) {
            const int id = parts[0].toInt();
            const QString fileType = parts[1].trimmed().toLower();
            const QString fileName = parts[2].trimmed();
            const QString url = parts[3].trimmed();
            const QUrl parsedUrl(url);
            if (id < 1 || id > DownloadOptionCount ||
                (fileType != "exe" && fileType != "zip") ||
                !isSafeFileName(fileName) || !parsedUrl.isValid() || parsedUrl.host().isEmpty() ||
                (fileType == "exe" && !fileName.endsWith(".exe", Qt::CaseInsensitive)) ||
                parsedUrl.scheme().compare("https", Qt::CaseInsensitive) != 0) {
                qWarning() << "忽略无效下载配置:" << line;
                continue;
            }
            configData[id] = DownloadConfig{fileType, fileName, url};
        } else {
            qWarning() << "忽略字段数量无效的下载配置:" << line;
        }
    }

    file.close();
    qDebug() << "配置文件加载完成，共" << configData.size() << "项";
}

void ConfigWindow::refreshConfigButtonLabels()
{
    for (int id = 1; id <= DownloadOptionCount; ++id) {
        if (!configData.contains(id) || id > buttons.size()) {
            continue;
        }
        const QString fullName = configData.value(id).fileName;
        const int dotIndex = fullName.lastIndexOf('.');
        const QString displayName = (dotIndex > 0) ? fullName.left(dotIndex) : fullName;
        originalButtonTexts[id] = displayName;
        // 下载进行中的按钮显示进度，不打扰。
        if (buttons[id - 1]->isEnabled()) {
            buttons[id - 1]->setText(displayName);
        }
    }
}

void ConfigWindow::preloadConfigFile()
{
    const QString configPath = ResourceExtractor::getResourcePath() + "/download_config.txt";
    if (QFile::exists(configPath)) return;
    auto *manager = new QNetworkAccessManager(qApp);
    auto *writer = new DownloadFileWriter(configPath, manager);
    if (!writer->isOpen()) {
        qWarning() << "配置预热无法创建临时文件:" << writer->errorString();
        manager->deleteLater();
        return;
    }
    QNetworkRequest request(configFileUrl());
    request.setHeader(QNetworkRequest::UserAgentHeader, "Mozilla/5.0");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    QNetworkReply *reply = manager->get(request);
    writer->attach(reply);
    connect(reply, &QNetworkReply::finished, reply, [manager, reply, writer] {
        const QString error = writer->finish();
        if (!error.isEmpty()) qWarning() << "配置预热下载失败:" << error;
        reply->deleteLater();
        manager->deleteLater();
    });
}

void ConfigWindow::downloadFile(int id, bool retry)
{
    if (!retry && activeDownloadPaths.contains(id)) return;
    if (!configData.contains(id)) {
        restoreDownloadButton(id);
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误", "配置信息不存在！", this);
        return;
    }
    const DownloadConfig entry = configData.value(id);
    const QUrl url(entry.url);
    if (!isSafeFileName(entry.fileName) || (entry.fileType != "exe" && entry.fileType != "zip")
        || (entry.fileType == "exe" && !entry.fileName.endsWith(".exe", Qt::CaseInsensitive))
        || !url.isValid() || url.host().isEmpty()
        || url.scheme().compare("https", Qt::CaseInsensitive) != 0) {
        restoreDownloadButton(id);
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误", "下载配置中的文件名、类型或 HTTPS 地址无效。", this);
        return;
    }
    QString actualFileName = entry.fileName;
    if (entry.fileType == "zip") {
        if (actualFileName.endsWith(".exe", Qt::CaseInsensitive)) actualFileName.chop(4);
        if (!actualFileName.endsWith(".zip", Qt::CaseInsensitive)) actualFileName += ".zip";
    }
    const QString filePath = ResourceExtractor::getResourcePath() + "/" + actualFileName;
    for (auto it = activeDownloadPaths.cbegin(); it != activeDownloadPaths.cend(); ++it) {
        if (it.key() != id && it.value().compare(filePath, Qt::CaseInsensitive) == 0) {
            restoreDownloadButton(id);
            UIHelper::showCenteredMessageBox(QMessageBox::Warning, "下载进行中", "同一目标文件正在下载，请稍后重试。", this);
            return;
        }
    }
    if (QFile::exists(filePath)) {
        restoreDownloadButton(id);
        if (entry.fileType == "exe") {
            openExecutable(filePath);
        } else {
            const QString exePath = ResourceExtractor::getResourcePath() + "/" + entry.fileName;
            if (QFile::exists(exePath)) openExecutable(exePath);
            else extractZipAndOpen(filePath, entry.fileName);
        }
        return;
    }
    auto *writer = new DownloadFileWriter(filePath, this);
    if (!writer->isOpen()) {
        const QString error = writer->errorString();
        delete writer;
        restoreDownloadButton(id);
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误", "无法创建下载临时文件：" + error, this);
        return;
    }
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, "Mozilla/5.0");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferNetwork);
    QNetworkReply *reply = fileDownloadManager->get(request);
    downloadingFiles[reply] = id;
    downloadFiles[reply] = writer;
    downloadPaths[reply] = filePath;
    activeDownloadPaths[id] = filePath;
    writer->attach(reply);
    connect(reply, &QNetworkReply::downloadProgress, this, &ConfigWindow::onDownloadProgress);
    if (id >= 1 && id <= buttons.size()) {
        buttons[id - 1]->setText("0%");
        buttons[id - 1]->setEnabled(false);
    }
}

void ConfigWindow::restoreDownloadButton(int id)
{
    activeDownloadPaths.remove(id);
    retryCount.remove(id);
    if (id >= 1 && id <= buttons.size()) {
        buttons[id - 1]->setText(originalButtonTexts.value(id));
        buttons[id - 1]->setEnabled(true);
    }
}

void ConfigWindow::onDownloadProgress(qint64 bytesReceived, qint64 bytesTotal)
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply || !downloadingFiles.contains(reply)) {
        return;
    }

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
    if (!downloadingFiles.contains(reply)) {
        reply->deleteLater();
        return;
    }
    const int id = downloadingFiles.take(reply);
    const QString path = downloadPaths.take(reply);
    DownloadFileWriter *writer = downloadFiles.take(reply);
    const QString error = writer ? writer->finish() : QStringLiteral("下载临时文件不存在");
    const bool retry = writer && writer->timedOut() && retryCount.value(id, 0) < 3;
    const qint64 written = writer ? writer->bytesWritten() : 0;
    // finish() already closes the temporary file. Defer object destruction in
    // case a timeout is still unwinding through the writer's abort callback.
    if (writer) writer->deleteLater();
    reply->deleteLater();
    if (retry) {
        ++retryCount[id];
        buttons[id - 1]->setText("重试中...");
        QTimer::singleShot(1000, this, [this, id] { downloadFile(id, true); });
        return;
    }
    restoreDownloadButton(id);
    if (!error.isEmpty()) {
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "下载失败", "文件下载失败：" + error, this);
        return;
    }
    qDebug() << "文件保存完成:" << path << "写入字节:" << written;
    const DownloadConfig entry = configData.value(id);
    if (entry.fileType == "exe") openExecutable(path);
    else extractZipAndOpen(path, entry.fileName);
}

void ConfigWindow::extractZipAndOpen(const QString &zipPath, const QString &exeName)
{
    if (DeviceOperationLease::busyFor(this)) { UIHelper::showCenteredMessageBox(QMessageBox::Warning, "设备通道占用", "请等待设备操作完成后再启动外部工具。", this); return; }
    QString qiubaiPath = ResourceExtractor::getResourcePath();

    if (!isSafeFileName(exeName)) {
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误", "解压配置中的目标文件名无效。", this);
        return;
    }

    // 保存待打开的exe文件名
    pendingExeName = exeName;

    // 通过脚本参数传递路径，不把用户/配置路径拼接进 PowerShell 代码，避免引号和特殊字符导致注入或解析错误。
    const QString psCommand =
        "& { param([string]$zipPath, [string]$destinationPath) "
        "Expand-Archive -LiteralPath $zipPath -DestinationPath $destinationPath -Force }";

    if (extractProcess) {
        extractProcess->deleteLater();
    }
    extractProcess = ProcessManager::createProcess(this);

    connect(extractProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &ConfigWindow::onExtractFinished);
    connect(extractProcess, &QProcess::errorOccurred,
            this, &ConfigWindow::onExtractError);

    extractProcess->start("powershell.exe", QStringList()
        << "-NoProfile" << "-NonInteractive" << "-Command" << psCommand
        << zipPath << qiubaiPath);
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

void ConfigWindow::onExtractError(QProcess::ProcessError error)
{
    if (error != QProcess::FailedToStart) {
        return;
    }

    const QString errorMsg = extractProcess->errorString().trimmed();
    disconnect(extractProcess, nullptr, this, nullptr);
    UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误",
        QString("无法启动解压程序：%1").arg(errorMsg), this);
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
    if (DeviceOperationLease::busyFor(this)) { UIHelper::showCenteredMessageBox(QMessageBox::Warning, "设备通道占用", "请等待设备操作完成后再启动外部工具。", this); return; }
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

    QProcess *process = ProcessManager::startProcess(exePath, QStringList(), workingDir);

    if (process) {
        qDebug() << "程序已启动，PID:" << process->processId() << "名称:" << fileName;
    } else {
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误",
            QString("无法启动程序：%1").arg(fileName), this);
    }
}

void ConfigWindow::onDownloadFinished(QNetworkReply *reply)
{
    DownloadFileWriter *writer = configDownloadFiles.take(reply);
    const QString error = writer ? writer->finish() : QStringLiteral("下载临时文件不存在");
    if (error.isEmpty()) {
        loadConfigFile();
        refreshConfigButtonLabels();
    } else {
        qWarning() << "配置文件下载失败:" << error;
    }
    reply->deleteLater();
}
