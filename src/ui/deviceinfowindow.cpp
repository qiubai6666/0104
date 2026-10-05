#include "deviceoperationlease.h"
#include "processmanager.h"
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
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <cmath>

namespace {
enum class InfoIcon { Phone, Tag, System, Storage, Lock, Connection, Settings, Alert, Authorization };

// 使用矢量线性图标，避免系统 Emoji 字体缺失时显示方框。
class DeviceInfoIcon : public QWidget
{
public:
    explicit DeviceInfoIcon(bool primary, QWidget *parent)
        : QWidget(parent), m_primary(primary), m_icon(InfoIcon::Phone), m_color("#4A90E2")
    {
        setFixedSize(primary ? 32 : 18, primary ? 32 : 18);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

    void setAppearance(InfoIcon icon, const QColor &color)
    {
        m_icon = icon;
        m_color = color;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        if (m_primary) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor("#E7F0FA"));
            painter.drawRoundedRect(QRectF(rect()), 9, 9);
            painter.translate(5, 5);
            painter.scale(22.0 / 24.0, 22.0 / 24.0);
        } else {
            painter.scale(width() / 24.0, height() / 24.0);
        }
        painter.setPen(QPen(m_color, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        QPainterPath path;
        switch (m_icon) {
        case InfoIcon::Phone:
        case InfoIcon::Connection:
            painter.drawRoundedRect(QRectF(6, 2, 12, 20), 2.5, 2.5);
            painter.drawLine(QPointF(10, 18.5), QPointF(14, 18.5));
            if (m_icon == InfoIcon::Connection) {
                painter.drawLine(QPointF(9, 9), QPointF(15, 13));
                painter.drawLine(QPointF(15, 9), QPointF(9, 13));
            }
            break;
        case InfoIcon::Tag:
            path.moveTo(3, 4); path.lineTo(12, 4); path.lineTo(21, 13);
            path.lineTo(13, 21); path.lineTo(3, 11); path.closeSubpath();
            painter.drawPath(path);
            painter.drawEllipse(QRectF(6, 7, 2, 2));
            break;
        case InfoIcon::System:
            painter.drawRoundedRect(QRectF(3, 4, 18, 13), 2, 2);
            painter.drawLine(QPointF(12, 17), QPointF(12, 21));
            painter.drawLine(QPointF(8, 21), QPointF(16, 21));
            break;
        case InfoIcon::Storage:
            painter.drawRoundedRect(QRectF(3, 5, 18, 14), 2, 2);
            painter.drawLine(QPointF(3, 11), QPointF(21, 11));
            painter.drawPoint(QPointF(17, 15));
            break;
        case InfoIcon::Lock:
            path.moveTo(8, 10); path.lineTo(8, 7);
            path.cubicTo(8, 1, 16, 1, 16, 7); path.lineTo(16, 10);
            painter.drawPath(path);
            painter.drawRoundedRect(QRectF(5, 10, 14, 11), 2, 2);
            painter.drawLine(QPointF(12, 14), QPointF(12, 17));
            break;
        case InfoIcon::Settings:
            painter.drawEllipse(QRectF(8, 8, 8, 8));
            for (int i = 0; i < 8; ++i) {
                const double angle = i * 3.141592653589793 / 4;
                painter.drawLine(QPointF(12 + 7 * std::cos(angle), 12 + 7 * std::sin(angle)),
                                 QPointF(12 + 10 * std::cos(angle), 12 + 10 * std::sin(angle)));
            }
            break;
        case InfoIcon::Alert:
            path.moveTo(13, 2); path.lineTo(5, 13); path.lineTo(11, 13);
            path.lineTo(10, 22); path.lineTo(19, 10); path.lineTo(13, 10);
            path.closeSubpath(); painter.drawPath(path);
            break;
        case InfoIcon::Authorization:
            path.moveTo(5, 4); path.lineTo(19, 4); path.quadTo(21, 4, 21, 6);
            path.lineTo(21, 15); path.quadTo(21, 17, 19, 17);
            path.lineTo(9, 17); path.lineTo(4, 21); path.lineTo(4, 17);
            path.quadTo(3, 17, 3, 15); path.lineTo(3, 6);
            path.quadTo(3, 4, 5, 4); painter.drawPath(path);
            painter.drawLine(QPointF(8, 10), QPointF(11, 13));
            painter.drawLine(QPointF(11, 13), QPointF(16, 8));
            break;
        }
    }

private:
    bool m_primary;
    InfoIcon m_icon;
    QColor m_color;
};

class DeviceInfoRow : public QWidget
{
public:
    DeviceInfoRow(const QString &name, QLabel *&label, bool primary, QWidget *parent)
        : QWidget(parent)
    {
        setObjectName(name);
        setAttribute(Qt::WA_StyledBackground);
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(12, primary ? 5 : 4, 12, primary ? 5 : 4);
        layout->setSpacing(10);
        icon = new DeviceInfoIcon(primary, this);
        label = new QLabel(this);
        label->setObjectName(name + "Text");
        label->setTextFormat(Qt::PlainText);
        layout->addWidget(icon, 0, Qt::AlignVCenter);
        layout->addWidget(label, 1);
    }
    DeviceInfoIcon *icon;
};
} // namespace

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
    
