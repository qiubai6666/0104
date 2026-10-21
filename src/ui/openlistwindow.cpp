#include "openlistwindow.h"
#include "openlistinstaller.h"
#include "devicemanager.h"
#include "uihelper.h"
#include <QCloseEvent>
#include <QResizeEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QScreen>
#include <QShowEvent>
#include <QShortcut>
#include <QStyle>
#include <QDateTime>
#include <QDir>
#include <QTime>
#include <QFileInfo>
#include <QFrame>
#include <QGroupBox>
#include <QHash>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QScrollArea>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QTimer>
#include <functional>

namespace {
QString relativeParent(const QString &path) {
    const int slash = path.lastIndexOf('/');
    return slash < 0 ? QString() : path.left(slash);
}
// 本地路径键：Windows 不区分大小写，避免同一文件因大小写不同被重复下载。
QString localPathKey(const QString &path) {
    const QString clean = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
#ifdef Q_OS_WIN
    return clean.toCaseFolded();
#else
    return clean;
#endif
}
// 日志分级：WARN 命中失败/取消类关键词，OK 命中成功/完成，其余 INFO。
int classifyLog(const QString &text) {
    if (text.contains(QStringLiteral("失败")) || text.contains(QStringLiteral("错误")) || text.contains(QStringLiteral("取消"))
        || text.contains(QStringLiteral("跳过")) || text.contains(QStringLiteral("无法")) || text.contains(QStringLiteral("不支持"))
        || text.contains(QStringLiteral("未选择")) || text.contains(QStringLiteral("未开始")) || text.contains(QStringLiteral("未检测")))
        return 2;
    if (text.contains(QStringLiteral("成功")) || text.contains(QStringLiteral("完成"))) return 1;
    return 0;
}
void stylePill(QLabel *pill, const QString &text, const QString &fg, const QString &bg, const QString &border) {
    pill->setText(text);
    pill->setStyleSheet(QStringLiteral("color:%1;background:%2;border:1px solid %3;border-radius:11px;padding:0 9px;")
                            .arg(fg, bg, border));
}
// 点击语义：普通单击只保留当前文件；只有按住 Ctrl 才切换多选。
// Shift 不参与范围选择，避免出现未按 Ctrl 的多选。
class OpenListFilesTable final : public QTableWidget {
public:
    explicit OpenListFilesTable(QWidget *parent = nullptr) : QTableWidget(parent) {}
    std::function<void()> selectionWillBeReplaced;
protected:
    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() != Qt::LeftButton) { QTableWidget::mousePressEvent(event); return; }
        const bool extend = event->modifiers().testFlag(Qt::ControlModifier);
        const QModelIndex index = indexAt(event->position().toPoint());
        auto *nameItem = index.isValid() ? item(index.row(), 0) : nullptr;
        // 文件夹行不参与选择：双击进入目录时不要清空已选资源。
        if (!extend && nameItem && nameItem->flags().testFlag(Qt::ItemIsSelectable)) {
            if (selectionWillBeReplaced) selectionWillBeReplaced();
            clearSelection();
        }
        if (event->modifiers() != (extend ? Qt::ControlModifier : Qt::NoModifier)) {
            QMouseEvent normalized(event->type(), event->position(), event->globalPosition(),
                                   event->button(), event->buttons(),
                                   extend ? Qt::ControlModifier : Qt::NoModifier, event->pointingDevice());
            QTableWidget::mousePressEvent(&normalized);
            return;
        }
        QTableWidget::mousePressEvent(event);
    }
    void keyPressEvent(QKeyEvent *event) override {
        const bool control = event->modifiers().testFlag(Qt::ControlModifier);
        // 空格在没有 Ctrl 时只切换当前文件，不允许累积多选；Ctrl+空格保留切换语义。
        if (event->key() == Qt::Key_Space && !control) {
            const int row = currentRow();
            auto *nameItem = row >= 0 ? item(row, 0) : nullptr;
            if (nameItem && nameItem->flags().testFlag(Qt::ItemIsSelectable)) {
                const bool wasSelected = nameItem->isSelected();
                if (selectionWillBeReplaced) selectionWillBeReplaced();
                clearSelection();
                if (!wasSelected) selectRow(row);
                event->accept();
                return;
            }
        }
        // Shift+方向键不扩展范围；只有 Ctrl 参与多选。
        if (!control && event->modifiers().testFlag(Qt::ShiftModifier)) {
            QKeyEvent normalized(event->type(), event->key(), event->modifiers() & ~Qt::ShiftModifier,
                                 event->text(), event->isAutoRepeat(), event->count());
            QTableWidget::keyPressEvent(&normalized);
            return;
        }
        QTableWidget::keyPressEvent(event);
    }
};
}

