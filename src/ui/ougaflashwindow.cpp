#include "ougaflashwindow.h"
#include "ougaflashservice.h"
#include "ougapreparation.h"
#include "ougaromservice.h"
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QAbstractItemView>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleOptionButton>
#include <QStyledItemDelegate>
#include <QSvgRenderer>
#include <QTableWidget>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTimer>
#include <QVBoxLayout>
using namespace Ouga;
QIcon OugaFlashWindow::referenceIcon(const QString &name) {
  QSvgRenderer svg(":/ouga/" + name + ".svg");
  QPixmap pixmap(32, 32);
  pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap);
  svg.render(&painter);
  painter.end();
  pixmap.setDevicePixelRatio(2);
  return QIcon(pixmap);
}
QPushButton *OugaFlashWindow::button(const QString &text, const QString &name,
                                     QWidget *parent, const QString &tone) {
  auto result = new QPushButton(text, parent);
  result->setObjectName(name);
  result->setProperty("tone", tone);
  result->setFixedHeight(32);
  result->setCursor(Qt::PointingHandCursor);
  return result;
}
QLineEdit *OugaFlashWindow::entry(const QString &placeholder,
                                  const QString &name, QWidget *parent) {
  auto result = new QLineEdit(parent);
  result->setObjectName(name);
  result->setPlaceholderText(placeholder);
  result->setFixedHeight(32);
  result->setAcceptDrops(false); // The page handles file/folder drops.
  return result;
}
namespace {
// Like the reference ComboBox: clicking the editable text opens the list,
// while typing a custom partition name still works.
class PopupOnClick final : public QObject {
public:
  explicit PopupOnClick(QComboBox *box) : QObject(box), m_box(box) {}
  bool eventFilter(QObject *watched, QEvent *event) override {
    if (event->type() == QEvent::MouseButtonPress && m_box->isEnabled() &&
        static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton &&
        !m_box->view()->isVisible())
      m_box->showPopup();
    return QObject::eventFilter(watched, event);
  }

private:
  QComboBox *m_box;
};
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
    : QWidget(nullptr), m_launcher(parent), m_logDirectory(logDirectory) {
  // Keep the always-on-top menu out of the native ownership/z-order chain.
  setWindowFlags(Qt::Window);
  if (parent)
    connect(parent, &QObject::destroyed, this, &QObject::deleteLater);
  setWindowTitle("秋白工作室 · 欧加线刷");
  setObjectName("OujiaFlashView");
  const QSize initialSize(qRound(866 * 0.9), qRound(729 * 0.9));
  setMinimumSize(initialSize);
  resize(initialSize);
  setAcceptDrops(true);
  QFont font("Microsoft YaHei UI");
  font.setPixelSize(12);
  setFont(font);
  setStyleSheet(R"(
    QWidget { font-family:"Microsoft YaHei UI"; }
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
  auto rootLayout = new QVBoxLayout(this);
  rootLayout->setContentsMargins(10, 10, 10, 10);
  m_canvas = new QWidget(this);
  m_canvas->setMinimumSize(minimumSize() - QSize(20, 20));
  rootLayout->addWidget(m_canvas);
  buildPage(0);
  buildPage(1);
  m_settings = card("刷写设置", m_canvas);
  auto settings = cardLayout(m_settings);
  auto modeRow = new QHBoxLayout;
  modeRow->setSpacing(0);
  modeRow->setContentsMargins(0, 0, 0, 0);
  auto label = new QLabel("刷写模式：");
  label->setStyleSheet("font-weight:600;");
  modeRow->addWidget(label);
  modeRow->addSpacing(6);
  m_full = option("全量包模式", "FullPackageModeCheckBox", m_settings);
  m_sales = option("售后包模式", "AfterSalesPackageModeCheckBox", m_settings);
  modeRow->addWidget(m_full);
  modeRow->addSpacing(8);
  modeRow->addWidget(m_sales);
  modeRow->addStretch();
  settings->addLayout(modeRow, 0, 0);
  m_validation = new QWidget;
  m_validation->setObjectName("PartitionTableValidationSettingPanel");
  m_validation->setToolTip("分区表校验仅适用于全量包模式；关闭后仍检查所选镜像"
                           "、目标、容量和设备状态");
  auto validation = new QHBoxLayout(m_validation);
  validation->setContentsMargins(0, 0, 0, 0);
  validation->setSpacing(0);
  label = new QLabel("分区表校验（实验性功能）：");
  label->setStyleSheet("font-weight:600;");
  validation->addWidget(label);
  validation->addSpacing(6);
  m_validateOn =
      option("开启", "PartitionTableValidationEnabledCheckBox", m_validation);
  m_validateOff =
      option("关闭", "PartitionTableValidationDisabledCheckBox", m_validation);
  validation->addWidget(m_validateOn);
  validation->addSpacing(8);
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
  initializeServices(runner);
  // Keep checkbox geometry and painting independent of the Windows theme.
  // The style belongs only to this window, not the rest of the application.
  if (auto style = QStyleFactory::create("Fusion")) {
    style->setObjectName("OugaReferenceCheckboxStyle");
    style->setParent(this);
    m_table->setStyle(style);
    for (auto box : findChildren<QCheckBox *>())
      box->setStyle(style);
  }
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
  page.preset->lineEdit()->installEventFilter(new PopupOnClick(page.preset));
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
  auto choiceLayout = new QHBoxLayout(choices);
  choiceLayout->setContentsMargins(0, 0, 0, 0);
  choiceLayout->setSpacing(0);
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
  choiceLayout->addWidget(page.wipe);
  choiceLayout->addWidget(page.reboot);
  if (!sales) {
    page.ab = option("AB通刷", "FlashABCheckBox", choices);
    page.force = option("强力线刷", "FixSuperCheckBox", choices);
    page.onlyFbd = option("仅FBD", "PureFBDCheckBox", choices);
    choiceLayout->addWidget(page.ab);
    choiceLayout->addWidget(page.force);
    choiceLayout->addWidget(page.onlyFbd);
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
    choiceLayout->addWidget(page.fbd);
    choiceLayout->addWidget(page.fb);
    page.rescue->setFixedWidth(72);
    choiceLayout->addWidget(page.rescue);
    exclusiveOptions({page.fbd, page.fb, page.rescue}, this);
    page.fbd->setChecked(true);
  }
  page.repair =
      button("修复FastbootD",
             named("FixFastbootDButton", "AfterSalesFixFastbootDButton"),
             choices, "repair");
  page.repair->setFixedHeight(26);
  page.repair->setFixedWidth(96);
  choiceLayout->addStretch();
  choiceLayout->addWidget(page.repair);
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
  // Shrink the table/log area with the window, without clipping the controls.
  const int extraWidth = m_canvas->width() - 846;
  const int extraHeight = m_canvas->height() - 709;
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
void OugaFlashWindow::showEvent(QShowEvent *event) {
  QWidget::showEvent(event);
  if (event->spontaneous())
    return;
  // Wait for native frame margins before centering on the launcher's screen.
  QTimer::singleShot(0, this, [this] {
    if (!isVisible() || isMinimized() || isMaximized())
      return;
    QScreen *targetScreen = m_launcher ? m_launcher->screen() : screen();
    if (!targetScreen)
      return;
    QRect frame = frameGeometry();
    frame.moveCenter(targetScreen->availableGeometry().center());
    move(frame.topLeft());
  });
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
      text.contains("FAILED") || text.startsWith("计划已阻止") ||
      text.startsWith("刷机包对应的机型为"))
    color = QColor("#DC2626");
  else if (text.startsWith("警告") || text.contains("请求停止") ||
           text.contains("未验证") || text.contains("取消") ||
           text.startsWith("正在判断刷机包对应的机型") ||
           text.startsWith("解析刷机包对应机型失败"))
    color = QColor("#D97706");
  else if (text.startsWith("完成：") || text.contains("成功") ||
           text.startsWith("发现 ") || text.startsWith("解包完成") ||
           text.startsWith("已加载 ") || text.endsWith("提取完成！"))
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
void OugaFlashWindow::payloadLogStart(const QString &name) {
  if (m_payloadLogBlocks.contains(name)) return;
  log("[提取] " + name + ".img...");
  const auto block = m_log->document()->lastBlock();
  m_payloadLogBlocks.insert(name, block);
  QTextCursor cursor(block);
  const int prefix = block.text().indexOf("[提取]");
  if (prefix >= 0) {
    cursor.setPosition(block.position() + prefix);
    cursor.setPosition(block.position() + prefix + 4, QTextCursor::KeepAnchor);
    QTextCharFormat format;
    format.setForeground(QColor("#9333EA"));
    format.setFontWeight(QFont::Bold);
    cursor.mergeCharFormat(format);
  }
}
void OugaFlashWindow::payloadLogFinish(const QString &name, bool success) {
  const auto block = m_payloadLogBlocks.take(name);
  if (!block.isValid()) return;
  QTextCursor cursor(block);
  cursor.movePosition(QTextCursor::EndOfBlock);
  QTextCharFormat format;
  format.setForeground(QColor(success ? "#16A34A" : "#DC2626"));
  format.setFontWeight(QFont::Bold);
  cursor.insertText(success ? " OK" : " 失败", format);
  m_log->verticalScrollBar()->setValue(m_log->verticalScrollBar()->maximum());
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
  ++m_generation;
  m_task = Task::PathCheck;
  m_stopRequested = false;
  updateBusy();
  inspectPath(previous.name, file,
              [this, row, previous](Partition image, const QString &error) {
                const bool ok = error.isEmpty() && !m_stopRequested;
                if (ok) {
                  image.selected = previous.selected;
                  m_pages[m_page].images[row] = image;
                }
                display(m_pages[m_page].images, m_pages[m_page].directory);
                endTask(ok,
                        ok ? "文件路径已重新校验，目标名称保持不变"
                           : (m_stopRequested ? "路径修改已取消"
                                              : error + "；保留原有效镜像"));
              });
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
             (info.suffix().compare("img", Qt::CaseInsensitive) == 0 ||
              info.suffix().compare("iso", Qt::CaseInsensitive) == 0 ||
              info.suffix().compare("raw", Qt::CaseInsensitive) == 0 ||
              info.suffix().compare("sparse", Qt::CaseInsensitive) == 0))
    load(info.absolutePath());
  else {
    log("请拖入有效刷写文件夹，或在全量模式选择Payload.bin/ZIP文件");
    return;
  }
  event->acceptProposedAction();
}

void OugaFlashWindow::setExecutionOverlay(bool active) {
  if (active)
    m_overlay->begin();
  else
    m_overlay->hide();
}
void OugaFlashWindow::executionProgress(int writes, int total,
                                        const QString &stage) {
  const int percent =
      stage == "全部步骤成功"
          ? 100
          : (total > 0 ? int(qMin<qint64>(99, qint64(writes) * 100 /
                                                  (qint64(total) + 1)))
                       : 0);
  m_progress->setValue(percent);
  m_progress->setProperty("rate",
                          stage == "全部步骤成功" ? "已完成" : "刷写中...");
  m_progress->update();
  m_overlay->advance(percent, stage);
}
void OugaFlashWindow::transferProgress(qint64 bytes, qint64 total) {
  if (!m_transferClock.isValid()) {
    m_transferClock.start();
    m_lastBytes = bytes;
  }
  const qint64 elapsed = m_transferClock.elapsed();
  if (elapsed >= 500) {
    m_progress->setProperty(
        "rate", QString::number(qMax<qint64>(0, bytes - m_lastBytes) * 1000.0 /
                                    elapsed / 1048576,
                                'f', 1) +
                    "MB/s");
    m_transferClock.restart();
    m_lastBytes = bytes;
  }
  m_progress->setValue(
      total > 0 ? int(qBound(0.0, double(bytes) * 100 / double(total), 99.0))
                : 0);
  m_progress->update();
}
