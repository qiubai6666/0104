#include "devicecheckwindow.h"
#include "processmanager.h"
#include "resourceextractor.h"
#include "uihelper.h"
#include "devicemanager.h"
#include <QScreen>
#include <QGuiApplication>
#include <QFile>
#include <QDir>
#include <QDesktopServices>
#include <QUrl>
#include <QStandardPaths>
#include <QThread>
#include <QFileDialog>
#include <QFileInfo>
#include <QWindow>
#include <QMoveEvent>

DeviceCheckWindow::DeviceCheckWindow(QWidget *parent)
    : QWidget(parent)
    , currentProcess(nullptr)
    , waitTimer(nullptr)
    , waitCounter(0)
    , isDragging(false)
    , opacityTimer(nullptr)
{
    // 设置窗口标志：无边框、置顶
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground);
    
    // 设置窗口标题
    setWindowTitle("设备检测");
    
    // 初始化透明度恢复定时器
    opacityTimer = new QTimer(this);
    opacityTimer->setSingleShot(true);
    connect(opacityTimer, &QTimer::timeout, this, &DeviceCheckWindow::restoreOpacity);
    
    setupUI();
    
    // 位置将由 setPosition() 设置
    
    // 连接设备管理器的信号
    connect(DeviceManager::instance(), &DeviceManager::deviceModeChanged,
            this, &DeviceCheckWindow::onDeviceModeChanged);
    connect(DeviceManager::instance(), &DeviceManager::deviceInfoUpdated,
            this, &DeviceCheckWindow::onDeviceInfoUpdated);
    
    // 确保设备监控已启动
    DeviceManager::instance()->ensureMonitoring();
    
    // 更新初始状态
    updateUIForMode(DeviceManager::instance()->currentMode());
    
    // 如果有设备信息，立即显示
    QString deviceInfo = DeviceManager::instance()->getDeviceInfo();
    if (!deviceInfo.isEmpty()) {
        infoLabel->setText(deviceInfo);
    }
}

DeviceCheckWindow::~DeviceCheckWindow()
{
    // 清理资源
    if (currentProcess) {
        currentProcess->kill();
        currentProcess->deleteLater();
    }
    if (waitTimer) {
        waitTimer->stop();
        waitTimer->deleteLater();
    }
    
    // 释放全模式监控引用
    DeviceManager::instance()->releaseFullModeMonitoring();
}

