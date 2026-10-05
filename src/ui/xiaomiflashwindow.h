#ifndef XIAOMIFLASHWINDOW_H
#define XIAOMIFLASHWINDOW_H
#include <QWidget>
#include <QPointer>
#include "xiaomiflashservice.h"
class QLineEdit;
class QLabel;
class QPushButton;
class QRadioButton;
class QProgressBar;
class QPlainTextEdit;
class XiaomiFlashWindow : public QWidget {
    Q_OBJECT
public:
    explicit XiaomiFlashWindow(QWidget *launcher = nullptr);
    bool isBusy() const { return m_service->isBusy(); }
protected:
    void closeEvent(QCloseEvent *event) override;
private:
    XiaomiFlashService *m_service;
    QPointer<QWidget> m_launcher;
    QLineEdit *m_path;
    QPushButton *m_choose, *m_start;
    QRadioButton *m_wipe, *m_keep, *m_lock;
    QProgressBar *m_progress;
    QLabel *m_status, *m_warning;
    QPlainTextEdit *m_log;
    Xiaomi::Mode mode() const;
    void startFlash();
    void setBusy(bool busy);
};
#endif
