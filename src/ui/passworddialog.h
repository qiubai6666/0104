#ifndef PASSWORDDIALOG_H
#define PASSWORDDIALOG_H

#include <QDialog>
#include <QString>

class QLabel;
class QLineEdit;

// 启动时的本地密码校验。界面和校验实现见 passworddialog.cpp。
class PasswordDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PasswordDialog(QWidget *parent = nullptr);

private slots:
    void onOkClicked();

private:
    void setupUI();
    void showError(const QString &message);
    void shakeAnimation();

    QLineEdit *passwordEdit = nullptr;
    QLabel *errorLabel = nullptr;
    QString correctPassword;
    int attemptCount = 0;
};

#endif // PASSWORDDIALOG_H
