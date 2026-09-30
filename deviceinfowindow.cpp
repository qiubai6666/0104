#include "deviceinfowindow.h"
#include "resourceextractor.h"
#include "uihelper.h"
#include <QScreen>
#include <QGuiApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QRegularExpression>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QFileInfo>
#include <QMessageBox>
#include <QFile>
#include <QWindow>
#include <QMoveEvent>

DeviceInfoWindow::DeviceInfoWindow(QWidget *parent)
    : QWidget(parent)
    , isDragging(false)
    , opacityTimer(nullptr)
    , queryProcess(nullptr)
    , transferProcess(nullptr)
    , transferIndex(0)
    , transferSuccessCount(0)
    , transferFailCount(0)
{
    // 设置窗口标志：无边框、置顶
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground);
    
    // 设置窗口标题
    setWindowTitle("投屏");
    
    // 启用拖放功能
    setAcceptDrops(true);
    
    // 初始化透明度恢复定时器
    opacityTimer = new QTimer(this);
    opacityTimer->setSingleShot(true);
    connect(opacityTimer, &QTimer::timeout, this, &DeviceInfoWindow::restoreOpacity);
    
    setupUI();
    
    // 创建 scrcpy 进程对象
    scrcpyProcess = new QProcess(this);
    
    // 监听scrcpy进程结束信号
    connect(scrcpyProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &DeviceInfoWindow::onScrcpyFinished);
    
    // 连接设备管理器的信号
    connect(DeviceManager::instance(), &DeviceManager::deviceModeChanged,
            this, &DeviceInfoWindow::onDeviceModeChanged);
    connect(DeviceManager::instance(), &DeviceManager::deviceInfoUpdated,
            this, &DeviceInfoWindow::onDeviceInfoUpdated);
    
    // 确保设备监控已启动（仅 ADB 检测）
    DeviceManager::instance()->ensureAdbOnlyMonitoring();
    
    // 使用单次定时器延迟初始化，等待第一次设备检测完成
    QTimer::singleShot(200, this, [this]() {
        // 初始化显示 - 仅在 ADB 模式下显示设备信息并启动投屏
        DeviceManager::DeviceMode currentMode = DeviceManager::instance()->currentMode();
        if (currentMode == DeviceManager::ADB) {
            showDeviceInfo();
            // 立即显示设备信息
            onDeviceInfoUpdated(DeviceManager::instance()->getDeviceInfo());
            // 启动 scrcpy
            startScrcpy();
        } else {
            // 非 ADB 模式统一显示未检测到设备
            showNoDeviceMessage();
        }
    });
}

DeviceInfoWindow::~DeviceInfoWindow()
{
    // 终止scrcpy进程
    if (scrcpyProcess && scrcpyProcess->state() == QProcess::Running) {
        scrcpyProcess->kill();
        scrcpyProcess->waitForFinished(100);
    }
    if (queryProcess) {
        queryProcess->kill();
        queryProcess->deleteLater();
    }
    if (transferProcess) {
        transferProcess->kill();
        transferProcess->deleteLater();
    }
    
    // 释放 ADB-only 监控引用
    DeviceManager::instance()->releaseAdbOnlyMonitoring();
}

