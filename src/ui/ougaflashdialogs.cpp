#include "deviceoperationlease.h"
#include "ougaflashservice.h"
#include "ougaflashwindow.h"
#include "ougapreparation.h"
#include "ougaromservice.h"
#include "resourceextractor.h"
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QIcon>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>
using namespace Ouga;
namespace {
QString selectItem(QWidget *parent, const QString &title, const QString &prompt,
                   const QStringList &items, int width = 470) {
  QDialog dialog(parent);
  dialog.setObjectName("OugaSelectionDialog");
  dialog.setWindowTitle(title);
  dialog.resize(width, 560);
  auto layout = new QVBoxLayout(&dialog);
  layout->setContentsMargins(18, 18, 18, 18);
  auto heading = new QLabel(title);
  heading->setStyleSheet("font-size:17px; font-weight:600; color:#334155;");
  layout->addWidget(heading);
  layout->addWidget(new QLabel(prompt));
  auto list = new QListWidget;
  list->addItems(items);
  list->setStyleSheet(
      "QListWidget { background:#F8FAFC; color:#526579; border:1px solid "
      "#D9E2EC; border-radius:8px; padding:4px; font-size:12.5px; } "
      "QListWidget::item { padding:7px; } QListWidget::item:selected { "
      "background:#E3EBF1; color:#334155; }");
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
  auto agree = new QCheckBox("设备为本人所有或已获授权；已核对计划及其中列出的 FRP 等不可逆操作，"
                             "并已备份数据",
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
  caution->setStyleSheet("color:#718192;");
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
    for (const QString ext :
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
