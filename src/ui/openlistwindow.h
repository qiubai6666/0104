#ifndef OPENLISTWINDOW_H
#define OPENLISTWINDOW_H
#include <QWidget>
#include "openlistservice.h"
#include "../services/mtmanagerservice.h"
class OpenListInstaller;
class QTableWidget; class QLineEdit; class QLabel; class QProgressBar;
class QPushButton; class QPlainTextEdit; class QFrame; class QResizeEvent;

class OpenListWindow final : public QWidget {
    Q_OBJECT
public:
    explicit OpenListWindow(QWidget *parent = nullptr, OpenListService *service = nullptr,
                            OpenListInstaller *installer = nullptr);
    bool isBusy() const { return m_busy; }
protected:
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    QPointer<QWidget> m_launcher;
    OpenListService *m_service;
    OpenListInstaller *m_installer;
    MtManagerService *m_mtService;
    QTableWidget *m_table;
    QLineEdit *m_search;
    QPushButton *m_up;
    QLabel *m_directory;
    QString m_currentDirectory;
    QLabel *m_count, *m_status;
    QPushButton *m_refresh, *m_cancel, *m_installSelected, *m_transferSelected, *m_installMt;
    QProgressBar *m_progress;
    QPlainTextEdit *m_log;
    QList<OpenList::Entry> m_entries;
    QSet<QString> m_selectedPaths; // 跨目录保留的选中项（remotePath）。
    OpenList::Entry m_selected;
    struct Downloaded { OpenList::Entry entry; QString file; };
    QList<OpenList::Entry> m_queue;
    QList<Downloaded> m_downloaded;
    QString m_serial;
    bool m_busy = false, m_downloading = false, m_mtTask = false;
    QString m_mtFile;
    int m_queueIndex = 0, m_installIndex = 0, m_pending = 0, m_succeeded = 0, m_failed = 0;
    bool m_transferOnly = false;
    bool m_installingBatch = false, m_cancelled = false;
    // 预览稿（TEMP/openlist-ui-preview-20261017）新增 UI 成员。
    QLabel *m_pill;                       // 面包屑右侧「N 项」计数胶囊。
    QLabel *m_deviceName, *m_deviceState; // 顶栏设备卡片两行文案。
    QLabel *m_deviceDot;                  // 设备头像右下角状态点（子控件）。
    QLabel *m_statusPill;                 // 任务面板状态 pill。
    QLabel *m_selectionMeta;              // 已选资源统计。
    QPushButton *m_selectAll, *m_clearSelection, *m_backToSelection;
    QWidget *m_idleView, *m_selectionList, *m_selectionEmpty;
    class QScrollArea *m_selectionScroll = nullptr;
    QWidget *m_runView, *m_queueBox, *m_successView;
    QLabel *m_runTile, *m_runFileName, *m_runFileSub, *m_runPercent, *m_runStatsLeft, *m_runStatsRight;
    QLabel *m_successOrb, *m_successTitle, *m_successMessage, *m_successStats;
    QLabel *m_targetName, *m_targetState;
    QLabel *m_footerDot, *m_updatedAt, *m_logCount, *m_logTotal;
    QPushButton *m_logButton, *m_openLogInline;
    QLabel *m_recentRow[3];
    QFrame *m_scrim, *m_logSheet;
    bool m_logOpen = false;
    struct LogEntry { QString time; QString text; int level; }; // 0 info / 1 ok / 2 warn。
    QList<LogEntry> m_logEntries;
    int m_logFilter = 0;
    QList<QPushButton*> m_logFilters;
    void setupUi();
    void refresh();
    void render();
    void activate(int row);
    void startSelection(bool transferOnly);
    void startBatch(const QList<OpenList::Entry> &entries, bool transferOnly = false);
    void appendSelection(const OpenList::Entry &selection, QList<OpenList::Entry> &expanded, QSet<QString> &paths) const;
    void advanceBatch();
    void setBusy(bool busy);
    void finish(const QString &message, bool ok = false);
    void log(const QString &text);
    void updateCount();
    void updateDevice();
    // 新增 UI 辅助。
    QPixmap glyphPixmap(const QString &name, const QColor &color, int size) const;
    QPixmap tilePixmap(const QString &name, const QColor &glyph, const QColor &bg, const QColor &border, int size, int radius) const;
    QPixmap typeTilePixmap(const OpenList::Entry &entry, int size) const;
    void syncChecks();
    void updateSelectionPanel();
    void showTaskView(int view); // 0 空闲选择 / 1 执行中 / 2 结束态。
    void updateRunProgress(qint64 received, qint64 total, double speed, const QString &footer);
    void rebuildQueue();
    void appendLogHtml(const LogEntry &entry);
    void renderLog();
    void setLogFilter(int filter);
    void toggleLogSheet(bool open);
    void updateDrawerGeometry();
    void updateRecent();
};
#endif
