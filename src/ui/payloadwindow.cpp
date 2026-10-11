#include "processmanager.h"
#include "payloadwindow.h"
#include "resourceextractor.h"
#include "uihelper.h"
#include <QCoreApplication>
#include <QFile>
#include <QRegularExpression>
#include <QScreen>
#include <QGuiApplication>
#include <QDesktopServices>
#include <QUrl>

PayloadWindow::PayloadWindow(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("Payload 提取");
    if (!parentWidget()) {
        setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        UIHelper::applyFloatingSurface(this);
    } else {
        // 嵌入控制中心面板：作为普通子页翻页展示。
        setWindowFlags(Qt::Widget);
    }
    setupUI();
    if (!parentWidget()) adjustSize();
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
    QLabel *urlLabel = new QLabel("Payload 文件：", container);
    urlLabel->setStyleSheet("QLabel { color: #2c3e50; background: transparent; font-size: 13px; }");
    mainLayout->addWidget(urlLabel);

    urlEdit = new QLineEdit(container);
    urlEdit->setPlaceholderText("粘贴 payload.bin 或 OTA ZIP 文件路径");
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

    // 提取镜像：boot / init_boot 可任意勾选（支持双选），并可自定义分区名。
    QLabel *imageLabel = new QLabel("提取镜像：", container);
    imageLabel->setStyleSheet("QLabel { color: #2c3e50; background: transparent; font-size: 13px; }");
    mainLayout->addWidget(imageLabel);

    const QString checkStyle = QStringLiteral(
        "QCheckBox { color:#2c3e50; background:transparent; font-size:12.5px; spacing:6px; }"
        "QCheckBox::indicator { width:16px; height:16px; border:1.5px solid #B9C6D2;"
        "border-radius:4px; background:#FFFFFF; }"
        "QCheckBox::indicator:hover { border-color:#649EB3; }"
        "QCheckBox::indicator:checked { background:#649EB3; border-color:#649EB3; }");
    QHBoxLayout *checkRow = new QHBoxLayout();
    checkRow->setSpacing(16);
    bootCheck = new QCheckBox("boot", container);
    bootCheck->setObjectName("PayloadBootCheck");
    bootCheck->setStyleSheet(checkStyle);
    bootCheck->setChecked(true);
    bootCheck->setCursor(Qt::PointingHandCursor);
    initBootCheck = new QCheckBox("init_boot", container);
    initBootCheck->setObjectName("PayloadInitBootCheck");
    initBootCheck->setStyleSheet(checkStyle);
    initBootCheck->setChecked(true);
    initBootCheck->setCursor(Qt::PointingHandCursor);
    checkRow->addWidget(bootCheck);
    checkRow->addWidget(initBootCheck);
    checkRow->addStretch();
    mainLayout->addLayout(checkRow);

    // 自定义分区：支持英文逗号分隔的多个分区名。
    customEdit = new QLineEdit(container);
    customEdit->setObjectName("PayloadCustomPartitionsEdit");
    customEdit->setPlaceholderText("自定义分区名，多个用英文逗号分隔（如 vbmeta,dtbo）");
    customEdit->setMinimumHeight(34);
    customEdit->setStyleSheet(R"(
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
    mainLayout->addWidget(customEdit);

    // 提取进度
    progressBar = new QProgressBar(container);
    progressBar->setRange(0, 100);
    progressBar->setValue(0);
    progressBar->setTextVisible(true);
    progressBar->setFixedHeight(20);
    progressBar->setStyleSheet(R"(
        QProgressBar {
            background-color: #FFFFFF;
            border: 1px solid #D0DEEB;
            border-radius: 6px;
            font-size: 11px;
            color: #2c3e50;
            text-align: center;
        }
        QProgressBar::chunk {
            background-color: #649EB3;
            border-radius: 5px;
        }
    )");
    progressBar->hide();
    mainLayout->addWidget(progressBar);

    // 提示信息
    QLabel *infoLabel = new QLabel("勾选或输入要提取的镜像分区\n输出路径：桌面/IMG 文件夹", container);
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
        QMessageBox::warning(this, "警告", "请输入 Payload 文件路径！");
        return;
    }

    if (url.startsWith("http://", Qt::CaseInsensitive) ||
        url.startsWith("https://", Qt::CaseInsensitive)) {
        QMessageBox::warning(this, "警告",
            "Payload 工具不支持远程链接，请先将文件下载到本地再输入路径！");
        return;
    }

    // 收集提取目标：勾选的 boot / init_boot + 自定义分区名（支持双选与多分区）。
    QStringList targets;
    if (bootCheck->isChecked())
        targets << QStringLiteral("boot");
    if (initBootCheck->isChecked())
        targets << QStringLiteral("init_boot");
    const QStringList customNames =
        customEdit->text().split(QRegularExpression("[,，\\s]+"), Qt::SkipEmptyParts);
    for (QString name : customNames) {
        name = name.trimmed().toLower();
        if (name.isEmpty())
            continue;
        if (!QRegularExpression("^[a-z0-9_.-]+$").match(name).hasMatch()) {
            QMessageBox::warning(this, "警告",
                QString("自定义分区名无效：%1\n仅允许字母、数字、下划线、点和中划线。").arg(name));
            return;
        }
        targets << name;
    }
    targets.removeDuplicates();
    if (targets.isEmpty()) {
        QMessageBox::warning(this, "警告", "请至少勾选一个镜像或输入自定义分区名！");
        return;
    }

    QString outputPath = getOutputPath();
    QString payloadTool = ResourceExtractor::getResourcePath() + "/payload-dumper-go.exe";

    // 检查提取工具是否存在
    if (!QFile::exists(payloadTool)) {
        QMessageBox::warning(this, "错误", "找不到 payload-dumper-go.exe 文件！");
        return;
    }

    // 执行 payload-dumper-go
    QProcess *process = ProcessManager::createProcess(this);
    process->setWorkingDirectory(ResourceExtractor::getResourcePath());

    QStringList args;
    // -p：仅提取所选分区（boot / init_boot / 自定义），避免全量解包。
    args << "--output" << outputPath;
    args << "-p" << targets.join(",");
    args << url;

    // 显示正在处理的提示并开启进度条（解析工具输出的百分比，单调递增）。
    extractButton->setEnabled(false);
    extractButton->setText("提取中...");
    progressBar->setValue(0);
    progressBar->show();
    auto progressReader = [this](const QByteArray &chunk) {
        static const QRegularExpression percentPattern("(\\d+(?:\\.\\d+)?)\\s*%");
        QRegularExpressionMatchIterator it = percentPattern.globalMatch(
            QString::fromLocal8Bit(chunk));
        while (it.hasNext()) {
            bool ok = false;
            const double value = it.next().captured(1).toDouble(&ok);
            if (!ok)
                continue;
            const int percent = qBound(0, qRound(value), 100);
            if (percent > progressBar->value())
                progressBar->setValue(percent);
        }
    };
    connect(process, &QProcess::readyReadStandardOutput, this,
            [this, process, progressReader] { progressReader(process->readAllStandardOutput()); });
    connect(process, &QProcess::readyReadStandardError, this,
            [this, process, progressReader] { progressReader(process->readAllStandardError()); });

    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, process, outputPath](int exitCode, QProcess::ExitStatus exitStatus) {
        extractButton->setEnabled(true);
        extractButton->setText("开始提取");

        if (exitCode == 0 && exitStatus == QProcess::NormalExit) {
            progressBar->setValue(100);
            // 提取成功，直接打开 IMG 文件夹
            QDesktopServices::openUrl(QUrl::fromLocalFile(outputPath));
            accept();
        } else {
            progressBar->setValue(0);
            progressBar->hide();
            QString error = process->readAllStandardError();
            QMessageBox::critical(this, "错误",
                QString("提取失败！\n错误信息：%1").arg(error.isEmpty() ? "未知错误" : error));
        }

        process->deleteLater();
    });

    process->start(payloadTool, args);
}