void DeviceInfoWindow::setupUI()
{
    setFixedSize(360, 360);
    setCursor(Qt::SizeAllCursor);
    
    // 创建主背景容器（避免直接给窗口设置背景导致DPI问题）
    QWidget *bgContainer = new QWidget(this);
    bgContainer->setGeometry(0, 0, 360, 360);
    bgContainer->setStyleSheet(
        "QWidget {"
        "   background-color: rgba(169, 204, 227, 180);"
        "   border-radius: 15px;"
        "}"
    );
    
    mainLayout = new QVBoxLayout(bgContainer);
    mainLayout->setContentsMargins(10, 10, 10, 10);
    mainLayout->setSpacing(5);
    
    // 创建一个容器Widget作为卡片的背景层
    cardContainer = new QWidget(bgContainer);
    cardContainer->setStyleSheet(
        "QWidget {"
        "   background-color: rgba(255, 255, 255, 100);"  // 半透明白色背景
        "   border-radius: 15px;"
        "}"
    );
    
    // 在容器内创建垂直布局
    QVBoxLayout *cardLayout = new QVBoxLayout(cardContainer);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(5);
    
    // 创建信息标签
    modelLabel = new QLabel(cardContainer);
    codenameLabel = new QLabel(cardContainer);
    versionLabel = new QLabel(cardContainer);
    slotLabel = new QLabel(cardContainer);
    unlockLabel = new QLabel(cardContainer);
    
    cardLayout->addWidget(modelLabel);
    cardLayout->addWidget(codenameLabel);
    cardLayout->addWidget(versionLabel);
    cardLayout->addWidget(slotLabel);
    cardLayout->addWidget(unlockLabel);
    
    // 将容器添加到主布局
    mainLayout->addWidget(cardContainer);
    
    // 显示在屏幕左上角（留出一点边距）
    QScreen *screen = QGuiApplication::primaryScreen();
    QRect screenGeometry = screen->geometry();
    int margin = 10;  // 距离屏幕边缘10像素
    move(screenGeometry.left() + margin, screenGeometry.top() + margin);
}

void DeviceInfoWindow::startScrcpy()
{
    // 使用绝对路径确保调用qiubai文件夹中的scrcpy
    QString scrcpyPath = ResourceExtractor::getResourcePath() + "/scrcpy.exe";
    scrcpyProcess->setWorkingDirectory(ResourceExtractor::getResourcePath());
    scrcpyProcess->start(scrcpyPath, QStringList() << "--max-size" << "1024" << "--video-bit-rate" << "4M");
}

void DeviceInfoWindow::onDeviceModeChanged(DeviceManager::DeviceMode mode)
{
    if (mode == DeviceManager::ADB) {
        // 设备连接（ADB 模式）- 显示设备信息并启动投屏
        showDeviceInfo();
        // 立即显示设备信息
        onDeviceInfoUpdated(DeviceManager::instance()->getDeviceInfo());
        // 启动 scrcpy
        startScrcpy();
    } else {
        // 非 ADB 模式一律视为未检测到可投屏设备
        showNoDeviceMessage();
        // 停止 scrcpy
        if (scrcpyProcess && scrcpyProcess->state() == QProcess::Running) {
            scrcpyProcess->kill();
        }
    }
}

void DeviceInfoWindow::onDeviceInfoUpdated(const QString &info)
{
    // 只在 ADB 模式下更新设备信息
    if (DeviceManager::instance()->currentMode() != DeviceManager::ADB) {
        return;
    }
    
    // 解析设备信息字符串 (格式: 代号:xxx\n分区:xxx\n解锁:xxx)
    QStringList lines = info.split('\n');
    pendingCodename = "未知";
    pendingSlot = "未知";
    pendingUnlock = "未知";
    
    for (const QString &line : lines) {
        if (line.startsWith("代号:")) {
            pendingCodename = line.mid(3).trimmed();
        } else if (line.startsWith("分区:")) {
            pendingSlot = line.mid(3).trimmed();
        } else if (line.startsWith("解锁:")) {
            pendingUnlock = line.mid(3).trimmed();
        }
    }
    
    // 异步获取型号
    if (queryProcess) {
        // 如果有进程在运行，先终止
        if (queryProcess->state() == QProcess::Running) {
            queryProcess->kill();
            queryProcess->waitForFinished(100);
        }
        queryProcess->deleteLater();
    }
    queryProcess = new QProcess(this);
    queryProcess->setWorkingDirectory(ResourceExtractor::getResourcePath());
    
    QString adbPath = ResourceExtractor::getAdbPath();
    
    connect(queryProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &DeviceInfoWindow::onModelQueryFinished);
    
    queryProcess->start(adbPath, QStringList() << "shell" << "getprop" << "ro.product.model");
}

