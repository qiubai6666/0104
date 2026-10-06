#include "deviceoperationlease.h"
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
#include <QCloseEvent>
#include <QPainter>
#include <QDialog>
#include <QTextEdit>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QAbstractTextDocumentLayout>
#include <QResizeEvent>
#include <QGridLayout>
#include <QPointer>
#include <QApplication>
#include <QEvent>

namespace {
// Keep a single selectable document, spreading fields rather than enlarging text.
class DeviceDetailsText final : public QTextEdit
{
public:
    explicit DeviceDetailsText(QWidget *parent) : QTextEdit(parent) {}

    void setDetails(const QString &details)
    {
        if (toPlainText() == details) return;
        setPlainText(details);
        distributeFields();
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QTextEdit::resizeEvent(event);
        distributeFields();
    }

private:
    void distributeFields()
    {
        auto *doc = document();
        if (doc->blockCount() < 2 || viewport()->height() <= 0) return;
        const QTextCursor selection = textCursor();
        auto setGap = [doc](qreal gap) {
            for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
                QTextCursor cursor(block);
                auto format = block.blockFormat();
                format.setTopMargin(0);
                format.setBottomMargin(block.next().isValid() ? gap : 0);
                cursor.setBlockFormat(format);
            }
        };
        // Measure wrapped lines first: a full kernel version can span several lines.
        setGap(0);
        const qreal naturalHeight = doc->documentLayout()->documentSize().height();
        const qreal gap = qMax<qreal>(0, (viewport()->height() - naturalHeight - 1)
                                       / (doc->blockCount() - 1));
        setGap(gap);
        setTextCursor(selection);
    }
};

class DeviceDetailsDialog final : public QDialog
{
public:
    explicit DeviceDetailsDialog(const QString &details, QWidget *parent = nullptr)
        : QDialog(parent)
    {
        setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        setAttribute(Qt::WA_TranslucentBackground);
        setModal(false);
        setObjectName(QStringLiteral("deviceDetailsDialog"));

        auto *surface = new QWidget(this);
        surface->setObjectName(QStringLiteral("deviceDetailsSurface"));
        auto *outer = new QVBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->addWidget(surface);

        auto *layout = new QVBoxLayout(surface);
        layout->setContentsMargins(12, 12, 12, 12);
        layout->setSpacing(0);

        auto *editor = new DeviceDetailsText(surface);
        editor->setObjectName(QStringLiteral("deviceDetailsText"));
        editor->setReadOnly(true);
        editor->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        editor->setDetails(details);
        editor->setLineWrapMode(QTextEdit::WidgetWidth);
        editor->setFrameShape(QFrame::NoFrame);
        editor->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        editor->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        layout->addWidget(editor, 1);

        setStyleSheet(QStringLiteral(
            "QWidget#deviceDetailsSurface { background:#FFFFFF; border:1px solid #D6E2EE; border-radius:12px; }"
            "QTextEdit#deviceDetailsText { color:#2c3e50; background:#F3F7FC; border:1px solid #D6E2EE; border-radius:8px; padding:8px; font-size:11px; selection-background-color:#B8D8E5; }"));
    }
};}
#include "processmanager.h"
#include "resourceextractor.h"
#include "uihelper.h"
#include "devicemanager.h"
DeviceCheckWindow::DeviceCheckWindow(QWidget *parent)
    : QWidget(parent)
    , currentProcess(nullptr)
    , waitTimer(nullptr)
    , waitCounter(0)
    , isDragging(false)
    , opacityTimer(nullptr)
    , detailsDialog(nullptr)
{
    // 设置窗口标志：无边框、置顶
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground);
    // The main menu owns this persistent submenu. Closing only hides it;
    // never destroy child QProcesses or trigger application exit here.
    setAttribute(Qt::WA_DeleteOnClose, false);
    setAttribute(Qt::WA_QuitOnClose, false);
    setProperty(DeviceOperationLease::RetainedWindowProperty, true);
    
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
    connect(DeviceManager::instance(), &DeviceManager::deviceDetailsUpdated,
            this, &DeviceCheckWindow::onDeviceInfoUpdated);
    connect(DeviceManager::instance(), &DeviceManager::extendedDeviceDetailsUpdated,
            this, [this](const QString &) {
        if (detailsDialog && detailsDialog->isVisible()) {
            static_cast<DeviceDetailsText *>(detailsDialog->findChild<QTextEdit *>(QStringLiteral("deviceDetailsText")))->setDetails(
                DeviceManager::instance()->getExtendedDeviceDetails());
        }
    });
    
    // 确保设备监控已启动
    DeviceManager::instance()->ensureMonitoring();
    
    // 更新初始状态
    updateUIForMode(DeviceManager::instance()->currentMode());
    
    // 如果有设备信息，立即显示
    QString deviceInfo = DeviceManager::instance()->getDeviceDetails();
    if (!deviceInfo.isEmpty()) {
        infoLabel->setText(deviceInfo);
    }
}