OpenListWindow::OpenListWindow(QWidget *parent, OpenListService *service, OpenListInstaller *installer,
                               const QString &desktopRoot)
    : QWidget(nullptr), m_launcher(parent),
      m_service(service ? service : new OpenListService(this, nullptr, QString(), OpenList::defaultListingCacheFile())),
      m_installer(installer ? installer : new OpenListInstaller(this)), m_mtService(new MtManagerService(this)),
      m_desktopDirectory(OpenList::desktopTransferDirectory(desktopRoot)) {
    if (parent) connect(parent, &QObject::destroyed, this, &QObject::deleteLater);
    setWindowTitle(QStringLiteral("Openlist"));
    setAttribute(Qt::WA_DeleteOnClose,false); setAttribute(Qt::WA_QuitOnClose,false);
    m_renderTimer = new QTimer(this);
    m_renderTimer->setSingleShot(true);
    m_renderTimer->setInterval(80);
    connect(m_renderTimer, &QTimer::timeout, this, [this] { render(true); });
    setupUi();
    connect(m_refresh,&QPushButton::clicked,this,&OpenListWindow::refresh);
    connect(m_cancel,&QPushButton::clicked,this,[this] {
        if (!m_busy || m_installingBatch) return;
        m_cancelled = true;
        if (m_downloading) m_service->cancel();
        else if (m_mtTask) m_mtService->cancel();
        else advanceBatch();
    });
    connect(m_installSelected,&QPushButton::clicked,this,[this] { startSelection(BatchMode::Install); });
    connect(m_transferSelected,&QPushButton::clicked,this,[this] { startSelection(BatchMode::PhoneTransfer); });
    connect(m_transferDesktop,&QPushButton::clicked,this,[this] { startSelection(BatchMode::DesktopTransfer); });
    connect(m_installMt,&QPushButton::clicked,this,[this] {
        if (m_busy) return;
        m_mtTask = true; m_mtFile.clear(); setBusy(true);
        m_runTile->setPixmap(tilePixmap("zap", QColor("#D08115"), QColor("#FFFAF0"), QColor("#F7E7C7"), 35, 11));
        m_runFileName->setText(QStringLiteral("MT 管理器"));
        m_runFileSub->setText(QStringLiteral("正在获取最新版本…"));
        m_status->setText(QStringLiteral("正在获取 MT 管理器最新版本…"));
        m_mtService->start();
    });
    connect(m_mtService,&MtManagerService::progress,this,[this](qint64 received,qint64 total,double speed) {
        if (!m_mtTask) return;
        updateRunProgress(received, total, speed, QStringLiteral("正在下载 MT 管理器 · %1 / %2 · %3/s")
            .arg(QLocale().formattedDataSize(received), total>0?QLocale().formattedDataSize(total):QStringLiteral("未知"),
                 QLocale().formattedDataSize(qint64(speed))));
    });
    connect(m_mtService,&MtManagerService::log,this,&OpenListWindow::log);
    connect(m_mtService,&MtManagerService::failed,this,[this](const QString &message) { if (!m_mtTask) return; m_mtTask=false; finish(message,false); });
    connect(m_mtService,&MtManagerService::ready,this,[this](const QString &version,const QString &file) {
        if (!m_mtTask) return;
        m_progress->setRange(0,100); m_progress->setValue(100); m_mtFile=file;
        m_runPercent->setText(QStringLiteral("100%"));
        m_runFileSub->setText(QStringLiteral("下载完成，正在检测手机…"));
        log(QStringLiteral("MT 管理器下载完成（版本 %1），正在检测手机…").arg(version));
        m_installer->inspectDevices();
    });
    connect(m_service,&OpenListService::listingCached,this,[this](const QList<OpenList::Entry> &entries,const QDateTime &savedAt) {
        m_renderTimer->stop();
        setListingEntries(entries); m_listingComplete = true; m_hasCachedSnapshot = true; render();
        m_updatedAt->setText(QStringLiteral("缓存于 %1").arg(savedAt.toLocalTime().toString("MM-dd HH:mm")));
        if (!m_busy) m_status->setText(QStringLiteral("已显示缓存 · 正在更新…"));
    });
    connect(m_service,&OpenListService::listingUpdated,this,[this](const QList<OpenList::Entry> &entries) {
        if (appendListingEntries(entries)) m_renderTimer->start();
        if (!m_busy && !m_listingComplete) m_status->setText(QStringLiteral("正在加载资源…"));
    });
    connect(m_service,&OpenListService::listingReady,this,[this](const QList<OpenList::Entry> &entries,const QString &error) {
        // 刷新失败时保留已有缓存行；服务层已尽量把缓存与本次结果合并。
        m_renderTimer->stop();
        if (error.isEmpty() || !entries.isEmpty() || m_entries.isEmpty()) setListingEntries(entries);
        m_listingComplete = error.isEmpty();
        if (m_listingComplete) pruneSelection();
        render(true);
        if (!error.isEmpty()) { m_count->setText(error); log(error); }
        else m_updatedAt->setText(QStringLiteral("更新于 %1").arg(QTime::currentTime().toString("HH:mm")));
        if (m_pending > 0 && --m_pending == 0 && !m_busy) {
            if (!error.isEmpty()) {
                m_status->setText(m_hasCachedSnapshot ? QStringLiteral("已显示缓存 · 更新失败")
                                                      : QStringLiteral("资源读取失败，可稍后重试"));
            } else {
                m_status->setText(QStringLiteral("资源已就绪 · 单击单选，按住 Ctrl 多选，双击文件直接安装"));
            }
            m_refresh->setEnabled(true);
        }
    });
    connect(m_service,&OpenListService::progress,this,[this](qint64 received,qint64 total,double speed) {
        if (m_batchMode == BatchMode::DesktopTransfer)
            updateRunProgress(received, total, speed, QStringLiteral("正在下载到电脑桌面（%1/%2）：%3")
                .arg(m_queueIndex).arg(m_queue.size()).arg(m_selected.name));
        else
            updateRunProgress(received, total, speed, QStringLiteral("正在下载 %1（%2/%3）")
                .arg(m_selected.name).arg(m_queueIndex).arg(m_queue.size()));
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
        m_usedLocalFiles.insert(localPathKey(file));
        m_downloaded.append({entry,file});
        if (m_batchMode == BatchMode::DesktopTransfer)
            log(QStringLiteral("已传输到电脑桌面：%1").arg(QFileInfo(file).fileName()));
        else
            log(QStringLiteral("下载完成（%1/%2）：%3").arg(m_queueIndex).arg(m_queue.size()).arg(entry.name));
        QTimer::singleShot(0,this,&OpenListWindow::advanceBatch);
    });
    connect(m_installer,&OpenListInstaller::devicesReady,this,[this](const QStringList &serials) {
        if (!m_busy || serials.isEmpty()) return;
        if (m_mtTask) {
            bool accepted = true; m_serial = serials.size() == 1 ? serials.first() : QInputDialog::getItem(this,QStringLiteral("选择目标手机"),QStringLiteral("已连接多台设备，请选择目标手机："),serials,0,false,&accepted);
            if (!accepted || m_serial.isEmpty()) { m_mtTask=false; finish(QStringLiteral("未选择手机，未开始安装。"),false); return; }
            updateDevice();
            m_runFileSub->setText(QStringLiteral("目标手机 %1").arg(m_serial));
            QTimer::singleShot(0, this, [this] {
                if (m_mtTask && m_busy) m_installer->install(m_mtFile,m_serial);
            });
            return;
        }
        bool accepted = true;
        m_serial = serials.size() == 1 ? serials.first() : QInputDialog::getItem(this,QStringLiteral("选择目标手机"),QStringLiteral("已连接多台设备，请选择目标手机："),serials,0,false,&accepted);
        if (!accepted || m_serial.isEmpty()) { finish(QStringLiteral("未选择手机，未开始任务。"),false); return; }
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
            m_mtTask=false; m_mtFile.clear(); finish(result,ok); return;
        }
        if (!m_installingBatch) { finish(message,ok); return; }
        if (ok) ++m_succeeded; else ++m_failed;
        log(message);
        QTimer::singleShot(0,this,&OpenListWindow::advanceBatch);
    });
    connect(m_installer,&OpenListInstaller::log,this,&OpenListWindow::log);
    connect(DeviceManager::instance(),&DeviceManager::deviceModeChanged,this,[this] { updateDevice(); });
    connect(DeviceManager::instance(),&DeviceManager::deviceInfoUpdated,this,[this] { updateDevice(); });
    DeviceManager::instance()->ensureAdbOnlyMonitoring();
    connect(this,&QObject::destroyed,DeviceManager::instance(),[] { DeviceManager::instance()->releaseAdbOnlyMonitoring(); });
    updateDevice(); QTimer::singleShot(0,this,&OpenListWindow::beginListing);
}
void OpenListWindow::setupUi() {
    setObjectName("openListWindow");
    // 与欧加线刷 / 小米线刷窗口保持一致的打开尺寸。
    const QSize initialSize(qRound(866 * 0.9), qRound(729 * 0.9));
    setMinimumSize(initialSize); resize(initialSize);
    QFont font(QStringLiteral("Microsoft YaHei UI")); font.setPixelSize(12); setFont(font);

    auto *rootLayout = new QVBoxLayout(this); rootLayout->setContentsMargins(0,0,0,0); rootLayout->setSpacing(0);

    // ---------- 顶栏 ----------
    auto *header = new QFrame(this); header->setObjectName("openListHeader"); header->setFixedHeight(76);
    auto *headerLayout = new QHBoxLayout(header); headerLayout->setContentsMargins(24,0,22,0); headerLayout->setSpacing(14);
    auto *brandMark = new QLabel(header); brandMark->setObjectName("openListBrandMark");
    brandMark->setFixedSize(42,42); brandMark->setAlignment(Qt::AlignCenter);
    brandMark->setPixmap(glyphPixmap("cloud", QColor("#FFFFFF"), 22));
    auto *brandCopy = new QVBoxLayout; brandCopy->setContentsMargins(0,0,0,0); brandCopy->setSpacing(3);
    auto *brandLine = new QHBoxLayout; brandLine->setContentsMargins(0,0,0,0); brandLine->setSpacing(7);
    auto *brandTitle = new QLabel(QStringLiteral("Openlist"), header);
    brandTitle->setStyleSheet("font-size:19px;font-weight:700;color:#192B40;");
    auto *brandTag = new QLabel(QStringLiteral("CLOUD"), header); brandTag->setObjectName("openListBrandTag");
    auto *brandSub = new QLabel(QStringLiteral("秋白工作室 · 云端资源"), header);
    brandSub->setStyleSheet("font-size:11px;color:#8A9AAC;");
    brandLine->addWidget(brandTitle); brandLine->addWidget(brandTag); brandLine->addStretch();
    brandCopy->addLayout(brandLine); brandCopy->addWidget(brandSub);
    auto *devicePill = new QFrame(header); devicePill->setObjectName("openListDevicePill"); devicePill->setFixedHeight(44);
    auto *deviceLayout = new QHBoxLayout(devicePill); deviceLayout->setContentsMargins(6,5,12,5); deviceLayout->setSpacing(9);
    auto *avatar = new QLabel(devicePill); avatar->setObjectName("openListDeviceAvatar");
    avatar->setFixedSize(32,32); avatar->setAlignment(Qt::AlignCenter);
    avatar->setPixmap(glyphPixmap("smartphone", QColor("#3F84DC"), 16));
    m_deviceDot = new QLabel(avatar); m_deviceDot->setObjectName("openListDeviceDot"); m_deviceDot->setGeometry(23,23,8,8);
    auto *deviceCopy = new QVBoxLayout; deviceCopy->setContentsMargins(0,0,0,0); deviceCopy->setSpacing(2);
    m_deviceName = new QLabel(devicePill); m_deviceName->setStyleSheet("font-size:11.5px;font-weight:600;color:#40556C;");
    m_deviceState = new QLabel(devicePill);
    deviceCopy->addWidget(m_deviceName); deviceCopy->addWidget(m_deviceState);
    deviceLayout->addWidget(avatar); deviceLayout->addLayout(deviceCopy);
    m_refresh = new QPushButton(QStringLiteral("刷新"), header); m_refresh->setObjectName("openListRefresh");
    m_refresh->setIcon(QIcon(glyphPixmap("refresh", QColor("#556A80"), 14)));
    m_refresh->setIconSize(QSize(14,14)); m_refresh->setCursor(Qt::PointingHandCursor);
    m_refresh->setToolTip(QStringLiteral("重新读取云端资源"));
    headerLayout->addWidget(brandMark); headerLayout->addLayout(brandCopy);
    headerLayout->addStretch(); headerLayout->addWidget(devicePill); headerLayout->addWidget(m_refresh);
    rootLayout->addWidget(header);

    // ---------- 主体 ----------
    auto *body = new QWidget(this);
    auto *bodyLayout = new QHBoxLayout(body); bodyLayout->setContentsMargins(18,14,18,14); bodyLayout->setSpacing(14);

    auto *card = new QGroupBox(QStringLiteral("秋白工作室 · 资源列表"), this);
    card->setObjectName("openListFilesCard");
    auto *cardLayout = new QVBoxLayout(card); cardLayout->setContentsMargins(1,1,1,1); cardLayout->setSpacing(0);
    auto *head = new QFrame(card); head->setObjectName("openListBrowserHead");
    auto *headLayout = new QVBoxLayout(head); headLayout->setContentsMargins(13,12,13,10); headLayout->setSpacing(10);
    auto *crumbRow = new QHBoxLayout; crumbRow->setContentsMargins(0,0,0,0); crumbRow->setSpacing(10);
    m_up = new QPushButton(head); m_up->setObjectName("openListUp");
    m_up->setFixedSize(28,28); m_up->setIconSize(QSize(14,14));
    m_up->setIcon(QIcon(glyphPixmap("chevron-left", QColor("#6E8297"), 14)));
    m_up->setToolTip(QStringLiteral("返回上级目录")); m_up->setCursor(Qt::PointingHandCursor);
    m_directory = new QLabel(QStringLiteral("秋白工作室"), head);
    m_directory->setStyleSheet("font-size:12.5px;font-weight:600;color:#2A3C51;");
    m_pill = new QLabel(QStringLiteral("—"), head); m_pill->setObjectName("openListCountPill");
    crumbRow->addWidget(m_up); crumbRow->addWidget(m_directory); crumbRow->addStretch(); crumbRow->addWidget(m_pill);
    auto *searchBox = new QFrame(head); searchBox->setObjectName("openListSearchBox"); searchBox->setFixedHeight(38);
    auto *searchLayout = new QHBoxLayout(searchBox); searchLayout->setContentsMargins(12,0,9,0); searchLayout->setSpacing(8);
    auto *searchIcon = new QLabel(searchBox); searchIcon->setFixedSize(15,15);
    searchIcon->setPixmap(glyphPixmap("search", QColor("#9AABBE"), 15));
    searchIcon->setStyleSheet("background:transparent;border:0;");
    m_search = new QLineEdit(searchBox); m_search->setObjectName("openListSearch");
    m_search->setPlaceholderText(QStringLiteral("搜索名称或子目录…")); m_search->setFrame(false);
    auto *searchKbd = new QLabel(QStringLiteral("/"), searchBox); searchKbd->setObjectName("openListSearchKbd");
    searchKbd->setFixedSize(18,18); searchKbd->setAlignment(Qt::AlignCenter);
    searchLayout->addWidget(searchIcon); searchLayout->addWidget(m_search,1); searchLayout->addWidget(searchKbd);
    headLayout->addLayout(crumbRow); headLayout->addWidget(searchBox);
    cardLayout->addWidget(head);

    auto *caption = new QWidget(card);
    auto *captionLayout = new QHBoxLayout(caption); captionLayout->setContentsMargins(50,7,16,3); captionLayout->setSpacing(8);
    auto *captionName = new QLabel(QStringLiteral("资源名称"), caption);
    captionName->setStyleSheet("font-size:9.5px;font-weight:600;color:#9BABBD;");
    auto *captionMeta = new QLabel(QStringLiteral("大小 / 更新"), caption);
    captionMeta->setStyleSheet("font-size:9.5px;font-weight:600;color:#9BABBD;");
    m_selectAll = new QPushButton(QStringLiteral("全选本页"), caption); m_selectAll->setObjectName("openListSelectAll");
    m_selectAll->setCheckable(true); m_selectAll->setCursor(Qt::PointingHandCursor);
    captionLayout->addWidget(captionName); captionLayout->addStretch();
    captionLayout->addWidget(captionMeta); captionLayout->addWidget(m_selectAll);
    cardLayout->addWidget(caption);

    auto *table = new OpenListFilesTable(card); m_table = table;
    // 未按 Ctrl 的普通单击只保留当前文件；勾选顺序在选中状态变化时记录。
    table->selectionWillBeReplaced = [this] {
        m_selectedPaths.clear();
        m_selectionOrder.clear();
    };
    table->setObjectName("openListFiles");
    // 与旧版一致的 4 列：名称（类型图标 + 原生复选框）/ 目录 / 大小 / 更新。
    table->setColumnCount(4);
    table->setHorizontalHeaderLabels({QStringLiteral("名称"),QStringLiteral("目录"),QStringLiteral("大小"),QStringLiteral("更新")});
    table->horizontalHeader()->hide(); table->verticalHeader()->hide();
    table->verticalHeader()->setDefaultSectionSize(44);
    table->setSelectionBehavior(QAbstractItemView::SelectRows); table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers); table->setWordWrap(false);
    table->setShowGrid(false); table->setFrameShape(QFrame::NoFrame);
    table->setIconSize(QSize(20,20));
    table->horizontalHeader()->setSectionResizeMode(0,QHeaderView::Stretch);
    table->setColumnWidth(1,96); table->setColumnWidth(2,64); table->setColumnWidth(3,88);
    cardLayout->addWidget(table,1);

    auto *cardFooter = new QFrame(card); cardFooter->setObjectName("openListCardFooter"); cardFooter->setFixedHeight(38);
    auto *cardFooterLayout = new QHBoxLayout(cardFooter); cardFooterLayout->setContentsMargins(16,0,16,0); cardFooterLayout->setSpacing(8);
    m_count = new QLabel(QStringLiteral("正在读取…"), cardFooter); m_count->setStyleSheet("font-size:10px;color:#93A4B6;");
    cardFooterLayout->addWidget(m_count); cardFooterLayout->addStretch();
    cardLayout->addWidget(cardFooter);

    connect(m_search,&QLineEdit::textChanged,this,[this,searchKbd] { searchKbd->setVisible(m_search->text().isEmpty()); render(); });
    connect(table,&QTableWidget::cellDoubleClicked,this,[this](int row,int) { activate(row); });
    // 选中状态以 remotePath 集合为准：切换目录 / 搜索后重新渲染时回显，跨目录保留。
    connect(table,&QTableWidget::itemSelectionChanged,this,[this] {
        for (int row = 0; row < m_table->rowCount(); ++row) {
            const auto *item = m_table->item(row,0);
            if (!item) continue;
            const int index = item->data(Qt::UserRole).toInt();
            if (index < 0 || index >= m_entries.size()) continue;
            if (m_entries[index].directory) continue; // 文件夹不参与选中，避免双击进入被当作选择文件。
            const QString &path = m_entries[index].remotePath;
            if (item->isSelected()) markSelected(path);
            else markDeselected(path);
        }
        syncChecks(); updateCount();
    });
    // 点击原生复选框：把勾选状态同步为该行的选中状态（选中集合仍是唯一真源）。
    connect(table,&QTableWidget::itemChanged,this,[this](QTableWidgetItem *item) {
        if (!item || item->column() != 0) return;
        const int index = item->data(Qt::UserRole).toInt();
        if (index < 0 || index >= m_entries.size()) return;
        if (m_entries[index].directory) return;
        const bool checked = item->checkState() == Qt::Checked;
        if (checked != item->isSelected()) {
            m_table->blockSignals(true);
            item->setSelected(checked);
            m_table->blockSignals(false);
            if (checked) markSelected(m_entries[index].remotePath);
            else markDeselected(m_entries[index].remotePath);
            updateCount(); updateSelectionPanel();
        }
    });
    connect(m_up, &QPushButton::clicked, this, [this] {
        if (m_busy) return;
        const int slash = m_currentDirectory.lastIndexOf('/');
        m_currentDirectory = slash > 0 ? m_currentDirectory.left(slash) : QString();
        m_search->clear(); render();
    });
    connect(m_selectAll,&QPushButton::clicked,this,[this] {
        if (m_selectAll->isChecked()) m_table->selectAll(); else m_table->clearSelection();
    });
    bodyLayout->addWidget(card,1);

    // ---------- 右侧任务面板 ----------
    auto *panel = new QFrame(this); panel->setObjectName("openListTaskPanel"); panel->setFixedWidth(280);
    auto *panelLayout = new QVBoxLayout(panel); panelLayout->setContentsMargins(15,14,15,12); panelLayout->setSpacing(0);
    auto *taskHead = new QHBoxLayout; taskHead->setContentsMargins(0,0,0,0); taskHead->setSpacing(8);
    auto *taskTitleBox = new QVBoxLayout; taskTitleBox->setContentsMargins(0,0,0,0); taskTitleBox->setSpacing(3);
    auto *taskOverline = new QLabel(QStringLiteral("当前任务"), panel);
    taskOverline->setStyleSheet("font-size:9px;font-weight:700;color:#95A7BA;");
    auto *taskTitle = new QLabel(QStringLiteral("安装任务"), panel);
    taskTitle->setStyleSheet("font-size:13px;font-weight:600;color:#26394F;");
    taskTitleBox->addWidget(taskOverline); taskTitleBox->addWidget(taskTitle);
    m_statusPill = new QLabel(panel); m_statusPill->setObjectName("openListStatusPill"); m_statusPill->setFixedHeight(23);
    m_statusPill->setAlignment(Qt::AlignCenter);
    stylePill(m_statusPill, QStringLiteral("● 就绪"), "#3D9666", "#EFFAF4", "#DCF3E6");
    taskHead->addLayout(taskTitleBox); taskHead->addStretch(); taskHead->addWidget(m_statusPill,0,Qt::AlignTop);
    panelLayout->addLayout(taskHead);

    // 空闲视图：已选资源 + 目标设备 + 主按钮。
    m_idleView = new QWidget(panel);
    auto *idleLayout = new QVBoxLayout(m_idleView); idleLayout->setContentsMargins(0,0,0,0); idleLayout->setSpacing(0);
    auto *selectionHead = new QHBoxLayout; selectionHead->setContentsMargins(0,6,0,0); selectionHead->setSpacing(7);
    auto *selectionLabel = new QLabel(QStringLiteral("已选资源"), m_idleView);
    selectionLabel->setStyleSheet("font-size:9.5px;font-weight:700;color:#8295A9;");
    m_selectionMeta = new QLabel(m_idleView); m_selectionMeta->setStyleSheet("font-size:9.5px;color:#A0AFBE;");
    m_clearSelection = new QPushButton(QStringLiteral("清空"), m_idleView); m_clearSelection->setObjectName("openListClearSelection");
    m_clearSelection->setCursor(Qt::PointingHandCursor);
    selectionHead->addWidget(selectionLabel); selectionHead->addWidget(m_selectionMeta,1); selectionHead->addWidget(m_clearSelection);
    idleLayout->addLayout(selectionHead);
    m_selectionList = new QWidget;
    auto *selectionListLayout = new QVBoxLayout(m_selectionList); selectionListLayout->setContentsMargins(0,5,0,0); selectionListLayout->setSpacing(4);
    // 已选列表放滚动区：高度随内容自适应（最多 5 行可见），选择再多也不截断、不重叠。
    m_selectionScroll = new QScrollArea(m_idleView);
    m_selectionScroll->setObjectName("openListSelectionScroll");
    m_selectionScroll->setWidgetResizable(true);
    m_selectionScroll->setFrameShape(QFrame::NoFrame);
    m_selectionScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_selectionScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_selectionScroll->setFocusPolicy(Qt::NoFocus);
    m_selectionScroll->setMinimumHeight(1); // 抵消 QScrollArea 默认 minimumSizeHint，避免抬高窗口最小高度。
    m_selectionScroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_selectionScroll->setMaximumHeight(0);
    m_selectionScroll->setWidget(m_selectionList);
    idleLayout->addWidget(m_selectionScroll, 1);
    m_selectionEmpty = new QFrame(m_idleView); m_selectionEmpty->setObjectName("openListSelectionEmpty");
    auto *selectionEmptyLayout = new QHBoxLayout(m_selectionEmpty); selectionEmptyLayout->setContentsMargins(9,6,9,6); selectionEmptyLayout->setSpacing(9);
    auto *emptyOrb = new QLabel(m_selectionEmpty); emptyOrb->setFixedSize(26,26); emptyOrb->setAlignment(Qt::AlignCenter);
    emptyOrb->setStyleSheet("background:#F0F8FF;border:1px dashed #CFE4FA;border-radius:8px;");
    emptyOrb->setPixmap(glyphPixmap("folder", QColor("#77A9DF"), 14));
    auto *emptyCopy = new QVBoxLayout; emptyCopy->setContentsMargins(0,0,0,0); emptyCopy->setSpacing(2);
    auto *emptyTitle = new QLabel(QStringLiteral("尚未选择文件"), m_selectionEmpty);
    emptyTitle->setStyleSheet("font-size:10px;font-weight:600;color:#71869B;");
    auto *emptyHint = new QLabel(QStringLiteral("单击文件行勾选，按住 Ctrl 点击可多选"), m_selectionEmpty);
    emptyHint->setStyleSheet("font-size:9px;color:#A0AFBE;"); emptyHint->setWordWrap(true);
    emptyCopy->addWidget(emptyTitle); emptyCopy->addWidget(emptyHint);
    selectionEmptyLayout->addWidget(emptyOrb); selectionEmptyLayout->addLayout(emptyCopy,1);
    auto *emptyHolder = new QVBoxLayout; emptyHolder->setContentsMargins(0,7,0,0); emptyHolder->addWidget(m_selectionEmpty);
    idleLayout->addLayout(emptyHolder);

    auto *targetStrip = new QFrame(m_idleView); targetStrip->setObjectName("openListTargetStrip");
    targetStrip->setFixedHeight(40);
    auto *targetLayout = new QHBoxLayout(targetStrip); targetLayout->setContentsMargins(8,5,8,5); targetLayout->setSpacing(8);
    auto *targetIcon = new QLabel(targetStrip); targetIcon->setFixedSize(28,28); targetIcon->setAlignment(Qt::AlignCenter);
    targetIcon->setPixmap(tilePixmap("smartphone", QColor("#4C8EDC"), QColor("#FFFFFF"), QColor("#E2EEFB"), 28, 8));
    auto *targetCopy = new QVBoxLayout; targetCopy->setContentsMargins(0,0,0,0); targetCopy->setSpacing(1);
    auto *targetLabel = new QLabel(QStringLiteral("目标设备"), targetStrip);
    targetLabel->setStyleSheet("font-size:9px;color:#99A9B9;");
    m_targetName = new QLabel(targetStrip); m_targetName->setStyleSheet("font-size:10.5px;font-weight:600;color:#50667C;");
    targetCopy->addWidget(targetLabel); targetCopy->addWidget(m_targetName);
    m_targetState = new QLabel(targetStrip);
    targetLayout->addWidget(targetIcon); targetLayout->addLayout(targetCopy);
    targetLayout->addStretch(); targetLayout->addWidget(m_targetState);
    auto *targetHolder = new QVBoxLayout; targetHolder->setContentsMargins(0,7,0,0); targetHolder->addWidget(targetStrip);
    idleLayout->addLayout(targetHolder);

    m_installSelected = new QPushButton(QStringLiteral("安装选中"), m_idleView);
    m_installSelected->setObjectName("openListInstallSelected"); m_installSelected->setCursor(Qt::PointingHandCursor);
    m_installSelected->setToolTip(QStringLiteral("按勾选顺序安装 APK、模块 ZIP；SH 以 Root 执行，请仅运行信任的脚本。"));
    m_transferSelected = new QPushButton(QStringLiteral("传输到手机"), m_idleView);
    m_transferSelected->setObjectName("openListTransferSelected"); m_transferSelected->setCursor(Qt::PointingHandCursor);
    m_transferSelected->setToolTip(QStringLiteral("下载选中文件并传输到手机内部存储。"));
    m_transferDesktop = new QPushButton(QStringLiteral("传输到桌面"), m_idleView);
    m_transferDesktop->setObjectName("openListTransferDesktop"); m_transferDesktop->setCursor(Qt::PointingHandCursor);
    m_transferDesktop->setToolTip(QStringLiteral("按勾选顺序下载选中文件到电脑桌面，不需要连接手机。"));
    idleLayout->addSpacing(8); idleLayout->addWidget(m_installSelected);
    auto *transferRow = new QHBoxLayout; transferRow->setContentsMargins(0,0,0,0); transferRow->setSpacing(6);
    transferRow->addWidget(m_transferSelected); transferRow->addWidget(m_transferDesktop);
    idleLayout->addSpacing(6); idleLayout->addLayout(transferRow);
    auto *actionHint = new QLabel(QStringLiteral("APK / ZIP 直接安装；SH 以 Root 执行，仅运行信任的脚本"), m_idleView);
    actionHint->setStyleSheet("font-size:9px;color:#9EACBB;");
    auto *hintHolder = new QVBoxLayout; hintHolder->setContentsMargins(0,5,0,0); hintHolder->addWidget(actionHint);
    idleLayout->addLayout(hintHolder);
    panelLayout->addWidget(m_idleView, 1);

    // 执行视图：当前文件 + 进度 + 队列 + 取消。
    m_runView = new QWidget(panel); m_runView->setVisible(false);
    auto *runLayout = new QVBoxLayout(m_runView); runLayout->setContentsMargins(0,14,0,0); runLayout->setSpacing(0);
    auto *runEyebrow = new QHBoxLayout; runEyebrow->setContentsMargins(0,0,0,0); runEyebrow->setSpacing(8);
    auto *runLabel = new QLabel(QStringLiteral("当前文件"), m_runView);
    runLabel->setStyleSheet("font-size:9.5px;color:#95A6B8;");
    auto *runLive = new QLabel(QStringLiteral("● 执行中"), m_runView);
    runLive->setStyleSheet("font-size:9.5px;font-weight:600;color:#C48626;");
    runEyebrow->addWidget(runLabel); runEyebrow->addStretch(); runEyebrow->addWidget(runLive);
    runLayout->addLayout(runEyebrow);
    auto *runCurrent = new QHBoxLayout; runCurrent->setContentsMargins(0,11,0,0); runCurrent->setSpacing(9);
    m_runTile = new QLabel(m_runView); m_runTile->setFixedSize(35,35); m_runTile->setAlignment(Qt::AlignCenter);
    auto *runFileBox = new QVBoxLayout; runFileBox->setContentsMargins(0,0,0,0); runFileBox->setSpacing(3);
    m_runFileName = new QLabel(m_runView); m_runFileName->setStyleSheet("font-size:11px;font-weight:600;color:#455B71;");
    m_runFileSub = new QLabel(m_runView); m_runFileSub->setStyleSheet("font-size:9px;color:#9AAABB;");
    runFileBox->addWidget(m_runFileName); runFileBox->addWidget(m_runFileSub);
    m_runPercent = new QLabel(m_runView); m_runPercent->setStyleSheet("font-size:22px;font-weight:600;color:#2F80ED;");
    runCurrent->addWidget(m_runTile); runCurrent->addLayout(runFileBox,1); runCurrent->addWidget(m_runPercent,0,Qt::AlignTop);
    runLayout->addLayout(runCurrent);
    m_progress = new QProgressBar(m_runView); m_progress->setObjectName("openListProgress");
    m_progress->setFixedHeight(6); m_progress->setTextVisible(false); m_progress->setRange(0,100); m_progress->setValue(0);
    runLayout->addSpacing(12); runLayout->addWidget(m_progress);
    auto *runStats = new QHBoxLayout; runStats->setContentsMargins(0,6,0,0); runStats->setSpacing(8);
    m_runStatsLeft = new QLabel(m_runView); m_runStatsLeft->setStyleSheet("font-size:9px;color:#9BABBC;");
    m_runStatsRight = new QLabel(m_runView); m_runStatsRight->setStyleSheet("font-size:9px;color:#9BABBC;");
    runStats->addWidget(m_runStatsLeft); runStats->addStretch(); runStats->addWidget(m_runStatsRight);
    runLayout->addLayout(runStats);
    m_queueBox = new QWidget(m_runView);
    auto *queueLayout = new QVBoxLayout(m_queueBox); queueLayout->setContentsMargins(0,13,0,0); queueLayout->setSpacing(5);
    // 队列放滚动区：文件较多时固定占用剩余空间并滚动，避免把取消按钮挤出或与列表重叠。
    m_queueScroll = new QScrollArea(m_runView);
    m_queueScroll->setObjectName("openListQueueScroll");
    m_queueScroll->setWidgetResizable(true);
    m_queueScroll->setFrameShape(QFrame::NoFrame);
    m_queueScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_queueScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_queueScroll->setFocusPolicy(Qt::NoFocus);
    m_queueScroll->setMinimumHeight(1);
    m_queueScroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_queueScroll->setMaximumHeight(0);
    m_queueScroll->setWidget(m_queueBox);
    m_queueScroll->setVisible(false);
    runLayout->addWidget(m_queueScroll, 1);
    m_cancel = new QPushButton(QStringLiteral("取消下载"), m_runView); m_cancel->setObjectName("openListCancel");
    m_cancel->setEnabled(false); m_cancel->setCursor(Qt::PointingHandCursor);
    runLayout->addSpacing(13); runLayout->addWidget(m_cancel);
    m_cancelNote = new QLabel(QStringLiteral("取消后，已下载完成的文件会保留在本地「软件」目录"), m_runView);
    m_cancelNote->setStyleSheet("font-size:9px;color:#93A5B8;"); m_cancelNote->setWordWrap(true);
    auto *noteHolder = new QVBoxLayout; noteHolder->setContentsMargins(0,9,0,0); noteHolder->addWidget(m_cancelNote);
    runLayout->addLayout(noteHolder);
    panelLayout->addWidget(m_runView, 1);

    // 结束视图：成功 / 失败徽章 + 结果。
    m_successView = new QWidget(panel); m_successView->setVisible(false);
    auto *successLayout = new QVBoxLayout(m_successView); successLayout->setContentsMargins(4,10,4,0); successLayout->setSpacing(0);
    m_successOrb = new QLabel(m_successView); m_successOrb->setFixedSize(52,52); m_successOrb->setAlignment(Qt::AlignCenter);
    m_successTitle = new QLabel(m_successView); m_successTitle->setAlignment(Qt::AlignCenter);
    m_successTitle->setStyleSheet("font-size:14px;font-weight:600;color:#354B61;");
    m_successMessage = new QLabel(m_successView); m_successMessage->setAlignment(Qt::AlignCenter);
    m_successMessage->setStyleSheet("font-size:9.5px;color:#9BAABB;"); m_successMessage->setWordWrap(true);
    m_successStats = new QLabel(m_successView); m_successStats->setAlignment(Qt::AlignCenter);
    m_successStats->setStyleSheet("font-size:9.5px;color:#9AAABB;");
    m_backToSelection = new QPushButton(QStringLiteral("返回继续选择"), m_successView);
    m_backToSelection->setObjectName("openListBackToSelection"); m_backToSelection->setCursor(Qt::PointingHandCursor);
    successLayout->addWidget(m_successOrb,0,Qt::AlignHCenter);
    successLayout->addSpacing(10); successLayout->addWidget(m_successTitle);
    successLayout->addSpacing(4); successLayout->addWidget(m_successMessage);
    successLayout->addSpacing(9); successLayout->addWidget(m_successStats);
    successLayout->addSpacing(12); successLayout->addWidget(m_backToSelection);
    panelLayout->addWidget(m_successView, 1);

    // 分隔线 + MT 行 + 最近动态；当前视图占满剩余高度，避免固定高度列表与下方控件重叠。
    auto *divider = new QFrame(panel); divider->setFixedHeight(1); divider->setStyleSheet("background:#EEF4FA;");
    panelLayout->addSpacing(10); panelLayout->addWidget(divider);
    m_installMt = new QPushButton(panel); m_installMt->setObjectName("openListInstallMt");
    m_installMt->setFixedHeight(40); m_installMt->setCursor(Qt::PointingHandCursor); m_installMt->setFlat(true);
    auto *mtLayout = new QHBoxLayout(m_installMt); mtLayout->setContentsMargins(7,6,10,6); mtLayout->setSpacing(9);
    auto *mtIcon = new QLabel(m_installMt); mtIcon->setFixedSize(28,28); mtIcon->setAlignment(Qt::AlignCenter);
    mtIcon->setPixmap(tilePixmap("zap", QColor("#D08115"), QColor("#FFFAF0"), QColor("#F7E7C7"), 28, 8));
    auto *mtCopy = new QVBoxLayout; mtCopy->setContentsMargins(0,0,0,0); mtCopy->setSpacing(2);
    auto *mtTitle = new QLabel(QStringLiteral("安装 MT 管理器"), m_installMt);
    mtTitle->setStyleSheet("font-size:10.5px;font-weight:600;color:#566B80;");
    auto *mtSub = new QLabel(QStringLiteral("自动获取最新版本并安装"), m_installMt);
    mtSub->setStyleSheet("font-size:9px;color:#9EACBB;");
    mtCopy->addWidget(mtTitle); mtCopy->addWidget(mtSub);
    auto *mtArrow = new QLabel(m_installMt); mtArrow->setFixedSize(13,13);
    mtArrow->setPixmap(glyphPixmap("chevron-right", QColor("#B8C6D4"), 13));
    mtArrow->setStyleSheet("background:transparent;");
    mtLayout->addWidget(mtIcon); mtLayout->addLayout(mtCopy,1); mtLayout->addWidget(mtArrow);
    panelLayout->addSpacing(6); panelLayout->addWidget(m_installMt);

    auto *recent = new QFrame(panel); recent->setObjectName("openListRecent");
    m_recentPanel = recent;
    auto *recentLayout = new QVBoxLayout(recent); recentLayout->setContentsMargins(8,6,8,6); recentLayout->setSpacing(4);
    auto *recentHead = new QHBoxLayout; recentHead->setContentsMargins(0,0,0,0); recentHead->setSpacing(6);
    auto *recentLabel = new QLabel(QStringLiteral("最近动态"), recent);
    recentLabel->setStyleSheet("font-size:9px;font-weight:700;color:#8FA2B5;");
    m_openLogInline = new QPushButton(QStringLiteral("运行日志 ›"), recent); m_openLogInline->setObjectName("openListOpenLog");
    m_openLogInline->setCursor(Qt::PointingHandCursor);
    recentHead->addWidget(recentLabel); recentHead->addStretch(); recentHead->addWidget(m_openLogInline);
    recentLayout->addLayout(recentHead);
    for (int i = 0; i < 3; ++i) {
        m_recentRow[i] = new QLabel(recent); m_recentRow[i]->setStyleSheet("font-size:9px;color:#8B9DAE;");
        recentLayout->addWidget(m_recentRow[i]);
    }
    auto *recentFootLine = new QFrame(recent); recentFootLine->setFixedHeight(1); recentFootLine->setStyleSheet("background:#EDF4FA;");
    recentLayout->addSpacing(3); recentLayout->addWidget(recentFootLine);
    auto *recentFoot = new QHBoxLayout; recentFoot->setContentsMargins(0,4,0,0); recentFoot->setSpacing(5);
    auto *recentLive = new QLabel(QStringLiteral("● 云端连接正常"), recent);
    recentLive->setStyleSheet("font-size:9px;color:#A0AFBE;");
    m_logTotal = new QLabel(QStringLiteral("共 0 条"), recent); m_logTotal->setStyleSheet("font-size:9px;color:#B3C0CD;");
    recentFoot->addWidget(recentLive); recentFoot->addStretch(); recentFoot->addWidget(m_logTotal);
    recentLayout->addLayout(recentFoot);
    panelLayout->addSpacing(7); panelLayout->addWidget(recent);
    bodyLayout->addWidget(panel);
    rootLayout->addWidget(body,1);

    // ---------- 底部状态条 ----------
    auto *footer = new QFrame(this); footer->setObjectName("openListFooter"); footer->setFixedHeight(44);
    auto *footerLayout = new QHBoxLayout(footer); footerLayout->setContentsMargins(20,0,20,0); footerLayout->setSpacing(12);
    m_footerDot = new QLabel(footer); m_footerDot->setFixedSize(6,6);
    m_footerDot->setStyleSheet("background:#28B770;border-radius:3px;");
    m_status = new QLabel(QStringLiteral("正在读取资源…"), footer); m_status->setStyleSheet("font-size:10px;color:#7F92A6;");
    m_updatedAt = new QLabel(QStringLiteral("更新于 —"), footer); m_updatedAt->setStyleSheet("font-size:9px;color:#A3B1BF;");
    m_logButton = new QPushButton(QStringLiteral("运行日志"), footer); m_logButton->setObjectName("openListLogTrigger");
    m_logButton->setCursor(Qt::PointingHandCursor);
    m_logCount = new QLabel(QStringLiteral("0"), footer); m_logCount->setObjectName("openListLogCount");
    m_logCount->setFixedHeight(15); m_logCount->setMinimumWidth(15); m_logCount->setAlignment(Qt::AlignCenter);
    footerLayout->addWidget(m_footerDot); footerLayout->addWidget(m_status,1);
    footerLayout->addWidget(m_updatedAt); footerLayout->addWidget(m_logButton); footerLayout->addWidget(m_logCount);
    rootLayout->addWidget(footer);

    // ---------- 日志抽屉（覆盖层，不参与布局） ----------
    m_scrim = new QFrame(this); m_scrim->setObjectName("openListScrim");
    m_scrim->hide(); m_scrim->installEventFilter(this);
    m_logSheet = new QFrame(this); m_logSheet->setObjectName("openListLogSheet"); m_logSheet->hide();
    auto *sheetLayout = new QVBoxLayout(m_logSheet); sheetLayout->setContentsMargins(0,0,0,0); sheetLayout->setSpacing(0);
    auto *logHead = new QFrame(m_logSheet); logHead->setObjectName("openListLogHead"); logHead->setFixedHeight(52);
    auto *logHeadLayout = new QHBoxLayout(logHead); logHeadLayout->setContentsMargins(15,0,14,0); logHeadLayout->setSpacing(8);
    auto *logMark = new QLabel(logHead); logMark->setFixedSize(27,27); logMark->setAlignment(Qt::AlignCenter);
    logMark->setPixmap(tilePixmap("terminal", QColor("#89C2FF"), QColor("#33506E"), QColor(), 27, 8));
    auto *logCopy = new QVBoxLayout; logCopy->setContentsMargins(0,0,0,0); logCopy->setSpacing(2);
    auto *logTitle = new QLabel(QStringLiteral("运行日志"), logHead);
    logTitle->setStyleSheet("font-size:11px;font-weight:600;color:#ECF5FD;");
    auto *logSub = new QLabel(QStringLiteral("实时记录 · 最近 500 条"), logHead);
    logSub->setStyleSheet("font-size:9px;color:#7F97AD;");
    logCopy->addWidget(logTitle); logCopy->addWidget(logSub);
    auto *liveBadge = new QLabel(QStringLiteral("LIVE"), logHead);
    liveBadge->setStyleSheet("font-size:9px;font-weight:700;color:#7FDCB0;");
    m_logFilters.clear();
    const QStringList filterNames = {QStringLiteral("全部"), QStringLiteral("INFO"), QStringLiteral("OK"), QStringLiteral("WARN")};
    for (int i = 0; i < filterNames.size(); ++i) {
        auto *filterButton = new QPushButton(filterNames[i], logHead);
        filterButton->setObjectName(QString("openListLogFilter%1").arg(i));
        filterButton->setCheckable(true); filterButton->setChecked(i == 0);
        filterButton->setCursor(Qt::PointingHandCursor);
        connect(filterButton,&QPushButton::clicked,this,[this,i] { setLogFilter(i); });
        m_logFilters.append(filterButton);
        logHeadLayout->addWidget(filterButton);
    }
    auto *logClear = new QPushButton(QStringLiteral("清空"), logHead); logClear->setObjectName("openListLogClear");
    logClear->setCursor(Qt::PointingHandCursor);
    auto *logClose = new QPushButton(logHead); logClose->setObjectName("openListLogClose");
    logClose->setIcon(QIcon(glyphPixmap("x", QColor("#8EA4B9"), 13)));
    logClose->setIconSize(QSize(13,13)); logClose->setFixedSize(24,24); logClose->setCursor(Qt::PointingHandCursor);
    logHeadLayout->addWidget(logMark); logHeadLayout->addLayout(logCopy);
    logHeadLayout->addStretch(); logHeadLayout->addWidget(liveBadge);
    logHeadLayout->addWidget(logClear); logHeadLayout->addWidget(logClose);
    sheetLayout->addWidget(logHead);
    m_log = new QPlainTextEdit(m_logSheet); m_log->setObjectName("openListLog");
    m_log->setReadOnly(true); m_log->setMaximumBlockCount(500); m_log->setFrameShape(QFrame::NoFrame);
    sheetLayout->addWidget(m_log,1);
    connect(logClear,&QPushButton::clicked,this,[this] {
        m_logEntries.clear(); m_log->clear();
        m_logCount->setText(QStringLiteral("0")); m_logTotal->setText(QStringLiteral("共 0 条")); updateRecent();
    });
    connect(logClose,&QPushButton::clicked,this,[this] { toggleLogSheet(false); });
    connect(m_logButton,&QPushButton::clicked,this,[this] { toggleLogSheet(!m_logOpen); });
    connect(m_openLogInline,&QPushButton::clicked,this,[this] { toggleLogSheet(true); });
    connect(m_clearSelection,&QPushButton::clicked,this,&OpenListWindow::clearSelectionState);
    connect(m_backToSelection,&QPushButton::clicked,this,[this] { showTaskView(0); });
    auto *escapeShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(escapeShortcut,&QShortcut::activated,this,[this] { if (m_logOpen) toggleLogSheet(false); });
    auto *slashShortcut = new QShortcut(QKeySequence(QStringLiteral("/")), this);
    connect(slashShortcut,&QShortcut::activated,this,[this] {
        if (!m_search->isEnabled()) return;
        m_search->setFocus(Qt::ShortcutFocusReason);
    });

    setStyleSheet(R"(
        QWidget#openListWindow { background:#F4F8FC; }
        QFrame#openListHeader { background:qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 #EAFFFFFF,stop:1 #ADFCFEFF); border-bottom:1px solid #E2ECF6; }
        QLabel#openListBrandMark { background:qlineargradient(x1:0,y1:0,x2:1,y2:1,stop:0 #62A8FF,stop:0.48 #3383F3,stop:1 #1F68DA); border-radius:13px; }
        QLabel#openListBrandTag { color:#4D8BD8; background:#EFF7FF; border:1px solid #D9EAFF; border-radius:5px; padding:2px 5px; font-size:9px; font-weight:700; }
        QFrame#openListDevicePill { background:#D9FFFFFF; border:1px solid #E5EDF6; border-radius:13px; }
        QLabel#openListDeviceAvatar { background:qlineargradient(x1:0,y1:0,x2:1,y2:1,stop:0 #F0F8FF,stop:1 #E4F1FF); border:1px solid #D9EAFF; border-radius:9px; }
        QPushButton#openListRefresh { background:#FFFFFF; color:#556A80; border:1px solid #E2EBF4; border-radius:11px; padding:0 14px; min-height:36px; font-size:11.5px; font-weight:500; }
        QPushButton#openListRefresh:hover { color:#2F80ED; border-color:#B9D8FA; background:#F7FBFF; }
        QPushButton#openListRefresh:disabled { color:#A9B5C1; border-color:#EDF2F7; background:#F8FAFC; }
        QGroupBox#openListFilesCard { background:#FFFFFF; border:1px solid #E0EBF5; border-radius:20px; margin-top:0px; padding:0px; font-weight:normal; }
        QGroupBox#openListFilesCard::title { color:transparent; subcontrol-origin:margin; }
        QFrame#openListTaskPanel { background:#FFFFFF; border:1px solid #E0EBF5; border-radius:20px; }
        QFrame#openListBrowserHead { background:qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 #FFFFFF,stop:1 #FDFEFE); border-bottom:1px solid #F0F5FA; }
        QFrame#openListSearchBox { background:#F6F9FD; border:1px solid #E6EEF7; border-radius:12px; }
        QLineEdit#openListSearch { background:transparent; border:0; color:#31455B; font-size:12.5px; padding:0; }
        QLineEdit#openListSearch:disabled { color:#A4B3C2; }
        QLabel#openListSearchKbd { background:#FFFFFF; border:1px solid #E1EAF3; border-radius:5px; color:#9AABBB; font-size:9px; font-family:"Consolas"; }
        QPushButton#openListUp { background:#F7FAFD; border:1px solid #E7EEF6; border-radius:9px; }
        QPushButton#openListUp:hover { background:#F3F9FF; border-color:#CFE5FD; }
        QPushButton#openListUp:disabled { color:#C2CEDA; border-color:#EEF3F8; background:#FAFCFE; }
        QLabel#openListCountPill { color:#7990A7; background:#F4F8FC; border:1px solid #E9F0F7; border-radius:9px; padding:3px 8px; font-size:10px; font-weight:600; }
        QPushButton#openListSelectAll { color:#7C90A5; border:1px solid #E4EDF6; background:#F8FBFE; border-radius:9px; padding:2px 8px; font-size:9px; font-weight:600; }
        QPushButton#openListSelectAll:hover { color:#2F80ED; border-color:#CFE4FB; background:#F1F8FF; }
        QPushButton#openListSelectAll:checked { color:#2F80ED; border-color:#BCD9F9; background:#ECF5FF; }
        QTableWidget#openListFiles { background:#FFFFFF; color:#293C52; border:0; gridline-color:transparent; font-size:12.5px; }
        QTableWidget#openListFiles::item { padding:4px 6px; }
        QTableWidget#openListFiles::item:hover { background:#F8FBFF; }
        QTableWidget#openListFiles::item:selected { background:#F2F8FF; color:#293C52; }
        QTableWidget#openListFiles::item:disabled { color:#B7C6D5; }
        QFrame#openListCardFooter { background:#FDFEFE; border-top:1px solid #EFF5FA; }
        QLabel#openListStatusPill { font-size:9.5px; font-weight:600; padding:0 9px; }
        QFrame#openListSelectionEmpty { border:1px dashed #DCE8F5; border-radius:10px; background:#FAFDFF; }
        QScrollArea#openListSelectionScroll, QScrollArea#openListSelectionScroll > QWidget > QWidget { background:transparent; }
        QScrollArea#openListSelectionScroll QScrollBar:vertical { background:transparent; width:5px; margin:1px 0; }
        QScrollArea#openListSelectionScroll QScrollBar::handle:vertical { background:#D9E6F3; border-radius:2px; min-height:24px; }
        QScrollArea#openListSelectionScroll QScrollBar::handle:vertical:hover { background:#BFD5EA; }
        QScrollArea#openListSelectionScroll QScrollBar::add-line:vertical, QScrollArea#openListSelectionScroll QScrollBar::sub-line:vertical { height:0; }
        QScrollArea#openListSelectionScroll QScrollBar::add-page:vertical, QScrollArea#openListSelectionScroll QScrollBar::sub-page:vertical { background:transparent; }
        QScrollArea#openListQueueScroll, QScrollArea#openListQueueScroll > QWidget > QWidget { background:transparent; border:0; }
        QScrollArea#openListQueueScroll QScrollBar:vertical { background:transparent; width:5px; margin:1px 0; }
        QScrollArea#openListQueueScroll QScrollBar::handle:vertical { background:#D9E6F3; border-radius:2px; min-height:24px; }
        QScrollArea#openListQueueScroll QScrollBar::handle:vertical:hover { background:#BFD5EA; }
        QScrollArea#openListQueueScroll QScrollBar::add-line:vertical, QScrollArea#openListQueueScroll QScrollBar::sub-line:vertical { height:0; }
        QScrollArea#openListQueueScroll QScrollBar::add-page:vertical, QScrollArea#openListQueueScroll QScrollBar::sub-page:vertical { background:transparent; }
        QFrame#openListTargetStrip { background:qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 #F7FBFF,stop:1 #F4F9FF); border:1px solid #E9F2FB; border-radius:10px; }
        QPushButton#openListInstallSelected { background:qlineargradient(x1:0,y1:0,x2:1,y2:1,stop:0 #4B97FA,stop:0.54 #2F80ED,stop:1 #226EDB); color:#FFFFFF; border:1px solid #2C7CE9; border-radius:10px; min-height:36px; font-size:11.5px; font-weight:600; }
        QPushButton#openListInstallSelected:hover { background:qlineargradient(x1:0,y1:0,x2:1,y2:1,stop:0 #5AA3FF,stop:0.54 #3B8BF2,stop:1 #2A77E3); }
        QPushButton#openListInstallSelected:disabled { background:#EFF4F9; color:#A6B5C4; border-color:#E7EEF6; }
        QPushButton#openListTransferSelected, QPushButton#openListTransferDesktop, QPushButton#openListBackToSelection { background:#FFFFFF; color:#536A81; border:1px solid #E1EBF5; border-radius:9px; min-height:30px; font-size:10.5px; }
        QPushButton#openListTransferSelected:hover, QPushButton#openListTransferDesktop:hover, QPushButton#openListBackToSelection:hover { color:#2F80ED; border-color:#C2DCFA; background:#F8FBFF; }
        QPushButton#openListTransferSelected:disabled, QPushButton#openListTransferDesktop:disabled { color:#B3C0CD; background:#FAFCFE; border-color:#EDF2F7; }
        QPushButton#openListClearSelection { background:transparent; border:0; color:#92A3B5; font-size:9.5px; padding:2px 5px; border-radius:5px; }
        QPushButton#openListClearSelection:hover { color:#D05B5B; background:#FFF4F4; }
        QPushButton#openListCancel { background:#FFFAFA; color:#BC5C5C; border:1px solid #F5DEDE; border-radius:9px; min-height:31px; font-size:10px; }
        QPushButton#openListCancel:hover { color:#D24A4A; background:#FFF3F3; border-color:#EFC9C9; }
        QPushButton#openListCancel:disabled { color:#C9A9A9; background:#FBF7F7; border-color:#F3E7E7; }
        QPushButton#openListInstallMt { background:transparent; border:0; border-radius:10px; }
        QPushButton#openListInstallMt:hover { background:#F8FBFF; }
        QPushButton#openListInstallMt:disabled { background:transparent; }
        QFrame#openListRecent { border:1px solid #EDF4FB; border-radius:12px; background:qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 #FBFDFF,stop:1 #F7FBFF); }
        QPushButton#openListOpenLog { background:transparent; border:0; color:#7FA8D9; font-size:9px; padding:2px; }
        QPushButton#openListOpenLog:hover { color:#2F80ED; }
        QFrame#openListFooter { background:qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 #A3FFFFFF,stop:1 #D1FBFDFF); border-top:1px solid #E3EDF6; }
        QPushButton#openListLogTrigger { background:#FFFFFF; color:#6A7E93; border:1px solid #E5EDF5; border-radius:8px; padding:4px 9px; font-size:9.5px; }
        QPushButton#openListLogTrigger:hover { color:#2F80ED; border-color:#CBE0F8; background:#F9FCFF; }
        QLabel#openListLogCount { background:#EDF5FF; color:#5D98D9; border-radius:4px; font-size:8.5px; font-weight:700; padding:0 3px; }
        QFrame#openListScrim { background:#730D1F33; }
        QFrame#openListLogSheet { background:qlineargradient(x1:0,y1:0,x2:1,y2:1,stop:0 #112137,stop:0.68 #0D1A2B,stop:1 #0B1726); border:1px solid #3D5A78; border-radius:17px; }
        QFrame#openListLogHead { background:transparent; border-bottom:1px solid #22384F; }
        QPushButton#openListLogFilter { color:#7F97AD; background:transparent; border:1px solid #2C4257; border-radius:10px; padding:3px 9px; font-size:9px; font-weight:600; }
        QPushButton#openListLogFilter:hover { color:#C6DCF0; border-color:#3D5A78; }
        QPushButton#openListLogFilter:checked { color:#0D1A2B; background:#8FC1F2; border-color:#8FC1F2; }
        QPushButton#openListLogClear { color:#93A8BD; background:transparent; border:1px solid #2C4257; border-radius:6px; padding:3px 8px; font-size:9px; }
        QPushButton#openListLogClear:hover { color:#DCEBF8; border-color:#3D5A78; background:#1A3049; }
        QPushButton#openListLogClose { background:transparent; border:0; border-radius:6px; }
        QPlainTextEdit#openListLog { background:transparent; color:#BED0E1; border:0; font-family:"Cascadia Code","Consolas","Microsoft YaHei UI"; font-size:10px; selection-background-color:#2A4057; }
        QScrollBar:vertical { background:transparent; width:6px; margin:2px 2px 2px 0; }
        QScrollBar::handle:vertical { background:#D3E0EC; border-radius:3px; min-height:30px; }
        QScrollBar::handle:vertical:hover { background:#B9CDE0; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background:transparent; }
        QPlainTextEdit#openListLog QScrollBar::handle:vertical { background:#2A4057; }
        QProgressBar#openListProgress { border:0; background:#EAF2FA; border-radius:3px; min-height:6px; max-height:6px; }
        QProgressBar#openListProgress::chunk { background:qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 #4D9CFF,stop:0.6 #2F80ED,stop:1 #2476E7); border-radius:3px; }
    )");
    updateDrawerGeometry();
    updateRecent();
}
void OpenListWindow::beginListing() {
    if (m_busy) return;
    m_renderTimer->stop();
    const QList<OpenList::Entry> snapshot = m_service->snapshotEntries();
    auto showSnapshot = [this, &snapshot] {
        if (!snapshot.isEmpty()) {
            setListingEntries(snapshot);
            m_hasCachedSnapshot = true;
            const QDateTime time = m_service->snapshotTime();
            if (time.isValid())
                m_updatedAt->setText(QStringLiteral("缓存于 %1").arg(time.toLocalTime().toString("MM-dd HH:mm")));
        }
    };
    if (m_service->listingInProgress()) {
        showSnapshot();
        m_pending = 1; m_refresh->setEnabled(false);
        m_currentDirectory.clear(); m_listingComplete = false;
        m_status->setText(QStringLiteral("正在加载资源…"));
        render();
        return;
    }
    // 启动预热刚完成且数据仍新鲜时直接复用，避免重复一次网络往返。
    if (m_service->listingComplete() && m_service->snapshotFresh()) {
        setListingEntries(snapshot);
        m_listingComplete = true; m_hasCachedSnapshot = true; m_pending = 0;
        m_currentDirectory.clear();
        m_refresh->setEnabled(true);
        const QDateTime time = m_service->snapshotTime();
        m_updatedAt->setText(QStringLiteral("更新于 %1")
            .arg((time.isValid() ? time : QDateTime::currentDateTime()).toLocalTime().toString("MM-dd HH:mm")));
        m_status->setText(QStringLiteral("资源已就绪 · 单击单选，按住 Ctrl 多选，双击文件直接安装"));
        render();
        return;
    }
    showSnapshot();
    m_pending = 1; m_refresh->setEnabled(false); m_status->setText(QStringLiteral("正在读取资源…"));
    m_currentDirectory.clear(); m_listingComplete = false;
    // 保留上一次结果与已选项：服务端缓存会在 refresh() 内同步先发，网络失败时也不闪空。
    render(); m_count->setText(QStringLiteral("正在读取…"));
    m_service->refresh();
}
void OpenListWindow::refresh() {
    if (m_busy) return;
    m_renderTimer->stop();
    m_pending = 1; m_refresh->setEnabled(false); m_status->setText(QStringLiteral("正在读取资源…"));
    m_currentDirectory.clear(); m_listingComplete = false;
    // 保留上一次结果与已选项：服务端缓存会在 refresh() 内同步先发，网络失败时也不闪空。
    render(); m_count->setText(QStringLiteral("正在读取…"));
    m_service->refresh();
}
void OpenListWindow::setListingEntries(const QList<OpenList::Entry> &entries) {
    m_entries = entries;
    m_entryPaths.clear();
    for (const auto &entry : m_entries) m_entryPaths.insert(entry.remotePath);
    rebuildEntryIndexes();
}
void OpenListWindow::rebuildEntryIndexes() {
    m_entryIndex.clear();
    m_childrenByParent.clear();
    m_entryIndex.reserve(m_entries.size());
    for (int index = 0; index < m_entries.size(); ++index) {
        const auto &entry = m_entries[index];
        // 展示层保留重复路径的原始条目；索引只记录首个，用于选中项定位。
        if (!m_entryIndex.contains(entry.remotePath)) m_entryIndex.insert(entry.remotePath, index);
        m_childrenByParent[relativeParent(entry.relativePath)].append(index);
    }
}
bool OpenListWindow::appendListingEntries(const QList<OpenList::Entry> &entries) {
    bool changed = false;
    for (const auto &entry : entries) {
        if (m_entryPaths.contains(entry.remotePath)) continue;
        m_entryPaths.insert(entry.remotePath);
        const int index = m_entries.size();
        m_entries.append(entry);
        m_entryIndex.insert(entry.remotePath, index);
        m_childrenByParent[relativeParent(entry.relativePath)].append(index);
        changed = true;
    }
    return changed;
}
void OpenListWindow::pruneSelection() {
    QSet<QString> kept;
    for (const auto &path : m_selectedPaths) {
        const int index = m_entryIndex.value(path, -1);
        if (index >= 0 && !m_entries[index].directory) kept.insert(path);
    }
    m_selectedPaths = kept;
    QList<QString> order;
    for (const auto &path : m_selectionOrder) if (kept.contains(path)) order.append(path);
    m_selectionOrder = order;
    updateCount();
}
void OpenListWindow::render(bool preserveScroll) {
    auto *table = m_table;
    const int scrollValue = preserveScroll ? table->verticalScrollBar()->value() : 0;
    const auto filter = m_search->text().trimmed();
    QList<int> visible;
    if (filter.isEmpty()) {
        visible = m_childrenByParent.value(m_currentDirectory);
    } else {
        visible.reserve(m_entries.size());
        for (int index = 0; index < m_entries.size(); ++index)
            if (m_entries[index].relativePath.contains(filter, Qt::CaseInsensitive)) visible.append(index);
    }
    table->setUpdatesEnabled(false);
    table->blockSignals(true);
    table->setRowCount(0);
    m_up->setEnabled(!m_busy && !m_currentDirectory.isEmpty());
    m_directory->setText(m_currentDirectory.isEmpty() ? QStringLiteral("秋白工作室")
                                                     : QStringLiteral("秋白工作室 / %1").arg(m_currentDirectory));
    for (int index : visible) {
        const auto &entry = m_entries[index];
        const int row = table->rowCount(); table->insertRow(row);
        auto *name = new QTableWidgetItem(entry.name);
        // 目录行 accent 蓝文件夹图标；文件按类型着色（APK 绿 / ZIP 蓝 / SH 琥珀 / 其他墨灰）。
        if (entry.directory) {
            name->setIcon(glyphPixmap(QStringLiteral("folder"), QColor(QStringLiteral("#2F80ED")), 20));
        } else {
            const QString lower = entry.name.toLower();
            QString tone = QStringLiteral("#647585");
            if (lower.endsWith(QStringLiteral(".apk"))) tone = QStringLiteral("#16A34A");
            else if (lower.endsWith(QStringLiteral(".zip"))) tone = QStringLiteral("#2F80ED");
            else if (lower.endsWith(QStringLiteral(".sh"))) tone = QStringLiteral("#D97706");
            name->setIcon(glyphPixmap(QStringLiteral("file"), QColor(tone), 20));
        }
        name->setData(Qt::UserRole,index); name->setToolTip(entry.relativePath);
        if (!entry.directory) {
            // 原生复选框（与欧加线刷一致），仅文件行可勾选。
            name->setFlags(name->flags() | Qt::ItemIsUserCheckable);
            name->setCheckState(m_selectedPaths.contains(entry.remotePath) ? Qt::Checked : Qt::Unchecked);
        }
        table->setItem(row,0,name);
        QString dir = relativeParent(entry.relativePath); if (dir.isEmpty()) dir = QStringLiteral("根目录");
        auto *dirItem = new QTableWidgetItem(dir);
        dirItem->setForeground(QColor("#5E7287")); dirItem->setToolTip(dir);
        table->setItem(row,1,dirItem);
        QString sizeText;
        if (entry.directory) {
            const int children = m_childrenByParent.value(entry.relativePath).size();
            sizeText = (!m_listingComplete && children == 0) ? QStringLiteral("—")
                                                             : QStringLiteral("%1 项").arg(children);
        } else {
            sizeText = entry.size < 0 ? QStringLiteral("未知") : QLocale().formattedDataSize(entry.size);
        }
        auto *sizeItem = new QTableWidgetItem(sizeText);
        sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        sizeItem->setForeground(QColor("#5E7287"));
        sizeItem->setToolTip(sizeText);
        table->setItem(row,2,sizeItem);
        const auto date = QDateTime::fromString(entry.modified,Qt::ISODateWithMs);
        auto *dateItem = new QTableWidgetItem(date.isValid() ? date.toLocalTime().toString("MM-dd HH:mm") : QStringLiteral("—"));
        dateItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        dateItem->setForeground(QColor("#9BABBB"));
        if (date.isValid()) dateItem->setToolTip(date.toLocalTime().toString("yyyy-MM-dd HH:mm"));
        table->setItem(row,3,dateItem);
        if (entry.directory) {
            // 文件夹仅支持双击进入，不可选中、不显示复选框。
            for (int col=0;col<4;++col) table->item(row,col)->setFlags(table->item(row,col)->flags() & ~(Qt::ItemIsSelectable | Qt::ItemIsUserCheckable));
        } else if (m_selectedPaths.contains(entry.remotePath)) {
            for (int col=0;col<4;++col) table->item(row,col)->setSelected(true);
        }
    }
    table->blockSignals(false);
    table->setUpdatesEnabled(true);
    if (preserveScroll) table->verticalScrollBar()->setValue(scrollValue);
    syncChecks();
    updateCount();
}
void OpenListWindow::updateCount() {
    const auto filter = m_search->text().trimmed();
    const int rows = m_table->rowCount();
    m_pill->setText(rows > 0 ? QStringLiteral("%1 项").arg(rows) : QStringLiteral("—"));
    if (rows == 0) m_count->setText(filter.isEmpty() ? QStringLiteral("暂无资源") : QStringLiteral("没有匹配的资源"));
    else if (m_selectedPaths.isEmpty()) m_count->setText(QStringLiteral("共 %1 项资源").arg(rows));
    else {
        qint64 total = 0; bool unknown = false;
        for (const auto &path : m_selectedPaths) {
            const int index = m_entryIndex.value(path, -1);
            if (index < 0) continue;
            const auto &entry = m_entries[index];
            if (entry.size >= 0) total += entry.size; else unknown = true;
        }
        m_count->setText(QStringLiteral("共 %1 项资源 · 已选 %2 项 · 合计 %3")
                             .arg(rows).arg(m_selectedPaths.size())
                             .arg(unknown ? QStringLiteral("大小未知") : QLocale().formattedDataSize(total)));
    }
    updateSelectionPanel();
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
    if (!paths.contains(selection.remotePath)) { paths.insert(selection.remotePath); expanded.append(selection); }
}
void OpenListWindow::markSelected(const QString &path) {
    if (m_selectedPaths.contains(path)) return;
    m_selectedPaths.insert(path);
    m_selectionOrder.append(path);
}
void OpenListWindow::markDeselected(const QString &path) {
    m_selectedPaths.remove(path);
    m_selectionOrder.removeAll(path);
}
void OpenListWindow::clearSelectionState() {
    m_selectedPaths.clear();
    m_selectionOrder.clear();
    m_table->clearSelection();
    updateCount();
}
void OpenListWindow::startSelection(BatchMode mode) {
    if (m_busy) return;
    QList<OpenList::Entry> selected;
    // 按勾选顺序收集，而不是按服务器列表顺序；批量下载和安装都沿用该顺序。
    for (const auto &path : m_selectionOrder) {
        const int index = m_entryIndex.value(path, -1);
        if (index >= 0 && m_selectedPaths.contains(path)) selected.append(m_entries[index]);
    }
    if (selected.isEmpty()) log(QStringLiteral("请先选择文件；单击单选，按住 Ctrl 点击可多选。"));
    else startBatch(selected, mode);
}void OpenListWindow::startBatch(const QList<OpenList::Entry> &entries, BatchMode mode) {
    if (m_busy || entries.isEmpty()) return;
    m_queue.clear();
    QSet<QString> paths;
    for (const auto &entry : entries) appendSelection(entry, m_queue, paths);
    if (mode == BatchMode::Install) {
        QList<OpenList::Entry> supported;
        for (const auto &entry : m_queue) {
            const QString extension = QFileInfo(entry.name).suffix().toLower();
            if (extension == "apk" || extension == "zip" || extension == "sh") supported.append(entry);
            else log(QStringLiteral("已跳过不支持安装或执行的文件：%1；可使用“传输到手机”或“传输到桌面”。").arg(entry.name));
        }
        m_queue = supported;
    }
    if (m_queue.isEmpty()) return;
    if (mode == BatchMode::DesktopTransfer && !OpenList::safeDirectory(m_desktopDirectory, true)) {
        const QString message = QStringLiteral("电脑桌面目录不可用，未开始传输。");
        log(message); m_status->setText(message); return;
    }
    m_batchMode = mode;
    m_downloaded.clear(); m_serial.clear(); m_queueIndex = 0; m_installIndex = 0;
    m_usedLocalFiles.clear();
    m_succeeded = 0; m_failed = 0; m_installingBatch = false; m_cancelled = false;
    setBusy(true);
    m_runTile->setPixmap(typeTilePixmap(m_queue.first(),35));
    m_runFileName->setText(QStringLiteral("准备中"));
    if (mode == BatchMode::DesktopTransfer) {
        m_runFileSub->setText(QStringLiteral("正在下载到电脑桌面…"));
        m_status->setText(QStringLiteral("正在准备下载到电脑桌面…（共 %1 个文件）").arg(m_queue.size()));
        m_cancelNote->setText(QStringLiteral("取消后，已下载完成的文件会保留在电脑桌面"));
        QTimer::singleShot(0, this, &OpenListWindow::advanceBatch);
    } else {
        m_runFileSub->setText(QStringLiteral("正在检测已授权手机…"));
        m_status->setText(QStringLiteral("正在检测已授权手机…（共 %1 个文件）").arg(m_queue.size()));
        m_cancelNote->setText(QStringLiteral("取消后，已下载完成的文件会保留在本地「软件」目录"));
        m_installer->inspectDevices();
    }
}
void OpenListWindow::advanceBatch() {
    if (!m_busy || m_downloading || m_installer->busy()) return;
    const bool desktop = m_batchMode == BatchMode::DesktopTransfer;
    const bool phone = m_batchMode == BatchMode::PhoneTransfer;
    if (m_cancelled) {
        for (const auto &download : m_downloaded)
            log(QStringLiteral("已取消，下载文件保留在：%1").arg(download.file));
        finish(QStringLiteral("批量任务已取消；已完成的下载保留。"),false); return;
    }
    if (!m_installingBatch && m_queueIndex < m_queue.size()) {
        m_selected = m_queue[m_queueIndex++];
        const QString existing = m_service->existingLocalFile(
            m_selected, desktop ? m_desktopDirectory : QString());
        if (!existing.isEmpty() && !m_usedLocalFiles.contains(localPathKey(existing))) {
            m_usedLocalFiles.insert(localPathKey(existing));
            m_downloaded.append({m_selected, existing});
            m_runTile->setPixmap(typeTilePixmap(m_selected,35));
            m_runFileName->setText(m_selected.name);
            m_runFileSub->setText(QStringLiteral("本地文件 · 第 %1/%2 个").arg(m_queueIndex).arg(m_queue.size()));
            m_runPercent->setText(QStringLiteral("100%"));
            m_runStatsLeft->setText(QStringLiteral("本地文件"));
            m_runStatsRight->setText(QStringLiteral("第 %1/%2 个").arg(m_queueIndex).arg(m_queue.size()));
            m_progress->setRange(0,100); m_progress->setValue(100);
            m_status->setText((desktop ? QStringLiteral("检测到本地文件，直接传输到桌面（%1/%2）：%3")
                                       : phone ? QStringLiteral("检测到本地文件，直接传输（%1/%2）：%3")
                                               : QStringLiteral("检测到本地文件，直接安装（%1/%2）：%3"))
                .arg(m_queueIndex).arg(m_queue.size()).arg(m_selected.name));
            log(QStringLiteral("本地已存在，直接使用：%1").arg(m_selected.name));
            rebuildQueue();
            QTimer::singleShot(0, this, &OpenListWindow::advanceBatch);
            return;
        }
        m_downloading = true; m_cancel->setEnabled(true);
        m_runTile->setPixmap(typeTilePixmap(m_selected,35));
        m_runFileName->setText(m_selected.name);
        m_runFileSub->setText(desktop ? QStringLiteral("下载到电脑桌面 · 第 %1/%2 个").arg(m_queueIndex).arg(m_queue.size())
                                      : QStringLiteral("云端下载 · 第 %1/%2 个").arg(m_queueIndex).arg(m_queue.size()));
        m_runPercent->setText(QStringLiteral("…"));
        m_runStatsLeft->setText(QString()); m_runStatsRight->setText(QString());
        m_progress->setRange(0,100); m_progress->setValue(0);
        m_status->setText(desktop
            ? QStringLiteral("正在下载到电脑桌面（%1/%2）：%3").arg(m_queueIndex).arg(m_queue.size()).arg(m_selected.name)
            : QStringLiteral("正在准备下载（%1/%2）：%3").arg(m_queueIndex).arg(m_queue.size()).arg(m_selected.name));
        rebuildQueue();
        if (desktop) m_service->download(m_selected, m_desktopDirectory);
        else m_service->download(m_selected);
        return;
    }
    if (desktop) {
        m_installingBatch = true; m_cancel->setEnabled(false);
        m_succeeded = m_downloaded.size();
        const QString summary = m_failed == 0
            ? QStringLiteral("已传输 %1 个文件到电脑桌面。").arg(m_downloaded.size())
            : QStringLiteral("已传输 %1 个文件到电脑桌面，失败 %2 个。").arg(m_downloaded.size()).arg(m_failed);
        finish(summary, m_failed == 0);
        return;
    }
    m_installingBatch = true; m_cancel->setEnabled(false);
    if (m_installIndex < m_downloaded.size()) {
        const auto download = m_downloaded[m_installIndex++];
        m_progress->setRange(0,0);
        m_runTile->setPixmap(typeTilePixmap(download.entry,35));
        m_runFileName->setText(download.entry.name);
        m_runFileSub->setText(QStringLiteral("目标手机 %1").arg(m_serial));
        m_runPercent->setText(QStringLiteral("…"));
        const bool script = QFileInfo(download.file).suffix().compare("sh",Qt::CaseInsensitive) == 0;
        m_runStatsLeft->setText(phone ? QStringLiteral("传输中") : script ? QStringLiteral("执行脚本中") : QStringLiteral("安装中"));
        m_runStatsRight->setText(QStringLiteral("第 %1/%2 个").arg(m_installIndex).arg(m_downloaded.size()));
        m_status->setText((phone ? QStringLiteral("正在传输（%1/%2）：%3；目标手机：%4") : (script ? QStringLiteral("正在执行脚本（%1/%2）：%3；目标手机：%4") : QStringLiteral("正在安装（%1/%2）：%3；目标手机：%4")))
            .arg(m_installIndex).arg(m_downloaded.size()).arg(download.entry.name,m_serial));
        rebuildQueue();
        if (phone) m_installer->transfer(download.file,m_serial);
        else m_installer->install(download.file,m_serial);
        return;
    }
    QString summary = QStringLiteral("批量任务完成：成功 %1，失败 %2，共 %3 个文件。")
        .arg(m_succeeded).arg(m_failed).arg(m_queue.size());
    finish(summary,m_failed == 0);
}
void OpenListWindow::setBusy(bool busy) {
    m_busy = busy; m_refresh->setEnabled(!busy && m_pending == 0); m_installSelected->setEnabled(!busy); m_transferSelected->setEnabled(!busy); m_transferDesktop->setEnabled(!busy); m_installMt->setEnabled(!busy);
    m_table->setEnabled(!busy); m_search->setEnabled(!busy); m_up->setEnabled(!busy && !m_currentDirectory.isEmpty());
    m_cancel->setEnabled(busy && m_downloading);
    if (busy) {
        stylePill(m_statusPill, QStringLiteral("● 执行中"), "#B27317", "#FFF8EC", "#F8E9CE");
        m_footerDot->setStyleSheet("background:#EDA838;border-radius:3px;");
        showTaskView(1);
        m_runPercent->setText(QStringLiteral("…"));
        m_runStatsLeft->setText(QString()); m_runStatsRight->setText(QString());
        rebuildQueue();
    } else {
        stylePill(m_statusPill, QStringLiteral("● 就绪"), "#3D9666", "#EFFAF4", "#DCF3E6");
        m_footerDot->setStyleSheet("background:#28B770;border-radius:3px;");
        showTaskView(0);
    }
}
void OpenListWindow::finish(const QString &message,bool ok) {
    const bool batch = m_installingBatch;
    const QString stats = batch ? QStringLiteral("成功 %1 · 失败 %2 · 共 %3 个文件").arg(m_succeeded).arg(m_failed).arg(m_queue.size()) : QString();
    clearSelectionState();
    m_downloading = false; m_queue.clear(); m_downloaded.clear(); setBusy(false); m_progress->setRange(0,100); m_progress->setValue(ok ? 100 : 0);
    m_batchMode = BatchMode::Install;
    const QString safe = OpenList::redact(message);
    m_status->setText(safe);
    m_successOrb->setPixmap(ok ? tilePixmap("check", QColor("#FFFFFF"), QColor("#22B96E"), QColor(), 52, 26)
                               : tilePixmap("x", QColor("#FFFFFF"), QColor("#DA4B4B"), QColor(), 52, 26));
    m_successTitle->setText(ok ? QStringLiteral("任务完成") : QStringLiteral("任务未完成"));
    m_successMessage->setText(safe);
    m_successStats->setText(stats);
    m_successStats->setVisible(!stats.isEmpty());
    stylePill(m_statusPill, ok ? QStringLiteral("● 已完成") : QStringLiteral("● 已停止"),
              ok ? "#2B7FCF" : "#C05555", ok ? "#F0F7FF" : "#FFF1F1", ok ? "#DCECFF" : "#F6D5D5");
    m_footerDot->setStyleSheet(QStringLiteral("background:%1;border-radius:3px;").arg(ok ? "#3D93EF" : "#DA4B4B"));
    showTaskView(2);
    updateDevice();
}
void OpenListWindow::log(const QString &text) {
    const LogEntry entry{ QDateTime::currentDateTime().toString("HH:mm:ss"), OpenList::redact(text), classifyLog(text) };
    m_logEntries.append(entry);
    while (m_logEntries.size() > 500) m_logEntries.removeFirst();
    if (m_logFilter == 0 || entry.level + 1 == m_logFilter) appendLogHtml(entry);
    m_logCount->setText(m_logEntries.size() > 99 ? QStringLiteral("99+") : QString::number(m_logEntries.size()));
    m_logTotal->setText(QStringLiteral("共 %1 条").arg(m_logEntries.size()));
    updateRecent();
}
void OpenListWindow::updateDevice() {
    auto *devices = DeviceManager::instance();
    const bool targeting = m_busy && !m_serial.isEmpty();
    const bool adb = devices->currentMode() == DeviceManager::ADB;
    const QString serial = targeting ? m_serial : adb ? devices->deviceSerial() : QString();
    const QString name = serial.isEmpty() ? QStringLiteral("未检测设备") : serial;
    m_deviceName->setText(name);
    m_targetName->setText(name);
    const QString stateColor = targeting ? QStringLiteral("#F0A42A") : adb ? QStringLiteral("#25C06F") : QStringLiteral("#9AA8B5");
    const QString stateText = targeting ? QStringLiteral("任务执行中") : adb ? QStringLiteral("ADB 已授权") : QStringLiteral("未连接");
    m_deviceState->setText(stateText);
    m_deviceState->setStyleSheet(QStringLiteral("color:%1;").arg(stateColor));
    m_deviceDot->setStyleSheet(QStringLiteral("background:%1;border:2px solid #FFFFFF;border-radius:4px;").arg(stateColor));
    m_targetState->setText(targeting ? QStringLiteral("执行中") : adb ? QStringLiteral("已授权") : QStringLiteral("未连接"));
    m_targetState->setStyleSheet(QStringLiteral("color:%1;").arg(stateColor));
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

void OpenListWindow::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    updateDrawerGeometry();
}

bool OpenListWindow::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_scrim && event->type() == QEvent::MouseButtonPress) {
        toggleLogSheet(false);
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

QPixmap OpenListWindow::glyphPixmap(const QString &name, const QColor &color, int size) const {
    const qreal dpr = devicePixelRatioF();
    const QString key = QStringLiteral("%1|%2|%3|%4")
        .arg(name, color.name(QColor::HexArgb)).arg(size).arg(dpr, 0, 'f', 3);
    const auto cached = m_glyphCache.constFind(key);
    if (cached != m_glyphCache.constEnd()) return cached.value();
    const QPixmap pixmap = UIHelper::menuIcon(name, color, dpr).pixmap(QSize(size, size));
    if (m_glyphCache.size() >= 256) m_glyphCache.clear();
    m_glyphCache.insert(key, pixmap);
    return pixmap;
}

QPixmap OpenListWindow::tilePixmap(const QString &name, const QColor &glyph, const QColor &bg, const QColor &border, int size, int radius) const {
    const qreal dpr = devicePixelRatioF();
    const QString key = QStringLiteral("%1|%2|%3|%4|%5|%6|%7")
        .arg(name, glyph.name(QColor::HexArgb), bg.name(QColor::HexArgb),
             border.name(QColor::HexArgb)).arg(size).arg(radius).arg(dpr, 0, 'f', 3);
    const auto cached = m_tileCache.constFind(key);
    if (cached != m_tileCache.constEnd()) return cached.value();
    QPixmap canvas(qRound(size * dpr), qRound(size * dpr));
    canvas.setDevicePixelRatio(dpr); canvas.fill(Qt::transparent);
    QPainter painter(&canvas); painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen); painter.setBrush(bg);
    painter.drawRoundedRect(QRectF(0.5, 0.5, size - 1, size - 1), radius, radius);
    if (border.isValid()) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(border, 1));
        painter.drawRoundedRect(QRectF(1, 1, size - 2, size - 2), radius, radius);
    }
    const QPixmap icon = glyphPixmap(name, glyph, qRound(size * 0.48));
    if (!icon.isNull()) {
        const qreal glyphRatio = icon.devicePixelRatio() > 0 ? icon.devicePixelRatio() : 1;
        const int logicalWidth = qRound(icon.width() / glyphRatio);
        const int logicalHeight = qRound(icon.height() / glyphRatio);
        painter.drawPixmap(QPoint((size - logicalWidth) / 2, (size - logicalHeight) / 2), icon);
    }
    painter.end();
    if (m_tileCache.size() >= 256) m_tileCache.clear();
    m_tileCache.insert(key, canvas);
    return canvas;
}

