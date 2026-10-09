#include "openlistwindow.h"
#include "openlistinstaller.h"
#include "devicemanager.h"
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
#include <QSplitter>
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
    connect(m_service,&OpenListService::listingReady,this,[this](bool module,const QList<OpenList::Entry> &entries,const QString &error) {
        m_entries[module] = entries; render(module);
        if (!error.isEmpty()) m_counts[module]->setText(error);
        if (!error.isEmpty()) log((module ? QStringLiteral("模块：") : QStringLiteral("软件：")) + error);
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
                if (m_mtTask && m_busy) m_installer->install(m_mtFile,false,m_serial);
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
    m_installSelected->setObjectName("openListInstallSelected"); top->addWidget(m_installSelected);
    m_transferSelected = new QPushButton(QStringLiteral("传输到手机"),this);
    m_transferSelected->setObjectName("openListTransferSelected"); top->addWidget(m_transferSelected);
    m_installMt = new QPushButton(QStringLiteral("安装 MT 管理器"),this); m_installMt->setObjectName("openListInstallMt"); top->addWidget(m_installMt);
    layout->addLayout(top);
    auto *split = new QSplitter(Qt::Horizontal,this); split->setChildrenCollapsible(false); layout->addWidget(split,3);
    for (int i = 0; i < 2; ++i) {
        auto *card = new QGroupBox(i ? QStringLiteral("模块 · Root 模块 ZIP") : QStringLiteral("软件 · Android APK"),split);
        auto *inner = new QVBoxLayout(card); inner->setContentsMargins(12,18,12,10);
        m_search[i] = new QLineEdit(card); m_search[i]->setPlaceholderText(QStringLiteral("搜索名称或子目录")); inner->addWidget(m_search[i]);
        auto *navigation = new QHBoxLayout;
        m_up[i] = new QPushButton(QStringLiteral("返回上级"), card);
        m_up[i]->setObjectName(i ? "openListModulesUp" : "openListAppsUp");
        m_directory[i] = new QLabel(QStringLiteral("根目录"), card);
        m_directory[i]->setWordWrap(true);
        navigation->addWidget(m_up[i]); navigation->addWidget(m_directory[i], 1); inner->addLayout(navigation);
        connect(m_up[i], &QPushButton::clicked, this, [this, i] {
            if (m_busy) return;
            const int slash = m_currentDirectory[i].lastIndexOf('/');
            m_currentDirectory[i] = slash < 0 ? QString() : m_currentDirectory[i].left(slash);
            m_search[i]->clear(); render(i);
        });
        auto *table = new QTableWidget(card); m_tables[i] = table;
        table->setObjectName(i ? "openListModules" : "openListApps");
        table->setColumnCount(4); table->setHorizontalHeaderLabels({QStringLiteral("名称"),QStringLiteral("目录"),QStringLiteral("大小"),QStringLiteral("更新时间")});
        table->setSelectionBehavior(QAbstractItemView::SelectRows); table->setSelectionMode(QAbstractItemView::ExtendedSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers); table->setAlternatingRowColors(true); table->setWordWrap(false);
        table->verticalHeader()->hide(); table->verticalHeader()->setDefaultSectionSize(32);
        table->horizontalHeader()->setSectionResizeMode(0,QHeaderView::Stretch);
        table->setColumnWidth(1,65); table->setColumnWidth(2,65); table->setColumnWidth(3,100);
        inner->addWidget(table,1); m_counts[i] = new QLabel(QStringLiteral("正在读取…"),card); inner->addWidget(m_counts[i]);
        connect(m_search[i],&QLineEdit::textChanged,this,[this,i] { render(i); });
        connect(table,&QTableWidget::cellDoubleClicked,this,[this,i](int row,int) { activate(i,row); });
    }
    m_status = new QLabel(QStringLiteral("Ctrl / Shift 多选文件或文件夹后安装或传输；传输至内部存储根目录，不安装"),this); m_status->setWordWrap(true); layout->addWidget(m_status);
    auto *progress = new QHBoxLayout; m_progress = new QProgressBar(this); m_progress->setValue(0); progress->addWidget(m_progress,1);
    m_cancel = new QPushButton(QStringLiteral("取消下载"),this); m_cancel->setEnabled(false); progress->addWidget(m_cancel); layout->addLayout(progress);
    auto *logs = new QGroupBox(QStringLiteral("操作日志"),this); auto *logsLayout = new QVBoxLayout(logs);
    m_log = new QPlainTextEdit(logs); m_log->setObjectName("openListLog"); m_log->setReadOnly(true); m_log->setMaximumBlockCount(500);
    logsLayout->addWidget(m_log); layout->addWidget(logs,1);
    setStyleSheet(R"(
        QWidget#openListWindow { background:#F3F6FA; }
        QGroupBox { background:#FFFFFF; border:1px solid #D9E2EC; border-radius:12px; margin-top:8px; color:#334155; font-weight:600; }
        QGroupBox::title { subcontrol-origin:margin; left:10px; padding:0 5px; background:#FFFFFF; }
        QLabel { color:#526579; }
        QPushButton { background:#E7EEF3; color:#526579; border:1px solid #CBD7E2; border-radius:8px; padding:7px 12px; }
        QPushButton:hover { background:#DCE7EF; border-color:#7892A8; }
        QPushButton:disabled { color:#A9B5C1; }
        QLineEdit { background:#FFFFFF; color:#526579; border:1px solid #D2DDE7; border-radius:6px; padding:7px; }
        QTableWidget { background:#FFFFFF; alternate-background-color:#F8FAFC; color:#334155; border:1px solid #DCE5ED; selection-background-color:#DCE7EF; selection-color:#334155; }
        QTableWidget::item { padding:0 4px; }
        QHeaderView::section { background:#EEF3F7; color:#526579; border:0; padding:6px; }
        QPlainTextEdit { background:#F8FAFC; color:#334155; border:0; font-family:"Microsoft YaHei UI","Consolas"; }
        QProgressBar { border:1px solid #CBD7E2; border-radius:6px; background:#FFFFFF; text-align:center; min-height:20px; }
        QProgressBar::chunk { background:#718CA4; }
    )");
}
void OpenListWindow::refresh() {
    if (m_busy) return;
    m_pending = 2; m_refresh->setEnabled(false); m_status->setText(QStringLiteral("正在读取资源…"));
    for (int i=0;i<2;++i) { m_currentDirectory[i].clear(); m_entries[i].clear(); render(i); m_counts[i]->setText(QStringLiteral("正在读取…")); }
    m_service->refresh();
}
void OpenListWindow::render(bool module) {
    auto *table = m_tables[module]; table->setSortingEnabled(false); table->setRowCount(0);
    const auto filter = m_search[module]->text().trimmed();
    m_up[module]->setEnabled(!m_busy && !m_currentDirectory[module].isEmpty());
    m_directory[module]->setText(m_currentDirectory[module].isEmpty() ? QStringLiteral("根目录") : m_currentDirectory[module]);
    for (int index=0;index<m_entries[module].size();++index) {
        const auto &entry = m_entries[module][index];
        const QString parent = entry.relativePath.contains('/') ? entry.relativePath.left(entry.relativePath.lastIndexOf('/')) : QString();
        if (filter.isEmpty() && parent != m_currentDirectory[module]) continue;
        if (!filter.isEmpty() && !entry.relativePath.contains(filter,Qt::CaseInsensitive)) continue;
        const int row = table->rowCount(); table->insertRow(row);
        auto *name = new QTableWidgetItem(entry.name);
        if (entry.directory) name->setIcon(style()->standardIcon(QStyle::SP_DirIcon)); name->setData(Qt::UserRole,index); name->setToolTip(entry.relativePath); table->setItem(row,0,name);
        QString dir = QFileInfo(entry.relativePath).path(); if (dir == ".") dir = QStringLiteral("根目录");
        table->setItem(row,1,new QTableWidgetItem(dir));
        table->setItem(row,2,new QTableWidgetItem(entry.directory ? QStringLiteral("文件夹") : entry.size < 0 ? QStringLiteral("未知") : QLocale().formattedDataSize(entry.size)));
        const auto date = QDateTime::fromString(entry.modified,Qt::ISODateWithMs);
        table->setItem(row,3,new QTableWidgetItem(date.isValid() ? date.toLocalTime().toString("yyyy-MM-dd HH:mm") : QStringLiteral("未知")));
        for (int col=1;col<4;++col) table->item(row,col)->setToolTip(table->item(row,col)->text());
    }
    if (table->rowCount() == 0) m_counts[module]->setText(filter.isEmpty() ? QStringLiteral("暂无资源") : QStringLiteral("没有匹配的资源"));
    else m_counts[module]->setText(QStringLiteral("%1 项 · 选中文件夹后可一键安装其中全部文件").arg(table->rowCount()));
}
void OpenListWindow::activate(bool module,int row) {
    if (m_busy || row < 0 || row >= m_tables[module]->rowCount()) return;
    const int index = m_tables[module]->item(row,0)->data(Qt::UserRole).toInt();
    if (index < 0 || index >= m_entries[module].size()) return;
    const auto entry = m_entries[module][index];
    if (entry.directory) {
        m_currentDirectory[module] = entry.relativePath; m_search[module]->clear(); render(module); return;
    }
    startBatch({entry});
}
void OpenListWindow::appendSelection(const OpenList::Entry &selection, QList<OpenList::Entry> &expanded, QSet<QString> &paths) const {
    const int category = selection.module ? 1 : 0;
    if (selection.directory) {
        const QString prefix = selection.relativePath + QStringLiteral("/");
        for (const auto &entry : m_entries[category]) {
            if (!entry.directory && entry.relativePath.startsWith(prefix)) {
                const QString key = QString::number(entry.module) + entry.remotePath;
                if (!paths.contains(key)) { paths.insert(key); expanded.append(entry); }
            }
        }
        return;
    }
    const QString key = QString::number(selection.module) + selection.remotePath;
    if (!paths.contains(key)) { paths.insert(key); expanded.append(selection); }
}
void OpenListWindow::startSelection(bool transferOnly) {
    if (m_busy) return;
    QList<OpenList::Entry> selected;
    for (int category = 0; category < 2; ++category) {
        for (int row = 0; row < m_tables[category]->rowCount(); ++row) {
            if (!m_tables[category]->item(row,0)->isSelected()) continue;
            const int index = m_tables[category]->item(row,0)->data(Qt::UserRole).toInt();
            if (index >= 0 && index < m_entries[category].size())
                selected.append(m_entries[category][index]);
        }
    }
    if (selected.isEmpty()) log(QStringLiteral("请先选择文件或文件夹；按 Ctrl / Shift 可多选。"));
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
    if (m_queue.isEmpty()) {
        if (folderSelected) log(QStringLiteral("所选文件夹内没有可安装的 APK 或模块文件。"));
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
        m_status->setText((m_transferOnly ? QStringLiteral("正在传输（%1/%2）：%3；目标手机：%4") : QStringLiteral("正在安装（%1/%2）：%3；目标手机：%4"))
            .arg(m_installIndex).arg(m_downloaded.size()).arg(download.entry.name,m_serial));
        if (m_transferOnly) m_installer->transfer(download.file,m_serial);
        else m_installer->install(download.file,download.entry.module,m_serial);
        return;
    }
    QString summary = QStringLiteral("批量任务完成：成功 %1，失败 %2，共 %3 个文件。")
        .arg(m_succeeded).arg(m_failed).arg(m_queue.size());
    finish(summary,m_failed == 0);
}
void OpenListWindow::setBusy(bool busy) {
    m_busy = busy; m_refresh->setEnabled(!busy && m_pending == 0); m_installSelected->setEnabled(!busy); m_transferSelected->setEnabled(!busy); m_installMt->setEnabled(!busy);
    for (int i=0;i<2;++i) { m_tables[i]->setEnabled(!busy); m_search[i]->setEnabled(!busy); m_up[i]->setEnabled(!busy && !m_currentDirectory[i].isEmpty()); }
    m_cancel->setEnabled(busy && m_downloading);
}
void OpenListWindow::finish(const QString &message,bool ok) {
    for (auto *table : m_tables) table->clearSelection();
    m_downloading = false; m_queue.clear(); m_downloaded.clear(); setBusy(false); m_progress->setRange(0,100); m_progress->setValue(ok ? 100 : 0);
    m_status->setText(OpenList::redact(message)); log(message); updateDevice();
}
void OpenListWindow::log(const QString &text) {
    m_log->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss ") + OpenList::redact(text));
}
void OpenListWindow::updateDevice() {
    auto *devices = DeviceManager::instance();
    m_device->setText(m_busy && !m_serial.isEmpty() ? QStringLiteral("目标手机：%1").arg(m_serial) :
        devices->currentMode() == DeviceManager::ADB ? QStringLiteral("ADB 已连接：%1").arg(devices->deviceSerial()) : QStringLiteral("未检测到已授权 ADB 手机"));
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

