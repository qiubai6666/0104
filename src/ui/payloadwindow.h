#ifndef PAYLOADWINDOW_H
#define PAYLOADWINDOW_H

#include <atomic>

#include <QDialog>
#include <QCheckBox>
#include <QFuture>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProcess>
#include <QMessageBox>
#include <QStandardPaths>
#include <QDir>

class PayloadWindow : public QDialog
{
    Q_OBJECT

public:
    explicit PayloadWindow(QWidget *parent = nullptr);
    ~PayloadWindow();

private slots:
    void onExtractClicked();

private:
    void setupUI();
    QString getOutputPath();
    // 远程链接：与欧加线刷云提取同源，HTTP Range 流式只下载所选分区数据，
    // 不落盘完整 payload / OTA ZIP。
    void startRemoteExtraction(const QString &url, const QStringList &targets);
    // 本地路径：调用 payload-dumper-go 提取。
    void runExtractor(const QString &localPayloadFile, const QStringList &targets);

    QLineEdit *urlEdit;
    QCheckBox *bootCheck;
    QCheckBox *initBootCheck;
    QLineEdit *customEdit;
    QProgressBar *progressBar;
    std::atomic_bool m_cancel{false};
    QFuture<void> m_future;
    QPushButton *extractButton;
    QPushButton *cancelButton;
};

#endif // PAYLOADWINDOW_H