QPixmap OpenListWindow::typeTilePixmap(const OpenList::Entry &entry, int size) const {
    const int radius = qRound(size * 0.3);
    if (entry.directory)
        return tilePixmap("folder", QColor("#2F80ED"), QColor("#EDF6FF"), QColor("#D8E9FF"), size, radius);
    const QString lower = entry.name.toLower();
    if (lower.endsWith(QStringLiteral(".apk")))
        return tilePixmap("package", QColor("#139A56"), QColor("#EDFAF3"), QColor("#D7F1E2"), size, radius);
    if (lower.endsWith(QStringLiteral(".zip")))
        return tilePixmap("archive", QColor("#6E5AE6"), QColor("#F5F2FF"), QColor("#E5DDFF"), size, radius);
    if (lower.endsWith(QStringLiteral(".sh")))
        return tilePixmap("terminal", QColor("#D07A12"), QColor("#FFF8EB"), QColor("#FBE9C9"), size, radius);
    return tilePixmap("file", QColor("#64778D"), QColor("#F6F9FC"), QColor("#E6EDF4"), size, radius);
}

void OpenListWindow::syncChecks() {
    // 勾选状态由行选中状态驱动（选中集合是唯一真源），blockSignals 防止 itemChanged 递归。
    m_table->blockSignals(true);
    for (int row = 0; row < m_table->rowCount(); ++row) {
        auto *nameItem = m_table->item(row, 0);
        if (!nameItem) continue;
        if (!nameItem->flags().testFlag(Qt::ItemIsUserCheckable)) continue; // 文件夹行没有复选框。
        nameItem->setCheckState(nameItem->isSelected() ? Qt::Checked : Qt::Unchecked);
    }
    m_table->blockSignals(false);
}