void DeviceInfoWindow::onModelQueryFinished()
{
    disconnect(queryProcess, nullptr, this, nullptr);
    
    // 检查设备是否仍然连接
    if (DeviceManager::instance()->currentMode() != DeviceManager::ADB) {
        queryProcess->deleteLater();
        queryProcess = nullptr;
        return;
    }
    
    pendingModel = QString::fromLocal8Bit(queryProcess->readAllStandardOutput()).trimmed();
    if (pendingModel.isEmpty()) pendingModel = "未知";
    
    // 删除旧进程，创建新进程获取版本
    queryProcess->deleteLater();
    queryProcess = new QProcess(this);
    queryProcess->setWorkingDirectory(ResourceExtractor::getResourcePath());
    
    QString adbPath = ResourceExtractor::getAdbPath();
    
    connect(queryProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &DeviceInfoWindow::onVersionQueryFinished);
    
    queryProcess->start(adbPath, QStringList() << "shell" << "getprop" << "ro.build.version.release");
}

void DeviceInfoWindow::onVersionQueryFinished()
{
    disconnect(queryProcess, nullptr, this, nullptr);
    
    // 检查设备是否仍然连接
    if (DeviceManager::instance()->currentMode() != DeviceManager::ADB) {
        queryProcess->deleteLater();
        queryProcess = nullptr;
        return;
    }
    
    pendingVersion = QString::fromLocal8Bit(queryProcess->readAllStandardOutput()).trimmed();
    if (pendingVersion.isEmpty()) pendingVersion = "未知";
    
    // 更新显示
    updateDeviceLabels(pendingModel, pendingCodename, pendingVersion, pendingSlot, pendingUnlock);
}

void DeviceInfoWindow::updateDeviceLabels(const QString &model, const QString &codename, 
                                       const QString &version, const QString &slot, const QString &unlock)
{
    modelLabel->setText("📱 手机型号: " + model);
    codenameLabel->setText("🔖 手机代号: " + codename);
    versionLabel->setText("🤖 系统版本: Android " + version);
    slotLabel->setText("💾 活动分区: " + slot);
    unlockLabel->setText("🔓 解锁状态: " + unlock);
}

void DeviceInfoWindow::onScrcpyFinished(int, QProcess::ExitStatus)
{
    // 如果设备仍然连接，1秒后重新启动scrcpy
    DeviceManager::DeviceMode currentMode = DeviceManager::instance()->currentMode();
    if (currentMode == DeviceManager::ADB) {
        QTimer::singleShot(1000, this, [this]() {
            if (DeviceManager::instance()->currentMode() == DeviceManager::ADB) {
                startScrcpy();
            }
        });
    }
}

void DeviceInfoWindow::showNoDeviceMessage()
{
    // 第一个卡片：警告信息
    modelLabel->setText("⚠ 未检测到设备\n请使用数据线连接手机或平板");
    modelLabel->setAlignment(Qt::AlignCenter);
    modelLabel->setWordWrap(true);
    modelLabel->setStyleSheet(
        "QLabel {"
        "   background-color: white;"
        "   color: #5B7C99;"
        "   font-size: 14px;"
        "   font-weight: bold;"
        "   padding: 16px;"
        "   border-radius: 12px;"
        "}"
    );
    
    // 第二个卡片：开发者模式和USB调试
    codenameLabel->setText(
        "📱 开启开发者模式\n"
        "设置 → 关于手机 → 版本信息 → 版本号\n"
        "连续点击直到提示进入开发者模式\n\n"
        "⚙ 开启USB调试\n"
        "设置 → 搜索开发者选项 → 进入开发者选项\n"
        "打开USB调试开关"
    );
    codenameLabel->setWordWrap(true);
    codenameLabel->setStyleSheet(
        "QLabel {"
        "   background-color: white;"
        "   color: #4A90E2;"
        "   font-size: 12px;"
        "   padding: 15px;"
        "   border-radius: 12px;"
        "   line-height: 1.5;"
        "}"
    );
    
    // 第三个卡片：小米特别提示
    versionLabel->setText(
        "⚡ 小米/红米设备特别提示\n"
        "还需开启：USB安装 和 USB调试(安全设置)"
    );
    versionLabel->setWordWrap(true);
    versionLabel->setStyleSheet(
        "QLabel {"
        "   background-color: rgba(255, 220, 160, 255);"
        "   color: #D97706;"
        "   font-size: 12px;"
        "   font-weight: bold;"
        "   padding: 14px;"
        "   border-radius: 12px;"
        "}"
    );
    
    // 第四个卡片：USB授权提示
    slotLabel->setText("💡 请注意手机上的USB调试授权弹窗");
    slotLabel->setAlignment(Qt::AlignCenter);
    slotLabel->setStyleSheet(
        "QLabel {"
        "   background-color: rgba(200, 220, 240, 255);"
        "   color: #5B7C99;"
        "   font-size: 12px;"
        "   font-weight: bold;"
        "   padding: 12px;"
        "   border-radius: 12px;"
        "}"
    );
    
    // 隐藏最后一个标签
    unlockLabel->hide();
}