DeviceCheckWindow::~DeviceCheckWindow()
{
    // 真正销毁时先断开业务回调，避免终止进程触发下一步操作。
    if (currentProcess) {
        disconnect(currentProcess, nullptr, this, nullptr);
        if (currentProcess->state() != QProcess::NotRunning) {
            currentProcess->kill();
            currentProcess->waitForFinished(1000);
        }
        delete currentProcess;
        currentProcess = nullptr;
    }
    finishOperation();
    DeviceManager::instance()->releaseFullModeMonitoring();
}

void DeviceCheckWindow::closeEvent(QCloseEvent *event)
{
    // Active reboot/flash operations continue in the retained window.
    QWidget::closeEvent(event);
}

bool DeviceCheckWindow::beginOperation()
{
    if (operationInProgress) {
        return false;
    }
    QString error;
    if (!DeviceOperationLease::acquire(this, &error)) {
        // 设备通道被占用时明确提示，避免点击后无任何反馈。
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "设备通道占用", error, this);
        return false;
    }
    operationInProgress = true;
    updateUIForMode(DeviceManager::instance()->currentMode());
    return true;
}

void DeviceCheckWindow::finishOperation()
{
    if (waitTimer) {
        waitTimer->stop();
        waitTimer->deleteLater();
        waitTimer = nullptr;
    }
    pendingFlashPartition.clear();
    pendingFlashImage.clear();
    DeviceOperationLease::release(this);
    if (monitoringPausedByOperation) {
        monitoringPausedByOperation = false;
        DeviceManager::instance()->resumeMonitoring();
    }
    operationInProgress = false;
    updateUIForMode(DeviceManager::instance()->currentMode());
}

QProcess *DeviceCheckWindow::createOperationProcess()
{
    Q_ASSERT(!currentProcess);
    QProcess *process = ProcessManager::createProcess(this);
    currentProcess = process;
    process->setWorkingDirectory(ResourceExtractor::getResourcePath());
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        // Crashed 随后还会触发 finished；只在启动失败时单独收尾。
        if (error != QProcess::FailedToStart || currentProcess != process) {
            return;
        }
        const QString message = process->errorString();
        releaseOperationProcess(process);
        finishOperation();
        UIHelper::showCenteredMessageBox(QMessageBox::Critical, "启动失败", message, this);
    });
    return process;
}

void DeviceCheckWindow::releaseOperationProcess(QProcess *process)
{
    disconnect(process, nullptr, this, nullptr);
    if (currentProcess == process) {
        currentProcess = nullptr;
    }
    process->deleteLater();
}