void OpenListWindow::updateSelectionPanel() {
    // 已选面板按勾选顺序显示，和实际批量下载 / 安装顺序一致。
    const QList<QString> &paths = m_selectionOrder;
    QList<const OpenList::Entry*> picked;
    qint64 total = 0; bool unknown = false;
    for (const auto &path : paths) {
        const int index = m_entryIndex.value(path, -1);
        if (index < 0) continue;
        const auto &entry = m_entries[index];
        if (entry.directory) continue; // 文件夹不可勾选，已选列表只显示文件。
        picked.append(&entry);
        if (entry.size >= 0) total += entry.size; else unknown = true;
    }
    m_selectionMeta->setText(picked.isEmpty() ? QString()
        : QStringLiteral("%1 个文件 · %2").arg(picked.size())
              .arg(unknown ? QStringLiteral("大小未知") : QLocale().formattedDataSize(total)));
    m_clearSelection->setVisible(!picked.isEmpty());
    m_selectionEmpty->setVisible(picked.isEmpty());
    auto *listLayout = static_cast<QVBoxLayout*>(m_selectionList->layout());
    while (QLayoutItem *item = listLayout->takeAt(0)) {
        if (auto *widget = item->widget()) widget->deleteLater();
        delete item;
    }
    for (int i = 0; i < picked.size(); ++i) {
        const auto *entry = picked[i];
        // 行固定高度：面板纵向空间再紧张也不允许 layout 压缩行导致绘制重叠。
        auto *row = new QWidget(m_selectionList);
        row->setFixedHeight(26);
        auto *rowLayout = new QHBoxLayout(row); rowLayout->setContentsMargins(6,3,6,3); rowLayout->setSpacing(7);
        auto *tile = new QLabel(row); tile->setFixedSize(20,20); tile->setAlignment(Qt::AlignCenter);
        tile->setPixmap(typeTilePixmap(*entry, 20));
        auto *nameLabel = new QLabel(row);
        QFontMetrics metrics(nameLabel->font());
        nameLabel->setText(metrics.elidedText(entry->name, Qt::ElideMiddle, 132));
        nameLabel->setStyleSheet("font-size:10px;font-weight:500;color:#52677D;");
        auto *sizeLabel = new QLabel(row);
        sizeLabel->setText(entry->size >= 0 ? QLocale().formattedDataSize(entry->size) : QStringLiteral("—"));
        sizeLabel->setStyleSheet("font-size:9px;color:#9BAABA;");
        rowLayout->addWidget(tile); rowLayout->addWidget(nameLabel,1);
        rowLayout->addWidget(sizeLabel);
        listLayout->addWidget(row);
    }
    // 已选列表高度由布局分配：空间充足时最多显示三行完整条目，
    // 空间不足时缩小滚动；不能固定高度，否则会与下方控件重叠。
    m_selectionScroll->setVisible(!picked.isEmpty());
    if (!picked.isEmpty()) {
        constexpr int MaxVisibleRows = 3;
        const int visibleRows = qMin(picked.size(), MaxVisibleRows);
        const int naturalHeight = visibleRows * 26 + (visibleRows - 1) * 4 + 5;
        m_selectionScroll->setMaximumHeight(naturalHeight);
        m_selectionScroll->setMinimumHeight(1);
    } else {
        m_selectionScroll->setMaximumHeight(0);
    }
    // 全选本页 chip 高亮：当前目录内所有文件行都被选中时置亮。
    int fileRows = 0, selectedRows = 0;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        const auto *item = m_table->item(row, 0);
        if (!item || !(item->flags() & Qt::ItemIsSelectable)) continue;
        ++fileRows;
        if (item->isSelected()) ++selectedRows;
    }
    m_selectAll->setChecked(fileRows > 0 && selectedRows == fileRows);
}

