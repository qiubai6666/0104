#ifndef OPENLISTWINDOW_H
#define OPENLISTWINDOW_H
#include <QWidget>
#include "openlistservice.h"
#include "../services/mtmanagerservice.h"
class OpenListInstaller;
class QTableWidget; class QLineEdit; class QLabel; class QProgressBar;
class QPushButton; class QPlainTextEdit;

class OpenListWindow final : public QWidget {
    Q_OBJECT
public:
    explicit OpenListWindow(QWidget *parent = nullptr, OpenListService *service = nullptr,
                            OpenListInstaller *installer = nullptr);
    bool isBusy() const { return m_busy; }
protected:
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;
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
    QLabel *m_count, *m_device, *m_status;
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
};
#endif