    queryTimer = new QTimer(this);
    queryTimer->setSingleShot(true);
    queryTimer->setInterval(5000);
    setupUI();
    
    // 创建 scrcpy 进程对象
    scrcpyProcess = ProcessManager::createProcess(this);
    // 投屏只是查看画面，设备重启后会自然断开，不阻止重启/刷写操作。
    scrcpyProcess->setProperty("orangeNonBlockingTool", true);
    
    // 监听scrcpy进程结束信号
    connect(scrcpyProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &DeviceInfoWindow::onScrcpyFinished);
    
    // 连接设备管理器的信号
    connect(DeviceManager::instance(), &DeviceManager::deviceModeChanged,
            this, &DeviceInfoWindow::onDeviceModeChanged);
    connect(DeviceManager::instance(), &DeviceManager::deviceInfoUpdated,
            this, &DeviceInfoWindow::onDeviceInfoUpdated);
    
    connect(DeviceOperationLease::instance(), &DeviceOperationLease::changed, this, [this](bool held) {
        if (held) cancelDeviceQuery();
        else if (DeviceManager::instance()->currentMode() == DeviceManager::ADB)
            onDeviceInfoUpdated(DeviceManager::instance()->getDeviceInfo());
    });

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
    cancelDeviceQuery();
    disconnect(DeviceOperationLease::instance(), nullptr, this, nullptr);
    disconnect(scrcpyProcess, nullptr, this, nullptr);
    // 终止scrcpy进程
    if (scrcpyProcess && scrcpyProcess->state() == QProcess::Running) {
        scrcpyProcess->kill();
        scrcpyProcess->waitForFinished(100);
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
    // 已连接和未连接始终保持同样的紧凑尺寸。
    setFixedSize(360, 212);
    setCursor(Qt::SizeAllCursor);

    QWidget *bgContainer = new QWidget(this);
    bgContainer->setObjectName("deviceInfoSurface");
    bgContainer->setGeometry(rect());
    bgContainer->setStyleSheet(
        "QWidget#deviceInfoSurface {"
        " background-color: #FFFFFF;"
        " border: 1px solid #D6E2EE;"
        " border-radius: 12px;"
        "}"
    );

    mainLayout = new QVBoxLayout(bgContainer);
    mainLayout->setContentsMargins(10, 10, 10, 10);
    mainLayout->setSpacing(0);

    cardContainer = new QWidget(bgContainer);
    cardContainer->setObjectName("deviceInfoCard");
    // 状态样式集中定义；切换设备状态时只更新内容、图标和模式。
    cardContainer->setStyleSheet(R"(
        QWidget#deviceInfoCard { background: transparent; border: none; }
        QWidget#deviceInfoCard QLabel {
            background: transparent; border: none; padding: 0;
            color: #4A90E2; font-size: 13px;
        }
        QWidget#deviceModelRow {
            background: #F3F7FC; border: 1px solid #E7EEF6; border-radius: 8px;
        }
        QWidget#deviceInfoCard QLabel#deviceModelRowText {
            font-size: 14px; font-weight: bold;
        }
        QWidget#deviceInfoCard[mode="connected"] QWidget#deviceCodenameRow,
        QWidget#deviceInfoCard[mode="connected"] QWidget#deviceVersionRow,
        QWidget#deviceInfoCard[mode="connected"] QWidget#deviceSlotRow {
            border: none; border-bottom: 1px solid #EDF2F7;
        }
        QWidget#deviceInfoCard[mode="disconnected"] QLabel {
            font-size: 12px;
        }
        QWidget#deviceInfoCard[mode="disconnected"] QLabel#deviceModelRowText {
            color: #5B7C99; font-size: 14px; font-weight: bold;
        }
        QWidget#deviceInfoCard[mode="disconnected"] QWidget#deviceVersionRow {
            background: #FFF7EB; border: 1px solid #F5E7D2; border-radius: 7px;
        }
        QWidget#deviceInfoCard[mode="disconnected"] QLabel#deviceVersionRowText {
            color: #D97706; font-weight: bold;
        }
        QWidget#deviceInfoCard[mode="disconnected"] QWidget#deviceSlotRow {
            border: none; border-top: 1px solid #EDF2F7;
        }
        QWidget#deviceInfoCard[mode="disconnected"] QLabel#deviceSlotRowText {
            color: #5B7C99; font-weight: bold;
        }
    )");

    auto *cardLayout = new QVBoxLayout(cardContainer);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(4);
    cardLayout->addWidget(new DeviceInfoRow("deviceModelRow", modelLabel, true, cardContainer));
    cardLayout->addWidget(new DeviceInfoRow("deviceCodenameRow", codenameLabel, false, cardContainer));
    cardLayout->addWidget(new DeviceInfoRow("deviceVersionRow", versionLabel, false, cardContainer));
    cardLayout->addWidget(new DeviceInfoRow("deviceSlotRow", slotLabel, false, cardContainer));
    cardLayout->addWidget(new DeviceInfoRow("deviceUnlockRow", unlockLabel, false, cardContainer));
    mainLayout->addWidget(cardContainer);
    showNoDeviceMessage();

    QScreen *screen = QGuiApplication::primaryScreen();
    QRect screenGeometry = screen->geometry();
    const int margin = 10;
    move(screenGeometry.left() + margin, screenGeometry.top() + margin);
}

