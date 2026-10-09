#include "processmanager.h"
#include "payloadwindow.h"
#include "resourceextractor.h"
#include "uihelper.h"
#include <QCoreApplication>
#include <QFile>
#include <QScreen>
#include <QGuiApplication>
#include <QDesktopServices>
#include <QUrl>

PayloadWindow::PayloadWindow(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("Payload 提取");
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    UIHelper::applyFloatingSurface(this);
    setupUI();
    adjustSize();
}

PayloadWindow::~PayloadWindow()
{
}

void PayloadWindow::setupUI()
{
    QWidget *container = this;
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(UIHelper::DrawerHorizontalMargin, UIHelper::DrawerVerticalMargin,
                                   UIHelper::DrawerHorizontalMargin, UIHelper::DrawerVerticalMargin);
    mainLayout->setSpacing(6);
    QLabel *titleLabel = new QLabel("Payload 分区提取", container);
    titleLabel->setStyleSheet("color:#2c3e50; background:transparent; font-size:17px; font-weight:normal;");
    mainLayout->addWidget(titleLabel);
    mainLayout->addSpacing(3);
    // URL 输入
    QLabel *urlLabel = new QLabel("Payload 链接：", container);
    urlLabel->setStyleSheet("QLabel { color: #2c3e50; background: transparent; font-size: 13px; }");
    mainLayout->addWidget(urlLabel);
    
    urlEdit = new QLineEdit(container);
    urlEdit->setPlaceholderText("粘贴链接或路径");
    urlEdit->setMinimumHeight(34);
    urlEdit->setStyleSheet(R"(
        QLineEdit {
            padding: 6px 10px;
            border: 1px solid #D0DEEB;
            border-radius: 6px;
            font-size: 12px;
            background-color: #F7FAFD;
            color: #2c3e50;
        }
        QLineEdit:focus {
            border: 1px solid #83AECA;
            background-color: #FFFFFF;
        }
    )");
    mainLayout->addWidget(urlEdit);
    
    // 提示信息
    QLabel *infoLabel = new QLabel("Payload 中的全部可用镜像将被提取\n输出路径：桌面/IMG 文件夹", container);
    infoLabel->setStyleSheet("QLabel { color: #666; background: transparent; font-size: 11px; }");
    mainLayout->addWidget(infoLabel);
    
    mainLayout->addStretch();
    
    // 按钮
    QHBoxLayout *buttonLayout = new QHBoxLayout();
    buttonLayout->setSpacing(10);
    
    cancelButton = new QPushButton("取消", container);
    cancelButton->setFixedHeight(UIHelper::DrawerButtonHeight);
    cancelButton->setCursor(Qt::PointingHandCursor);
    cancelButton->setStyleSheet(R"(
        QPushButton {
            padding: 6px 12px;
            background-color: #F3F7FB;
            color: #2c3e50;
            border: 1px solid #D0DEEB;
            border-radius: 6px;
            font-size: 13px;
            font-weight: normal;
        }
        QPushButton:hover {
            background-color: #EAF2F9;
        }
        QPushButton:pressed {
            background-color: #DEEAF5;
        }
        QPushButton:focus {
            border-color: #83AECA;
        }
    )");
    
    extractButton = new QPushButton("开始提取", container);
    extractButton->setFixedHeight(UIHelper::DrawerButtonHeight);
    extractButton->setCursor(Qt::PointingHandCursor);
    extractButton->setStyleSheet(R"(
        QPushButton {
            padding: 6px 12px;
            background-color: #649EB3;
            color: white;
            border: 1px solid #D0DEEB;
            border-radius: 6px;
            font-size: 13px;
            font-weight: normal;
        }
        QPushButton:hover {
            background-color: #578FA6;
        }
        QPushButton:pressed {
            background-color: #477F96;
        }
        QPushButton:focus {
            border-color: #3F7894;
        }
        QPushButton:disabled {
            background-color: #A8C3D0;
            border-color: #A8C3D0;
            color: white;
        }
    )");
    
    buttonLayout->addStretch();
    buttonLayout->addWidget(cancelButton);
    buttonLayout->addWidget(extractButton);
    
    mainLayout->addLayout(buttonLayout);
    
    // 连接信号
    connect(extractButton, &QPushButton::clicked, this, &PayloadWindow::onExtractClicked);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
}

QString PayloadWindow::getOutputPath()
{
    // 获取桌面路径
    QString desktopPath = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    QString outputPath = desktopPath + "/IMG";
    
    // 创建 IMG 文件夹
    QDir dir;
    if (!dir.exists(outputPath)) {
        dir.mkpath(outputPath);
    }
    
    return outputPath;
}

void PayloadWindow::onExtractClicked()
{
    QString url = urlEdit->text().trimmed();
    
    if (url.isEmpty()) {
        QMessageBox::warning(this, "警告", "请输入 Payload 链接或路径！");
        return;
    }
    
    QString outputPath = getOutputPath();
    QString payloadExe = ResourceExtractor::getResourcePath() + "/payload.exe";
    
    // 检查 payload.exe 是否存在
    if (!QFile::exists(payloadExe)) {
        QMessageBox::warning(this, "错误", "找不到 payload.exe 文件！");
        return;
    }
    
    // 执行 payload.exe
    QProcess *process = ProcessManager::createProcess(this);
    process->setWorkingDirectory(ResourceExtractor::getResourcePath());
    
    QStringList args;
    args << "--out" << outputPath;
    args << url;
    
    // 显示正在处理的提示
    extractButton->setEnabled(false);
    extractButton->setText("提取中...");
    
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, process, outputPath](int exitCode, QProcess::ExitStatus exitStatus) {
        extractButton->setEnabled(true);
        extractButton->setText("开始提取");
        
        if (exitCode == 0 && exitStatus == QProcess::NormalExit) {
            // 提取成功，直接打开 IMG 文件夹
            QDesktopServices::openUrl(QUrl::fromLocalFile(outputPath));
            accept();
        } else {
            QString error = process->readAllStandardError();
            QMessageBox::critical(this, "错误", 
                QString("提取失败！\n错误信息：%1").arg(error.isEmpty() ? "未知错误" : error));
        }
        
        process->deleteLater();
    });
    
    process->start(payloadExe, args);
}