void OpenListWindow::showTaskView(int view) {
    m_idleView->setVisible(view == 0);
    m_runView->setVisible(view == 1);
    m_successView->setVisible(view == 2);
    // 执行中把底部最近动态让给任务队列；日志仍可从底部状态条打开。
    if (m_recentPanel) m_recentPanel->setVisible(view != 1);
}

void OpenListWindow::updateRunProgress(qint64 received, qint64 total, double speed, const QString &footer) {
    m_progress->setRange(0, total > 0 ? 100 : 0);
    if (total > 0) {
        const int percent = received >= total ? 100 : qMin(99, int(received * 100 / total));
        m_progress->setValue(percent);
        m_runPercent->setText(QStringLiteral("%1%").arg(percent));
    } else {
        m_runPercent->setText(QStringLiteral("…"));
    }
    m_runStatsLeft->setText(QStringLiteral("%1 / %2").arg(QLocale().formattedDataSize(received),
        total >= 0 ? QLocale().formattedDataSize(total) : QStringLiteral("未知")));
    QString right = QStringLiteral("%1/s").arg(QLocale().formattedDataSize(qint64(speed)));
    if (speed > 1 && total > received)
        right += QStringLiteral(" · 剩余 %1s").arg(qMax<qint64>(1, qint64((total - received) / speed)));
    m_runStatsRight->setText(right);
    m_status->setText(footer);
}

