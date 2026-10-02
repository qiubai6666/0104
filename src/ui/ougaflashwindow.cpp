#include "ougaflashwindow.h"
#include "deviceoperationlease.h"
#include "ougaflashplanner.h"
#include "ougaflashservice.h"
#include "ougapreparation.h"
#include "ougaromservice.h"
#include "resourceextractor.h"
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleOptionButton>
#include <QStyledItemDelegate>
#include <QSvgRenderer>
#include <QTableWidget>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>
using namespace Ouga;
namespace {
QIcon referenceIcon(const QString &name) {
  QSvgRenderer svg(":/ouga/" + name + ".svg");
  QPixmap pixmap(32, 32);
  pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap);
  svg.render(&painter);
  painter.end();
  pixmap.setDevicePixelRatio(2);
  return QIcon(pixmap);
}
QGroupBox *card(const QString &title, QWidget *parent) {
  auto box = new QGroupBox(title, parent);
  box->setObjectName(title);
  return box;
}
QGridLayout *cardLayout(QGroupBox *box) {
  auto layout = new QGridLayout(box);
  // The stylesheet already reserves the title margin. Do not count it twice.
  layout->setContentsMargins(10, 8, 10, 6);
  layout->setHorizontalSpacing(8);
  layout->setVerticalSpacing(6);
  return layout;
}
QPushButton *button(const QString &text, const QString &name, QWidget *parent,
                    const QString &tone = {}) {
  auto result = new QPushButton(text, parent);
  result->setObjectName(name);
  result->setProperty("tone", tone);
  result->setFixedHeight(32);
  result->setCursor(Qt::PointingHandCursor);
  return result;
}
QLineEdit *entry(const QString &placeholder, const QString &name,
                 QWidget *parent) {
  auto result = new QLineEdit(parent);
  result->setObjectName(name);
  result->setPlaceholderText(placeholder);
  result->setFixedHeight(32);
  result->setAcceptDrops(false); // The page handles file/folder drops.
  return result;
}
QCheckBox *option(const QString &text, const QString &name, QWidget *parent) {
  auto result = new QCheckBox(text, parent);
  result->setObjectName(name);
  result->setCursor(Qt::PointingHandCursor);
  result->setFixedHeight(26);
  return result;
}
void exclusiveOptions(const QList<QCheckBox *> &boxes, QObject *context) {
  // SMT permits all three flash-method checkboxes to be unchecked.
  for (auto box : boxes)
    QObject::connect(box, &QCheckBox::toggled, context, [boxes, box](bool on) {
      if (on)
        for (auto other : boxes)
          if (other != box)
            other->setChecked(false);
    });
}
class MarqueeCheckBox : public QCheckBox {
public:
  explicit MarqueeCheckBox(QWidget *parent)
      : QCheckBox("自动救砖模式", parent) {
    setToolTip("自动救砖模式");
    setAccessibleName(text());
    clock.start();
    auto timer = new QTimer(this);
    QObject::connect(timer, &QTimer::timeout, this, [this] {
      if (isVisible())
        update();
    });
    timer->start(33);
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QStyleOptionButton opt;
    initStyleOption(&opt);
    opt.text.clear();
    QPainter painter(this);
    style()->drawControl(QStyle::CE_CheckBox, &opt, &painter, this);
    const QRect indicator =
        style()->subElementRect(QStyle::SE_CheckBoxIndicator, &opt, this);
    QRect textArea(indicator.right() + 5, 0, 52, height());
    painter.setClipRect(textArea);
    painter.setPen(isEnabled() ? QColor("#475569") : QColor("#ADB4C0"));
    const int time = int(clock.elapsed() % 5000);
    const qreal offset =
        time < 800 ? 0 : (time > 4200 ? 87 : (time - 800) * 87.0 / 3400);
    const int baseline =
        (height() + fontMetrics().ascent() - fontMetrics().descent()) / 2;
    for (int i = 0; i < 2; ++i)
      painter.drawText(QPointF(textArea.left() - offset + i * 87, baseline),
                       text());
  }

private:
  QElapsedTimer clock;
};
class CheckDelegate : public QStyledItemDelegate {
public:
  using QStyledItemDelegate::QStyledItemDelegate;
  void paint(QPainter *painter, const QStyleOptionViewItem &option,
             const QModelIndex &index) const override {
    QStyleOptionViewItem background(option);
    initStyleOption(&background, index);
    background.features &= ~QStyleOptionViewItem::HasCheckIndicator;
    QStyledItemDelegate::paint(painter, background, index);
    QStyleOptionButton check;
    check.rect = QRect(option.rect.center().x() - 8,
                       option.rect.center().y() - 8, 16, 16);
    check.state = (option.state & QStyle::State_Enabled) |
                  (index.data(Qt::CheckStateRole).toInt() == Qt::Checked
                       ? QStyle::State_On
                       : QStyle::State_Off);
    option.widget->style()->drawPrimitive(QStyle::PE_IndicatorCheckBox, &check,
                                          painter, option.widget);
  }
  bool editorEvent(QEvent *event, QAbstractItemModel *model,
                   const QStyleOptionViewItem &option,
                   const QModelIndex &index) override {
    if (!(option.state & QStyle::State_Enabled))
      return false;
    if (event->type() == QEvent::MouseButtonRelease ||
        (event->type() == QEvent::KeyPress &&
         static_cast<QKeyEvent *>(event)->key() == Qt::Key_Space))
      return model->setData(
          index,
          index.data(Qt::CheckStateRole).toInt() == Qt::Checked ? Qt::Unchecked
                                                                : Qt::Checked,
          Qt::CheckStateRole);
    return false;
  }
};
class TransferProgress : public QProgressBar {
public:
  using QProgressBar::QProgressBar;

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QRectF track = rect().adjusted(0, 0, -1, -1);
    painter.setBrush(QColor("#F1F3F7"));
    painter.setPen(QColor("#DCE1E8"));
    painter.drawRoundedRect(track, 4, 4);
    qreal ratio =
        maximum() > minimum()
            ? qBound(0.0, qreal(value() - minimum()) / (maximum() - minimum()),
                     1.0)
            : 0.0;
    if (ratio > 0) {
      QPainterPath clip;
      clip.addRoundedRect(track, 4, 4);
      painter.save();
      painter.setClipPath(clip);
      QLinearGradient gradient(track.topLeft(), track.topRight());
      gradient.setColorAt(0, QColor("#87CEEB"));
      gradient.setColorAt(1, QColor("#4A90E2"));
      painter.fillRect(QRectF(1, 1, (width() - 2) * ratio, height() - 2),
                       gradient);
      painter.restore();
    }
    painter.setPen(QColor("#333333"));
    painter.drawText(rect().adjusted(8, 0, -8, 0),
                     Qt::AlignVCenter | Qt::AlignLeft,
                     property("rate").toString());
    painter.drawText(rect().adjusted(8, 0, -8, 0),
                     Qt::AlignVCenter | Qt::AlignRight,
                     QString::number(int(ratio * 100)) + "%");
  }
};
QString uniqueWork(const QString &directory, const QString &name) {
  return QDir(directory).filePath(name + "-" +
                                  QUuid::createUuid().toString(QUuid::Id128));
}
QString selectItem(QWidget *parent, const QString &title, const QString &prompt,
                   const QStringList &items, int width = 470) {
  QDialog dialog(parent);
  dialog.setObjectName("OugaSelectionDialog");
  dialog.setWindowTitle(title);
  dialog.resize(width, 560);
  auto layout = new QVBoxLayout(&dialog);
  layout->setContentsMargins(18, 18, 18, 18);
  auto heading = new QLabel(title);
  heading->setStyleSheet("font-size:17px; font-weight:600; color:#263142;");
  layout->addWidget(heading);
  layout->addWidget(new QLabel(prompt));
  auto list = new QListWidget;
  list->addItems(items);
  list->setStyleSheet(
      "QListWidget { background:#FAFBFD; color:#334155; border:1px solid "
      "#E2E8F0; border-radius:8px; padding:4px; font-size:12.5px; } "
      "QListWidget::item { padding:7px; } QListWidget::item:selected { "
      "background:#F8ECFF; color:#334155; }");
  layout->addWidget(list);
  auto buttons =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  buttons->button(QDialogButtonBox::Ok)->setText("确定");
  buttons->button(QDialogButtonBox::Cancel)->setText("取消");
  layout->addWidget(buttons);
  buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
  for (auto b : {buttons->button(QDialogButtonBox::Ok),
                 buttons->button(QDialogButtonBox::Cancel)})
    b->setFixedSize(88, 32);
  QObject::connect(
      list, &QListWidget::currentRowChanged, &dialog, [buttons](int row) {
        buttons->button(QDialogButtonBox::Ok)->setEnabled(row >= 0);
      });
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
    if (list->currentItem() && !list->currentItem()->isHidden())
      dialog.accept();
  });
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog,
                   &QDialog::reject);
  QObject::connect(list, &QListWidget::itemDoubleClicked, &dialog,
                   &QDialog::accept);
  return dialog.exec() == QDialog::Accepted && list->currentItem()
             ? list->currentItem()->text()
             : QString();
}
} // namespace

class OugaFlashOverlay : public QWidget {
public:
  explicit OugaFlashOverlay(QWidget *parent) : QWidget(parent) {
    setObjectName("OugaFlashOverlay");
    auto timer = new QTimer(this);
    QObject::connect(timer, &QTimer::timeout, this, [this] {
      if (isVisible())
        update();
    });
    timer->start(250);
    hide();
  }
  void begin() {
    clock.start();
    percent = 0;
    detail = "准备刷写";
    show();
    raise();
  }
  void advance(int value, const QString &stage) {
    percent = value;
    detail = stage;
    update();
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), QColor(255, 255, 255, 170));
    QRectF ring((width() - 150) / 2.0 + 6, (height() - 180) / 2.0 + 6, 138,
                138);
    painter.setPen(QPen(QColor("#E2E8F0"), 12));
    painter.drawEllipse(ring);
    QLinearGradient gradient(ring.topLeft(), ring.bottomRight());
    gradient.setColorAt(0, QColor("#DDB8F7"));
    gradient.setColorAt(1, QColor("#CA91F3"));
    painter.setPen(QPen(QBrush(gradient), 12, Qt::SolidLine, Qt::RoundCap));
    painter.drawArc(ring, 90 * 16, -percent * 360 * 16 / 100);
    QFont font("Microsoft YaHei UI");
    font.setPixelSize(24);
    font.setWeight(QFont::Bold);
    painter.setFont(font);
    painter.setPen(QColor("#1E90FF"));
    painter.drawText(QRectF(ring.left(), ring.top() + 42, 138, 35),
                     Qt::AlignCenter, QString::number(percent) + "%");
    font.setPixelSize(12);
    font.setWeight(QFont::Normal);
    painter.setFont(font);
    painter.setPen(QColor("#718096"));
    painter.drawText(QRectF(ring.left() - 75, ring.top() + 79, 288, 24),
                     Qt::AlignCenter,
                     fontMetrics().elidedText(detail, Qt::ElideRight, 280));
    font.setPixelSize(18);
    painter.setFont(font);
    painter.setPen(QColor("#B876DD"));
    painter.drawText(QRectF(ring.left() - 40, ring.bottom() + 14, 218, 28),
                     Qt::AlignCenter,
                     QString("耗时 %1:%2")
                         .arg(clock.elapsed() / 60000)
                         .arg(clock.elapsed() / 1000 % 60, 2, 10, QChar('0')));
  }

