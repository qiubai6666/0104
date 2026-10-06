#include "passworddialog.h"
#include "version.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace {
constexpr int MaxPasswordAttempts = 3;
}

PasswordDialog::PasswordDialog(QWidget *parent)
    : QDialog(parent)
    , correctPassword(DEFAULT_PASSWORD)
{
    setupUI();
}

void PasswordDialog::setupUI()
{
    setWindowTitle("密码验证");
    setFixedSize(300, 164);

    // 无边框窗口，启用透明背景
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);

    // 创建主容器（用于圆角背景）- 浅蓝色风格
    QWidget *container = new QWidget(this);
    container->setObjectName("passwordSurface");
    container->setGeometry(rect());
    container->setStyleSheet(R"(
        QWidget#passwordSurface {
            background-color: rgba(211, 230, 237, 250);
            border: 1px solid rgba(255, 255, 255, 180);
            border-radius: 10px;
        }
    )");

    // 创建布局
    QVBoxLayout *mainLayout = new QVBoxLayout(container);
    mainLayout->setContentsMargins(20, 12, 20, 12);
    mainLayout->setSpacing(8);

    // 标题标签
    QLabel *titleLabel = new QLabel("🔒 密码验证", container);
    titleLabel->setStyleSheet(R"(
        QLabel {
            font-size: 15px;
            font-weight: bold;
            color: #2c3e50;
            background: transparent;
        }
    )");
    titleLabel->setAlignment(Qt::AlignCenter);
    mainLayout->addWidget(titleLabel);

    mainLayout->addSpacing(4);

    // 密码输入框
    passwordEdit = new QLineEdit(container);
    passwordEdit->setEchoMode(QLineEdit::Password);
    passwordEdit->setPlaceholderText("请输入密码");
    passwordEdit->setStyleSheet(R"(
        QLineEdit {
            padding: 6px 12px;
            border: 1px solid rgba(100, 160, 180, 90);
            border-radius: 6px;
            font-size: 12px;
            background-color: rgba(255, 255, 255, 150);
            color: #2c3e50;
        }
        QLineEdit:focus {
            border: 1px solid rgba(100, 160, 180, 210);
            background-color: rgba(255, 255, 255, 200);
        }
    )");
    mainLayout->addWidget(passwordEdit);

    // 按钮布局
    QHBoxLayout *buttonLayout = new QHBoxLayout();
    buttonLayout->setSpacing(8);

    QPushButton *cancelButton = new QPushButton("取消", container);
    cancelButton->setAutoDefault(false);
    cancelButton->setFixedHeight(32);
    cancelButton->setCursor(Qt::PointingHandCursor);
    cancelButton->setStyleSheet(R"(
        QPushButton {
            padding: 6px 18px;
            background-color: rgba(160, 190, 200, 200);
            color: #2c3e50;
            border: 1px solid rgba(100, 160, 180, 90);
            border-radius: 6px;
            font-size: 12px;
            font-weight: bold;
        }
        QPushButton:hover {
            background-color: rgba(140, 170, 180, 220);
        }
        QPushButton:pressed {
            background-color: rgba(120, 150, 160, 240);
        }
    )");

    QPushButton *okButton = new QPushButton("确定", container);
    // 统一由默认确定按钮处理回车，避免校验后继续触发取消或重复校验。
    okButton->setDefault(true);
    okButton->setFixedHeight(32);
    okButton->setCursor(Qt::PointingHandCursor);
    okButton->setStyleSheet(R"(
        QPushButton {
            padding: 6px 18px;
            background-color: rgba(100, 160, 180, 220);
            color: white;
            border: 1px solid rgba(100, 160, 180, 90);
            border-radius: 6px;
            font-size: 12px;
            font-weight: bold;
        }
        QPushButton:hover {
            background-color: rgba(80, 140, 160, 240);
        }
        QPushButton:pressed {
            background-color: rgba(60, 120, 140, 250);
        }
    )");

    buttonLayout->addWidget(cancelButton);
    buttonLayout->addWidget(okButton);

    mainLayout->addLayout(buttonLayout);

    // 错误提示标签（添加到布局中）
    errorLabel = new QLabel(container);
    errorLabel->setStyleSheet(R"(
        QLabel {
            color: #f44336;
            font-size: 11px;
            background: transparent;
            padding: 2px 3px;
        }
    )");
    errorLabel->setAlignment(Qt::AlignCenter);
    errorLabel->setMinimumHeight(20);
    errorLabel->setMaximumHeight(20);
    errorLabel->hide();
    mainLayout->addWidget(errorLabel);

    // 连接信号
    connect(okButton, &QPushButton::clicked, this, &PasswordDialog::onOkClicked);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);

    // 设置焦点
    passwordEdit->setFocus();

}

void PasswordDialog::onOkClicked()
{
    QString inputPassword = passwordEdit->text();

    if (inputPassword.isEmpty()) {
        showError("密码不能为空！");
        passwordEdit->setFocus();
        return;
    }

    if (inputPassword == correctPassword) {
        accept();  // 密码正确，关闭对话框并返回 Accepted
    } else {
        attemptCount++;

        if (attemptCount >= MaxPasswordAttempts) {
            showError("密码错误次数过多，程序将退出！");
            QTimer::singleShot(1500, this, &QDialog::reject);
        } else {
            showError(QString("密码错误！还有 %1 次机会").arg(MaxPasswordAttempts - attemptCount));
            passwordEdit->clear();
            passwordEdit->setFocus();

            // 添加抖动动画
            shakeAnimation();
        }
    }
}

void PasswordDialog::showError(const QString &message)
{
    if (errorLabel) {
        errorLabel->setText("❌ " + message);
        errorLabel->show();

        // 2秒后自动隐藏
        QTimer::singleShot(2000, errorLabel, &QLabel::hide);
    }
}

void PasswordDialog::shakeAnimation()
{
    // 简单的抖动效果
    QPoint originalPos = pos();
    int shakeAmount = 10;

    QTimer::singleShot(0, this, [this, originalPos, shakeAmount]() { move(originalPos.x() - shakeAmount, originalPos.y()); });
    QTimer::singleShot(50, this, [this, originalPos, shakeAmount]() { move(originalPos.x() + shakeAmount, originalPos.y()); });
    QTimer::singleShot(100, this, [this, originalPos, shakeAmount]() { move(originalPos.x() - shakeAmount, originalPos.y()); });
    QTimer::singleShot(150, this, [this, originalPos]() { move(originalPos); });
}