void DeviceCheckWindow::setupUI()
{
    setFixedSize(252, UIHelper::MenuButtonHeight * 9);  // 显示前再同步主菜单实际高度
    
    // 创建主容器
    QWidget *container = new QWidget(this);
    container->setObjectName("deviceCheckSurface");
    QVBoxLayout *windowLayout = new QVBoxLayout(this);
    windowLayout->setContentsMargins(0, 0, 0, 0);
    windowLayout->setSpacing(0);
    windowLayout->addWidget(container);
    container->setStyleSheet(
        "QWidget#deviceCheckSurface {"
        "   background-color: #FFFFFF;"
        "   border: 1px solid #D6E2EE;"
        "   border-radius: 12px;"
        "}"
    );
    
    mainLayout = new QVBoxLayout(container);
    mainLayout->setSpacing(0);
    mainLayout->setContentsMargins(12, 12, 12, 12);
    
    // 标题去掉独立边框，避免小窗口内重复套卡。
    QLabel *titleLabel = new QLabel("设备检测", container);
    titleLabel->setAlignment(Qt::AlignCenter);
    titleLabel->setFixedHeight(28);
    titleLabel->setStyleSheet(
        "QLabel {"
        "   color: #2c3e50;"
        "   font-size: 15px;"
        "   font-weight: bold;"
        "   background: transparent;"
        "   border: none;"
        "}"
    );
    mainLayout->addWidget(titleLabel);
    mainLayout->addSpacing(8);
    
    // 单一浅蓝信息面板，细色带区分信息与操作，不使用图标或表情。
    QWidget *statusCard = new QWidget(container);
    statusCard->setObjectName("deviceStatusCard");
    statusCard->setFixedHeight(120);  // 足够容纳五行信息，连接前后保持相同尺寸
    statusCard->setStyleSheet(
        "QWidget#deviceStatusCard {"
        "   background-color: #F3F7FC;"
        "   border: none;"
        "   border-left: 3px solid #83AECA;"
        "   border-radius: 8px;"
        "}"
    );
    QVBoxLayout *statusLayout = new QVBoxLayout(statusCard);
    statusLayout->setSpacing(4);
    statusLayout->setContentsMargins(12, 7, 12, 7);
    
    statusLabel = new QLabel("检测中...", statusCard);
    statusLabel->setAlignment(Qt::AlignCenter);
    statusLabel->setFixedHeight(25);  // 固定状态标签高度
    statusLabel->setStyleSheet(
        "QLabel {"
        "   color: #2c3e50;"
        "   font-size: 13px;"
        "   font-weight: bold;"
        "   background: transparent;"
        "   border-bottom: 1px solid #E7EEF6;"
        "}"
    );
    statusLayout->addWidget(statusLabel);
    
    infoLabel = new QLabel("等待设备", statusCard);
    infoLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);  // 左对齐，顶部对齐
    infoLabel->setWordWrap(true);
    infoLabel->setCursor(Qt::PointingHandCursor);
    infoLabel->installEventFilter(this);
    infoLabel->setMinimumHeight(76);  // 容纳多行设备信息，不添加滚动区域
    infoLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    infoLabel->setStyleSheet(
        "QLabel {"
        "   color: #555;"
        "   font-size: 10px;"
        "   background: transparent;"
        "   padding: 3px;"
        "   line-height: 1.3;"
        "}"
    );
    statusLayout->addWidget(infoLabel);
    
    mainLayout->addWidget(statusCard);
    mainLayout->addStretch(1);
    
    // 标签和下拉框作为一个整体参与均匀分布
    QVBoxLayout *rebootLayout = new QVBoxLayout();
    rebootLayout->setContentsMargins(0, 0, 0, 0);
    rebootLayout->setSpacing(4);
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
    rebootLayout->addWidget(rebootLabel);
    
    // 下拉选择框
    // 绘制矢量下拉指示，避免样式表的三角边框在不同 DPI 下变成短横线。
    class RebootComboBox : public QComboBox {
    public:
        using QComboBox::QComboBox;
    protected:
        void paintEvent(QPaintEvent *event) override {
            QComboBox::paintEvent(event);
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(QPen(QColor("#2c3e50"), 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            const qreal x = width() - 18;
            const qreal y = height() / 2.0;
            const QPointF chevron[] = {QPointF(x - 4, y - 2), QPointF(x, y + 2), QPointF(x + 4, y - 2)};
            painter.drawPolyline(chevron, 3);
        }
    };
    rebootComboBox = new RebootComboBox(container);
    rebootComboBox->addItem("重启到系统");
    rebootComboBox->addItem("重启到Fastboot");
    rebootComboBox->addItem("重启到Fastbootd");
    rebootComboBox->addItem("重启到EDL");
    rebootComboBox->setFixedHeight(34);
    rebootComboBox->setEnabled(false);
    rebootComboBox->setStyleSheet(
        "QComboBox {"
        "   background-color: #F7FAFD;"
        "   color: #2c3e50;"
        "   border: 1px solid #D0DEEB;"
        "   border-radius: 8px;"
        "   padding: 4px 10px;"
        "   font-size: 13px;"
        "}"
        "QComboBox:hover {"
        "   background-color: rgba(255, 255, 255, 220);"
        "   border: 1px solid #83AECA;"
        "}"
        "QComboBox:focus {"
        "   border: 1px solid #83AECA;"
        "}"
        "QComboBox:disabled {"
        "   background-color: #F1F4F8;"
        "   border: 1px solid #E0E7EF;"
        "}"
        "QComboBox::drop-down {"
        "   border: none;"
        "   border-left: 1px solid #E0E7EF;"
        "   width: 28px;"
        "}"
        "QComboBox::down-arrow { image: none; }"
        "QComboBox QAbstractItemView {"
        "   background-color: #FFFFFF;"
        "   color: #2c3e50;"
        "   border: 1px solid #83AECA;"
        "   border-radius: 8px;"
        "   selection-background-color: #649EB3;"
        "   selection-color: white;"
        "   padding: 5px;"
        "}"
    );
    rebootLayout->addWidget(rebootComboBox);
    mainLayout->addLayout(rebootLayout);
    mainLayout->addStretch(1);
    
    // 与 PAYLOAD「开始提取」采用同一套按钮色板，仅保留各按钮原文字颜色。
    const QString actionButtonStyle = QStringLiteral(
        "QPushButton {"
        "   background-color: #649EB3;"
        "   color: white;"
        "   border: 1px solid #649EB3;"
        "   border-radius: 8px;"
        "   font-size: 13px;"
        "   font-weight: bold;"
        "   padding: 4px;"
        "}"
        "QPushButton:hover { background-color: #578FA6; }"
        "QPushButton:pressed { background-color: #477F96; }"
        "QPushButton:focus { border-color: #3F7894; }"
        "QPushButton:disabled {"
        "   background-color: #A8C3D0;"
        "   border-color: #A8C3D0;"
        "}"
    );

    // CMD 保留白字，使用同色系的低强调蓝；重启和刷入仍沿用 PAYLOAD 主色。
    const QString utilityButtonStyle = actionButtonStyle + QStringLiteral(
        "QPushButton { background-color: #83AECA; border-color: #83AECA; }"
        "QPushButton:hover { background-color: #729DB8; border-color: #729DB8; }"
        "QPushButton:pressed { background-color: #6490AA; border-color: #6490AA; }"
        "QPushButton:focus { border-color: #3F7894; }"
        "QPushButton:disabled { background-color: #A8C3D0; border-color: #A8C3D0; }"
    );

    // 执行按钮
    executeButton = new QPushButton("执行重启", container);
    executeButton->setFixedHeight(34);
    executeButton->setCursor(Qt::PointingHandCursor);
    executeButton->setEnabled(false);
    executeButton->setStyleSheet(actionButtonStyle +
        "QPushButton:disabled { color: rgba(255, 255, 255, 150); }");
    connect(executeButton, &QPushButton::clicked, this, &DeviceCheckWindow::onRebootButtonClicked);
    mainLayout->addWidget(executeButton);
    mainLayout->addStretch(1);
    
    // 打开CMD按钮
    cmdButton = new QPushButton("打开CMD", container);
    cmdButton->setFixedHeight(34);
    cmdButton->setCursor(Qt::PointingHandCursor);
    cmdButton->setStyleSheet(utilityButtonStyle);
    connect(cmdButton, &QPushButton::clicked, this, &DeviceCheckWindow::onOpenCmdClicked);
    mainLayout->addWidget(cmdButton);
    mainLayout->addStretch(1);
    
    // 底部刷入区用一条轻分隔线收尾，按钮仍与菜单底边保持固定距离。
    QWidget *footerDivider = new QWidget(container);
    footerDivider->setObjectName("deviceFooterDivider");
    footerDivider->setFixedHeight(1);
    footerDivider->setStyleSheet("QWidget#deviceFooterDivider { background: #E7EEF6; border: none; }");
    mainLayout->addWidget(footerDivider);
    mainLayout->addSpacing(7);

    // 刷入分区按钮（2列1行）
    QWidget *flashWidget = new QWidget(container);
    flashWidget->setFixedHeight(34);
    QHBoxLayout *flashLayout = new QHBoxLayout(flashWidget);
    flashLayout->setSpacing(8);
    flashLayout->setContentsMargins(0, 0, 0, 0);
    
    bootButton = new QPushButton("刷入Boot", flashWidget);
    bootButton->setFixedHeight(34);
    bootButton->setCursor(Qt::PointingHandCursor);
    bootButton->setStyleSheet(actionButtonStyle);
    connect(bootButton, &QPushButton::clicked, this, &DeviceCheckWindow::onFlashBootClicked);
    flashLayout->addWidget(bootButton);
    
    initBootButton = new QPushButton("刷入Init_Boot", flashWidget);
    initBootButton->setFixedHeight(34);
    initBootButton->setCursor(Qt::PointingHandCursor);
    initBootButton->setStyleSheet(actionButtonStyle);
    connect(initBootButton, &QPushButton::clicked, this, &DeviceCheckWindow::onFlashInitBootClicked);
    flashLayout->addWidget(initBootButton);
    
    // 最后一行无底部弹性空间，刷入按钮始终贴合内容区底边。
    mainLayout->addWidget(flashWidget);
}

void DeviceCheckWindow::setPosition(int mainMenuX, int mainMenuY, int mainMenuHeight)
{
    // 按实际菜单高度同步，上下边缘始终对齐。
    setFixedHeight(mainMenuHeight);
    // 窗口紧靠主菜单左侧
    int x = mainMenuX - width();
    int y = mainMenuY;
    move(x, y);
}

bool DeviceCheckWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == infoLabel) {
        if (event->type() == QEvent::MouseButtonPress && detailsDialog && detailsDialog->isVisible()) {
            detailsDialog->close();
            return true;
        }
        if (event->type() == QEvent::MouseButtonDblClick) {
            if (DeviceManager::instance()->currentMode() != DeviceManager::None &&
                !DeviceManager::instance()->getDeviceDetails().isEmpty()) {
                showDeviceDetails();
            }
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void DeviceCheckWindow::showDeviceDetails()
{
    if (detailsDialog && detailsDialog->isVisible()) {
        detailsDialog->close();
        return;
    }
    const QString details = DeviceManager::instance()->getExtendedDeviceDetails().isEmpty()
        ? DeviceManager::instance()->getDeviceDetails()
        : DeviceManager::instance()->getExtendedDeviceDetails();
    auto *dialog = new DeviceDetailsDialog(details, this);
    detailsDialog = dialog;
    dialog->setFixedSize(size());
    const QRect area = (windowHandle() && windowHandle()->screen())
        ? windowHandle()->screen()->availableGeometry()
        : QGuiApplication::primaryScreen()->availableGeometry();
    dialog->move(area.center() - QPoint(dialog->width() / 2, dialog->height() / 2));
    connect(dialog, &QDialog::finished, this, [this]() { detailsDialog = nullptr; });
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void DeviceCheckWindow::onDeviceModeChanged(DeviceManager::DeviceMode mode)
{
    updateUIForMode(mode);
}

void DeviceCheckWindow::onDeviceInfoUpdated(const QString &info)
{
    infoLabel->setText(DeviceManager::instance()->currentMode() == DeviceManager::None ? QStringLiteral("等待设备") :
                       info.isEmpty() ? QStringLiteral("获取中...") : info);
}

void DeviceCheckWindow::updateUIForMode(DeviceManager::DeviceMode mode)
{
    const bool connected = mode != DeviceManager::None;
    infoLabel->setToolTip(QStringLiteral("双击查看设备信息，再次点击关闭"));
    statusLabel->setText(mode == DeviceManager::ADB ? "ADB 模式" :
                         mode == DeviceManager::Fastboot ? "Fastboot 模式" : "未连接");
    if (!connected) {
        infoLabel->setText("等待设备");
    }
    const bool available = connected && !operationInProgress;
    rebootComboBox->setEnabled(available);
    executeButton->setEnabled(available);
    bootButton->setEnabled(available);
    initBootButton->setEnabled(available);
    cmdButton->setEnabled(!operationInProgress);
}

void DeviceCheckWindow::onRebootButtonClicked()
{
    const auto mode = DeviceManager::instance()->currentMode();
    if (mode == DeviceManager::None || !beginOperation()) {
        return;
    }
    const int index = rebootComboBox->currentIndex();
    QStringList arguments;
    if (mode == DeviceManager::ADB) {
        arguments << "reboot";
        if (index == 1) arguments << "bootloader";
        else if (index == 2) arguments << "fastboot";
        else if (index == 3) arguments << "edl";
    } else {
        if (index == 1) arguments << "reboot-bootloader";
        else if (index == 2) arguments << "reboot-fastboot";
        else if (index == 3) arguments << "oem" << "edl";
        else arguments << "reboot";
        DeviceManager::instance()->pauseMonitoring();
        monitoringPausedByOperation = true;
    }
    QProcess *process = createOperationProcess();
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, process](int exitCode, QProcess::ExitStatus status) {
        const QString error = QString::fromLocal8Bit(process->readAllStandardError());
        releaseOperationProcess(process);
        finishOperation();
        if (exitCode != 0 || status != QProcess::NormalExit) {
            UIHelper::showCenteredMessageBox(QMessageBox::Warning, "重启失败", error, this);
        }
    });
    process->start(mode == DeviceManager::ADB ? ResourceExtractor::getAdbPath() :
                                             ResourceExtractor::getFastbootPath(), arguments);
}

void DeviceCheckWindow::onOpenCmdClicked()
{
    if (DeviceOperationLease::busyFor(this)) return;
    if (operationInProgress) return;
    QString qiubaiPath = ResourceExtractor::getResourcePath();
    QString cmdBatPath = qiubaiPath + "/CMD.bat";
    
    // 检查文件是否存在
    if (!QFile::exists(cmdBatPath)) {
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误", "找不到 CMD.bat 文件！\n路径: " + cmdBatPath, this);
        return;
    }
    
    // 直接持有运行批处理的 CMD，不使用 start 创建无法持有的中间进程。
    QProcess *process = ProcessManager::startProcess("cmd.exe",
                           QStringList() << "/d" << "/c" << "CMD.bat", qiubaiPath, true);

    if (process) {
        qDebug() << "CMD已启动，PID:" << process->processId();
    } else {
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误", "无法打开 CMD！", this);
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
    // 选择镜像、等待 Fastboot、刷写及后续重启均属于同一次互斥操作。
    if (!beginOperation()) return;
    const QString desktopPath = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    const QString imgPath = desktopPath + "/IMG";
    const QString imagePath = QFileDialog::getOpenFileName(this,
        QString("选择%1镜像文件").arg(partition), QDir(imgPath).exists() ? imgPath : desktopPath,
        "镜像文件 (*.img);;所有文件 (*.*)");
    if (imagePath.isEmpty()) {
        finishOperation();
        return;
    }
    if (UIHelper::showCenteredQuestion("确认刷入",
            QString("确定要刷入以下镜像到%1分区吗？\n\n%2")
                .arg(partition, QFileInfo(imagePath).fileName()), this) != QMessageBox::Yes) {
        finishOperation();
        return;
    }
    const auto mode = DeviceManager::instance()->currentMode();
    if (mode == DeviceManager::ADB) {
        pendingFlashPartition = partition;
        pendingFlashImage = imagePath;
        QProcess *process = createOperationProcess();
        connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this, process](int exitCode, QProcess::ExitStatus status) {
            const QString error = QString::fromLocal8Bit(process->readAllStandardError());
            releaseOperationProcess(process);
            if (exitCode == 0 && status == QProcess::NormalExit) {
                waitForFastbootMode();
            } else {
                finishOperation();
                UIHelper::showCenteredMessageBox(QMessageBox::Warning, "重启失败", error, this);
            }
        });
        process->start(ResourceExtractor::getAdbPath(), {"reboot", "bootloader"});
    } else if (mode == DeviceManager::Fastboot) {
        performFlash(partition, imagePath);
    } else {
        finishOperation();
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误",
            "请先连接设备并进入Fastboot模式", this);
    }
}