void DeviceInfoWindow::showDeviceInfo()
{
    // 显示所有标签
    modelLabel->show();
    codenameLabel->show();
    versionLabel->show();
    slotLabel->show();
    unlockLabel->show();
    
    // 重置标签样式为设备信息样式
    QString labelStyle = 
        "QLabel {"
        "   background-color: white;"
        "   color: #4A90E2;"
        "   font-size: 13px;"
        "   padding: 12px 15px;"
        "   border-radius: 10px;"
        "}";
    
    modelLabel->setText("手机型号: 获取中...");
    modelLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    modelLabel->setWordWrap(false);
    modelLabel->setStyleSheet(
        "QLabel {"
        "   background-color: white;"
        "   color: #4A90E2;"
        "   font-size: 14px;"
        "   font-weight: bold;"
        "   padding: 12px 15px;"
        "   border-radius: 10px;"
        "}"
    );
    
    codenameLabel->setText("手机代号: 获取中...");
    codenameLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    codenameLabel->setWordWrap(false);
    codenameLabel->setStyleSheet(labelStyle);
    
    versionLabel->setText("系统版本: 获取中...");
    versionLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    versionLabel->setWordWrap(false);
    versionLabel->setStyleSheet(labelStyle);
    
    slotLabel->setText("活动分区: 获取中...");
    slotLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    slotLabel->setWordWrap(false);
    slotLabel->setStyleSheet(labelStyle);
    
    unlockLabel->setText("解锁状态: 获取中...");
    unlockLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    unlockLabel->setWordWrap(false);
    unlockLabel->setStyleSheet(labelStyle);
}

void DeviceInfoWindow::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        // 拖动时设置不透明，提高流畅度
        setWindowOpacity(1.0);
        isDragging = true;
        
        // 使用系统原生拖动
        if (windowHandle()) {
            windowHandle()->startSystemMove();
        }
        event->accept();
    }
}

void DeviceInfoWindow::mouseMoveEvent(QMouseEvent *event)
{
    Q_UNUSED(event);
}

void DeviceInfoWindow::mouseReleaseEvent(QMouseEvent *event)
{
    Q_UNUSED(event);
}

void DeviceInfoWindow::moveEvent(QMoveEvent *event)
{
    QWidget::moveEvent(event);
    
    // 窗口移动时重置定时器，移动停止200ms后恢复透明度
    if (isDragging) {
        opacityTimer->start(200);
    }
}

void DeviceInfoWindow::restoreOpacity()
{
    isDragging = false;
    setWindowOpacity(0.95);  // 恢复到轻微透明
}