void DeviceCheckWindow::setupUI()
{
    setFixedSize(320, 384);  // 收紧控件留白，保留完整的设备信息区域
    
    // 创建主容器
    QWidget *container = new QWidget(this);
    container->setObjectName("deviceCheckSurface");
    container->setGeometry(rect());
    container->setStyleSheet(
        "QWidget#deviceCheckSurface {"
        "   background-color: rgba(235, 244, 249, 248);"
        "   border: 1px solid rgba(255, 255, 255, 180);"
        "   border-radius: 10px;"
        "}"
    );
    
    mainLayout = new QVBoxLayout(container);
    mainLayout->setSpacing(6);
    mainLayout->setContentsMargins(15, 12, 15, 12);
    
    // 标题
    QLabel *titleLabel = new QLabel("📱 设备检测", container);
    titleLabel->setAlignment(Qt::AlignCenter);
    titleLabel->setFixedHeight(30);  // 固定标题高度
    titleLabel->setStyleSheet(
        "QLabel {"
        "   color: #2c3e50;"
        "   font-size: 15px;"  // 稍微减小字体
        "   font-weight: bold;"
        "   background: transparent;"
        "}"
    );
    mainLayout->addWidget(titleLabel);
    
    // 状态卡片
    QWidget *statusCard = new QWidget(container);
    statusCard->setObjectName("deviceStatusCard");
    statusCard->setStyleSheet(
        "QWidget#deviceStatusCard {"
        "   background-color: rgba(255, 255, 255, 210);"
        "   border-radius: 8px;"
        "}"
    );
    QVBoxLayout *statusLayout = new QVBoxLayout(statusCard);
    statusLayout->setSpacing(4);
    statusLayout->setContentsMargins(10, 8, 10, 8);
    
    statusLabel = new QLabel("检测中...", statusCard);
    statusLabel->setAlignment(Qt::AlignCenter);
    statusLabel->setFixedHeight(25);  // 固定状态标签高度
    statusLabel->setStyleSheet(
        "QLabel {"
        "   color: #2c3e50;"
        "   font-size: 13px;"  // 稍微减小字体
        "   font-weight: bold;"
        "   background: transparent;"
        "}"
    );
    statusLayout->addWidget(statusLabel);
    
    infoLabel = new QLabel("等待设备", statusCard);
    infoLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);  // 左对齐，顶部对齐
    infoLabel->setWordWrap(true);
    infoLabel->setMinimumHeight(80);  // 设置合适的高度
    infoLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    infoLabel->setStyleSheet(
        "QLabel {"
        "   color: #555;"
        "   font-size: 10px;"  // 减小字体以显示更多内容
        "   background: transparent;"
        "   padding: 3px;"
        "   line-height: 1.3;"
        "}"
    );
    statusLayout->addWidget(infoLabel);
    
    mainLayout->addWidget(statusCard);
    
    // 重启选项区域
    QLabel *rebootLabel = new QLabel("重启选项", container);
    rebootLabel->setStyleSheet(
        "QLabel {"
        "   color: #2c3e50;"
        "   font-size: 13px;"
        "   font-weight: bold;"
        "   background: transparent;"
        "}"
    );
    mainLayout->addWidget(rebootLabel);
    
    // 下拉选择框
    rebootComboBox = new QComboBox(container);
    rebootComboBox->addItem("🔄 重启到系统");
    rebootComboBox->addItem("⚡ 重启到Fastboot");
    rebootComboBox->addItem("🔧 重启到Fastbootd");
    rebootComboBox->addItem("🔌 重启到EDL");
    rebootComboBox->setMinimumHeight(36);
    rebootComboBox->setEnabled(false);
    rebootComboBox->setStyleSheet(
        "QComboBox {"
        "   background-color: rgba(255, 255, 255, 180);"
        "   color: #2c3e50;"
        "   border: 1px solid rgba(100, 160, 180, 150);"
        "   border-radius: 8px;"
        "   padding: 6px 10px;"
        "   font-size: 13px;"
        "}"
        "QComboBox:hover {"
        "   background-color: rgba(255, 255, 255, 220);"
        "   border: 1px solid rgba(100, 160, 180, 200);"
        "}"
        "QComboBox::drop-down {"
        "   border: none;"
        "   width: 30px;"
        "}"
        "QComboBox::down-arrow {"
        "   image: none;"
        "   border-left: 5px solid transparent;"
        "   border-right: 5px solid transparent;"
        "   border-top: 6px solid #2c3e50;"
        "   margin-right: 10px;"
        "}"
        "QComboBox QAbstractItemView {"
        "   background-color: rgba(255, 255, 255, 240);"
        "   color: #2c3e50;"
        "   border: 1px solid rgba(100, 160, 180, 200);"
        "   border-radius: 8px;"
        "   selection-background-color: rgba(100, 160, 180, 180);"
        "   selection-color: white;"
        "   padding: 5px;"
        "}"
    );
    mainLayout->addWidget(rebootComboBox);
    
    // 执行按钮
    executeButton = new QPushButton("执行重启", container);
    executeButton->setMinimumHeight(36);
    executeButton->setCursor(Qt::PointingHandCursor);
    executeButton->setEnabled(false);
    executeButton->setStyleSheet(UIHelper::getStandardButtonStyle());
    connect(executeButton, &QPushButton::clicked, this, &DeviceCheckWindow::onRebootButtonClicked);
    mainLayout->addWidget(executeButton);
    
    // 打开CMD按钮
    QPushButton *cmdButton = new QPushButton("💻 打开CMD", container);
    cmdButton->setMinimumHeight(36);
    cmdButton->setCursor(Qt::PointingHandCursor);
    cmdButton->setStyleSheet(
        "QPushButton {"
        "   background-color: rgba(120, 120, 120, 180);"
        "   color: white;"
        "   border: none;"
        "   border-radius: 8px;"
        "   font-size: 13px;"
        "   font-weight: bold;"
        "   padding: 6px 10px;"
        "}"
        "QPushButton:hover {"
        "   background-color: rgba(100, 100, 100, 200);"
        "}"
        "QPushButton:pressed {"
        "   background-color: rgba(80, 80, 80, 220);"
        "}"
    );
    connect(cmdButton, &QPushButton::clicked, this, &DeviceCheckWindow::onOpenCmdClicked);
    mainLayout->addWidget(cmdButton);
    
    // 刷入分区按钮（2列1行）
    QWidget *flashWidget = new QWidget(container);
    QHBoxLayout *flashLayout = new QHBoxLayout(flashWidget);
    flashLayout->setSpacing(10);
    flashLayout->setContentsMargins(0, 0, 0, 0);
    
    QPushButton *bootButton = new QPushButton("📦 刷入Boot", flashWidget);
    bootButton->setMinimumHeight(36);
    bootButton->setCursor(Qt::PointingHandCursor);
    bootButton->setStyleSheet(
        "QPushButton {"
        "   background-color: rgba(100, 149, 237, 180);"
        "   color: white;"
        "   border: none;"
        "   border-radius: 8px;"
        "   font-size: 13px;"
        "   font-weight: bold;"
        "}"
        "QPushButton:hover {"
        "   background-color: rgba(100, 149, 237, 220);"
        "}"
        "QPushButton:pressed {"
        "   background-color: rgba(80, 129, 217, 240);"
        "}"
    );
    connect(bootButton, &QPushButton::clicked, this, &DeviceCheckWindow::onFlashBootClicked);
    flashLayout->addWidget(bootButton);
    
    QPushButton *initBootButton = new QPushButton("📦 刷入Init_Boot", flashWidget);
    initBootButton->setMinimumHeight(36);
    initBootButton->setCursor(Qt::PointingHandCursor);
    initBootButton->setStyleSheet(
        "QPushButton {"
        "   background-color: rgba(100, 149, 237, 180);"
        "   color: white;"
        "   border: none;"
        "   border-radius: 8px;"
        "   font-size: 13px;"
        "   font-weight: bold;"
        "}"
        "QPushButton:hover {"
        "   background-color: rgba(100, 149, 237, 220);"
        "}"
        "QPushButton:pressed {"
        "   background-color: rgba(80, 129, 217, 240);"
        "}"
    );
    connect(initBootButton, &QPushButton::clicked, this, &DeviceCheckWindow::onFlashInitBootClicked);
    flashLayout->addWidget(initBootButton);
    
    mainLayout->addWidget(flashWidget);
    
    mainLayout->addStretch();
}