void DeviceInfoWindow::applyPresentation(bool connected)
{
    cardContainer->setProperty("mode", connected ? "connected" : "disconnected");
    const InfoIcon connectedIcons[] = {InfoIcon::Phone, InfoIcon::Tag, InfoIcon::System,
                                       InfoIcon::Storage, InfoIcon::Lock};
    const InfoIcon disconnectedIcons[] = {InfoIcon::Connection, InfoIcon::Settings, InfoIcon::Alert,
                                          InfoIcon::Authorization, InfoIcon::Lock};
    QLabel *labels[] = {modelLabel, codenameLabel, versionLabel, slotLabel, unlockLabel};
    for (int i = 0; i < 5; ++i) {
        auto *row = static_cast<DeviceInfoRow *>(labels[i]->parentWidget());
        const QColor color = !connected && i == 2 ? QColor("#D97706")
            : (!connected && (i == 0 || i == 3) ? QColor("#5B7C99") : QColor("#4A90E2"));
        row->icon->setAppearance(connected ? connectedIcons[i] : disconnectedIcons[i], color);
        row->setVisible(connected || i != 4);
        labels[i]->show();
        row->style()->unpolish(row);
        row->style()->polish(row);
        labels[i]->style()->unpolish(labels[i]);
        labels[i]->style()->polish(labels[i]);
        labels[i]->updateGeometry();
        row->layout()->invalidate();
        row->updateGeometry();
        row->update();
    }
    cardContainer->layout()->invalidate();
    cardContainer->update();
}
void DeviceInfoWindow::startScrcpy()
{
    if (DeviceOperationLease::busyFor(this) || DeviceManager::instance()->currentMode() != DeviceManager::ADB) return;
    if (scrcpyProcess->state() != QProcess::NotRunning) return;
    scrcpySerial = DeviceManager::instance()->deviceSerial();
    QStringList args = {"--max-size", "1024", "--video-bit-rate", "4M"};
    if (!scrcpySerial.isEmpty()) args << "--serial" << scrcpySerial;
    scrcpyProcess->setWorkingDirectory(ResourceExtractor::getResourcePath());
    scrcpyProcess->start(ResourceExtractor::getResourcePath() + "/scrcpy.exe", args);
}

void DeviceInfoWindow::onDeviceModeChanged(DeviceManager::DeviceMode mode)
{
    if (mode == DeviceManager::ADB) {
        showDeviceInfo();
        onDeviceInfoUpdated(DeviceManager::instance()->getDeviceInfo());
        startScrcpy();
    } else {
        cancelDeviceQuery();
        showNoDeviceMessage();
        if (scrcpyProcess->state() != QProcess::NotRunning) scrcpyProcess->kill();
    }
}