void DeviceInfoWindow::dragEnterEvent(QDragEnterEvent *event)
{
    // 只有在设备连接时才接受拖放
    if (DeviceManager::instance()->isDeviceConnected() && event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void DeviceInfoWindow::dropEvent(QDropEvent *event)
{
    if (!DeviceManager::instance()->isDeviceConnected()) {
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "提示", "请先连接设备！", this);
        return;
    }
    
    const QMimeData *mimeData = event->mimeData();
    if (!mimeData->hasUrls()) {
        return;
    }
    
    QStringList filePaths;
    QList<QUrl> urls = mimeData->urls();
    
    // 收集所有文件路径
    for (const QUrl &url : urls) {
        if (url.isLocalFile()) {
            QString filePath = url.toLocalFile();
            QFileInfo fileInfo(filePath);
            
            if (fileInfo.exists() && fileInfo.isFile()) {
                filePaths.append(filePath);
            }
        }
    }
    
    if (filePaths.isEmpty()) {
        QMessageBox::warning(this, "提示", "没有有效的文件！");
        return;
    }
    
    // 传输文件
    transferFiles(filePaths);
    event->acceptProposedAction();
}

void DeviceInfoWindow::transferFiles(const QStringList &filePaths)
{
    // 保存待传输文件列表
    pendingTransferFiles = filePaths;
    transferIndex = 0;
    transferSuccessCount = 0;
    transferFailCount = 0;
    transferFailedFiles.clear();
    
    // 创建传输进程
    if (transferProcess) {
        if (transferProcess->state() == QProcess::Running) {
            transferProcess->kill();
            transferProcess->waitForFinished(100);
        }
        transferProcess->deleteLater();
    }
    transferProcess = new QProcess(this);
    transferProcess->setWorkingDirectory(ResourceExtractor::getResourcePath());
    
    connect(transferProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &DeviceInfoWindow::onTransferFinished);
    
    // 开始传输第一个文件
    transferNextFile();
}

void DeviceInfoWindow::transferNextFile()
{
    // 检查设备是否仍然连接
    if (DeviceManager::instance()->currentMode() != DeviceManager::ADB) {
        disconnect(transferProcess, nullptr, this, nullptr);
        if (transferIndex > 0) {
            UIHelper::showCenteredMessageBox(QMessageBox::Warning, "传输中断", 
                QString("设备已断开，已成功传输 %1 个文件").arg(transferSuccessCount), this);
        }
        return;
    }
    
    if (transferIndex >= pendingTransferFiles.size()) {
        // 所有文件传输完成
        disconnect(transferProcess, nullptr, this, nullptr);
        
        // 只在有失败时才显示提示
        if (transferFailCount > 0) {
            QString message = QString("传输失败 %1 个文件:\n%2").arg(transferFailCount).arg(transferFailedFiles.join("\n"));
            QMessageBox::warning(this, "传输失败", message);
        }
        
        qDebug() << "传输完成 - 成功:" << transferSuccessCount << "失败:" << transferFailCount;
        return;
    }
    
    QString filePath = pendingTransferFiles[transferIndex];
    QFileInfo fileInfo(filePath);
    QString fileName = fileInfo.fileName();
    
    qDebug() << "正在传输文件:" << fileName << "路径:" << filePath;
    
    QString adbPath = ResourceExtractor::getAdbPath();
    QByteArray localFilePath = QFile::encodeName(filePath);
    QString targetPath = QString("/sdcard/%1").arg(fileName);
    
    QStringList args;
    args << "push";
    args << QString::fromLocal8Bit(localFilePath);
    args << targetPath;
    
    qDebug() << "执行命令: adb" << args.join(" ");
    
    transferProcess->start(adbPath, args);
}

void DeviceInfoWindow::onTransferFinished(int exitCode, QProcess::ExitStatus)
{
    QString output = QString::fromLocal8Bit(transferProcess->readAllStandardOutput());
    QString error = QString::fromLocal8Bit(transferProcess->readAllStandardError());
    
    QString filePath = pendingTransferFiles[transferIndex];
    QFileInfo fileInfo(filePath);
    QString fileName = fileInfo.fileName();
    
    qDebug() << "传输输出:" << output;
    qDebug() << "退出代码:" << exitCode;
    if (!error.isEmpty()) {
        qDebug() << "传输错误:" << error;
    }
    
    bool hasError = error.contains("error", Qt::CaseInsensitive) || 
                   error.contains("failed", Qt::CaseInsensitive) ||
                   output.contains("error", Qt::CaseInsensitive) ||
                   output.contains("failed", Qt::CaseInsensitive);
    
    if (exitCode == 0 && !hasError) {
        qDebug() << "✓ 文件传输成功:" << fileName;
        transferSuccessCount++;
    } else {
        qDebug() << "✗ 文件传输失败:" << fileName;
        transferFailedFiles.append(fileName);
        transferFailCount++;
    }
    
    // 传输下一个文件
    transferIndex++;
    transferNextFile();
}