void OpenListWindow::rebuildQueue() {
    auto *layout = static_cast<QVBoxLayout*>(m_queueBox->layout());
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (auto *widget = item->widget()) widget->deleteLater();
        delete item;
    }
    if (m_queue.isEmpty()) {
        m_queueBox->setVisible(false);
        m_queueScroll->setVisible(false);
        m_queueScroll->setMaximumHeight(0);
        return;
    }
    m_queueScroll->setVisible(true);
    m_queueBox->setVisible(true);
    constexpr int QueueRowHeight = 18;
    QPointer<QWidget> activeRow;
    for (int i = 0; i < m_queue.size(); ++i) {
        const bool done = i < m_queueIndex - 1 || m_installingBatch;
        const bool active = !done && i == m_queueIndex - 1 && !m_installingBatch;
        auto *row = new QWidget(m_queueBox);
        row->setFixedHeight(QueueRowHeight);
        if (active) activeRow = row;
        auto *rowLayout = new QHBoxLayout(row); rowLayout->setContentsMargins(0,0,0,0); rowLayout->setSpacing(6);
        auto *state = new QLabel(row); state->setFixedSize(14,14); state->setAlignment(Qt::AlignCenter);
        if (done) {
            state->setPixmap(tilePixmap("check", QColor("#FFFFFF"), QColor("#25B970"), QColor(), 14, 7));
        } else {
            state->setStyleSheet(active
                ? "border:1px solid #9CCAFB;background:#EEF7FF;border-radius:7px;"
                : "border:1px solid #DDE8F2;background:#F8FBFD;border-radius:7px;");
        }
        auto *name = new QLabel(row);
        QFontMetrics metrics(name->font());
        name->setText(metrics.elidedText(m_queue[i].name, Qt::ElideMiddle, 180));
        name->setStyleSheet(QStringLiteral("font-size:9.5px;color:%1;%2").arg(
            active ? "#5D738A" : "#8FA0B1", active ? "font-weight:600;" : QString()));
        auto *size = new QLabel(row);
        size->setText(m_queue[i].size >= 0 ? QLocale().formattedDataSize(m_queue[i].size) : QStringLiteral("—"));
        size->setStyleSheet("font-size:9px;color:#B0BECB;");
        rowLayout->addWidget(state); rowLayout->addWidget(name,1); rowLayout->addWidget(size);
        layout->addWidget(row);
    }
    layout->activate();
    // 内容高度由任务面板剩余空间约束；超出时滚动查看，不再压缩行或覆盖按钮。
    const int queueRows = m_queue.size();
    const int naturalHeight = 13 + queueRows * QueueRowHeight + qMax(0, queueRows - 1) * 5;
    m_queueScroll->setMaximumHeight(naturalHeight);
    m_queueScroll->setMinimumHeight(1);
    if (activeRow) {
        QTimer::singleShot(0, m_queueScroll, [this, activeRow] {
            if (activeRow && m_queueScroll && m_queueScroll->isVisible())
                m_queueScroll->ensureWidgetVisible(activeRow, 0, 6);
        });
    }
}

