#include "payloadwindow.h"
#include "resourceextractor.h"
#include <QCoreApplication>
#include <QFile>
#include <QScreen>
#include <QGuiApplication>
#include <QDesktopServices>
#include <QUrl>
#include <QPainter>
#include <QPainterPath>
#include <QStyleOptionButton>
#include <QStyle>

namespace {
// 自绘勾选框，统一高 DPI 下的外观，同时保留 QCheckBox 的键盘和点击行为。
class PayloadCheckBox : public QCheckBox
{
public:
    explicit PayloadCheckBox(const QString &text, QWidget *parent)
        : QCheckBox(text, parent)
    {
        setCursor(Qt::PointingHandCursor);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QStyleOptionButton option;
        initStyleOption(&option);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRect indicator = style()->subElementRect(QStyle::SE_CheckBoxIndicator, &option, this);
        const bool checked = option.state & QStyle::State_On;
        const bool hovered = option.state & QStyle::State_MouseOver;
        const QColor fill = checked ? QColor(hovered ? "#578FA6" : "#649EB3")
                                    : QColor(hovered ? "#F3F7FB" : "#FFFFFF");
        const QColor border = hasFocus() ? QColor("#83AECA")
                                        : QColor(checked ? "#5C93A8" : "#CBD9E6");
        painter.setPen(QPen(border, hasFocus() ? 1.5 : 1.0));
        painter.setBrush(fill);
        painter.drawRoundedRect(QRectF(indicator).adjusted(0.75, 0.75, -0.75, -0.75), 4, 4);
        if (checked) {
            const qreal x = indicator.x(), y = indicator.y();
            QPainterPath check;
            check.moveTo(x + 4.5, y + 9);
            check.lineTo(x + 7.5, y + 12);
            check.lineTo(x + 13.5, y + 5.5);
            painter.setPen(QPen(Qt::white, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(check);
        }
        QStyleOptionButton labelOption = option;
        labelOption.rect = style()->subElementRect(QStyle::SE_CheckBoxContents, &option, this);
        style()->drawControl(QStyle::CE_CheckBoxLabel, &labelOption, &painter, this);
    }
};
} // namespace

PayloadWindow::PayloadWindow(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("Payload 提取");
    setFixedSize(450, 252);
    
    // 无边框窗口，启用透明背景
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    
    setupUI();
    
    // 窗口在屏幕中央显示
    QScreen *screen = QGuiApplication::primaryScreen();
    QRect screenGeometry = screen->geometry();
    int x = (screenGeometry.width() - width()) / 2;
    int y = (screenGeometry.height() - height()) / 2;
    move(x, y);
}

PayloadWindow::~PayloadWindow()
{
}

void PayloadWindow::setupUI()
{
    // 创建主容器
    QWidget *container = new QWidget(this);
    container->setObjectName("payloadSurface");
    container->setGeometry(rect());
    container->setStyleSheet(R"(
        QWidget#payloadSurface {
            background-color: #FFFFFF;
            border: 1px solid #D6E2EE;
            border-radius: 12px;
        }
    )");
    
    QVBoxLayout *mainLayout = new QVBoxLayout(container);
    mainLayout->setContentsMargins(20, 16, 20, 16);
    mainLayout->setSpacing(8);
    
    // 标题
    QLabel *titleLabel = new QLabel("Payload 分区提取", container);
    titleLabel->setStyleSheet(R"(
        QLabel {
            font-size: 16px;
            font-weight: bold;
            color: #2c3e50;
            background: #F3F7FC;
            border: 1px solid #E7EEF6;
            border-radius: 8px;
            padding: 4px 0;
        }
    )");
    titleLabel->setAlignment(Qt::AlignCenter);
    mainLayout->addWidget(titleLabel);
    
    // URL 输入
    QLabel *urlLabel = new QLabel("Payload 链接：", container);
    urlLabel->setStyleSheet("QLabel { color: #2c3e50; background: transparent; font-size: 13px; }");
    mainLayout->addWidget(urlLabel);
    