void DeviceCheckWindow::waitForFastbootMode()
{
    Q_ASSERT(!waitTimer);
    // No write is running here. The lease owner explicitly permits read-only
    // mode detection, then suspends it again before the flash delay.
    monitoringPausedByOperation = true;
    DeviceManager::instance()->resumeMonitoring(this);
    waitCounter = 0;
    waitTimer = new QTimer(this);
    connect(waitTimer, &QTimer::timeout, this, [this]() {
        ++waitCounter;
        if (DeviceManager::instance()->currentMode() == DeviceManager::Fastboot) {
            DeviceManager::instance()->pauseMonitoring();
            waitTimer->stop();
            waitTimer->deleteLater();
            waitTimer = nullptr;
            const QString partition = pendingFlashPartition;
            const QString image = pendingFlashImage;
            // 延迟期间仍保持互斥，参数属于当前任务，不读取可变的共享字段。
            QTimer::singleShot(2000, this, [this, partition, image]() {
                if (operationInProgress) performFlash(partition, image);
            });
        } else if (waitCounter >= 40) {
            finishOperation();
            UIHelper::showCenteredMessageBox(QMessageBox::Warning, "超时",
                "设备未能进入Fastboot模式，请手动重试", this);
        }
    });
    waitTimer->start(500);
}

