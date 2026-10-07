#ifndef OPENLISTWINDOW_H
#define OPENLISTWINDOW_H
#include <QWidget>
#include "openlistservice.h"
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
    QTableWidget *m_tables[2];
    QLineEdit *m_search[2];
    QPushButton *m_up[2];
    QLabel *m_directory[2];
    QString m_currentDirectory[2];
    QLabel *m_counts[2], *m_device, *m_status;
    QPushButton *m_refresh, *m_cancel, *m_installSelected;
    QProgressBar *m_progress;
    QPlainTextEdit *m_log;
    QList<OpenList::Entry> m_entries[2];
    OpenList::Entry m_selected;
    struct Downloaded { OpenList::Entry entry; QString file; };
    QList<OpenList::Entry> m_queue;
    QList<Downloaded> m_downloaded;
    QString m_serial;
    bool m_busy = false, m_downloading = false;
    int m_queueIndex = 0, m_installIndex = 0, m_pending = 0, m_succeeded = 0, m_failed = 0;
    bool m_installingBatch = false, m_cancelled = false;
    void setupUi();
    void refresh();
    void render(bool module);
    void activate(bool module, int row);
    void startBatch(const QList<OpenList::Entry> &entries);
    void advanceBatch();
    void setBusy(bool busy);
    void finish(const QString &message, bool ok = false);
    void log(const QString &text);
    void updateDevice();
};
#endif
