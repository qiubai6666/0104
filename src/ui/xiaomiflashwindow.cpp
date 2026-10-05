#include "xiaomiflashwindow.h"
#include "resourceextractor.h"
#include "deviceoperationlease.h"
#include <QCloseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QCheckBox>
#include <QDir>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>
#include <QTextCursor>
#include <QPainter>
#include <QSvgRenderer>

XiaomiFlashWindow::XiaomiFlashWindow(QWidget *launcher)
    : QWidget(nullptr), m_service(new XiaomiFlashService(this)), m_launcher(launcher) {
    // Match the existing Ouga taskbar window, not the always-on-top menu's
    // native ownership chain. The launcher still protects active tasks.
    setWindowFlags(Qt::Window);
    if (launcher) connect(launcher, &QObject::destroyed, this, &QObject::deleteLater);
    setWindowTitle("秋白工作室 · 小米线刷");
    setObjectName("XiaomiFlashView");
    setMinimumSize(780, 650);
    resize(900, 740);
    QFont font("Microsoft YaHei UI"); font.setPixelSize(13); setFont(font);
    auto root = new QVBoxLayout(this);
    root->setContentsMargins(18, 16, 18, 16); root->setSpacing(10);
    auto card = new QGroupBox(this); card->setObjectName("XiaomiFlashCard");
    auto body = new QVBoxLayout(card); body->setContentsMargins(16, 14, 16, 14); body->setSpacing(8);
    auto title = new QLabel("小米官方线刷", card);
    title->setStyleSheet("font-size:20px;font-weight:600;color:#263142;");
    body->addWidget(title);
    body->addWidget(new QLabel("刷机包文件夹", card));
    auto pathRow = new QHBoxLayout;
    m_path = new QLineEdit(card); m_path->setObjectName("XiaomiFlashScriptPathTextBox");
    m_path->setPlaceholderText("选择已解压、包含 flash_all*.bat 的刷机包根目录");
    m_path->setMinimumHeight(36);
    QPixmap folder(16, 16); folder.fill(Qt::transparent);
    QSvgRenderer svg(QStringLiteral(":/ouga/folder.svg"));
    { QPainter painter(&folder); svg.render(&painter); }
    m_choose = new QPushButton(QIcon(folder), "选择", card);
    m_choose->setObjectName("SelectXiaomiFlashScriptButton"); m_choose->setProperty("tone", "sky");
    m_choose->setMinimumSize(84, 36);
    pathRow->addWidget(m_path, 1); pathRow->addWidget(m_choose); body->addLayout(pathRow);
    auto actionRow = new QHBoxLayout;
    auto modes = new QHBoxLayout;
    modes->setSpacing(14);
    m_wipe = new QRadioButton("清除数据刷机", card); m_wipe->setObjectName("CompleteWipeCheckBox");
    m_keep = new QRadioButton("保留数据刷机", card); m_keep->setObjectName("KeepDataCheckBox");
    m_lock = new QRadioButton("清除数据并回锁 BL", card); m_lock->setObjectName("WipeAndLockBLCheckBox");
    m_wipe->setChecked(true);
    m_wipe->setToolTip("执行 flash_all.bat，清除用户数据");
    m_keep->setToolTip("执行 flash_all_except_storage.bat，仍建议先备份数据");
    m_lock->setToolTip("执行 flash_all_lock.bat；仅用于与设备完全匹配的官方 ROM");
    modes->addWidget(m_wipe); modes->addWidget(m_keep); modes->addWidget(m_lock);
    actionRow->addLayout(modes, 1);
    m_start = new QPushButton("开始线刷", card); m_start->setObjectName("StartXiaomiFlashButton");
    m_start->setProperty("tone", "purple"); m_start->setMinimumSize(126, 42);
    actionRow->addWidget(m_start); body->addLayout(actionRow);
    auto hint = new QLabel("仅支持小米 / Redmi 官方 Fastboot 线刷包；需已解锁 BL。开始前自动检测设备，请只连接一台目标设备。\n清除数据 / 回锁会带来数据丢失和无法开机风险，开始前请备份并核对机型。", card);
    hint->setWordWrap(true); hint->setStyleSheet("color:#64748B;font-size:12px;"); body->addWidget(hint);
    root->addWidget(card);
    m_cancelCheck = new QPushButton("取消检测", this);
    m_cancelCheck->setObjectName("CancelXiaomiCheckButton");
    m_cancelCheck->hide();
    m_progress = new QProgressBar(this); m_progress->setObjectName("XiaomiFlashProgressBar");
    m_progress->setRange(0, 100); m_progress->setValue(0); m_progress->setFormat("准备就绪"); root->addWidget(m_progress);
    m_status = new QLabel("0MB/s  |  Time:0s", this);
    m_status->setObjectName("XiaomiFlashStatusLabel");
    auto statusRow = new QHBoxLayout; statusRow->addWidget(m_status, 1);
    statusRow->addWidget(m_cancelCheck); root->addLayout(statusRow);
    m_warning = new QLabel(this); m_warning->setWordWrap(true);
    m_warning->setObjectName("XiaomiFlashWarningLabel");
    m_warning->setStyleSheet("color:#B45309;font-size:12px;");
    m_warning->hide(); root->addWidget(m_warning);
    root->addWidget(new QLabel("线刷日志", this));
    m_log = new QPlainTextEdit(this); m_log->setObjectName("XiaomiFlashLogTextBox"); m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(6000);
    m_log->setMinimumHeight(240); root->addWidget(m_log, 1);
    auto notice = new QLabel("刷写中不提供强制停止：请勿拔线、关闭程序或结束进程。", this);
    notice->setStyleSheet("color:#B45309;font-size:12px;"); root->addWidget(notice);
    setStyleSheet(R"(
        XiaomiFlashWindow { background:#F7F8FC; }
        QGroupBox { background:white; border:1px solid #E2E6ED; border-radius:10px; }
        QLabel, QRadioButton { color:#334155; }
        QRadioButton { spacing:8px; padding:4px 0; }
        QLineEdit, QPlainTextEdit { background:white; color:#475569; border:1px solid #DDE2EA; border-radius:5px; padding:6px; }
        QPushButton { border:1px solid #E6E6E6; border-radius:6px; padding:5px 12px; }
        QPushButton[tone="sky"] { background:#87CEEB; color:white; }
        QPushButton[tone="purple"] { background:#B876DD; color:white; }
        QPushButton:hover { border-color:#8B5CB3; }
        QPushButton:disabled { background:#E2E6ED; color:#94A3B8; }
        QProgressBar { background:white; border:1px solid #DDE2EA; border-radius:5px; text-align:center; color:#475569; min-height:22px; }
        QProgressBar::chunk { background:#B876DD; border-radius:4px; }
    )");
    connect(m_choose, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getExistingDirectory(this, "选择小米刷机包文件夹", m_path->text());
        if (!path.isEmpty()) m_path->setText(QDir::toNativeSeparators(path));
    });
    connect(m_start, &QPushButton::clicked, this, &XiaomiFlashWindow::startFlash);
    connect(m_cancelCheck, &QPushButton::clicked, m_service, &XiaomiFlashService::cancelCheck);
    connect(m_service, &XiaomiFlashService::checkingChanged, this, [this](bool checking) {
        m_cancelCheck->setVisible(checking);
        if (checking) m_progress->setFormat("检测设备中，尚未开始刷机");
    });
    connect(m_service, &XiaomiFlashService::log, this, [this](const QString &text) {
        m_log->moveCursor(QTextCursor::End); m_log->insertPlainText(text);
        if (!text.endsWith('\n')) m_log->insertPlainText("\n");
        m_log->ensureCursorVisible();
    });
    connect(m_service, &XiaomiFlashService::progress, this, [this](int percent) {
        m_progress->setRange(0, percent < 0 ? 0 : 100);
        if (percent >= 0) m_progress->setValue(percent);
        m_progress->setFormat(percent < 0 ? "执行中（无法估算总进度）" : (percent == 100 ? "脚本执行完成" : "线刷进度 %p%"));
    });
    connect(m_service, &XiaomiFlashService::progressInfo, m_status, &QLabel::setText);
    connect(m_service, &XiaomiFlashService::warning, this, [this](const QString &text) {
        m_warning->setText(text); m_warning->show();
    });
    connect(m_service, &XiaomiFlashService::finished, this, [this](bool success, const QString &) {
        m_progress->setRange(0, 100);
        m_progress->setFormat(success ? "脚本执行完成，请核对日志" :
            (m_service->hasStartedScript() ? "未成功，请检查日志" : "未开始刷机，请检查日志")); setBusy(false);
    });
}
Xiaomi::Mode XiaomiFlashWindow::mode() const {
    return m_lock->isChecked() ? Xiaomi::Mode::WipeAndLock : (m_keep->isChecked() ? Xiaomi::Mode::KeepData : Xiaomi::Mode::Wipe);
}
void XiaomiFlashWindow::startFlash() {
    if (isBusy()) return;
    Xiaomi::Package package; QString error;
    const auto selectedMode = mode();
    if (!Xiaomi::inspectPackage(m_path->text(), selectedMode, &package, &error)) {
        QMessageBox::warning(this, "刷机包检查失败", error); return;
    }
    QDialog dialog(this); dialog.setObjectName("XiaomiFlashConfirmDialog"); dialog.setWindowTitle("确认小米线刷");
    auto layout = new QVBoxLayout(&dialog);
    auto description = new QLabel(QString("模式：%1\n脚本：%2\n进度估算：%3\n\n将执行刷机包内原版 BAT，请仅使用可信的官方刷机包。\n确认手机为对应机型、已解锁 BL 并处于 Fastboot 模式。\n%4\n刷写中不允许中断，型号和防回滚检查不会被移除。")
        .arg(Xiaomi::modeName(selectedMode), package.script, package.progressPlanValid ? QString("%1 个镜像，按总字节数计算").arg(package.images.size()) : QString("无法建立完整计划；仍执行原脚本并使用回退进度"))
        .arg(selectedMode == Xiaomi::Mode::KeepData ? "保留数据不保证数据安全，请仍先备份。" : "本模式会清除用户数据，无法撤销。"), &dialog);
    description->setWordWrap(true); layout->addWidget(description);
    auto agree = new QCheckBox("我已备份数据、核对机型，并信任该刷机脚本", &dialog);
    agree->setObjectName("XiaomiConfirmAgreement"); layout->addWidget(agree);
    QCheckBox *lock = nullptr;
    if (selectedMode == Xiaomi::Mode::WipeAndLock) {
        lock = new QCheckBox("确认回锁 BL：官方 ROM 与设备/地区完全匹配，否则可能无法开机", &dialog);
        lock->setObjectName("XiaomiConfirmLockAgreement"); layout->addWidget(lock);
    }
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText("确认开始"); buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    buttons->button(QDialogButtonBox::Cancel)->setText("取消"); layout->addWidget(buttons);
    auto update = [=] { buttons->button(QDialogButtonBox::Ok)->setEnabled(agree->isChecked() && (!lock || lock->isChecked())); };
    connect(agree, &QCheckBox::toggled, &dialog, update);
    if (lock) connect(lock, &QCheckBox::toggled, &dialog, update);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    const auto confirmed = package;
    // Re-inspect after the modal confirmation, before handing anything to cmd.
    if (!Xiaomi::inspectPackage(m_path->text(), selectedMode, &package, &error) ||
        package.scriptSha256 != confirmed.scriptSha256) {
        QMessageBox::warning(this, "刷机包已变化", "检查后刷机包已变化，请重新确认。" + error); return;
    }
    m_log->clear(); m_warning->clear(); m_warning->hide();
    m_progress->setRange(0, 100); m_progress->setValue(0);
    m_service->configure(ResourceExtractor::getFastbootPath());
    setBusy(true);
    if (!m_service->start(package, &error)) {
        setBusy(false); QMessageBox::warning(this, "无法开始小米线刷", error);
    }
}
void XiaomiFlashWindow::setBusy(bool busy) {
    m_path->setEnabled(!busy); m_choose->setEnabled(!busy); m_start->setEnabled(!busy);
    m_wipe->setEnabled(!busy); m_keep->setEnabled(!busy); m_lock->setEnabled(!busy);
}
void XiaomiFlashWindow::closeEvent(QCloseEvent *event) {
    if (isBusy()) { event->ignore(); return; }
    QWidget::closeEvent(event);
}