void DeviceInfoWindow::cancelDeviceQuery()
{
    ++queryGeneration;
    queryActive = false;
    queryTimer->stop();
    disconnect(queryTimer, nullptr, this, nullptr);
    QProcess *old = queryProcess;
    queryProcess = nullptr;
    if (old) {
        // Invalidate and disconnect BEFORE kill: finished can run synchronously.
        disconnect(old, nullptr, this, nullptr);
        if (old->state() != QProcess::NotRunning) {
            old->kill();
            old->waitForFinished(1000);
        }
        old->deleteLater();
    }
}

bool DeviceInfoWindow::isCurrentDeviceQuery(quint64 generation, QProcess *process, int step) const
{
    return queryActive && generation == queryGeneration && process == queryProcess && step == queryStep &&
           DeviceManager::instance()->currentMode() == DeviceManager::ADB &&
           DeviceManager::instance()->deviceSerial() == querySerial && !DeviceOperationLease::busyFor();
}

void DeviceInfoWindow::onDeviceInfoUpdated(const QString &info)
{
    auto *manager = DeviceManager::instance();
    // An empty snapshot invalidates both mode changes and same-mode device replacement.
    if (info.isEmpty()) {
        cancelDeviceQuery();
        pendingModel = pendingVersion = pendingCodename = pendingSlot = pendingUnlock = QStringLiteral("未知");
        if (manager->currentMode() == DeviceManager::ADB) {
            showDeviceInfo();
            updateDeviceLabels(pendingModel, pendingCodename, pendingVersion, pendingSlot, pendingUnlock);
        } else showNoDeviceMessage();
        if (scrcpySerial != manager->deviceSerial() && scrcpyProcess->state() != QProcess::NotRunning)
            scrcpyProcess->kill();
        return;
    }
    if (manager->currentMode() != DeviceManager::ADB) { cancelDeviceQuery(); return; }
    if (DeviceOperationLease::busyFor()) { cancelDeviceQuery(); return; }
    pendingCodename = pendingSlot = pendingUnlock = QStringLiteral("未知");
    for (const QString &line : info.split('\n')) {
        if (line.startsWith("代号:")) pendingCodename = line.mid(3).trimmed();
        else if (line.startsWith("分区:")) pendingSlot = line.mid(3).trimmed();
        else if (line.startsWith("解锁:")) pendingUnlock = line.mid(3).trimmed();
    }
    // A slow model/version chain belongs to this device, not to each polling tick.
    if (queryActive && querySerial == manager->deviceSerial()) return;
    cancelDeviceQuery();
    querySerial = manager->deviceSerial();
    queryActive = true;
    pendingModel = pendingVersion = QStringLiteral("未知");
    startDeviceQueryStep(0);
    startScrcpy();
}

void DeviceInfoWindow::startDeviceQueryStep(int step)
{
    queryStep = step;
    if (queryProcess) {
        disconnect(queryProcess, nullptr, this, nullptr);
        queryProcess->deleteLater();
    }
    QProcess *process = ProcessManager::createProcess(this);
    queryProcess = process;
    process->setWorkingDirectory(ResourceExtractor::getResourcePath());
    const quint64 generation = queryGeneration;
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, process, generation, step](int code, QProcess::ExitStatus status) {
        if (isCurrentDeviceQuery(generation, process, step))
            finishDeviceQueryStep(status == QProcess::NormalExit && code == 0,
                                  QString::fromLocal8Bit(process->readAllStandardOutput()));
    });
    connect(process, &QProcess::errorOccurred, this,
            [this, process, generation, step](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && isCurrentDeviceQuery(generation, process, step))
            finishDeviceQueryStep(false, QString());
    });
    disconnect(queryTimer, nullptr, this, nullptr);
    connect(queryTimer, &QTimer::timeout, this, [this, process, generation, step]() {
        if (isCurrentDeviceQuery(generation, process, step)) finishDeviceQueryStep(false, QString());
    });
    QStringList args;
    if (!querySerial.isEmpty()) args << "-s" << querySerial;
    args << "shell" << "getprop" << (step == 0 ? "ro.product.model" : "ro.build.version.release");
    queryTimer->start();
    process->start(ResourceExtractor::getAdbPath(), args);
}

