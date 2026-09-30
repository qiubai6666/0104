#ifndef PASSWORDDIALOG_H
#define PASSWORDDIALOG_H

#include <QDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QGraphicsDropShadowEffect>
#include <QTimer>
#include <QPoint>

class PasswordDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PasswordDialog(QWidget *parent = nullptr)
        : QDialog(parent)
    {
        setWindowTitle("密码验证");
        setFixedSize(300, 200);
        
        // 无边框窗口，启用透明背景
        setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
        setAttribute(Qt::WA_TranslucentBackground);
        
        // 创建主容器（用于圆角背景）- 浅蓝色风格
        QWidget *container = new QWidget(this);
        container->setGeometry(0, 0, 300, 200);
        container->setStyleSheet(R"(
            QWidget {
                background-color: rgba(180, 210, 220, 240);
                border-radius: 12px;
            }
        )");
        
        // 创建布局
        QVBoxLayout *mainLayout = new QVBoxLayout(container);
        mainLayout->setContentsMargins(20, 15, 20, 15);
        mainLayout->setSpacing(10);
        
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
        
        mainLayout->addSpacing(5);
        
        // 密码输入框
        passwordEdit = new QLineEdit(container);
        passwordEdit->setEchoMode(QLineEdit::Password);
        passwordEdit->setPlaceholderText("请输入密码");
        passwordEdit->setStyleSheet(R"(
            QLineEdit {
                padding: 8px 12px;
                border: 1px solid rgba(255, 255, 255, 100);
                border-radius: 6px;
                font-size: 12px;
                background-color: rgba(255, 255, 255, 150);
                color: #2c3e50;
            }
            QLineEdit:focus {
                border: 1px solid rgba(255, 255, 255, 200);
                background-color: rgba(255, 255, 255, 200);
            }
        )");
        mainLayout->addWidget(passwordEdit);
        
        // 按钮布局
        QHBoxLayout *buttonLayout = new QHBoxLayout();
        buttonLayout->setSpacing(8);
        
        QPushButton *cancelButton = new QPushButton("取消", container);
        cancelButton->setFixedHeight(32);
        cancelButton->setCursor(Qt::PointingHandCursor);
        cancelButton->setStyleSheet(R"(
            QPushButton {
                padding: 6px 18px;
                background-color: rgba(160, 190, 200, 200);
                color: #2c3e50;
                border: 1px solid rgba(255, 255, 255, 100);
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
        okButton->setFixedHeight(32);
        okButton->setCursor(Qt::PointingHandCursor);
        okButton->setStyleSheet(R"(
            QPushButton {
                padding: 6px 18px;
                background-color: rgba(100, 160, 180, 220);
                color: white;
                border: 1px solid rgba(255, 255, 255, 100);
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
                padding: 3px;
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
        connect(passwordEdit, &QLineEdit::returnPressed, this, &PasswordDialog::onOkClicked);
        
        // 设置焦点
        passwordEdit->setFocus();
        
        // 添加阴影效果
        QGraphicsDropShadowEffect *shadow = new QGraphicsDropShadowEffect(this);
        shadow->setBlurRadius(20);
        shadow->setColor(QColor(0, 0, 0, 60));
        shadow->setOffset(0, 5);
        container->setGraphicsEffect(shadow);
    }
    


private slots:
    void onOkClicked()
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
            
            if (attemptCount >= 3) {
                showError("密码错误次数过多，程序将退出！");
                QTimer::singleShot(1500, this, &QDialog::reject);
            } else {
                showError(QString("密码错误！还有 %1 次机会").arg(3 - attemptCount));
                passwordEdit->clear();
                passwordEdit->setFocus();
                
                // 添加抖动动画
                shakeAnimation();
            }
        }
    }
    
    void showError(const QString &message)
    {
        if (errorLabel) {
            errorLabel->setText("❌ " + message);
            errorLabel->show();
            
            // 2秒后自动隐藏
            QTimer::singleShot(2000, errorLabel, &QLabel::hide);
        }
    }
    
    void shakeAnimation()
    {
        // 简单的抖动效果
        QPoint originalPos = pos();
        int shakeAmount = 10;
        
        QTimer::singleShot(0, this, [this, originalPos, shakeAmount]() { move(originalPos.x() - shakeAmount, originalPos.y()); });
        QTimer::singleShot(50, this, [this, originalPos, shakeAmount]() { move(originalPos.x() + shakeAmount, originalPos.y()); });
        QTimer::singleShot(100, this, [this, originalPos, shakeAmount]() { move(originalPos.x() - shakeAmount, originalPos.y()); });
        QTimer::singleShot(150, this, [this, originalPos]() { move(originalPos); });
    }

private:
    QLineEdit *passwordEdit;
    QLabel *errorLabel = nullptr;
    QString correctPassword = "123456...";  // 默认密码
    int attemptCount = 0;
};

#endif // PASSWORDDIALOG_H
