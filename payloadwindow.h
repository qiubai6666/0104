#ifndef PAYLOADWINDOW_H
#define PAYLOADWINDOW_H

#include <QDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QCheckBox>
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
    
    QLineEdit *urlEdit;
    QCheckBox *bootCheckBox;
    QCheckBox *initBootCheckBox;
    QPushButton *extractButton;
    QPushButton *cancelButton;
};

#endif // PAYLOADWINDOW_H