void DeviceInfoWindow::finishDeviceQueryStep(bool success, const QString &output)
{
    queryTimer->stop();
    disconnect(queryProcess, nullptr, this, nullptr);
    if (queryProcess->state() != QProcess::NotRunning) {
        queryProcess->kill();
        queryProcess->waitForFinished(1000);
    }
    const QString value = output.trimmed();
    const QString validated = success && !value.isEmpty() && !value.contains('\n') && !value.contains('\r')
        ? value : QStringLiteral("未知");
    if (queryStep == 0) { pendingModel = validated; startDeviceQueryStep(1); return; }
    pendingVersion = validated;
    queryActive = false;
    queryProcess->deleteLater();
    queryProcess = nullptr;
    updateDeviceLabels(pendingModel, pendingCodename, pendingVersion, pendingSlot, pendingUnlock);
    if (pendingModel == QStringLiteral("未知") || pendingVersion == QStringLiteral("未知")) {
        const quint64 generation = queryGeneration;
        QTimer::singleShot(1000, this, [this, generation]() {
            if (generation == queryGeneration && !queryActive && !DeviceOperationLease::busyFor() &&
                DeviceManager::instance()->currentMode() == DeviceManager::ADB &&
                DeviceManager::instance()->deviceSerial() == querySerial)
                onDeviceInfoUpdated(DeviceManager::instance()->getDeviceInfo());
        });
    }
}

void DeviceInfoWindow::updateDeviceLabels(const QString &model, const QString &codename, 
                                       const QString &version, const QString &slot, const QString &unlock)
{
    modelLabel->setText("手机型号: " + model);
    codenameLabel->setText("手机代号: " + codename);
    versionLabel->setText("系统版本: Android " + version);
    slotLabel->setText("活动分区: " + slot);
    unlockLabel->setText("解锁状态: " + unlock);
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
    modelLabel->setText("未检测到设备\n请用数据线连接手机或平板");
    modelLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    modelLabel->setWordWrap(true);
    codenameLabel->setText(
        "开启开发者模式和USB调试\n"
        "设置 → 关于手机 → 连续点击版本号\n"
        "开发者选项 → 开启USB调试"
    );
    codenameLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    codenameLabel->setWordWrap(true);
    versionLabel->setText("小米/红米：还需开启\nUSB安装、USB调试（安全设置）");
    versionLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    versionLabel->setWordWrap(true);
    slotLabel->setText("请在手机上允许USB调试授权");
    slotLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    slotLabel->setWordWrap(false);
    applyPresentation(false);
}

void DeviceInfoWindow::showDeviceInfo()
{
    modelLabel->setText("手机型号: 获取中...");
    codenameLabel->setText("手机代号: 获取中...");
    versionLabel->setText("系统版本: 获取中...");
    slotLabel->setText("活动分区: 获取中...");
    unlockLabel->setText("解锁状态: 获取中...");
    QLabel *labels[] = {modelLabel, codenameLabel, versionLabel, slotLabel, unlockLabel};
    for (QLabel *label : labels) {
        label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        label->setWordWrap(false);
    }
    applyPresentation(true);
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
    if (DeviceOperationLease::busyFor(this)) return;
    if (!DeviceOperationLease::acquire(this)) return;
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
    transferProcess = ProcessManager::createProcess(this);
    connect(transferProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) { if(e==QProcess::FailedToStart) DeviceOperationLease::release(this); });
    transferProcess->setWorkingDirectory(ResourceExtractor::getResourcePath());
    
    connect(transferProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &DeviceInfoWindow::onTransferFinished);
    
    // 开始传输第一个文件
    transferNextFile();
}

void DeviceInfoWindow::transferNextFile()
{
    if (DeviceOperationLease::busyFor(this)) return;
    // 检查设备是否仍然连接
    if (DeviceManager::instance()->currentMode() != DeviceManager::ADB) {
        disconnect(transferProcess, nullptr, this, nullptr);
        DeviceOperationLease::release(this);
        if (transferIndex > 0) {
            UIHelper::showCenteredMessageBox(QMessageBox::Warning, "传输中断", 
                QString("设备已断开，已成功传输 %1 个文件").arg(transferSuccessCount), this);
        }
        return;
    }
    
    if (transferIndex >= pendingTransferFiles.size()) {
        // 所有文件传输完成
        disconnect(transferProcess, nullptr, this, nullptr);
        DeviceOperationLease::release(this);
        
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