private:
  int percent = 0;
  QString detail;
  QElapsedTimer clock;
};

OugaFlashWindow::OugaFlashWindow(QWidget *parent, OugaCommandRunner *runner,
                                 const QString &logDirectory)
    : QWidget(parent), m_logDirectory(logDirectory) {
  setWindowFlags(Qt::Window);
  setWindowTitle("秋白工作室 · 欧加线刷");
  setObjectName("OujiaFlashView");
  setMinimumSize(866, 729);
  resize(866, 729);
  setAcceptDrops(true);
  QFont font("Microsoft YaHei UI");
  font.setPixelSize(12);
  setFont(font);
  setStyleSheet(R"(
    OugaFlashWindow { background:#F7F8FC; }
    QGroupBox { background:white; border:1px solid #E2E6ED; border-radius:8px;
      margin-top:8px; color:#263142; font-weight:600; font-size:12px; }
    QGroupBox::title { subcontrol-origin:margin; subcontrol-position:top left;
      left:10px; padding:0 5px; background:white; }
    QPushButton { background:white; color:#475569; border:1px solid #D8DEE8;
      border-radius:6px; font-size:12px; font-weight:400; padding:0 5px; }
    QPushButton:hover { border-color:#B876DD; background:#F8F3FC; }
    QPushButton:pressed { background:#F0E7F8; }
    QPushButton[tone="sky"] { background:#87CEEB; color:white; border-color:#E6E6E6; }
    QPushButton[tone="purple"] { background:#B876DD; color:white; border-color:#E6E6E6; }
    QPushButton[tone="repair"] { background:#F8ECFF; border-color:#E6E6E6; }
    QPushButton[tone="stop"] { background:#F18D96; color:white; border-color:#E6E6E6; }
    QPushButton:disabled { color:#ADB4C0; border-color:#E2E6ED; }
    QLineEdit, QComboBox { background:white; color:#64748B; border:1px solid #DDE2EA;
      border-radius:3px; padding:0 9px; font-size:12px; font-weight:400; }
    QLineEdit:focus, QComboBox:focus { border-color:#B876DD; }
    QComboBox { color:#475569; padding-right:23px; }
    QComboBox::drop-down { border:0; width:22px; }
    QComboBox QAbstractItemView { background:white; color:#475569;
      selection-background-color:#F8ECFF; selection-color:#475569; }
    QCheckBox { color:#475569; font-size:11.5px; font-weight:400; spacing:4px; }
    QCheckBox:disabled, QLabel:disabled { color:#ADB4C0; }
    QLabel { color:#475569; font-weight:400; }
    QTableWidget { background:white; alternate-background-color:#FCFBFD;
      color:#334155; border:1px solid #E7E9EF; font-size:12px;
      selection-background-color:#87CEEB; selection-color:#263142; }
    QTableWidget::item { border-bottom:1px solid #F0F1F5; padding:0 4px; }
    QTableWidget::item:hover { background:#F6F1FF; }
    QHeaderView::section { background:#F5F3F8; color:#475569; border:0;
      border-bottom:1px solid #E7E9EF; font-weight:600; }
    QPlainTextEdit { background:#FAFBFD; color:#334155; border:0; padding:7px;
      font-family:"Cascadia Mono","Microsoft YaHei UI","Consolas";
      font-size:11.75px; font-weight:400; }
    QScrollBar:vertical { width:8px; background:#FAFBFD; margin:0; }
    QScrollBar::handle:vertical { background:#D8DEE8; border-radius:4px; min-height:24px; }
    QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }
  )");
  m_service =
      new OugaFlashService(runner ? runner : new OugaProcessRunner(this), this);
  m_prepare = new OugaPreparation(this);
  m_rom = new OugaRomService(this);
  auto rootLayout = new QVBoxLayout(this);
  rootLayout->setContentsMargins(10, 10, 10, 10);
  m_canvas = new QWidget(this);
  m_canvas->setMinimumSize(846, 709);
  rootLayout->addWidget(m_canvas);
  buildPage(0);
  buildPage(1);
  m_settings = card("刷写设置", m_canvas);
  auto settings = cardLayout(m_settings);
  auto modeRow = new QHBoxLayout;
  modeRow->setSpacing(0);
  modeRow->setContentsMargins(8, 0, 0, 0);
  auto label = new QLabel("刷写模式：");
  label->setStyleSheet("font-weight:600;");
  modeRow->addWidget(label);
  modeRow->addSpacing(14);
  m_full = option("全量包模式", "FullPackageModeCheckBox", m_settings);
  m_sales = option("售后包模式", "AfterSalesPackageModeCheckBox", m_settings);
  modeRow->addWidget(m_full);
  modeRow->addSpacing(18);
  modeRow->addWidget(m_sales);
  modeRow->addStretch();
  settings->addLayout(modeRow, 0, 0);
  m_validation = new QWidget;
  m_validation->setObjectName("PartitionTableValidationSettingPanel");
  m_validation->setToolTip("分区表校验仅适用于全量包模式；关闭后仍检查所选镜像"
                           "、目标、容量和设备状态");
  auto validation = new QHBoxLayout(m_validation);
  validation->setContentsMargins(8, 0, 0, 0);
  validation->setSpacing(0);
  label = new QLabel("分区表校验（实验性功能）：");
  label->setStyleSheet("font-weight:600;");
  validation->addWidget(label);
  validation->addSpacing(14);
  m_validateOn =
      option("开启", "PartitionTableValidationEnabledCheckBox", m_validation);
  m_validateOff =
      option("关闭", "PartitionTableValidationDisabledCheckBox", m_validation);
  validation->addWidget(m_validateOn);
  validation->addSpacing(18);
  validation->addWidget(m_validateOff);
  validation->addStretch();
  settings->addWidget(m_validation, 1, 0);
  m_full->setChecked(true);
  m_validateOn->setChecked(true);
  for (auto check : {m_full, m_sales})
    connect(check, &QCheckBox::toggled, this, [this, check](bool on) {
      if (m_loading)
        return;
      selectPackage(on ? check == m_sales : check == m_full);
    });
  for (auto check : {m_validateOn, m_validateOff})
    connect(check, &QCheckBox::toggled, this, [this, check](bool on) {
      QSignalBlocker enabled(m_validateOn), disabled(m_validateOff);
      bool use = on ? check == m_validateOn : check == m_validateOff;
      m_validateOn->setChecked(use);
      m_validateOff->setChecked(!use);
    });
  m_partitions = card("分区表", m_canvas);
  auto tableLayout = cardLayout(m_partitions);
  tableLayout->setContentsMargins(10, 18, 10, 10);
  m_table = new QTableWidget(0, 4, m_partitions);
  m_table->setObjectName("OugaPartitionTableDataGrid");
  m_table->setHorizontalHeaderLabels({"", "名称", "大小", "文件路径"});
  m_table->setShowGrid(false);
  m_table->setWordWrap(false);
  m_table->setAlternatingRowColors(true);
  m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
  m_table->setEditTriggers(QAbstractItemView::DoubleClicked |
                           QAbstractItemView::EditKeyPressed);
  m_table->verticalHeader()->hide();
  m_table->verticalHeader()->setDefaultSectionSize(29);
  m_table->horizontalHeader()->setFixedHeight(32);
  m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
  m_table->setColumnWidth(0, 35);
  m_table->setColumnWidth(1, 120);
  m_table->setColumnWidth(2, 90);
  m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
  m_table->setItemDelegateForColumn(0, new CheckDelegate(m_table));
  tableLayout->addWidget(m_table);
  m_selectAll = new QCheckBox(m_table->horizontalHeader());
  m_selectAll->setObjectName("SelectAllCheckBox");
  m_selectAll->setTristate(true);
  m_selectAll->setGeometry(9, 8, 16, 16);
  connect(m_selectAll, &QCheckBox::checkStateChanged, this,
          [this](Qt::CheckState state) {
            if (m_loading || isBusy())
              return;
            m_loading = true;
            for (int i = 0; i < m_table->rowCount(); ++i) {
              bool selected = state != Qt::Unchecked;
              m_pages[m_page].images[i].selected = selected;
              m_table->item(i, 0)->setCheckState(selected ? Qt::Checked
                                                          : Qt::Unchecked);
            }
            m_loading = false;
            updateSelection();
          });
  connect(m_table, &QTableWidget::itemChanged, this,
          &OugaFlashWindow::tableChanged);
  m_overlay = new OugaFlashOverlay(m_partitions);
  m_logs = card("执行日志", m_canvas);
  auto logs = cardLayout(m_logs);
  logs->setContentsMargins(1, 18, 1, 1);
  m_log = new QPlainTextEdit(m_logs);
  m_log->setObjectName("OugaFlashLogTextBox");
  m_log->setReadOnly(true);
  m_log->setMaximumBlockCount(5000);
  m_log->setLineWrapMode(QPlainTextEdit::WidgetWidth);
  logs->addWidget(m_log);
  m_progress = new TransferProgress(m_canvas);
  m_progress->setObjectName("FlashProgressBar");
  m_progress->setRange(0, 100);
  m_progress->setValue(0);
  m_progress->setProperty("rate", "0MB/s");
  m_stop = button("停止操作", "OugaFlashStopPanel", m_canvas, "stop");
  m_stop->setIcon(referenceIcon("power"));
  m_stop->setIconSize(QSize(16, 16));
  m_stop->setToolTip(
      "当前写入结束后停止，不回滚已写内容，不杀死正在刷写的进程");
  connect(m_stop, &QPushButton::clicked, this, &OugaFlashWindow::requestStop);
  connect(m_service, &OugaFlashService::log, this, &OugaFlashWindow::log);
  connect(m_prepare, &OugaPreparation::log, this, &OugaFlashWindow::log);
  connect(m_rom, &OugaRomService::log, this, &OugaFlashWindow::log);
  connect(m_service, &OugaFlashService::busyChanged, this,
          &OugaFlashWindow::updateBusy);
  connect(m_prepare, &OugaPreparation::busyChanged, this,
          &OugaFlashWindow::updateBusy);
  connect(m_service, &OugaFlashService::devicesFound, this,
          [this](const QStringList &serials) { m_serials = serials; });
  connect(m_service, &OugaFlashService::deviceReady, this,
          [this](const Device &device) { m_device = device; });
  connect(m_service, &OugaFlashService::finished, this,
          &OugaFlashWindow::serviceFinished);
  connect(m_prepare, &OugaPreparation::prepared, this,
          [this](const QVector<Partition> &images, const QString &directory) {
            QVector<Partition> copy = images;
            if (m_task == Task::Super)
              for (auto &image : copy)
                if (baseName(image.name) != "super")
                  for (const auto &previous : m_pages[m_page].images)
                    if (baseName(previous.name) == baseName(image.name))
                      image.selected = previous.selected;
            display(copy, m_task == Task::Super ? m_pages[m_page].directory
                                                : directory);
          });
  connect(m_prepare, &OugaPreparation::archiveExtracted, this,
          [this](const QString &directory) { m_archiveRoot = directory; });
  connect(m_prepare, &OugaPreparation::finished, this,
          &OugaFlashWindow::preparationFinished);
  connect(m_prepare, &OugaPreparation::adbDevicesFound, this,
          [this](const QStringList &serials) { m_serials = serials; });
  connect(
      m_prepare, &OugaPreparation::arbRead, this,
      [this](const QString &file, quint32 index) {
        m_currentArb = file;
        log(QString("当前槽 xbl_config 镜像 ARB = %1；已保存可信比较基准：%2")
                .arg(index)
                .arg(file));
      });
  connect(m_rom, &OugaRomService::downloaded, this,
          [this](const QString &file) { m_downloaded = file; });
  connect(m_rom, &OugaRomService::finished, this,
          &OugaFlashWindow::networkFinished);
  connect(m_rom, &OugaRomService::progress, this,
          [this](qint64 bytes, qint64 total) {
            if (!m_transferClock.isValid()) {
              m_transferClock.start();
              m_lastBytes = bytes;
            }
            qint64 elapsed = m_transferClock.elapsed();
            if (elapsed >= 500) {
              m_progress->setProperty(
                  "rate", QString::number((bytes - m_lastBytes) * 1000.0 /
                                              elapsed / 1048576,
                                          'f', 1) +
                              "MB/s");
              m_transferClock.restart();
              m_lastBytes = bytes;
            }
            m_progress->setValue(
                total > 0 ? int(qMin<qint64>(99, bytes * 100 / total)) : 0);
            m_progress->update();
          });
  connect(m_service, &OugaFlashService::progress, this,
          [this](int writes, int total, const QString &stage) {
            int percent =
                stage == "全部步骤成功"
                    ? 100
                    : (total > 0 ? qMin(99, writes * 100 / (total + 1)) : 0);
            m_progress->setValue(percent);
            m_progress->setProperty(
                "rate", stage == "全部步骤成功" ? "已完成" : "刷写中...");
            m_progress->update();
            m_overlay->advance(percent, stage);
          });
  connect(m_service, &OugaFlashService::checkpoint, this,
          [this](const Plan &plan) {
            const quint64 generation = m_generation;
            const bool accepted = !m_stopRequested && confirm(plan);
            m_service->confirmCheckpoint(accepted && !m_stopRequested &&
                                         generation == m_generation);
          });
  selectPackage(false);
  layoutCards();
  updateBusy();
}

void OugaFlashWindow::buildPage(int index) {
  Page &page = m_pages[index];
  const bool sales = index == 1;
  page.widget = new QWidget(m_canvas);
  page.widget->setObjectName(sales ? "AfterSalesContent"
                                   : "StandardFlashContent");
  page.cloud = card("快捷云提取", page.widget);
  auto cloud = cardLayout(page.cloud);
  page.url =
      entry("全量包链接 or Payload.bin路径...",
            sales ? "AfterSalesBinUrlTextBox" : "BinUrlTextBox", page.cloud);
  cloud->addWidget(page.url, 0, 0, 1, 2);
  page.preset = new QComboBox(page.cloud);
  page.preset->setObjectName(sales ? "AfterSalesPayloadPartitionComboBox"
                                   : "PayloadPartitionComboBox");
  page.preset->setEditable(true);
  page.preset->setFixedHeight(32);
  page.preset->addItems(
      {"请选择快捷提取方案 ↓", "boot", "init_boot", "高通修复FastbootD关键分区",
       "联发科修复FastbootD关键分区", "云解包方案-从云端提取线刷文件"});
  page.extract = button("提取分区",
                        sales ? "AfterSalesExtractPartitionButton"
                              : "ExtractPartitionButton",
                        page.cloud);
  page.extract->setFixedWidth(104);
  cloud->addWidget(page.preset, 1, 0);
  cloud->addWidget(page.extract, 1, 1);
  cloud->setColumnStretch(0, 1);
  connect(page.extract, &QPushButton::clicked, this,
          [this] { extractPayload(false); });
  connect(page.preset, &QComboBox::currentIndexChanged, this, [this](int i) {
    if (i == 5)
      log("已选择云解包方案，点击提取分区按钮开始云端解包");
  });
  page.files = card("刷写文件", page.widget);
  auto files = cardLayout(page.files);
  // Preserve SMT's 92 px card and 74 px after-sales controls without clipping.
  files->setContentsMargins(10, sales ? 4 : 8, 10, sales ? 0 : 4);
  if (!sales) {
    page.payloadFile = entry("请选择Payload.bin文件或全量包Zip文件...",
                             "PayloadFilePathTextBox", page.files);
    auto selectPayload =
        button("选择", "SelectPayloadFileButton", page.files, "sky");
    selectPayload->setFixedWidth(84);
    selectPayload->setIcon(referenceIcon("folder"));
    selectPayload->setIconSize(QSize(16, 16));
    files->addWidget(page.payloadFile, 0, 0);
    files->addWidget(selectPayload, 0, 1);
    connect(selectPayload, &QPushButton::clicked, this, [this] {
      QString file = QFileDialog::getOpenFileName(
          this, "选择Payload.bin文件或全量包ZIP文件", {},
          "Payload.bin或ZIP文件 (*.bin *.zip);;所有文件 (*)");
      if (!file.isEmpty())
        m_pages[0].payloadFile->setText(file);
    });
  }
  page.folder = entry(
      sales ? "选择一个散包文件夹或将散包拖动到此自动选择..."
            : "请选择解包好的文件夹或解包输出路径...",
      sales ? "AfterSalesFlashPackTextBox" : "FolderPathTextBox", page.files);
  auto choose = button(
      "选择", sales ? "AfterSalesSelectFolderButton" : "SelectFolderButton",
      page.files, "sky");
  choose->setIcon(referenceIcon("folder"));
  choose->setIconSize(QSize(16, 16));
  choose->setFixedWidth(sales ? 105 : 84);
  if (sales) {
    page.folder->setFixedHeight(74);
    choose->setFixedHeight(74);
  }
  files->addWidget(page.folder, sales ? 0 : 1, 0);
  files->addWidget(choose, sales ? 0 : 1, 1);
  files->setColumnStretch(0, 1);
  connect(choose, &QPushButton::clicked, this, [this] {
    QString directory = QFileDialog::getExistingDirectory(
        this,
        m_page ? "选择售后散包文件夹" : "选择解包好的文件夹或解包输出路径",
        m_pages[m_page].folder->text());
    if (!directory.isEmpty())
      load(directory);
  });
  connect(page.folder, &QLineEdit::editingFinished, this, [this, index] {
    if (!m_loading && index == m_page && !isBusy()) {
      QString path = m_pages[index].folder->text().trimmed();
      if (QFileInfo(path).isDir() && path != m_pages[index].directory)
        load(path);
    }
  });
  page.actions = card("操作", page.widget);
  auto actions = cardLayout(page.actions);
  auto choices = new QWidget(page.actions);
  choices->setFixedHeight(26);
  auto named = [sales](const char *full, const char *after) {
    return QString::fromLatin1(sales ? after : full);
  };
  page.wipe = option("清除数据",
                     named("ClearDataCheckBox", "AfterSalesClearDataCheckBox"),
                     choices);
  page.reboot = option(
      "自动重启",
      named("AutoRebootOugaCheckBox", "AfterSalesAutoRebootCheckBox"), choices);
  page.wipe->setChecked(true);
  page.reboot->setChecked(true);
  page.wipe->setGeometry(0, 0, 78, 26);
  page.reboot->setGeometry(78, 0, 78, 26);
  if (!sales) {
    page.ab = option("AB通刷", "FlashABCheckBox", choices);
    page.force = option("强力线刷", "FixSuperCheckBox", choices);
    page.onlyFbd = option("仅FBD", "PureFBDCheckBox", choices);
    page.ab->setGeometry(156, 0, 67, 26);
    page.force->setGeometry(223, 0, 77, 26);
    page.onlyFbd->setGeometry(300, 0, 65, 26);
    page.ab->setToolTip("同时刷入物理分区A/B两槽，逻辑分区写入所选启动槽");
    page.force->setToolTip("部分大分区刷不进去时修复super，默认刷A槽位");
    page.onlyFbd->setToolTip("没有Fastboot的机型使用；联发科设备请勿勾选");
    exclusiveOptions({page.ab, page.force, page.onlyFbd}, this);
  } else {
    page.fbd = option("FBD模式", "AfterSalesFastbootDModeToggle", choices);
    page.fb = option("FB模式", "AfterSalesBootloaderModeToggle", choices);
    page.rescue = new MarqueeCheckBox(choices);
    page.rescue->setObjectName("AfterSalesAutoBrickRecoveryModeToggle");
    page.rescue->setCursor(Qt::PointingHandCursor);
    page.fbd->setGeometry(156, 0, 75, 26);
    page.fb->setGeometry(231, 0, 63, 26);
    page.rescue->setGeometry(294, 0, 72, 26);
    exclusiveOptions({page.fbd, page.fb, page.rescue}, this);
    page.fbd->setChecked(true);
  }
  page.repair =
      button("修复FastbootD",
             named("FixFastbootDButton", "AfterSalesFixFastbootDButton"),
             choices, "repair");
  page.repair->setFixedHeight(26);
  page.repair->setGeometry(370, 0, 105, 26);
  actions->addWidget(choices, 0, 0);
  auto buttons = new QHBoxLayout;
  buttons->setSpacing(8);
  page.arb = button("ARB熔断检测",
                    named("ArbFuseCheckButton", "AfterSalesArbFuseCheckButton"),
                    page.actions);
  page.arb->setIcon(referenceIcon("shield"));
  page.arb->setIconSize(QSize(16, 16));
  page.unpack =
      button("解包Payload",
             named("UnpackPayloadButton", "AfterSalesUnpackPayloadButton"),
             page.actions, "purple");
  page.start = button("开始线刷",
                      named("StartFlashButton", "AfterSalesStartFlashButton"),
                      page.actions, "purple");
  for (auto item : {page.arb, page.unpack, page.start})
    buttons->addWidget(item, 1);
  actions->addLayout(buttons, 1, 0);
  page.unpack->setEnabled(!sales);
  if (sales)
    page.unpack->setToolTip("解包Payload仅在全量包模式下可用");
  connect(page.unpack, &QPushButton::clicked, this,
          [this] { extractPayload(true); });
  connect(page.arb, &QPushButton::clicked, this, &OugaFlashWindow::arbDialog);
  connect(page.start, &QPushButton::clicked, this, [this] { startFlash(); });
  connect(page.repair, &QPushButton::clicked, this,
          [this] { startFlash(true); });
}
void OugaFlashWindow::selectPackage(bool afterSales) {
  if (m_loading)
    return;
  QSignalBlocker full(m_full), sales(m_sales);
  m_full->setChecked(!afterSales);
  m_sales->setChecked(afterSales);
  m_page = afterSales ? 1 : 0;
  m_pages[0].widget->setVisible(!afterSales);
  m_pages[1].widget->setVisible(afterSales);
  m_validation->setEnabled(!afterSales && !isBusy());
  display(m_pages[m_page].images, m_pages[m_page].directory);
}
void OugaFlashWindow::layoutCards() {
  const int extraWidth = qMax(0, m_canvas->width() - 846);
  const int extraHeight = qMax(0, m_canvas->height() - 709);
  const int left = 497 + extraWidth * 59 / 100;
  const int rightX = left + 7, right = m_canvas->width() - rightX;
  for (auto &page : m_pages) {
    page.widget->setGeometry(m_canvas->rect());
    page.cloud->setGeometry(0, 0, left, 95);
    page.files->setGeometry(0, 98, left, 92);
    page.actions->setGeometry(0, 613 + extraHeight, left, 96);
  }
  m_settings->setGeometry(rightX, 0, right, 95);
  m_partitions->setGeometry(0, 197, left, 406 + extraHeight);
  m_logs->setGeometry(rightX, 98, right, 559 + extraHeight);
  m_progress->setGeometry(rightX + 1, 673 + extraHeight, right - 88, 26);
  m_stop->setGeometry(m_canvas->width() - 80, 671 + extraHeight, 80, 32);
  m_overlay->setGeometry(11, 18, left - 22, m_partitions->height() - 29);
  m_selectAll->raise();
}
void OugaFlashWindow::resizeEvent(QResizeEvent *event) {
  QWidget::resizeEvent(event);
  if (m_settings && m_overlay)
    layoutCards();
}
bool OugaFlashWindow::isBusy() const {
  return m_task != Task::None || m_service->busy() || m_prepare->busy() ||
         m_rom->busy();
}
void OugaFlashWindow::updateBusy() {
  const bool busy = isBusy();
  for (auto &page : m_pages) {
    page.widget->setEnabled(!busy);
    if (!busy)
      page.unpack->setEnabled(&page == &m_pages[0]);
  }
  m_settings->setEnabled(!busy);
  m_validation->setEnabled(!busy && !m_page);
  m_table->setEnabled(!busy);
  m_selectAll->setEnabled(!busy && m_table->rowCount() > 0);
  m_stop->setEnabled(true);
}
void OugaFlashWindow::updateSelection() {
  int count = 0;
  for (const auto &image : m_pages[m_page].images)
    count += image.selected;
  QSignalBlocker block(m_selectAll);
  m_selectAll->setCheckState(count == 0 ? Qt::Unchecked
                             : count == m_pages[m_page].images.size()
                                 ? Qt::Checked
                                 : Qt::PartiallyChecked);
}
void OugaFlashWindow::log(const QString &message) {
  const QString text = message.trimmed();
  if (text.isEmpty())
    return;
  QTextCursor cursor(m_log->document());
  cursor.movePosition(QTextCursor::End);
  if (!m_log->document()->isEmpty())
    cursor.insertBlock();
  QTextBlockFormat block;
  block.setLineHeight(19, QTextBlockFormat::FixedHeight);
  cursor.setBlockFormat(block);
  QTextCharFormat timestamp;
  timestamp.setForeground(QColor("#94A3B8"));
  cursor.insertText(QDateTime::currentDateTime().toString("[HH:mm:ss ] "),
                    timestamp);
  QColor color("#334155");
  if (text.startsWith("错误") || text.startsWith("已停止/失败") ||
      text.contains("FAILED") || text.startsWith("计划已阻止"))
    color = QColor("#DC2626");
  else if (text.startsWith("警告") || text.contains("请求停止") ||
           text.contains("未验证") || text.contains("取消"))
    color = QColor("#D97706");
  else if (text.startsWith("完成：") || text.contains("成功"))
    color = QColor("#16A34A");
  else if (text.startsWith("开始") || text.startsWith("加载") ||
           text.startsWith("已选择"))
    color = QColor("#2563EB");
  QTextCharFormat content;
  content.setForeground(color);
  cursor.insertText(text, content);
  m_log->setTextCursor(cursor);
  m_log->verticalScrollBar()->setValue(m_log->verticalScrollBar()->maximum());
}
void OugaFlashWindow::endTask(bool success, const QString &message) {
  ++m_generation;
  log((success ? "完成：" : "已停止/失败：") + message);
  if (m_session && !m_service->busy()) {
    DeviceOperationLease::release(m_service);
    m_session = false;
  }
  m_task = Task::None;
  m_stopRequested = false;
  m_overlay->hide();
  m_progress->setProperty("rate", success ? "阶段完成" : "已停止");
  m_progress->update();
  updateBusy();
}
bool OugaFlashWindow::continueTask(quint64 generation) {
  if (generation != m_generation || m_task == Task::None)
    return false;
  if (m_stopRequested) {
    endTask(false, "已取消后续阶段；已产生的文件保留");
    return false;
  }
  return true;
}
QString OugaFlashWindow::toolPath(const QString &key,
                                  const QString &fallback) const {
  QSettings settings;
  return settings.value("Ouga/" + key, fallback).toString().trimmed();
}
QString OugaFlashWindow::requireTool(const QString &key, const QString &label,
                                     const QString &fallback) {
  QString path = toolPath(key, fallback);
  QFileInfo tool(path);
  if (tool.isFile() && tool.isReadable())
    return path;
  const quint64 generation = m_generation;
  path = QFileDialog::getOpenFileName(this, "选择 " + label, {},
                                      "程序 (*.exe);;所有文件 (*)");
  if (generation != m_generation || m_stopRequested)
    return {};
  QFileInfo selected(path);
  if (!selected.isFile() || !selected.isReadable())
    return {};
  QSettings settings;
  settings.setValue("Ouga/" + key, selected.absoluteFilePath());
  return selected.absoluteFilePath();
}
void OugaFlashWindow::configureService() {
  m_service->configure(
      toolPath("fastboot", ResourceExtractor::getFastbootPath()),
      m_logDirectory.isEmpty() ? QStandardPaths::writableLocation(
                                     QStandardPaths::AppLocalDataLocation) +
                                     "/ouga-logs"
                               : m_logDirectory);
}
void OugaFlashWindow::load(const QString &path) {
  if (isBusy())
    return;
  ++m_generation;
  m_task = Task::Scan;
  m_stopRequested = false;
  m_pages[m_page].images.clear();
  m_pages[m_page].directory.clear();
  m_pages[m_page].folder->setText(path);
  display({}, path);
  updateBusy();
  m_prepare->scan(path);
}
void OugaFlashWindow::display(const QVector<Partition> &images,
                              const QString &directory) {
  // Copy first: switching pages can pass our own cached vector as the input.
  QVector<Partition> copy = images;
  m_pages[m_page].images = copy;
  m_pages[m_page].directory = directory;
  m_loading = true;
  m_pages[m_page].folder->setText(directory);
  m_table->setRowCount(copy.size());
  for (int row = 0; row < copy.size(); ++row) {
    const auto &image = copy[row];
    auto check = new QTableWidgetItem;
    check->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable |
                    Qt::ItemIsUserCheckable);
    check->setCheckState(image.selected ? Qt::Checked : Qt::Unchecked);
    m_table->setItem(row, 0, check);
    auto name = new QTableWidgetItem(image.name);
    name->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    name->setTextAlignment(Qt::AlignCenter);
    name->setToolTip(image.name);
    m_table->setItem(row, 1, name);
    auto size = new QTableWidgetItem(sizeText(image.bytes));
    size->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    size->setTextAlignment(Qt::AlignCenter);
    size->setToolTip("展开大小：" + sizeText(image.expandedBytes));
    m_table->setItem(row, 2, size);
    auto file = new QTableWidgetItem(image.path);
    file->setToolTip(image.path + "\nSHA-256：" +
                     QString::fromLatin1(image.sha256.toHex()));
    m_table->setItem(row, 3, file);
  }
  m_loading = false;
  updateSelection();
  updateBusy();
}
void OugaFlashWindow::tableChanged(QTableWidgetItem *item) {
  if (m_loading || isBusy() || item->row() >= m_pages[m_page].images.size())
    return;
  if (item->column() == 0) {
    m_pages[m_page].images[item->row()].selected =
        item->checkState() == Qt::Checked;
    updateSelection();
    return;
  }
  if (item->column() != 3)
    return;
  const int row = item->row();
  Partition previous = m_pages[m_page].images[row];
  QString file = item->text().trimmed();
  const quint64 generation = ++m_generation;
  m_task = Task::PathCheck;
  m_stopRequested = false;
  updateBusy();
  struct Checked {
    Partition image;
    QString error;
  };
  auto watcher = new QFutureWatcher<Checked>(this);
  connect(watcher, &QFutureWatcher<Checked>::finished, this,
          [this, watcher, row, previous, generation] {
            auto result = watcher->result();
            watcher->deleteLater();
            if (generation != m_generation)
              return;
            bool ok = result.error.isEmpty() && !m_stopRequested;
            if (ok) {
              result.image.selected = previous.selected;
              m_pages[m_page].images[row] = result.image;
            }
            display(m_pages[m_page].images, m_pages[m_page].directory);
            endTask(ok,
                    ok ? "文件路径已重新校验，目标名称保持不变"
                       : (m_stopRequested ? "路径修改已取消"
                                          : result.error + "；保留原有效镜像"));
          });
  watcher->setFuture(QtConcurrent::run([previous, file] {
    Checked result;
    QFileInfo info(file);
    if (info.isSymLink() || info.isJunction())
      result.error = "不接受镜像链接或目录联接";
    else
      OugaPackage::inspect(previous.name, file, &result.image, &result.error);
    return result;
  }));
}

void OugaFlashWindow::extractPayload(bool all) {
  if (isBusy() || (all && m_page))
    return;
  Page &page = m_pages[m_page];
  const int preset = page.preset->currentIndex();
  m_extractNames.clear();
  if (!all) {
    QString selection = page.preset->currentText().trimmed();
    if (selection == "请选择快捷提取方案 ↓" || selection.isEmpty()) {
      log("错误：请选择或输入要提取的分区名称");
      return;
    }
    if (selection == "高通修复FastbootD关键分区")
      m_extractNames = {"boot",      "recovery",      "dtbo",
                        "modem",     "vbmeta",        "vendor_boot",
                        "init_boot", "vbmeta_system", "vbmeta_vendor"};
    else if (selection == "联发科修复FastbootD关键分区")
      m_extractNames = {
          "boot",   "init_boot",   "dtbo",          "lk",
          "vbmeta", "vendor_boot", "vbmeta_system", "vbmeta_vendor"};
    else if (selection != "云解包方案-从云端提取线刷文件") {
      if (!safeName(selection)) {
        log("错误：无效的分区名称");
        return;
      }
      m_extractNames = {selection.toLower()};
    }
  }
  m_payloadSource = !m_page && (all || preset != 5)
                        ? m_pages[0].payloadFile->text().trimmed()
                        : QString();
  if (m_payloadSource.isEmpty() && !all)
    m_payloadSource = page.url->text().trimmed();
  m_payloadSource.remove(QChar(0x60));
  if (m_payloadSource.isEmpty()) {
    log("错误：请先选择Payload.bin文件、全量包ZIP文件，或输入有效的全量包链接");
    return;
  }
  if (all && !QFileInfo(m_payloadSource).isFile()) {
    log("错误：本地Payload/ZIP文件不存在；链接请使用快捷云提取");
    return;
  }
  const quint64 generation = ++m_generation;
  m_task = Task::PayloadExtract;
  m_stopRequested = false;
  m_progress->setValue(0);
  m_progress->setProperty("rate", "准备中...");
  updateBusy();
  QString output = all ? m_pages[0].folder->text().trimmed() : QString();
  if (output.isEmpty())
    output = QFileDialog::getExistingDirectory(
        this,
        all ? "选择解包输出目录" : "选择提取文件的保存路径（独立空目录）");
  if (!continueTask(generation))
    return;
  if (output.isEmpty()) {
    endTask(false, "用户取消了输出目录选择");
    return;
  }
  m_payloadOutput = all ? QDir(output).filePath("images") : output;
  QUrl url(m_payloadSource);
  if (url.scheme() == "https" || url.scheme() == "http") {
    if (!OugaRomService::safeUrl(url)) {
      endTask(false, "无效或不安全的下载地址");
      return;
    }
    QString name = QFileInfo(url.path()).fileName();
    if (name.isEmpty())
      name = "payload.bin";
    QString destination = QFileDialog::getSaveFileName(
        this, "保存完整全量包（下载完成后本地提取，不自动删除）", name);
    if (!continueTask(generation))
      return;
    if (destination.isEmpty()) {
      endTask(false, "用户取消下载");
      return;
    }
    m_task = Task::PayloadDownload;
    m_downloaded.clear();
    m_transferClock.invalidate();
    m_lastBytes = 0;
    log("开始云提取：本版先完整下载，再本地提取所选分区");
    m_rom->download(url, destination);
  } else
    preparePayloadSource();
}
void OugaFlashWindow::preparePayloadSource() {
  const quint64 generation = m_generation;
  if (!continueTask(generation))
    return;
  QFile file(m_payloadSource);
  if (!file.open(QIODevice::ReadOnly)) {
    endTask(false, "源文件无法读取");
    return;
  }
  const QByteArray magic = file.read(4);
  file.close();
  if (magic.startsWith("PK") ||
      QFileInfo(m_payloadSource).suffix().compare("zip", Qt::CaseInsensitive) ==
          0) {
    QString sevenZip = requireTool("7z", "7z.exe");
    if (!continueTask(generation))
      return;
    if (sevenZip.isEmpty()) {
      endTask(false, "未配置7z，无法读取全量包ZIP");
      return;
    }
    m_archiveRoot = uniqueWork(QFileInfo(m_payloadOutput).absolutePath(),
                               "ouga-payload-source");
    m_task = Task::PayloadArchive;
    m_progress->setProperty("rate", "解压中...");
    updateBusy();
    m_prepare->extractArchive(sevenZip, m_payloadSource, m_archiveRoot, false);
  } else
    runPayload();
}
void OugaFlashWindow::runPayload() {
  const quint64 generation = m_generation;
  if (!continueTask(generation))
    return;
  QVector<OugaPayloadEntry> entries;
  bool delta = false;
  QString error;
  if (!OugaPackage::payloadManifest(m_payloadSource, &entries, &delta,
                                    &error)) {
    endTask(false, error);
    return;
  }
  if (!m_extractNames.isEmpty()) {
    QStringList available, matched;
    for (const auto &entry : entries)
      available << entry.name;
    for (const QString &name : m_extractNames) {
      if (available.contains(name))
        matched << name;
      else
        log("警告：未找到分区 '" + name + "'，该ROM可能不包含此分区");
    }
    if (matched.isEmpty()) {
      endTask(false, "Payload中没有所请求分区");
      return;
    }
    m_extractNames = matched;
  }
  QString oldDirectory;
  if (delta) {
    oldDirectory = QFileDialog::getExistingDirectory(
        this, "增量Payload：选择匹配的旧镜像目录");
    if (!continueTask(generation))
      return;
    if (oldDirectory.isEmpty()) {
      endTask(false, "增量Payload缺少旧镜像，拒绝提取");
      return;
    }
  }
  QString tool =
      requireTool("payload", "Payload工具",
                  ResourceExtractor::getResourcePath() + "/payload.exe");
  if (!continueTask(generation))
    return;
  if (tool.isEmpty()) {
    endTask(false, "Payload工具不可用");
    return;
  }
  m_task = Task::PayloadExtract;
  m_progress->setProperty("rate", "提取中...");
  updateBusy();
  m_prepare->payload(tool, m_payloadSource, m_payloadOutput, m_extractNames,
                     oldDirectory);
}
void OugaFlashWindow::preparationFinished(bool success,
                                          const QString &message) {
  if (m_task == Task::None)
    return;
  log(message);
  if (!success || m_stopRequested) {
    endTask(false,
            m_stopRequested ? "用户已请求停止；准备输出予以保留" : message);
    return;
  }
  const Task completed = m_task;
  const quint64 generation = m_generation;
  QTimer::singleShot(0, this, [this, completed, generation] {
    if (generation != m_generation)
      return;
    if (m_stopRequested) {
      endTask(false, "用户已请求停止");
      return;
    }
    if (completed == Task::PayloadArchive) {
      QStringList candidates;
      QDirIterator it(m_archiveRoot, {"payload.bin"}, QDir::Files,
                      QDirIterator::Subdirectories);
      while (it.hasNext())
        candidates << it.next();
      if (candidates.size() != 1) {
        endTask(false, candidates.isEmpty()
                           ? "ZIP中没有payload.bin"
                           : "ZIP中有多个payload.bin，请消除歧义");
        return;
      }
      m_payloadSource = candidates.first();
      runPayload();
    } else if (completed == Task::RescueArchive) {
      m_pages[1].fb->setChecked(true);
      m_requestedMode = FlashMode::AfterSalesBootloader;
      prepareFlash();
    } else if (completed == Task::ArbDiscover) {
      if (m_serials.isEmpty()) {
        endTask(false, "没有已授权ADB设备；Fastboot不能可靠回读xbl_config");
        return;
      }
      bool accepted = true;
      QString serial =
          m_serials.size() == 1
              ? m_serials.first()
              : QInputDialog::getItem(this, "选择ARB检测设备",
                                      "仅回读所选已授权设备的当前槽镜像：",
                                      m_serials, 0, false, &accepted);
      if (!continueTask(generation))
        return;
      if (!accepted || serial.isEmpty()) {
        endTask(false, "用户取消ADB设备选择");
        return;
      }
      QString output = QFileDialog::getSaveFileName(
          this, "保存当前槽xbl_config（独立新文件，不自动删除）",
          "xbl_config-current.img", "镜像 (*.img)");
      if (!continueTask(generation))
        return;
      if (output.isEmpty()) {
        endTask(false, "用户取消镜像保存");
        return;
      }
      m_currentArb.clear();
      m_currentArbSerial = serial;
      m_task = Task::Arb;
      m_prepare->readCurrentArb(m_adbTool, serial, output);
    } else if (completed == Task::Super)
      discoverDevice();
    else {
      if (completed == Task::PayloadExtract)
        m_progress->setValue(100);
      endTask(true, completed == Task::Arb
                        ? "镜像索引检测完成；不代表完整硬件熔断状态或整包安全"
                        : "文件已准备；尚未写入设备");
    }
  });
}
void OugaFlashWindow::networkFinished(bool success, const QString &message) {
  if (m_task == Task::RescueSelection)
    return;
  if (m_task != Task::PayloadDownload && m_task != Task::RescueDownload)
    return;
  log(message);
  if (!success || m_stopRequested || m_downloaded.isEmpty()) {
    endTask(false, m_stopRequested ? "下载已取消，断点文件保留" : message);
    return;
  }
  const Task completed = m_task;
  const quint64 generation = m_generation;
  QTimer::singleShot(0, this, [this, completed, generation] {
    if (!continueTask(generation))
      return;
    if (completed == Task::PayloadDownload) {
      m_payloadSource = m_downloaded;
      preparePayloadSource();
    } else {
      QString tool = requireTool("7z", "7z.exe");
      if (!continueTask(generation))
        return;
      if (tool.isEmpty()) {
        endTask(false, "下载包已保留；未配置7z，无法解压");
        return;
      }
      QString output =
          QFileDialog::getExistingDirectory(this, "选择售后包独立空解压目录");
      if (!continueTask(generation))
        return;
      if (output.isEmpty()) {
        endTask(false, "下载包已保留，用户取消解压");
        return;
      }
      m_task = Task::RescueArchive;
      m_progress->setProperty("rate", "解压中...");
      m_prepare->extractArchive(tool, m_downloaded, output);
    }
  });
}

void OugaFlashWindow::startFlash(bool repair) {
  if (isBusy())
    return;
  if (!repair && m_page && m_pages[1].rescue->isChecked()) {
    rescueDialog();
    return;
  }
  Page &page = m_pages[m_page];
  if (page.images.isEmpty() || page.directory.isEmpty()) {
    log("错误：请先选择并加载有效的刷写文件夹");
    return;
  }
  if (QDir::cleanPath(page.folder->text().trimmed()) !=
      QDir::cleanPath(page.directory)) {
    log("错误：目录输入已改变，请先完成重新扫描，不使用旧分区表刷写");
    return;
  }
  if (repair)
    m_requestedMode = FlashMode::RepairFastbootd;
  else if (m_page) {
    if (!page.fb->isChecked() && !page.fbd->isChecked()) {
      log("错误：请选择FBD模式、FB模式或自动救砖模式");
      return;
    }
    m_requestedMode = page.fb->isChecked() ? FlashMode::AfterSalesBootloader
                                           : FlashMode::AfterSalesFastbootd;
  } else
    m_requestedMode = page.ab->isChecked()        ? FlashMode::BothSlots
                      : page.force->isChecked()   ? FlashMode::Force
                      : page.onlyFbd->isChecked() ? FlashMode::OnlyFastbootd
                                                  : FlashMode::Normal;
  ++m_generation;
  m_task = Task::Discover;
  m_stopRequested = false;
  m_selectedTargetSlot.clear();
  m_packagePlatform = Platform::Unknown;
  m_device = Device();
  m_plan = Plan();
  m_progress->setValue(0);
  m_progress->setProperty("rate", "准备中...");
  updateBusy();
  prepareFlash();
}
void OugaFlashWindow::prepareFlash() {
  const quint64 generation = m_generation;
  if (!continueTask(generation))
    return;
  QString error;
  if (!DeviceOperationLease::acquire(m_service, &error)) {
    endTask(false, error);
    return;
  }
  m_session = true;
  QString fastboot = requireTool("fastboot", "fastboot.exe",
                                 ResourceExtractor::getFastbootPath());
  if (!continueTask(generation))
    return;
  if (fastboot.isEmpty()) {
    endTask(false, "Fastboot工具不可用");
    return;
  }
  configureService();
  if (m_requestedMode == FlashMode::AfterSalesBootloader) {
    const auto &images = m_pages[m_page].images;
    auto super =
        std::find_if(images.cbegin(), images.cend(), [](const Partition &p) {
          return p.selected && baseName(p.name) == "super";
        });
    if (super == images.cend()) {
      const auto answer = QMessageBox::question(
          this, "生成售后Super",
          "FB模式需要有效的super.img。现在根据所选售后包的super_def生成吗？\n"
          "只在独立工作目录生成，不修改原包，不写入设备。",
          QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
      if (!continueTask(generation))
        return;
      if (answer != QMessageBox::Yes) {
        endTask(false, "售后FB模式缺少选中的Super，用户取消生成");
        return;
      }
      QString tool = requireTool("lpmake", "lpmake.exe");
      if (!continueTask(generation))
        return;
      if (tool.isEmpty()) {
        endTask(false, "lpmake不可用，不能生成Super");
        return;
      }
      QString output = QFileDialog::getExistingDirectory(
          this, "选择源包之外的独立空Super工作目录");
      if (!continueTask(generation))
        return;
      if (output.isEmpty()) {
        endTask(false, "用户取消Super工作目录选择");
        return;
      }
      m_task = Task::Super;
      m_progress->setProperty("rate", "生成Super...");
      updateBusy();
      m_prepare->makeSuper(tool, m_pages[m_page].directory, output);
      return;
    }
    if (super->merged.isEmpty()) {
      endTask(false, "Super缺少经过校验的合并清单，请重新加载有效售后包");
      return;
    }
  }
  discoverDevice();
}
void OugaFlashWindow::discoverDevice() {
  if (m_stopRequested) {
    endTask(false, "已取消设备读取");
    return;
  }
  m_task = Task::Discover;
  m_serials.clear();
  updateBusy();
  m_service->discover();
}
void OugaFlashWindow::serviceFinished(bool success, const QString &message) {
  if (m_task == Task::None)
    return;
  log(message);
  if (!success || m_stopRequested) {
    endTask(false,
            m_stopRequested ? "已在命令边界停止；已写内容不会回滚" : message);
    return;
  }
  const Task completed = m_task;
  if (completed == Task::Execute) {
    endTask(true, "线刷计划全部执行成功；请另行核实手机启动结果");
    return;
  }
  QString error;
  if (!DeviceOperationLease::acquire(m_service, &error)) {
    endTask(false, error);
    return;
  }
  m_session = true;
  const quint64 generation = m_generation;
  QTimer::singleShot(0, this, [this, completed, generation] {
    if (generation != m_generation)
      return;
    if (m_stopRequested) {
      endTask(false, "用户取消后续步骤");
      return;
    }
    if (completed == Task::Discover) {
      if (m_serials.isEmpty()) {
        endTask(false, "没有检测到Fastboot设备，请连接设备后重试");
        return;
      }
      QString serial = m_serials.first();
      if (m_serials.size() > 1) {
        bool accepted = false;
        serial =
            QInputDialog::getItem(this, "选择线刷设备", "仅操作所选序列号：",
                                  m_serials, 0, false, &accepted);
        if (!continueTask(generation))
          return;
        if (!accepted || serial.isEmpty()) {
          endTask(false, "用户取消设备选择；没有写入");
          return;
        }
      }
      m_task = Task::Probe;
      m_service->probe(serial);
      return;
    }
    if (completed != Task::Probe && completed != Task::Switch) {
      endTask(false, "意外的设备准备状态，拒绝继续");
      return;
    }
    const bool bootloader = m_requestedMode == FlashMode::RepairFastbootd ||
                            m_requestedMode == FlashMode::AfterSalesBootloader;
    const bool userspace = !bootloader;
    if (m_requestedMode == FlashMode::OnlyFastbootd &&
        (!m_device.modeKnown || !m_device.userspace ||
         m_device.platform != Platform::Qualcomm)) {
      endTask(false,
              "仅FBD模式只允许已处于FastbootD的高通设备；不自动回普通Fastboot");
      return;
    }
    if (!m_device.modeKnown || !m_device.unlockKnown || !m_device.unlocked ||
        m_device.product.isEmpty() || m_device.platform == Platform::Unknown ||
        (m_device.slot != "a" && m_device.slot != "b")) {
      endTask(false, "设备模式、解锁状态、平台、机型或槽位无法确认，拒绝执行");
      return;
    }
    if (m_device.userspace != userspace) {
      if (completed == Task::Switch) {
        endTask(false, "模式准备结果与请求不符，拒绝继续");
        return;
      }
      const QString mode = userspace ? "FastbootD" : "普通Fastboot";
      const auto answer = QMessageBox::question(
          this, "准备线刷模式",
          "设备：" + m_device.serial + "\n本操作需要" + mode +
              "，是否先重启到该模式并重新读取分区表？\n此步只切换模式，不开始刷"
              "写。",
          QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
      if (!continueTask(generation))
        return;
      if (answer != QMessageBox::Yes) {
        endTask(false, "用户取消模式切换；没有开始刷写");
        return;
      }
      m_task = Task::Switch;
      m_service->prepareMode(m_device.serial, userspace);
      return;
    }
    buildAndExecute();
  });
}
void OugaFlashWindow::buildAndExecute() {
  const quint64 taskGeneration = m_generation;
  if (!continueTask(taskGeneration))
    return;
  Page &page = m_pages[m_page];
  const bool additional = m_requestedMode == FlashMode::BothSlots ||
                          m_requestedMode == FlashMode::Force ||
                          m_requestedMode == FlashMode::OnlyFastbootd;
  if (additional) {
    for (const QString &name : {QString("my_company"), QString("my_preload")}) {
      auto image = std::find_if(
          page.images.begin(), page.images.end(),
          [&](const Partition &p) { return baseName(p.name) == name; });
      if (image != page.images.end()) {
        if (!image->selected) {
          const auto answer = QMessageBox::question(
              this, "必要附加镜像",
              "此模式必须刷入匹配的" + name +
                  ".img。是否使用包中已校验的镜像？\n" + image->path,
              QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
          if (!continueTask(taskGeneration))
            return;
          if (answer != QMessageBox::Yes) {
            endTask(false, "未选择必要附加镜像：" + name);
            return;
          }
          image->selected = true;
          display(page.images, page.directory);
        }
        continue;
      }
      QString file = QFileDialog::getOpenFileName(
          this, "选择与设备及ROM匹配的" + name + ".img（不使用内置替代镜像）",
          {}, "镜像 (*.img)");
      if (!continueTask(taskGeneration))
        return;
      if (file.isEmpty()) {
        endTask(false, "缺少必要附加镜像：" + name);
        return;
      }
      m_task = Task::PathCheck;
      updateBusy();
      const quint64 generation = m_generation;
      struct Checked {
        Partition image;
        QString error;
      };
      auto watcher = new QFutureWatcher<Checked>(this);
      connect(watcher, &QFutureWatcher<Checked>::finished, this,
              [this, watcher, generation] {
                Checked result = watcher->result();
                watcher->deleteLater();
                if (generation != m_generation)
                  return;
                if (m_stopRequested || !result.error.isEmpty()) {
                  endTask(false, m_stopRequested ? "已取消附加镜像校验"
                                                 : result.error);
                  return;
                }
                m_pages[m_page].images << result.image;
                display(m_pages[m_page].images, m_pages[m_page].directory);
                buildAndExecute();
              });
      watcher->setFuture(QtConcurrent::run([name, file] {
        Checked result;
        QFileInfo info(file);
        if (info.isSymLink() || info.isJunction())
          result.error = "附加镜像不能为链接";
        else
          OugaPackage::inspect(name, file, &result.image, &result.error);
        return result;
      }));
      return;
    }
  }
  bool xbl = false, lk = false;
  for (const auto &image : page.images) {
    xbl |= baseName(image.name) == "xbl";
    lk |= baseName(image.name) == "lk";
  }
  if (!xbl && !lk && m_packagePlatform == Platform::Unknown) {
    bool accepted = false;
    QString selected = QInputDialog::getItem(
        this, "核对刷机包平台",
        "设备平台：" + platformText(m_device.platform) +
            "\n无法从包中的xbl/lk判定平台。请核对机型并明确选择刷机包平台：",
        {"高通", "联发科"}, m_device.platform == Platform::MediaTek ? 1 : 0,
        false, &accepted);
    if (!continueTask(taskGeneration))
      return;
    if (!accepted) {
      endTask(false, "用户取消平台核对");
      return;
    }
    m_packagePlatform =
        selected == "高通" ? Platform::Qualcomm : Platform::MediaTek;
  }
  if (m_requestedMode == FlashMode::BothSlots &&
      m_selectedTargetSlot.isEmpty()) {
    bool accepted = false;
    QString selected = QInputDialog::getItem(
        this, "选择最终启动槽",
        "当前槽：" + m_device.slot.toUpper() + "\nAB通刷后的启动槽：",
        {"A", "B"}, m_device.slot == "b" ? 1 : 0, false, &accepted);
    if (!continueTask(taskGeneration))
      return;
    if (!accepted) {
      endTask(false, "用户取消目标槽选择");
      return;
    }
    m_selectedTargetSlot = selected.toLower();
  }
  Options options;
  options.packageMode = m_page ? PackageMode::AfterSales : PackageMode::Full;
  options.mode = m_requestedMode;
  options.clearData =
      options.mode != FlashMode::RepairFastbootd && page.wipe->isChecked();
  options.autoReboot =
      options.mode != FlashMode::RepairFastbootd && page.reboot->isChecked();
  options.validateTable = !m_page &&
                          options.mode != FlashMode::RepairFastbootd &&
                          m_validateOn->isChecked();
  options.targetSlot = m_selectedTargetSlot;
  options.packagePlatform = m_packagePlatform;
  auto formatReady = [this] {
    QDir tools(
        QFileInfo(toolPath("fastboot", ResourceExtractor::getFastbootPath()))
            .absolutePath());
    for (const QString &name : {"mke2fs.exe", "make_f2fs.exe", "mke2fs.conf"}) {
      QFileInfo f(tools.filePath(name));
      if (!f.isFile() || !f.isReadable() || f.size() == 0)
        return false;
    }
    return true;
  };
  options.formatToolsReady = formatReady();
  if (options.clearData && m_device.platform == Platform::Qualcomm &&
      !options.formatToolsReady) {
    const auto answer = QMessageBox::question(
        this, "缺少格式化依赖",
        "清除数据需要与fastboot匹配的完整platform-tools：mke2fs.exe、make_f2fs."
        "exe、mke2fs.conf。\n"
        "是否选择完整platform-tools中的fastboot.exe？否则在首次写入前停止。",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (!continueTask(taskGeneration))
      return;
    if (answer != QMessageBox::Yes) {
      endTask(false, "格式化依赖缺失；未开始写入");
      return;
    }
    QString tool = QFileDialog::getOpenFileName(
        this, "选择可信完整platform-tools中的fastboot.exe", {},
        "Fastboot (fastboot.exe)");
    if (!continueTask(taskGeneration))
      return;
    QFileInfo file(tool);
    if (!file.isFile() || !file.isReadable()) {
      endTask(false, "没有选择有效Fastboot工具");
      return;
    }
    QSettings settings;
    settings.setValue("Ouga/fastboot", file.absoluteFilePath());
    options.formatToolsReady = formatReady();
    if (!options.formatToolsReady) {
      endTask(false, "所选platform-tools仍缺少完整格式化依赖");
      return;
    }
    configureService();
  }
  QVector<Partition> planImages = page.images;
  if (options.mode == FlashMode::RepairFastbootd) {
    const QStringList required = OugaPackage::repairNames(m_device.platform);
    for (auto &image : planImages)
      image.selected = required.contains(baseName(image.name));
  }
  if (!m_currentArb.isEmpty() && m_currentArbSerial == m_device.serial) {
    quint32 current = 0;
    QString error;
    if (!OugaPackage::readArb(m_currentArb, &current, &error)) {
      endTask(false, "ARB基准无法读取：" + error);
      return;
    }
    for (const auto &image : planImages)
      if (image.selected && baseName(image.name) == "xbl_config") {
        quint32 next = 0;
        if (!OugaPackage::readArb(image.path, &next, &error)) {
          endTask(false, "固件ARB解析失败：" + error);
          return;
        }
        options.currentXblConfig = m_currentArb;
        options.arbVerified = true;
        options.arbDowngrade = next < current;
        log(QString("镜像ARB比较：当前 %1 → 固件 %2；不等于硬件熔断状态")
                .arg(current)
                .arg(next));
      }
  }
  QString error;
  if (!OugaFlashPlanner::build(planImages, m_device, options, &m_plan,
                               &error)) {
    endTask(false, "计划已阻止：" + error);
    return;
  }
  log(planText(m_plan));
  const bool accepted = confirm(m_plan);
  if (taskGeneration != m_generation)
    return;
  if (!accepted || m_stopRequested) {
    endTask(false, "用户取消最终确认；没有开始写入");
    return;
  }
  m_task = Task::Execute;
  m_overlay->begin();
  updateBusy();
  m_service->execute(m_plan);
}

bool OugaFlashWindow::confirm(const Plan &plan) {
  QDialog dialog(this);
  dialog.setObjectName("OugaFlashConfirmDialog");
  dialog.setWindowTitle("最终确认：将写入设备，不能自动回滚");
  dialog.resize(900, 650);
  auto layout = new QVBoxLayout(&dialog);
  auto text = new QPlainTextEdit(planText(plan), &dialog);
  text->setObjectName("OugaConfirmedPlan");
  text->setReadOnly(true);
  layout->addWidget(text);
  auto agree = new QCheckBox("已核对设备序列号、机型、平台、镜像来源、槽位及删"
                             "除/清除步骤，并已备份数据",
                             &dialog);
  agree->setObjectName("OugaConfirmAgreement");
  layout->addWidget(agree);
  auto buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  buttons->button(QDialogButtonBox::Ok)
      ->setObjectName("OugaConfirmWriteButton");
  buttons->button(QDialogButtonBox::Ok)->setText("确认写入");
  buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
  buttons->button(QDialogButtonBox::Cancel)->setText("取消");
  connect(agree, &QCheckBox::toggled, buttons->button(QDialogButtonBox::Ok),
          &QPushButton::setEnabled);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  layout->addWidget(buttons);
  return dialog.exec() == QDialog::Accepted;
}
void OugaFlashWindow::requestStop() {
  if (!isBusy()) {
    log("当前无可停止的任务");
    return;
  }
  if (m_stopRequested) {
    log("已收到停止请求，将在当前操作完成后结束任务");
    return;
  }
  m_stopRequested = true;
  log("停止请求已记录：不回滚已写内容，当前写入结束后不启动下一条命令。");
  updateBusy();
  if (m_service->busy())
    m_service->requestStop();
  else if (m_prepare->busy())
    m_prepare->cancel();
  else if (m_rom->busy())
    m_rom->cancel();
  else if (m_task != Task::PathCheck && m_task != Task::Arb)
    endTask(false, "已取消后续阶段；已产生的文件保留");
}
void OugaFlashWindow::arbDialog() {
  if (isBusy())
    return;
  QDialog dialog(this);
  dialog.setObjectName("OugaArbDialog");
  dialog.setWindowTitle("ARB熔断检测");
  dialog.setFixedWidth(470);
  auto layout = new QVBoxLayout(&dialog);
  layout->setContentsMargins(24, 20, 24, 20);
  layout->setSpacing(10);
  auto current = new QRadioButton("检测当前设备是否熔断", &dialog);
  current->setObjectName("OugaArbCurrentDevice");
  current->setChecked(true);
  layout->addWidget(current);
  auto currentHelp =
      new QLabel("设备须已授权ADB并已具备当前槽xbl_"
                 "config读取权限。程序不主动提权或解锁；Fastboot不能可靠回读。",
                 &dialog);
  currentHelp->setWordWrap(true);
  layout->addWidget(currentHelp);
  auto firmware = new QRadioButton("检测固件包是否熔断", &dialog);
  firmware->setObjectName("OugaArbFirmware");
  layout->addWidget(firmware);
  auto fileRow = new QHBoxLayout;
  auto file = entry("未选择文件", "OugaArbFirmwarePath", &dialog);
  file->setReadOnly(true);
  auto choose = button("选择", "OugaArbSelectFile", &dialog, "sky");
  choose->setIcon(referenceIcon("folder"));
  choose->setFixedWidth(84);
  fileRow->addWidget(file, 1);
  fileRow->addWidget(choose);
  layout->addLayout(fileRow);
  file->setEnabled(false);
  choose->setEnabled(false);
  connect(firmware, &QRadioButton::toggled, file, &QWidget::setEnabled);
  connect(firmware, &QRadioButton::toggled, choose, &QWidget::setEnabled);
  connect(choose, &QPushButton::clicked, &dialog, [&] {
    QString path = QFileDialog::getOpenFileName(
        &dialog, "选择固件包中的xbl_config.img", {}, "镜像 (*.img)");
    if (!path.isEmpty())
      file->setText(path);
  });
  auto caution = new QLabel("仅检测镜像OEM反回滚索引，不等于完整硬件熔断状态，"
                            "也不能保证整包没有其他反回滚风险。",
                            &dialog);
  caution->setWordWrap(true);
  caution->setStyleSheet("color:#64748B;");
  layout->addWidget(caution);
  auto buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  buttons->button(QDialogButtonBox::Ok)->setText("开始检测");
  buttons->button(QDialogButtonBox::Cancel)->setText("取消");
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
    if (firmware->isChecked() && !QFileInfo(file->text()).isFile()) {
      QMessageBox::warning(&dialog, "未选择有效文件",
                           "请选择固件包中的xbl_config.img");
      return;
    }
    dialog.accept();
  });
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  if (dialog.exec() != QDialog::Accepted)
    return;
  const quint64 taskGeneration = ++m_generation;
  m_stopRequested = false;
  m_task = Task::Arb;
  updateBusy();
  if (firmware->isChecked()) {
    const QString source = file->text();
    const quint64 generation = m_generation;
    struct ArbResult {
      quint32 value = 0;
      QString error;
    };
    auto watcher = new QFutureWatcher<ArbResult>(this);
    connect(watcher, &QFutureWatcher<ArbResult>::finished, this,
            [this, watcher, source, generation] {
              auto result = watcher->result();
              watcher->deleteLater();
              if (generation != m_generation)
                return;
              if (m_stopRequested || !result.error.isEmpty()) {
                endTask(false,
                        m_stopRequested ? "已取消ARB检测" : result.error);
                return;
              }
              log("固件文件：" + source);
              log(QString("固件xbl_config镜像ARB索引 = "
                          "%1；不是硬件熔断结论，未建立当前设备比较基准")
                      .arg(result.value));
              endTask(true, "固件镜像索引解析完成");
            });
    watcher->setFuture(QtConcurrent::run([source] {
      ArbResult result;
      OugaPackage::readArb(source, &result.value, &result.error);
      return result;
    }));
  } else {
    m_adbTool = requireTool("adb", "adb.exe", ResourceExtractor::getAdbPath());
    if (!continueTask(taskGeneration))
      return;
    if (m_adbTool.isEmpty()) {
      endTask(false, "ADB工具不可用，不能检测当前镜像");
      return;
    }
    m_serials.clear();
    m_task = Task::ArbDiscover;
    m_prepare->discoverAdb(m_adbTool);
  }
}
void OugaFlashWindow::closeEvent(QCloseEvent *event) {
  if (isBusy()) {
    event->ignore();
    QMessageBox::warning(
        this, "任务进行中",
        "当前任务尚未结束，不能关闭窗口；可以最小化。\n请请求停止并等待当前写入"
        "或准备结束。不会自动回滚已写内容。");
    return;
  }
  event->accept();
}
void OugaFlashWindow::dragEnterEvent(QDragEnterEvent *event) {
  if (!isBusy() && event->mimeData()->hasUrls() &&
      event->mimeData()->urls().size() == 1 &&
      event->mimeData()->urls().first().isLocalFile())
    event->acceptProposedAction();
}
void OugaFlashWindow::dropEvent(QDropEvent *event) {
  if (isBusy() || !event->mimeData()->hasUrls() ||
      event->mimeData()->urls().size() != 1)
    return;
  const QUrl url = event->mimeData()->urls().first();
  if (!url.isLocalFile())
    return;
  QString file = url.toLocalFile();
  QFileInfo info(file);
  if (info.isDir())
    load(file);
  else if (!m_page && info.isFile() &&
           (info.suffix().compare("bin", Qt::CaseInsensitive) == 0 ||
            info.suffix().compare("zip", Qt::CaseInsensitive) == 0)) {
    m_pages[0].payloadFile->setText(file);
    log("已选择Payload/全量包文件，点击解包Payload准备文件；不会自动写入。");
  } else if (info.isFile() &&
             info.suffix().compare("img", Qt::CaseInsensitive) == 0)
    load(info.absolutePath());
  else {
    log("请拖入有效刷写文件夹，或在全量模式选择Payload.bin/ZIP文件");
    return;
  }
  event->acceptProposedAction();
}

void OugaFlashWindow::rescueDialog() {
  if (isBusy())
    return;
  QUrl base(toolPath("rom"));
  if (!OugaRomService::safeUrl(base)) {
    log("错误：未配置ROM服务地址，自动救砖不可用；不会访问参考作者的线上服务");
    return;
  }
  QString error;
  if (!DeviceOperationLease::acquire(m_service, &error)) {
    log(error);
    return;
  }
  m_session = true;
  ++m_generation;
  const quint64 generation = m_generation;
  m_task = Task::RescueSelection;
  m_stopRequested = false;
  m_selectedTargetSlot.clear();
  m_packagePlatform = Platform::Unknown;
  m_device = Device();
  m_plan = Plan();
  m_progress->setValue(0);
  m_progress->setProperty("rate", "准备中...");
  updateBusy();
  struct Model {
    QString display, series, api;
  };
  static const QVector<Model> models = {
      {"一加ACE6T", "ACE系列", "一加 Ace 6T"},
      {"一加ACE6", "ACE系列", "一加 Ace 6"},
      {"一加ACE5Pro", "ACE系列", "一加 Ace 5 Pro"},
      {"一加ACE5", "ACE系列", "一加 Ace 5"},
      {"一加ACE5至尊版", "ACE系列", "一加 Ace 5 至尊版"},
      {"一加ACE5竞速版", "ACE系列", "一加_Ace_5_竞速版"},
      {"一加ACE3Pro", "ACE系列", "一加 Ace 3 Pro"},
      {"一加ACE3", "ACE系列", "一加 Ace 3"},
      {"一加ACE3V", "ACE系列", "一加 Ace 3V"},
      {"一加ACE2Pro", "ACE系列", "一加 Ace 2 Pro"},
      {"一加ACE2V", "ACE系列", "一加 Ace 2V"},
      {"一加ACE2", "ACE系列", "一加Ace_2"},
      {"一加ACE Pro", "ACE系列", "一加 Ace Pro"},
      {"一加ACE", "ACE系列", "一加 Ace"},
      {"一加ACE竞速版", "ACE系列", "一加_Ace_竞速版"},
      {"一加15", "数字系列", "一加 15"},
      {"一加13T", "数字系列", "一加13T"},
      {"一加13", "数字系列", "一加13"},
      {"一加12", "数字系列", "一加12"},
      {"一加11", "数字系列", "一加11"},
      {"一加10Pro", "数字系列", "一加 10 Pro"},
      {"一加10R", "数字系列", "一加10R_5G"},
      {"一加9", "数字系列", "OnePlus_9"},
      {"一加9RT", "数字系列", "OnePlus_9RT_5G"},
      {"一加9R", "数字系列", "OnePlus_9R_5G"},
      {"一加9Pro", "数字系列", "OnePlus_9_Pro"},
      {"一加Pad", "Pad系列", "一加平板"},
      {"一加Pad2", "Pad系列", "一加平板 2"},
      {"一加PadPro", "Pad系列", "一加平板_Pro"},
      {"一加Pad2Pro", "Pad系列", "一加平板_2_Pro"}};
  QStringList names;
  for (const auto &model : models)
    names << model.display;
  log("启动自动救砖模式（Fastboot/FastbootD工作流）");
  QString name =
      selectItem(this, "选择设备机型", "请选择您的设备机型：", names);
  if (generation != m_generation)
    return;
  auto model = std::find_if(models.cbegin(), models.cend(),
                            [&](const Model &m) { return m.display == name; });
  if (model == models.cend() || m_stopRequested) {
    endTask(false, "用户取消机型选择");
    return;
  }
  log("已选择机型：" + model->display);
  m_rom->setBaseUrl(base);
  auto fetch = [this, generation](const QString &endpoint,
                                  const QJsonObject &parameters,
                                  QJsonObject *response) {
    QEventLoop loop;
    QObject scope;
    bool complete = false, success = false;
    connect(m_rom, &OugaRomService::response, &scope,
            [response, endpoint](const QString &received,
                                 const QJsonObject &object) {
              if (received == endpoint)
                *response = object;
            });
    connect(m_rom, &OugaRomService::finished, &scope,
            [&](bool ok, const QString &message) {
              complete = true;
              success = ok;
              if (!ok)
                log(message);
              loop.quit();
            });
    m_rom->request(endpoint, parameters);
    if (!complete)
      loop.exec();
    return success && !m_stopRequested && generation == m_generation;
  };
  QJsonObject parameters{{"packageType", "afterSales"},
                         {"brand", "oneplus"},
                         {"series", model->series},
                         {"device", model->api}};
  QJsonObject versions;
  log("加载售后包版本列表...");
  if (!fetch("/versions", parameters, &versions)) {
    endTask(false, "加载版本失败或已取消");
    return;
  }
  QStringList items = OugaRomService::items(versions);
  if (items.isEmpty()) {
    endTask(false, "ROM服务暂未收录该机型的售后包版本");
    return;
  }
  QString version =
      selectItem(this, "选择售后包版本",
                 "请选择 " + model->display + " 的售后包版本：", items, 720);
  if (generation != m_generation)
    return;
  if (version.isEmpty() || m_stopRequested) {
    endTask(false, "用户取消版本选择");
    return;
  }
  parameters["version"] = version;
  QJsonObject links;
  log("获取售后包下载链接...");
  if (!fetch("/download-link", parameters, &links)) {
    endTask(false, "获取下载链接失败或已取消");
    return;
  }
  QList<QUrl> urls;
  for (const auto &value : OugaRomService::field(links, "links").toArray()) {
    QUrl url(value.toString().trimmed());
    if (OugaRomService::safeUrl(url))
      urls << url;
  }
  if (urls.isEmpty()) {
    endTask(false, "该版本没有有效的售后包下载链接");
    return;
  }
  auto archive = [](const QUrl &url) {
    QString file = url.path().toLower();
    for (const QString &ext :
         {".zip", ".ozip", ".tgz", ".tar.gz", ".7z", ".rar"})
      if (file.endsWith(ext))
        return true;
    return false;
  };
  auto preferred = std::find_if(urls.cbegin(), urls.cend(), archive);
  const QUrl url = preferred == urls.cend() ? urls.first() : *preferred;
  QMap<QByteArray, QByteArray> headers;
  const auto values = OugaRomService::field(links, "requestHeaders").toObject();
  for (auto i = values.begin(); i != values.end(); ++i)
    headers[i.key().toLatin1()] = i.value().toString().toUtf8();
  QString hashText =
      OugaRomService::field(links, "sha256").toString().trimmed();
  if (!hashText.isEmpty() &&
      !QRegularExpression("^[0-9a-fA-F]{64}$").match(hashText).hasMatch()) {
    endTask(false, "服务提供的SHA-256无效，拒绝下载");
    return;
  }
  qint64 length = -1;
  auto lengthValue = OugaRomService::field(links, "length");
  if (lengthValue.isDouble()) {
    const double value = lengthValue.toDouble();
    if (value < 1 || value > 9007199254740991.0 || value != std::floor(value))
      length = -2;
    else
      length = qint64(value);
  } else if (lengthValue.isString()) {
    bool valid = false;
    length = lengthValue.toString().toLongLong(&valid);
    if (!valid)
      length = -2;
  }
  if (length < -1 || length == 0) {
    endTask(false, "服务提供的文件长度无效");
    return;
  }
  QString defaultName = QFileInfo(url.path()).fileName();
  if (defaultName.isEmpty())
    defaultName = "after-sales.zip";
  QString destination = QFileDialog::getSaveFileName(
      this, "选择刷机包下载保存路径（保留原包）", defaultName);
  if (generation != m_generation)
    return;
  if (destination.isEmpty() || m_stopRequested) {
    endTask(false, "用户取消下载保存路径选择");
    return;
  }
  m_downloaded.clear();
  m_transferClock.invalidate();
  m_lastBytes = 0;
  m_task = Task::RescueDownload;
  m_progress->setProperty("rate", "下载中...");
  updateBusy();
  m_rom->download(url, destination, headers,
                  QByteArray::fromHex(hashText.toLatin1()), length);
}