void OpenListWindow::appendLogHtml(const LogEntry &entry) {
    static const char *tags[] = {"INFO", "OK", "WARN"};
    static const char *tagColors[] = {"#80B8F6", "#6FD8A6", "#E8BD6C"};
    static const char *textColors[] = {"#BED0E1", "#B7DCCB", "#E8D0A6"};
    m_log->appendHtml(QStringLiteral(
        "<span style=\"color:#637B92;\">%1</span>&nbsp;&nbsp;"
        "<span style=\"color:%2;font-weight:700;\">%3</span>&nbsp;&nbsp;"
        "<span style=\"color:%4;\">%5</span>")
        .arg(entry.time, tagColors[entry.level], tags[entry.level], textColors[entry.level], entry.text.toHtmlEscaped()));
}

void OpenListWindow::renderLog() {
    m_log->setUpdatesEnabled(false);
    m_log->clear();
    for (const auto &entry : m_logEntries)
        if (m_logFilter == 0 || entry.level + 1 == m_logFilter) appendLogHtml(entry);
    m_log->setUpdatesEnabled(true);
    m_log->verticalScrollBar()->setValue(m_log->verticalScrollBar()->maximum());
}

void OpenListWindow::setLogFilter(int filter) {
    m_logFilter = filter;
    for (int i = 0; i < m_logFilters.size(); ++i)
        if (m_logFilters[i]) m_logFilters[i]->setChecked(i == filter);
    renderLog();
}

void OpenListWindow::toggleLogSheet(bool open) {
    if (m_logOpen == open) return;
    m_logOpen = open;
    m_scrim->setVisible(open);
    m_logSheet->setVisible(open);
    if (open) {
        m_scrim->raise();
        m_logSheet->raise();
        renderLog();
    }
}

void OpenListWindow::updateDrawerGeometry() {
    if (!m_scrim || !m_logSheet) return;
    const QSize size = this->size();
    m_scrim->setGeometry(0, 0, size.width(), size.height());
    m_logSheet->setGeometry(18, qMax(0, size.height() - 56 - 306), qMax(0, size.width() - 36), 306);
}

void OpenListWindow::updateRecent() {
    for (int i = 0; i < 3; ++i) {
        const int index = m_logEntries.size() - 3 + i;
        if (index < 0) {
            m_recentRow[i]->setText(i == 0 ? QStringLiteral("暂无动态") : QString());
            continue;
        }
        const auto &entry = m_logEntries[index];
        const QString text = entry.text.length() > 26 ? entry.text.left(26) + QStringLiteral("…") : entry.text;
        m_recentRow[i]->setText(QStringLiteral("• %1   %2").arg(text, entry.time.left(5)));
    }
}
