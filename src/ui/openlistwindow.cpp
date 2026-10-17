#include "openlistwindow.h"
#include "openlistinstaller.h"
#include "devicemanager.h"
#include "uihelper.h"
#include <QCloseEvent>
#include <QScreen>
#include <QShowEvent>
#include <QStyle>
#include <QDateTime>
#include <QFileInfo>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QTimer>

OpenListWindow::OpenListWindow(QWidget *parent, OpenListService *service, OpenListInstaller *installer)
    : QWidget(nullptr), m_launcher(parent), m_service(service ? service : new OpenListService(this)),
      m_installer(installer ? installer : new OpenListInstaller(this)), m_mtService(new MtManagerService(this)) {
    if (parent) connect(parent, &QObject::destroyed, this, &QObject::deleteLater);
    setWindowTitle(QStringLiteral("Openlist"));
    setAttribute(Qt::WA_DeleteOnClose,false); setAttribute(Qt::WA_QuitOnClose,false);
    setupUi();
    connect(m_refresh,&QPushButton::clicked,this,&OpenListWindow::refresh);
    connect(m_cancel,&QPushButton::clicked,this,[this] {
        if (!m_busy || m_installingBatch) return;
        m_cancelled = true;
        if (m_downloading) m_service->cancel();
        else if (m_mtTask) m_mtService->cancel();
        else advanceBatch();
    });
    connect(m_installSelected,&QPushButton::clicked,this,[this] { startSelection(false); });
    connect(m_transferSelected,&QPushButton::clicked,this,[this] { startSelection(true); });
    connect(m_installMt,&QPushButton::clicked,this,[this] {
        if (m_busy) return; m_mtTask = true; m_mtFile.clear(); setBusy(true); m_progress->setRange(0,100); m_status->setText(QStringLiteral("正在获取 MT 管理器最新版本…")); m_mtService->start();
    });
    connect(m_mtService,&MtManagerService::progress,this,[this](qint64 received,qint64 total,double speed) {
        if (!m_mtTask) return; m_progress->setRange(0,total > 0 ? 100 : 0); if (total > 0) m_progress->setValue(received >= total ? 100 : qMin(99, int(received * 100 / total))); m_status->setText(QStringLiteral("正在下载 MT 管理器 · %1 / %2 · %3/s").arg(QLocale().formattedDataSize(received),total>0?QLocale().formattedDataSize(total):QStringLiteral("未知"),QLocale().formattedDataSize(qint64(speed))));
    });
    connect(m_mtService,&MtManagerService::log,this,&OpenListWindow::log);
    connect(m_mtService,&MtManagerService::failed,this,[this](const QString &message) { if (!m_mtTask) return; m_mtTask=false; finish(message); });
    connect(m_mtService,&MtManagerService::ready,this,[this](const QString &version,const QString &file) { if (!m_mtTask) return; m_progress->setRange(0,100); m_progress->setValue(100); m_mtFile=file; log(QStringLiteral("MT 管理器下载完成（版本 %1），正在检测手机…").arg(version)); m_installer->inspectDevices(); });
    connect(m_service,&OpenListService::listingReady,this,[this](const QList<OpenList::Entry> &entries,const QString &error) {
        m_entries = entries; render();
        if (!error.isEmpty()) m_count->setText(error);
        if (!error.isEmpty()) log(error);
        if (m_pending > 0 && --m_pending == 0 && !m_busy) { m_status->setText(QStringLiteral("资源读取完成；可多选后安装或传输到手机，双击文件直接安装")); m_refresh->setEnabled(true); }
    });
    connect(m_service,&OpenListService::progress,this,[this](qint64 received,qint64 total,double speed) {
        m_progress->setRange(0,total > 0 ? 100 : 0);
        if (total > 0) m_progress->setValue(received >= total ? 100 : qMin(99, int(received * 100 / total)));
        m_status->setText(QStringLiteral("正在下载 %1 · %2 / %3 · %4/s（%5/%6）")
            .arg(m_selected.name,QLocale().formattedDataSize(received),total >= 0 ? QLocale().formattedDataSize(total) : QStringLiteral("未知"),QLocale().formattedDataSize(qint64(speed))).arg(m_queueIndex).arg(m_queue.size()));
    });
    connect(m_service,&OpenListService::log,this,&OpenListWindow::log);
    connect(m_service,&OpenListService::failed,this,[this](const QString &error) {
        if (!m_busy || !m_downloading) return;
        m_downloading = false; ++m_failed;
        log(QStringLiteral("%1：%2").arg(m_selected.name,error));
        QTimer::singleShot(0,this,&OpenListWindow::advanceBatch);
    });
    connect(m_service,&OpenListService::ready,this,[this](const OpenList::Entry &entry,const QString &file) {
        if (!m_busy || !m_downloading) return;
        m_downloading = false;
        m_downloaded.append({entry,file});
        log(QStringLiteral("下载完成（%1/%2）：%3").arg(m_queueIndex).arg(m_queue.size()).arg(entry.name));
        QTimer::singleShot(0,this,&OpenListWindow::advanceBatch);
    });
    connect(m_installer,&OpenListInstaller::devicesReady,this,[this](const QStringList &serials) {
        if (!m_busy || serials.isEmpty()) return;
        if (m_mtTask) {
            bool accepted = true; m_serial = serials.size() == 1 ? serials.first() : QInputDialog::getItem(this,QStringLiteral("选择目标手机"),QStringLiteral("已连接多台设备，请选择目标手机："),serials,0,false,&accepted);
            if (!accepted || m_serial.isEmpty()) { m_mtTask=false; finish(QStringLiteral("未选择手机，未开始安装。")); return; }
            updateDevice();
            QTimer::singleShot(0, this, [this] {
                if (m_mtTask && m_busy) m_installer->install(m_mtFile,m_serial);
            });
            return;
        }
        bool accepted = true;
        m_serial = serials.size() == 1 ? serials.first() : QInputDialog::getItem(this,QStringLiteral("选择目标手机"),QStringLiteral("已连接多台设备，请选择目标手机："),serials,0,false,&accepted);
        if (!accepted || m_serial.isEmpty()) { finish(QStringLiteral("未选择手机，未开始任务。")); return; }
        updateDevice();
        QTimer::singleShot(0,this,&OpenListWindow::advanceBatch);
    });
    connect(m_installer,&OpenListInstaller::rootChoiceRequired,this,[this](const QStringList &managers) {
        bool accepted = false;
        const auto manager = QInputDialog::getItem(this,QStringLiteral("选择 Root 管理器"),QStringLiteral("检测到多个管理器，请选择模块安装器："),managers,0,false,&accepted);
        m_installer->chooseRoot(accepted ? manager : QString());
    });
    connect(m_installer,&OpenListInstaller::completed,this,[this](bool ok,const QString &message) {
        if (!m_busy) return;
        if (m_mtTask) {
            const QString result = !ok && !m_mtFile.isEmpty() && !message.contains(m_mtFile)
                ? message + QStringLiteral(" 安装包保留在：%1").arg(m_mtFile) : message;
            m_mtTask=false; m_mtFile.clear(); finish(result); return;
        }
        if (!m_installingBatch) { finish(message); return; }
        if (ok) ++m_succeeded; else ++m_failed;
        log(message);
        QTimer::singleShot(0,this,&OpenListWindow::advanceBatch);
    });
    connect(m_installer,&OpenListInstaller::log,this,&OpenListWindow::log);
    connect(DeviceManager::instance(),&DeviceManager::deviceModeChanged,this,[this] { updateDevice(); });
    connect(DeviceManager::instance(),&DeviceManager::deviceInfoUpdated,this,[this] { updateDevice(); });
    DeviceManager::instance()->ensureAdbOnlyMonitoring();
    connect(this,&QObject::destroyed,DeviceManager::instance(),[] { DeviceManager::instance()->releaseAdbOnlyMonitoring(); });
    updateDevice(); QTimer::singleShot(0,this,&OpenListWindow::refresh);
}
void OpenListWindow::setupUi() {
    setObjectName("openListWindow");
    const QSize initialSize(qRound(866 * 0.9), qRound(729 * 0.9));
    setMinimumSize(initialSize); resize(initialSize);
    QFont font(QStringLiteral("Microsoft YaHei UI")); font.setPixelSize(12); setFont(font);
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(16,12,16,12); layout->setSpacing(10);
    auto *top = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("Openlist 云端安装"),this); title->setStyleSheet("font-size:18px;font-weight:600;color:#334155;");
    top->addWidget(title); top->addStretch(); m_device = new QLabel(this); top->addWidget(m_device);
    m_refresh = new QPushButton(QStringLiteral("刷新资源"),this); m_refresh->setObjectName("openListRefresh"); top->addWidget(m_refresh);
    m_installSelected = new QPushButton(QStringLiteral("安装选中"),this);
    m_installSelected->setToolTip(QStringLiteral("安装 APK、模块 ZIP；SH 以 Root 执行，请仅运行信任的脚本。"));
    m_installSelected->setObjectName("openListInstallSelected"); top->addWidget(m_installSelected);
    m_transferSelected = new QPushButton(QStringLiteral("传输到手机"),this);
    m_transferSelected->setObjectName("openListTransferSelected"); top->addWidget(m_transferSelected);
    m_installMt = new QPushButton(QStringLiteral("安装 MT 管理器"),this); m_installMt->setObjectName("openListInstallMt"); top->addWidget(m_installMt);
    layout->addLayout(top);
    auto *card = new QGroupBox(QStringLiteral("秋白工作室 · 资源列表"),this);
    card->setObjectName("openListFilesCard");
    auto *inner = new QVBoxLayout(card); inner->setContentsMargins(12,18,12,10);
    m_search = new QLineEdit(card); m_search->setPlaceholderText(QStringLiteral("搜索名称或子目录")); inner->addWidget(m_search);
    auto *navigation = new QHBoxLayout;
    m_up = new QPushButton(QStringLiteral("返回上级"), card);
    m_up->setObjectName("openListUp");
    m_directory = new QLabel(QStringLiteral("根目录"), card);
    m_directory->setWordWrap(true);
    navigation->addWidget(m_up); navigation->addWidget(m_directory, 1); inner->addLayout(navigation);
    connect(m_up, &QPushButton::clicked, this, [this] {
        if (m_busy) return;
        const int slash = m_currentDirectory.lastIndexOf('/');
        m_currentDirectory = slash < 0 ? QString() : m_currentDirectory.left(slash);
        m_search->clear(); render();
    });
    auto *table = new QTableWidget(card); m_table = table;
    table->setObjectName("openListFiles");
    table->setColumnCount(4); table->setHorizontalHeaderLabels({QStringLiteral("名称"),QStringLiteral("目录"),QStringLiteral("大小"),QStringLiteral("更新时间")});
    table->setSelectionBehavior(QAbstractItemView::SelectRows); table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers); table->setAlternatingRowColors(true); table->setWordWrap(false);
    table->verticalHeader()->hide(); table->verticalHeader()->setDefaultSectionSize(32);
    table->horizontalHeader()->setSectionResizeMode(0,QHeaderView::Stretch);
    table->setColumnWidth(1,110); table->setColumnWidth(2,80); table->setColumnWidth(3,120);
    inner->addWidget(table,1); m_count = new QLabel(QStringLiteral("正在读取…"),card); inner->addWidget(m_count);
    connect(m_search,&QLineEdit::textChanged,this,[this] { render(); });
    connect(table,&QTableWidget::cellDoubleClicked,this,[this](int row,int) { activate(row); });
    // 选中状态以 remotePath 集合为准：切换目录 / 搜索后重新渲染时回显，跨目录保留。
    connect(table,&QTableWidget::itemSelectionChanged,this,[this] {
        for (int row = 0; row < m_table->rowCount(); ++row) {
            const auto *item = m_table->item(row,0);
            if (!item) continue;
            const int index = item->data(Qt::UserRole).toInt();
            if (index < 0 || index >= m_entries.size()) continue;
            const QString &path = m_entries[index].remotePath;
            if (item->isSelected()) m_selectedPaths.insert(path);
            else m_selectedPaths.remove(path);
        }
        updateCount();
    });
    layout->addWidget(card,3);
    m_status = new QLabel(QStringLiteral("Ctrl / Shift 多选文件或文件夹后安装或传输；传输至内部存储根目录，不安装"),this); m_status->setWordWrap(true); layout->addWidget(m_status);
    auto *progress = new QHBoxLayout; m_progress = new QProgressBar(this); m_progress->setValue(0); progress->addWidget(m_progress,1);
    m_cancel = new QPushButton(QStringLiteral("取消下载"),this); m_cancel->setEnabled(false);
    m_cancel->setObjectName("openListCancel"); progress->addWidget(m_cancel); layout->addLayout(progress);
    auto *logs = new QGroupBox(QStringLiteral("操作日志"),this); auto *logsLayout = new QVBoxLayout(logs);
    m_log = new QPlainTextEdit(logs); m_log->setObjectName("openListLog"); m_log->setReadOnly(true); m_log->setMaximumBlockCount(500);
    logsLayout->addWidget(m_log); layout->addWidget(logs,1);
    setStyleSheet(R"(
        QWidget#openListWindow { background:#F3F6FA; }
        QGroupBox { background:#FFFFFF; border:1px solid #D9E2EC; border-radius:12px; margin-top:8px; color:#334155; font-weight:600; }
        QGroupBox::title { subcontrol-origin:margin; left:10px; padding:0 5px; background:#FFFFFF; }
        QLabel { color:#526579; }
        QPushButton { background:#FFFFFF; color:#526579; border:1px solid #CBD7E2; border-radius:8px; padding:7px 12px; }
        QPushButton:hover { border-color:#2F80ED; color:#2F80ED; background:rgba(47,128,237,.06); }
        QPushButton:disabled { color:#A9B5C1; border-color:#D9E2EC; background:#F3F6FA; }
        QPushButton#openListInstallSelected { background:#2F80ED; color:#FFFFFF; border-color:#2F80ED; font-weight:600; }
        QPushButton#openListInstallSelected:hover { background:#2A73D6; border-color:#2A73D6; color:#FFFFFF; }
        QPushButton#openListCancel { color:#DC2626; border-color:#F3C9C9; background:#FFFFFF; }
        QPushButton#openListCancel:hover { color:#DC2626; border-color:#DC2626; background:#FEF2F2; }
        QPushButton#openListCancel:disabled { color:#A9B5C1; border-color:#D9E2EC; background:#F3F6FA; }
        QLineEdit { background:#FFFFFF; color:#526579; border:1px solid #D2DDE7; border-radius:6px; padding:7px; }
        QTableWidget { background:#FFFFFF; alternate-background-color:#F8FAFC; color:#334155; border:1px solid #DCE5ED; selection-background-color:#DCE7EF; selection-color:#334155; }
        QTableWidget::item { padding:0 4px; }
        QHeaderView::section { background:#EEF3F7; color:#526579; border:0; padding:6px; }
        QPlainTextEdit { background:#F8FAFC; color:#334155; border:0; font-family:"Microsoft YaHei UI","Consolas"; }
        QProgressBar { border:1px solid #CBD7E2; border-radius:6px; background:#FFFFFF; text-align:center; min-height:20px; }
        QProgressBar::chunk { background:#2F80ED; }
    )");
}
void OpenListWindow::refresh() {
    if (m_busy) return;
    m_pending = 1; m_refresh->setEnabled(false); m_status->setText(QStringLiteral("正在读取资源…"));
    m_currentDirectory.clear(); m_entries.clear(); m_selectedPaths.clear();
    render(); m_count->setText(QStringLiteral("正在读取…"));
    m_service->refresh();
}
void OpenListWindow::render() {
    auto *table = m_table; table->setSortingEnabled(false);
    table->blockSignals(true);
    table->setRowCount(0);
    const auto filter = m_search->text().trimmed();
    m_up->setEnabled(!m_busy && !m_currentDirectory.isEmpty());
    m_directory->setText(m_currentDirectory.isEmpty() ? QStringLiteral("根目录") : m_currentDirectory);
    for (int index=0;index<m_entries.size();++index) {
        const auto &entry = m_entries[index];
        const QString parent = entry.relativePath.contains('/') ? entry.relativePath.left(entry.relativePath.lastIndexOf('/')) : QString();
        if (filter.isEmpty() && parent != m_currentDirectory) continue;
        if (!filter.isEmpty() && !entry.relativePath.contains(filter,Qt::CaseInsensitive)) continue;
        const int row = table->rowCount(); table->insertRow(row);
        auto *name = new QTableWidgetItem(entry.name);
        // 预览稿：目录为 accent 蓝文件夹图标；文件按类型着色（APK 绿 / ZIP 蓝 / SH 琥珀 / 其他墨灰）。
        if (entry.directory) {
            name->setIcon(UIHelper::menuIcon(QStringLiteral("folder"), QColor(QStringLiteral("#2F80ED")),
                                             devicePixelRatioF()));
        } else {
            const QString lower = entry.name.toLower();
            QString tone = QStringLiteral("#647585");
            if (lower.endsWith(QStringLiteral(".apk"))) tone = QStringLiteral("#16A34A");
            else if (lower.endsWith(QStringLiteral(".zip"))) tone = QStringLiteral("#2F80ED");
            else if (lower.endsWith(QStringLiteral(".sh"))) tone = QStringLiteral("#D97706");
            name->setIcon(UIHelper::menuIcon(QStringLiteral("file"), QColor(tone), devicePixelRatioF()));
        }
        name->setData(Qt::UserRole,index); name->setToolTip(entry.relativePath); table->setItem(row,0,name);
        QString dir = QFileInfo(entry.relativePath).path(); if (dir == ".") dir = QStringLiteral("根目录");
        table->setItem(row,1,new QTableWidgetItem(dir));
        table->setItem(row,2,new QTableWidgetItem(entry.directory ? QStringLiteral("文件夹") : entry.size < 0 ? QStringLiteral("未知") : QLocale().formattedDataSize(entry.size)));
        const auto date = QDateTime::fromString(entry.modified,Qt::ISODateWithMs);
        table->setItem(row,3,new QTableWidgetItem(date.isValid() ? date.toLocalTime().toString("yyyy-MM-dd HH:mm") : QStringLiteral("未知")));
        for (int col=1;col<4;++col) table->item(row,col)->setToolTip(table->item(row,col)->text());
        if (m_selectedPaths.contains(entry.remotePath)) table->item(row,0)->setSelected(true);
    }
    table->blockSignals(false);
    updateCount();
}
void OpenListWindow::updateCount() {
    const auto filter = m_search->text().trimmed();
    if (m_table->rowCount() == 0) { m_count->setText(filter.isEmpty() ? QStringLiteral("暂无资源") : QStringLiteral("没有匹配的资源")); return; }
    if (m_selectedPaths.isEmpty()) m_count->setText(QStringLiteral("%1 项 · 选中文件夹后可安装其中 APK / ZIP / SH 或传输全部文件").arg(m_table->rowCount()));
    else m_count->setText(QStringLiteral("%1 项 · 已选 %2 项（切换目录后仍保留）").arg(m_table->rowCount()).arg(m_selectedPaths.size()));
}
void OpenListWindow::activate(int row) {
    if (m_busy || row < 0 || row >= m_table->rowCount()) return;
    const int index = m_table->item(row,0)->data(Qt::UserRole).toInt();
    if (index < 0 || index >= m_entries.size()) return;
    const auto entry = m_entries[index];
    if (entry.directory) {
        m_currentDirectory = entry.relativePath; m_search->clear(); render(); return;
    }
    startBatch({entry});
}
void OpenListWindow::appendSelection(const OpenList::Entry &selection, QList<OpenList::Entry> &expanded, QSet<QString> &paths) const {
    if (selection.directory) {
        const QString prefix = selection.relativePath + QStringLiteral("/");
        for (const auto &entry : m_entries) {
            if (!entry.directory && entry.relativePath.startsWith(prefix)) {
                if (!paths.contains(entry.remotePath)) { paths.insert(entry.remotePath); expanded.append(entry); }
            }
        }
        return;
    }
    if (!paths.contains(selection.remotePath)) { paths.insert(selection.remotePath); expanded.append(selection); }
}
void OpenListWindow::startSelection(bool transferOnly) {
    if (m_busy) return;
    QList<OpenList::Entry> selected;
    for (const auto &entry : m_entries)
        if (m_selectedPaths.contains(entry.remotePath)) selected.append(entry);
    if (selected.isEmpty()) log(QStringLiteral("请先选择文件或文件夹；选择会跨目录保留，按 Ctrl / Shift 可多选。"));
    else startBatch(selected,transferOnly);
}
void OpenListWindow::startBatch(const QList<OpenList::Entry> &entries,bool transferOnly) {
    if (m_busy || entries.isEmpty()) return;
    m_queue.clear();
    QSet<QString> paths;
    bool folderSelected = false;
    for (const auto &entry : entries) {
        folderSelected = folderSelected || entry.directory;
        appendSelection(entry, m_queue, paths);
    }
    if (!transferOnly) {
        QList<OpenList::Entry> supported;
        for (const auto &entry : m_queue) {
            const QString extension = QFileInfo(entry.name).suffix().toLower();
            if (extension == "apk" || extension == "zip" || extension == "sh") supported.append(entry);
            else log(QStringLiteral("已跳过不支持安装或执行的文件：%1；可使用“传输到手机”。").arg(entry.name));
        }
        m_queue = supported;
    }
    if (m_queue.isEmpty()) {
        if (folderSelected) log(transferOnly ? QStringLiteral("所选文件夹内没有文件。") :
            QStringLiteral("所选文件夹内没有可安装或执行的 APK、模块 ZIP 或 SH 文件。"));
        return;
    }
    m_transferOnly = transferOnly;
    m_downloaded.clear(); m_serial.clear(); m_queueIndex = 0; m_installIndex = 0;
    m_succeeded = 0; m_failed = 0; m_installingBatch = false; m_cancelled = false;
    setBusy(true);
    m_status->setText(QStringLiteral("正在检测已授权手机…（共 %1 个文件）").arg(m_queue.size()));
    m_installer->inspectDevices();
}
void OpenListWindow::advanceBatch() {
    if (!m_busy || m_downloading || m_installer->busy()) return;
    if (m_cancelled) {
        for (const auto &download : m_downloaded)
            log(QStringLiteral("已取消，下载文件保留在：%1").arg(download.file));
        finish(QStringLiteral("批量任务已取消，未开始手机操作；已完成的下载保留。")); return;
    }
    if (!m_installingBatch && m_queueIndex < m_queue.size()) {
        m_selected = m_queue[m_queueIndex++]; m_downloading = true; m_cancel->setEnabled(true);
        m_status->setText(QStringLiteral("正在准备下载（%1/%2）：%3").arg(m_queueIndex).arg(m_queue.size()).arg(m_selected.name));
        m_service->download(m_selected); return;
    }
    m_installingBatch = true; m_cancel->setEnabled(false);
    if (m_installIndex < m_downloaded.size()) {
        const auto download = m_downloaded[m_installIndex++];
        m_progress->setRange(0,0);
        m_status->setText((m_transferOnly ? QStringLiteral("正在传输（%1/%2）：%3；目标手机：%4") : (QFileInfo(download.file).suffix().compare("sh",Qt::CaseInsensitive) == 0 ? QStringLiteral("正在执行脚本（%1/%2）：%3；目标手机：%4") : QStringLiteral("正在安装（%1/%2）：%3；目标手机：%4")))
            .arg(m_installIndex).arg(m_downloaded.size()).arg(download.entry.name,m_serial));
        if (m_transferOnly) m_installer->transfer(download.file,m_serial);
        else m_installer->install(download.file,m_serial);
        return;
    }
    QString summary = QStringLiteral("批量任务完成：成功 %1，失败 %2，共 %3 个文件。")
        .arg(m_succeeded).arg(m_failed).arg(m_queue.size());
    finish(summary,m_failed == 0);
}
void OpenListWindow::setBusy(bool busy) {
    m_busy = busy; m_refresh->setEnabled(!busy && m_pending == 0); m_installSelected->setEnabled(!busy); m_transferSelected->setEnabled(!busy); m_installMt->setEnabled(!busy);
    m_table->setEnabled(!busy); m_search->setEnabled(!busy); m_up->setEnabled(!busy && !m_currentDirectory.isEmpty());
    m_cancel->setEnabled(busy && m_downloading);
}
void OpenListWindow::finish(const QString &message,bool ok) {
    m_selectedPaths.clear();
    m_table->clearSelection();
    m_downloading = false; m_queue.clear(); m_downloaded.clear(); setBusy(false); m_progress->setRange(0,100); m_progress->setValue(ok ? 100 : 0);
    m_status->setText(OpenList::redact(message)); log(message); updateDevice();
}
void OpenListWindow::log(const QString &text) {
    m_log->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss ") + OpenList::redact(text));
}
void OpenListWindow::updateDevice() {
    auto *devices = DeviceManager::instance();
    const bool targeting = m_busy && !m_serial.isEmpty();
    const bool adb = devices->currentMode() == DeviceManager::ADB;
    m_device->setText(targeting ? QStringLiteral("目标手机：%1").arg(m_serial) :
        adb ? QStringLiteral("ADB 已连接：%1").arg(devices->deviceSerial()) : QStringLiteral("未检测到已授权 ADB 手机"));
    // 预览稿状态 pill：琥珀=执行中 / 绿=已连接 / 灰=未连接。
    m_device->setStyleSheet(QStringLiteral(
        "border-radius:9px;padding:2px 9px;font-size:10.5px;color:#FFFFFF;background:%1;")
        .arg(targeting ? QStringLiteral("#F59E0B") : adb ? QStringLiteral("#22C55E") : QStringLiteral("#9AA8B5")));
}
void OpenListWindow::closeEvent(QCloseEvent *event) {
    // Openlist is a child tool window; closing it must not veto application shutdown.
    // Any active request keeps its own QObject alive until the application exits.
    QWidget::closeEvent(event);
}

void OpenListWindow::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    if (event->spontaneous()) return;
    QTimer::singleShot(0, this, [this] {
        if (!isVisible() || isMinimized() || isMaximized()) return;
        QScreen *target = m_launcher ? m_launcher->screen() : screen();
        if (!target) return;
        QRect frame = frameGeometry(); frame.moveCenter(target->availableGeometry().center());
        move(frame.topLeft());
    });
}