void DeviceCheckWindow::setPosition(int mainMenuX, int mainMenuY)
{
    // 窗口紧靠主菜单左侧
    int x = mainMenuX - width();
    int y = mainMenuY;
    move(x, y);
}

void DeviceCheckWindow::onDeviceModeChanged(DeviceManager::DeviceMode mode)
{
    updateUIForMode(mode);
}

void DeviceCheckWindow::onDeviceInfoUpdated(const QString &info)
{
    infoLabel->setText(info);
}

void DeviceCheckWindow::updateUIForMode(DeviceManager::DeviceMode mode)
{
    if (mode == DeviceManager::ADB) {
        statusLabel->setText("ADB 模式");
        rebootComboBox->setEnabled(true);
        executeButton->setEnabled(true);
    } else if (mode == DeviceManager::Fastboot) {
        statusLabel->setText("Fastboot 模式");
        rebootComboBox->setEnabled(true);
        executeButton->setEnabled(true);
    } else {
        statusLabel->setText("未连接");
        infoLabel->setText("等待设备");
        rebootComboBox->setEnabled(false);
        executeButton->setEnabled(false);
    }
}

void DeviceCheckWindow::onRebootButtonClicked()
{
    int index = rebootComboBox->currentIndex();
    QString adbPath = ResourceExtractor::getAdbPath();
    QString fastbootPath = ResourceExtractor::getFastbootPath();
    
    if (currentProcess) {
        if (currentProcess->state() == QProcess::Running) {
            currentProcess->kill();
            currentProcess->waitForFinished(100);
        }
        currentProcess->deleteLater();
    }
    
    currentProcess = new QProcess(this);
    currentProcess->setWorkingDirectory(ResourceExtractor::getResourcePath());
    
    DeviceManager::DeviceMode currentMode = DeviceManager::instance()->currentMode();
    
    connect(currentProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
        Q_UNUSED(exitCode);
        Q_UNUSED(exitStatus);
        currentProcess->deleteLater();
        currentProcess = nullptr;
    });
    
    if (currentMode == DeviceManager::ADB) {
        if (index == 0) {
            // 重启到系统
            currentProcess->start(adbPath, QStringList() << "reboot");
        } else if (index == 1) {
            // 重启到 Fastboot
            currentProcess->start(adbPath, QStringList() << "reboot" << "bootloader");
        } else if (index == 2) {
            // 重启到 Fastbootd
            currentProcess->start(adbPath, QStringList() << "reboot" << "fastboot");
        } else if (index == 3) {
            // 重启到 EDL
            currentProcess->start(adbPath, QStringList() << "reboot" << "edl");
        }
    } else if (currentMode == DeviceManager::Fastboot) {
        if (index == 0) {
            // 重启到系统
            currentProcess->start(fastbootPath, QStringList() << "reboot");
        } else if (index == 1) {
            // 重启到 Fastboot
            currentProcess->start(fastbootPath, QStringList() << "reboot-bootloader");
        } else if (index == 2) {
            // 重启到 Fastbootd
            currentProcess->start(fastbootPath, QStringList() << "reboot-fastboot");
        } else if (index == 3) {
            // 重启到 EDL (Fastboot模式下使用oem edl命令)
            currentProcess->start(fastbootPath, QStringList() << "oem" << "edl");
        }
    }
}