void DeviceCheckWindow::performFlash(const QString &partition, const QString &imagePath)
{
    if (!operationInProgress || currentProcess) return;
    if (DeviceManager::instance()->currentMode() != DeviceManager::Fastboot) {
        finishOperation();
        UIHelper::showCenteredMessageBox(QMessageBox::Warning, "错误", "设备已离开Fastboot模式", this);
        return;
    }
    DeviceManager::instance()->pauseMonitoring();
    monitoringPausedByOperation = true;
    QProcess *process = createOperationProcess();
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, process, partition](int exitCode, QProcess::ExitStatus status) {
        const QString output = QString::fromLocal8Bit(process->readAllStandardOutput()) + "\n" +
                               QString::fromLocal8Bit(process->readAllStandardError());
        releaseOperationProcess(process);
        if (exitCode != 0 || status != QProcess::NormalExit ||
                output.contains("FAILED") || output.contains("error")) {
            finishOperation();
            UIHelper::showCenteredMessageBox(QMessageBox::Critical, "失败",
                QString("%1分区刷入失败！\n\n%2").arg(partition, output), this);
            return;
        }
        if (!output.contains("OKAY") || !output.contains("Finished")) {
            finishOperation();
            UIHelper::showCenteredMessageBox(QMessageBox::Information, "完成",
                QString("%1分区刷入完成\n\n%2").arg(partition, output), this);
            return;
        }
        // 刷写后的重启也由当前窗口持有，结束/启动失败均走同一个收尾路径。
        QProcess *reboot = createOperationProcess();
        connect(reboot, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this, reboot](int code, QProcess::ExitStatus rebootStatus) {
            const QString error = QString::fromLocal8Bit(reboot->readAllStandardError());
            releaseOperationProcess(reboot);
            finishOperation();
            if (code != 0 || rebootStatus != QProcess::NormalExit) {
                UIHelper::showCenteredMessageBox(QMessageBox::Warning, "刷写后重启失败", error, this);
            }
        });
        reboot->start(ResourceExtractor::getFastbootPath(), {"reboot"});
    });
    process->start(ResourceExtractor::getFastbootPath(), {"flash", partition, imagePath});
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