    urlEdit = new QLineEdit(container);
    urlEdit->setPlaceholderText("粘贴链接或路径");
    urlEdit->setMinimumHeight(36);
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
    
    // 分区选择
    QLabel *partLabel = new QLabel("选择要提取的分区：", container);
    partLabel->setStyleSheet("QLabel { color: #2c3e50; background: transparent; font-size: 13px; }");
    mainLayout->addWidget(partLabel);
    
    QHBoxLayout *checkBoxLayout = new QHBoxLayout();
    checkBoxLayout->setSpacing(15);  // 保留原有复选框横向位置
    
    bootCheckBox = new PayloadCheckBox("boot", container);
    bootCheckBox->setStyleSheet(R"(
        QCheckBox {
            color: #2c3e50;
            background: transparent;
            font-size: 13px;
        }
        QCheckBox::indicator {
            width: 18px;
            height: 18px;
        }
    )");
    bootCheckBox->setChecked(true);
    
    initBootCheckBox = new PayloadCheckBox("init_boot", container);
    initBootCheckBox->setStyleSheet(R"(
        QCheckBox {
            color: #2c3e50;
            background: transparent;
            font-size: 13px;
        }
        QCheckBox::indicator {
            width: 18px;
            height: 18px;
        }
    )");
    
    checkBoxLayout->addWidget(bootCheckBox);
    checkBoxLayout->addWidget(initBootCheckBox);
    checkBoxLayout->addStretch();
    
    mainLayout->addLayout(checkBoxLayout);
    
    // 提示信息
    QLabel *infoLabel = new QLabel("输出路径：桌面/IMG 文件夹", container);
    infoLabel->setStyleSheet("QLabel { color: #666; background: transparent; font-size: 11px; }");
    mainLayout->addWidget(infoLabel);
    
    mainLayout->addStretch();
    
    // 按钮
    QHBoxLayout *buttonLayout = new QHBoxLayout();
    buttonLayout->setSpacing(10);
    
    cancelButton = new QPushButton("取消", container);
    cancelButton->setFixedHeight(36);
    cancelButton->setCursor(Qt::PointingHandCursor);
    cancelButton->setStyleSheet(R"(
        QPushButton {
            padding: 6px 20px;
            background-color: #F3F7FB;
            color: #2c3e50;
            border: 1px solid #D0DEEB;
            border-radius: 6px;
            font-size: 13px;
            font-weight: bold;
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
    extractButton->setFixedHeight(36);
    extractButton->setCursor(Qt::PointingHandCursor);
    extractButton->setStyleSheet(R"(
        QPushButton {
            padding: 6px 20px;
            background-color: #649EB3;
            color: white;
            border: 1px solid #D0DEEB;
            border-radius: 6px;
            font-size: 13px;
            font-weight: bold;
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
    
    // 检查是否选择了分区
    if (!bootCheckBox->isChecked() && !initBootCheckBox->isChecked()) {
        QMessageBox::warning(this, "警告", "请至少选择一个分区！");
        return;
    }
    
    // 构建分区参数
    QStringList partitions;
    if (bootCheckBox->isChecked()) {
        partitions << "boot";
    }
    if (initBootCheckBox->isChecked()) {
        partitions << "init_boot";
    }
    
    QString partitionsArg = partitions.join(",");
    QString outputPath = getOutputPath();
    QString payloadExe = ResourceExtractor::getResourcePath() + "/payload.exe";
    
    // 检查 payload.exe 是否存在
    if (!QFile::exists(payloadExe)) {
        QMessageBox::warning(this, "错误", "找不到 payload.exe 文件！");
        return;
    }
    
    // 执行 payload.exe
    QProcess *process = new QProcess(this);
    process->setWorkingDirectory(ResourceExtractor::getResourcePath());
    
    QStringList args;
    args << "--partitions" << partitionsArg;
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