void DeviceCheckWindow::onOpenCmdClicked()
{
    QString qiubaiPath = ResourceExtractor::getResourcePath();
    QString cmdBatPath = qiubaiPath + "/CMD.bat";
    
    // 检查文件是否存在
    if (!QFile::exists(cmdBatPath)) {
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误", "找不到 CMD.bat 文件！\n路径: " + cmdBatPath, this);
        return;
    }
    
    // 使用Shell执行（允许UAC提示）
    qint64 pid = 0;
    bool success = QProcess::startDetached("cmd.exe", 
                           QStringList() << "/c" << "start" << "CMD.bat",
                           qiubaiPath,
                           &pid);
    
    if (success && pid > 0) {
        ProcessManager::recordProcess(pid, "cmd.exe");
        qDebug() << "CMD已启动，PID:" << pid;
    }
}

void DeviceCheckWindow::onFlashBootClicked()
{
    flashPartition("boot");
}

void DeviceCheckWindow::onFlashInitBootClicked()
{
    flashPartition("init_boot");
}

void DeviceCheckWindow::flashPartition(const QString &partition)
{
    // 获取桌面IMG文件夹路径
    QString desktopPath = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    QString imgPath = desktopPath + "/IMG";
    
    // 检查IMG文件夹是否存在，如果不存在则使用桌面目录
    QString selectPath = imgPath;
    if (!QDir(imgPath).exists()) {
        selectPath = desktopPath;
    }
    
    // 让用户选择镜像文件
    QString imagePath = QFileDialog::getOpenFileName(
        this,
        QString("选择%1镜像文件").arg(partition),
        selectPath,
        "镜像文件 (*.img);;所有文件 (*.*)"
    );
    
    if (imagePath.isEmpty()) {
        // 用户取消选择
        return;
    }
    
    QFileInfo fileInfo(imagePath);
    QString selectedFile = fileInfo.fileName();
    
    // 确认刷入
    QMessageBox::StandardButton reply = UIHelper::showCenteredQuestion("确认刷入",
        QString("确定要刷入以下镜像到%1分区吗？\n\n%2").arg(partition, selectedFile),
        this);
    
    if (reply != QMessageBox::Yes) {
        return;
    }
    
    DeviceManager::DeviceMode currentMode = DeviceManager::instance()->currentMode();
    
    // 根据当前模式处理
    if (currentMode == DeviceManager::ADB) {
        // 在ADB模式，需要重启到Fastboot
        qDebug() << "设备在ADB模式，正在重启到Fastboot...";
        
        pendingFlashPartition = partition;
        pendingFlashImage = imagePath;
        
        QString adbPath = ResourceExtractor::getAdbPath();
        QString qiubaiPath = ResourceExtractor::getResourcePath();
        
        if (currentProcess) {
            if (currentProcess->state() == QProcess::Running) {
                currentProcess->kill();
                currentProcess->waitForFinished(100);
            }
            currentProcess->deleteLater();
        }
        
        currentProcess = new QProcess(this);
        currentProcess->setWorkingDirectory(qiubaiPath);
        
        connect(currentProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
            Q_UNUSED(exitCode);
            Q_UNUSED(exitStatus);
            
            // 等待设备进入Fastboot模式
            waitForFastbootMode();
        });
        
        currentProcess->start(adbPath, QStringList() << "reboot" << "bootloader");
        
    } else if (currentMode == DeviceManager::Fastboot) {
        // 已在Fastboot模式，直接刷入
        performFlash(partition, imagePath);
    } else {
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误", 
            "请先连接设备并进入Fastboot模式", this);
        return;
    }
}

void DeviceCheckWindow::waitForFastbootMode()
{
    // 如果已经有定时器在运行，先停止并清理
    if (waitTimer) {
        waitTimer->stop();
        waitTimer->deleteLater();
        waitTimer = nullptr;
    }
    
    waitCounter = 0;
    waitTimer = new QTimer(this);
    
    connect(waitTimer, &QTimer::timeout, this, [this]() {
        waitCounter++;
        
        // 检查是否已进入Fastboot模式
        if (DeviceManager::instance()->currentMode() == DeviceManager::Fastboot) {
            waitTimer->stop();
            waitTimer->deleteLater();
            waitTimer = nullptr;
            
            // 延迟2秒后执行刷入，确保设备稳定
            QTimer::singleShot(2000, this, [this]() {
                performFlash(pendingFlashPartition, pendingFlashImage);
                pendingFlashPartition.clear();
                pendingFlashImage.clear();
            });
            return;
        }
        
        // 超时20秒
        if (waitCounter >= 40) {  // 40 * 500ms = 20秒
            waitTimer->stop();
            waitTimer->deleteLater();
            waitTimer = nullptr;
            
            // 清空pending变量
            pendingFlashPartition.clear();
            pendingFlashImage.clear();
            
            UIHelper::showCenteredMessageBox(QMessageBox::Warning, "超时", 
                "设备未能进入Fastboot模式，请手动重试", this);
            return;
        }
    });
    
    waitTimer->start(500);  // 每500ms检查一次
}

void DeviceCheckWindow::performFlash(const QString &partition, const QString &imagePath)
{
    QString qiubaiPath = ResourceExtractor::getResourcePath();
    QString fastbootPath = qiubaiPath + "/fastboot.exe";
    
    // 暂停设备监控，避免fastboot冲突
    DeviceManager::instance()->pauseMonitoring();
    
    if (currentProcess) {
        if (currentProcess->state() == QProcess::Running) {
            currentProcess->kill();
            currentProcess->waitForFinished(100);
        }
        currentProcess->deleteLater();
    }
    
    currentProcess = new QProcess(this);
    currentProcess->setWorkingDirectory(qiubaiPath);
    
    connect(currentProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, partition](int exitCode, QProcess::ExitStatus exitStatus) {
        Q_UNUSED(exitStatus);
        
        QString output = currentProcess->readAllStandardOutput();
        QString error = currentProcess->readAllStandardError();
        QString allOutput = output + "\n" + error;
        
        qDebug() << "Flash output:" << output;
        qDebug() << "Flash error:" << error;
        qDebug() << "Exit code:" << exitCode;
        
        // 检查是否成功：查找OKAY和Finished关键字
        if (allOutput.contains("OKAY") && allOutput.contains("Finished")) {
            qDebug() << partition << "分区刷入成功，正在重启...";
            
            // 重启到系统
            QString fastbootPath = ResourceExtractor::getResourcePath() + "/fastboot.exe";
            QProcess *rebootProcess = new QProcess(this);
            rebootProcess->setWorkingDirectory(ResourceExtractor::getResourcePath());
            
            connect(rebootProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                    this, [rebootProcess](int exitCode, QProcess::ExitStatus exitStatus) {
                Q_UNUSED(exitCode);
                Q_UNUSED(exitStatus);
                rebootProcess->deleteLater();
                // 重启后恢复监控
                DeviceManager::instance()->resumeMonitoring();
            });
            
            rebootProcess->start(fastbootPath, QStringList() << "reboot");
            
        } else if (allOutput.contains("FAILED") || allOutput.contains("error")) {
            // 刷入失败，恢复监控
            DeviceManager::instance()->resumeMonitoring();
            UIHelper::showCenteredMessageBox(QMessageBox::Critical, "失败", 
                QString("%1分区刷入失败！\n\n%2").arg(partition, allOutput), this);
        } else {
            // 不确定的情况，恢复监控
            DeviceManager::instance()->resumeMonitoring();
            UIHelper::showCenteredMessageBox(QMessageBox::Information, "完成", 
                QString("%1分区刷入完成\n\n%2").arg(partition, allOutput), this);
        }
        
        currentProcess->deleteLater();
        currentProcess = nullptr;
    });
    
    currentProcess->start(fastbootPath, QStringList() << "flash" << partition << imagePath);
}

void DeviceCheckWindow::mousePressEvent(QMouseEvent *event)
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

void DeviceCheckWindow::mouseMoveEvent(QMouseEvent *event)
{
    Q_UNUSED(event);
}

void DeviceCheckWindow::mouseReleaseEvent(QMouseEvent *event)
{
    Q_UNUSED(event);
}

void DeviceCheckWindow::moveEvent(QMoveEvent *event)
{
    QWidget::moveEvent(event);
    
    // 窗口移动时重置定时器，移动停止200ms后恢复透明度
    if (isDragging) {
        opacityTimer->start(200);
    }
}

void DeviceCheckWindow::restoreOpacity()
{
    isDragging = false;
    setWindowOpacity(0.95);  // 恢复到轻微透明
}
