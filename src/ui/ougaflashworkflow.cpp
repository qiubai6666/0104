#include "deviceoperationlease.h"
#include "ougaflashplanner.h"
#include "ougaflashservice.h"
#include "ougaflashwindow.h"
#include "ougapayloadextractor.h"
#include "ougapreparation.h"
#include "ougaromservice.h"
#include "resourceextractor.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QCoreApplication>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <QtConcurrent>
#include <algorithm>
using namespace Ouga;
namespace {
QString uniqueWork(const QString &directory, const QString &name) {
  return QDir(directory).filePath(name + "-" +
                                  QUuid::createUuid().toString(QUuid::Id128));
}
// Only choose a new path here. Preparation still validates and creates an empty
// destination, so a collision/race never authorizes overwriting existing files.
QString availablePayloadOutput(const QString &preferred) {
  const QFileInfo target(preferred);
  if (target.isDir() && !target.isSymLink() && !target.isJunction() &&
      !QDir(preferred)
           .entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden |
                      QDir::System)
           .isEmpty())
    return uniqueWork(target.absolutePath(), "images");
  return preferred;
}
} // namespace

void OugaFlashWindow::initializeServices(OugaCommandRunner *runner) {
  m_service =
      new OugaFlashService(runner ? runner : new OugaProcessRunner(this), this);
  m_prepare = new OugaPreparation(this);
  m_rom = new OugaRomService(this);
  connect(m_service, &OugaFlashService::log, this, &OugaFlashWindow::log);
  connect(m_prepare, &OugaPreparation::log, this, &OugaFlashWindow::log);
  connect(m_prepare, &OugaPreparation::archiveProgress, this, [this](int percent) {
    if (!m_stopRequested &&
        (m_task == Task::PayloadArchive || m_task == Task::RescueArchive)) {
      m_progress->setValue(percent);
      m_progress->setProperty("rate", QString("解压中 %1%").arg(percent));
      m_progress->update();
    }
  });
  connect(m_prepare, &OugaPreparation::payloadProgress, this, [this](int percent) {
    if (!m_stopRequested && m_task == Task::PayloadExtract) {
      m_progress->setValue(percent);
      m_progress->setProperty("rate", QString(m_unpackPayload ? "解包中 %1%" : "提取中 %1%").arg(percent));
      m_progress->update();
    }
  });
  connect(m_prepare, &OugaPreparation::payloadListed, this, [this](const QStringList &names) {
    if (m_task != Task::PayloadExtract || m_stopRequested) return;
    log(QString(m_unpackPayload ? "发现 %1 个分区: %2" : "本次将提取 %1 个分区: %2")
            .arg(names.size()).arg(names.join(", ")));
    if (m_unpackPayload) log("开始解包分区...");
  });
  connect(m_prepare, &OugaPreparation::payloadPartitionStarted, this,
          &OugaFlashWindow::payloadLogStart);
  connect(m_prepare, &OugaPreparation::payloadPartitionFinished, this,
          &OugaFlashWindow::payloadLogFinish);
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
          &OugaFlashWindow::transferProgress);
  connect(m_service, &OugaFlashService::progress, this,
          &OugaFlashWindow::executionProgress);
  connect(m_service, &OugaFlashService::checkpoint, this,
          [this](const Plan &plan) {
            const quint64 generation = m_generation;
            const bool accepted = !m_stopRequested && confirm(plan);
            m_service->confirmCheckpoint(accepted && !m_stopRequested &&
                                         generation == m_generation);
          });
}

void OugaFlashWindow::endTask(bool success, const QString &message) {
  ++m_generation;
  if (success && m_task == Task::PayloadExtract) {
    if (m_unpackPayload) {
      log("Payload解包成功！");
      log("解包完成，文件保存在: " + m_payloadOutput);
    } else {
      log(m_extractNames.size() == 1
              ? "分区 " + m_extractNames.first() + " 提取完成！" : "镜像提取完成！");
    }
    log(QString("已加载 %1 个镜像文件").arg(m_pages[m_page].images.size()));
  } else {
    log((success ? "完成：" : "已停止/失败：") + message);
  }
  if (m_session && !m_service->busy()) {
    DeviceOperationLease::release(m_service);
    m_session = false;
  }
  const bool payloadComplete = success && m_task == Task::PayloadExtract;
  m_task = Task::None;
  m_stopRequested = false;
  setExecutionOverlay(false);
  m_progress->setProperty("rate", success ? (payloadComplete ? "完成" : "阶段完成") : "已停止");
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
  return OugaProcessRunner::resolveToolPath(
      ResourceExtractor::getResourcePath(), QCoreApplication::applicationDirPath(),
      key, settings.value("Ouga/" + key).toString(), fallback);
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
  if (!selected.isFile() || !selected.isReadable() || selected.size() == 0)
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
  const QFileInfo directory(path);
  if (!m_page && directory.isDir() && directory.isReadable() &&
      !directory.isSymLink() && !directory.isJunction() &&
      !OugaPackage::hasImageCandidates(path)) {
    // The full-package field also selects an extraction destination. Choosing
    // it must not claim a scan failure or keep the previous package's images.
    m_task = Task::None;
    m_progress->setValue(0);
    m_progress->setProperty("rate", "待解包");
    m_progress->update();
    updateBusy();
    log("已选择解包输出目录：" + path +
        "；请选择Payload.bin或全量包ZIP，再点击“解包Payload”。"
        "提取和校验完成后才加载分区表；尚未写入设备。");
    return;
  }
  updateBusy();
  m_prepare->scan(path);
}
void OugaFlashWindow::inspectPath(
    const QString &name, const QString &file,
    std::function<void(Partition, const QString &)> ready) {
  const quint64 generation = m_generation;
  struct Checked {
    Partition image;
    QString error;
  };
  auto watcher = new QFutureWatcher<Checked>(this);
  connect(watcher, &QFutureWatcher<Checked>::finished, this,
          [this, watcher, ready, generation] {
            const auto result = watcher->result();
            watcher->deleteLater();
            if (generation == m_generation)
              ready(result.image, result.error);
          });
  watcher->setFuture(QtConcurrent::run([name, file] {
    Checked result;
    OugaPackage::inspect(name, file, &result.image, &result.error);
    return result;
  }));
}

void OugaFlashWindow::extractPayload(bool all) {
  if (isBusy() || (all && m_page))
    return;
  Page &page = m_pages[m_page];
  m_unpackPayload = all;
  m_extractNames.clear();
  if (!all) {
    QString selection = page.preset->currentText().trimmed();
    if (selection == "请选择快捷提取方案 ↓" || selection.isEmpty()) {
      log("错误：请选择或输入要提取的分区名称");
      return;
    }
    if (selection == "高通修复FastbootD关键分区")
      m_extractNames = criticalExtractionImages(Platform::Qualcomm);
    else if (selection == "联发科修复FastbootD关键分区")
      m_extractNames = criticalExtractionImages(Platform::MediaTek);
    else if (selection != "云解包方案-从云端提取线刷文件") {
      if (!safeName(selection)) {
        log("错误：无效的分区名称");
        return;
      }
      m_extractNames = {selection.toLower()};
    }
  }
  const bool cloudUnpack = page.preset->currentText().trimmed() ==
                           "云解包方案-从云端提取线刷文件";
  m_payloadSource = !m_page && (all || !cloudUnpack)
                        ? m_pages[0].payloadFile->text().trimmed()
                        : QString();
  if (m_payloadSource.isEmpty() && !all)
    m_payloadSource = page.url->text().trimmed();
  m_payloadSource.remove(QChar(0x60));
  if (m_payloadSource.isEmpty()) {
    log("错误：请先选择Payload.bin文件、全量包ZIP文件，或输入有效的全量包链接");
    return;
  }
  if (all && QUrl(m_payloadSource).scheme() != "https" &&
      QUrl(m_payloadSource).scheme() != "http" &&
      !QFileInfo(m_payloadSource).isFile()) {
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
  const QDir outputDirectory(output);
  // The field is updated to the actual result directory after extraction.
  // Repeating Unpack must use a sibling, not append /images to that result.
  const bool previousOutput = !m_payloadOutput.isEmpty() &&
      outputDirectory.absolutePath().compare(QDir(m_payloadOutput).absolutePath(),
                                             Qt::CaseInsensitive) == 0;
  m_payloadOutput =
      all && !previousOutput &&
              outputDirectory.dirName().compare("images", Qt::CaseInsensitive) != 0
          ? outputDirectory.filePath("images")
          : output;
  QUrl url(m_payloadSource);
  if (url.scheme() == "https" || url.scheme() == "http") {
    if (!OugaRomService::safeUrl(url)) {
      endTask(false, "无效或不安全的下载地址");
      return;
    }
    log("开始云提取：使用 HTTP Range 按需读取远程 Payload，不下载完整全量包");
    preparePayloadSource();
  } else
    preparePayloadSource();
}
void OugaFlashWindow::preparePayloadSource() {
  const quint64 generation = m_generation;
  if (!continueTask(generation))
    return;
  const QUrl sourceUrl(m_payloadSource);
  if (!m_unpackPayload || sourceUrl.scheme() == "https" ||
      sourceUrl.scheme() == "http") {
    startPayloadPreparation();
    return;
  }
  log("正在判断刷机包对应的机型...");
  m_task = Task::PayloadInspect;
  updateBusy();
  auto watcher = new QFutureWatcher<QString>(this);
  connect(watcher, &QFutureWatcher<QString>::finished, this,
          [this, watcher, generation] {
    const QString model = watcher->result();
    watcher->deleteLater();
    if (!continueTask(generation))
      return;
    if (model.isEmpty()) {
      log("解析刷机包对应机型失败，跳过解析步骤...");
      startPayloadPreparation();
      return;
    }
    log("刷机包对应的机型为" + model + "...");
    log("刷错包会导致设备黑砖，5秒后开始解包...");
    m_task = Task::PayloadModelWait;
    m_progress->setProperty("rate", "5秒后开始解包...");
    m_progress->update();
    updateBusy();
    // Reference warning delay, without sleeping/blocking the GUI thread.
    // The generation guard also invalidates a cancelled task's timer.
    QTimer::singleShot(5000, Qt::PreciseTimer, this, [this, generation] {
      if (continueTask(generation))
        startPayloadPreparation();
    });
  });
  const QString source = m_payloadSource;
  watcher->setFuture(QtConcurrent::run([source] {
    return OugaPackage::payloadDeviceModel(source);
  }));
}
void OugaFlashWindow::startPayloadPreparation() {
  if (!continueTask(m_generation))
    return;
  log(m_unpackPayload ? "开始解包，输出将实时显示在日志窗口中。"
                      : m_extractNames.isEmpty() ? "开始提取镜像..."
                                               : "开始提取分区: " + m_extractNames.join(", "));
  const QUrl sourceUrl(m_payloadSource);
  const bool remote = sourceUrl.scheme() == "https" || sourceUrl.scheme() == "http";
  log("源文件: " + (remote ? sourceUrl.toDisplayString(QUrl::RemoveQuery |
      QUrl::RemoveFragment | QUrl::RemoveUserInfo) : m_payloadSource));
  log("输出目录: " + m_payloadOutput);
  runPayload();
}
void OugaFlashWindow::extractPayloadArchive() {
  const quint64 generation = m_generation;
  {
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
    m_progress->setValue(0);
    m_progress->setProperty("rate", "解压中...");
    updateBusy();
    m_prepare->extractArchive(sevenZip, m_payloadSource, m_archiveRoot, false);
  }
}
void OugaFlashWindow::runPayload() {
  const quint64 generation = m_generation;
  if (!continueTask(generation))
    return;
  const QUrl sourceUrl(m_payloadSource);
  if (sourceUrl.scheme() == "https" || sourceUrl.scheme() == "http") {
    if (m_unpackPayload) {
      const QString output = availablePayloadOutput(m_payloadOutput);
      if (output != m_payloadOutput)
        log("输出目录已有文件，原文件保留；本次解包输出目录: " + output);
      m_payloadOutput = output;
    }
    m_task = Task::PayloadExtract;
    m_payloadLogBlocks.clear();
    m_progress->setValue(0);
    m_progress->setProperty("rate", "云提取中...");
    updateBusy();
    m_prepare->payloadUrl(sourceUrl, m_payloadOutput, m_extractNames);
    return;
  }
  struct Inspection {
    QString error, output;
    QStringList selected, missing;
    bool archive = false, delta = false, native = false;
  };
  // ZIP lookup, protobuf parsing and extent sorting are all file/CPU work.
  // Keep the task alive until its worker returns, including after Stop.
  m_task = Task::PayloadInspect;
  m_progress->setProperty("rate", "读取分区信息...");
  m_progress->update();
  updateBusy();
  auto watcher = new QFutureWatcher<Inspection>(this);
  connect(watcher, &QFutureWatcher<Inspection>::finished, this,
          [this, watcher, generation] {
    const auto result = watcher->result();
    watcher->deleteLater();
    if (!continueTask(generation))
      return;
    if (!result.error.isEmpty()) {
      endTask(false, result.error);
      return;
    }
    if (result.output != m_payloadOutput) {
      m_payloadOutput = result.output;
      log("输出目录已有文件，原文件保留；本次解包输出目录: " + m_payloadOutput);
    }
    for (const QString &name : result.missing)
      log("警告：未找到分区 '" + name + "'，该ROM可能不包含此分区");
    m_extractNames = result.selected;
    if (result.archive) {
      extractPayloadArchive();
      return;
    }
    QString tool, oldDirectory;
    if (!result.native) {
      if (result.delta) {
        oldDirectory = QFileDialog::getExistingDirectory(
            this, "增量Payload：选择匹配的旧镜像目录");
        if (!continueTask(generation))
          return;
        if (oldDirectory.isEmpty()) {
          endTask(false, "增量Payload缺少旧镜像，拒绝提取");
          return;
        }
      }
      tool = requireTool("payload", "Payload工具",
                         ResourceExtractor::getResourcePath() + "/payload.exe");
      if (!continueTask(generation))
        return;
      if (tool.isEmpty()) {
        endTask(false, "Payload工具不可用");
        return;
      }
    }
    m_task = Task::PayloadExtract;
    m_payloadLogBlocks.clear();
    m_progress->setValue(0);
    m_progress->setProperty("rate", m_unpackPayload ? "解包中..." : "提取中...");
    updateBusy();
    m_prepare->payload(tool, m_payloadSource, m_payloadOutput, m_extractNames,
                       oldDirectory);
  });
  const QString source = m_payloadSource;
  const QStringList requested = m_extractNames;
  const QString preferredOutput = m_payloadOutput;
  const bool unpack = m_unpackPayload;
  watcher->setFuture(QtConcurrent::run([source, requested, preferredOutput, unpack] {
    Inspection result;
    result.output = unpack ? availablePayloadOutput(preferredOutput) : preferredOutput;
    result.selected = requested;
    QFile file(source);
    if (!file.open(QIODevice::ReadOnly)) {
      result.error = "源文件无法读取";
      return result;
    }
    const QByteArray magic = file.read(4);
    file.close();
    if (magic.startsWith("PK") ||
        QFileInfo(source).suffix().compare("zip", Qt::CaseInsensitive) == 0) {
      quint64 offset = 0, size = 0;
      if (OugaPayloadExtractor::locate(source, &offset, &size) !=
          OugaPayloadExtractor::Zip::Stored) {
        result.archive = true;
        return result;
      }
    }
    QVector<OugaPayloadEntry> entries;
    OugaPayloadLayout layout;
    bool delta = false;
    if (!OugaPackage::payloadManifest(source, &entries, &delta, &result.error,
                                      &layout))
      return result;
    if (!requested.isEmpty()) {
      QSet<QString> available;
      for (const auto &entry : entries)
        available.insert(entry.name);
      result.selected.clear();
      for (const QString &name : requested)
        if (available.contains(name))
          result.selected << name;
        else
          result.missing << name;
      if (result.selected.isEmpty()) {
        result.error = "Payload中没有所请求分区";
        return result;
      }
    }
    // An unselected delta partition must not require a baseline.
    for (const auto &entry : entries)
      if (result.selected.isEmpty() || result.selected.contains(entry.name))
        result.delta |= entry.requiresOldImage;
    result.native = !result.delta &&
        OugaPayloadExtractor::supported(entries, result.selected, layout);
    // The external tool only accepts a plain payload.bin.
    result.archive = !result.native && layout.base != 0;
    return result;
  }));
}
void OugaFlashWindow::preparationFinished(bool success,
                                          const QString &message) {
  if (m_task == Task::None)
    return;
  // Successful Payload stages have the reference-style messages above; keep
  // validation diagnostics on failures without adding duplicate success lines.
  if (!success || (m_task != Task::PayloadArchive && m_task != Task::PayloadExtract))
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
  if (m_task != Task::RescueDownload)
    return;
  log(message);
  if (!success || m_stopRequested || m_downloaded.isEmpty()) {
    endTask(false, m_stopRequested ? "下载已取消，断点文件保留" : message);
    return;
  }
  const quint64 generation = m_generation;
  QTimer::singleShot(0, this, [this, generation] {
    if (!continueTask(generation))
      return;
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
    m_progress->setValue(0);
    m_progress->setProperty("rate", "解压中...");
    m_prepare->extractArchive(tool, m_downloaded, output);
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
    const bool userspace = startsInFastbootd(m_requestedMode);
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
  const bool additional = needsAdditionalImages(m_requestedMode);
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
      inspectPath(name, file, [this](Partition image, const QString &error) {
        if (m_stopRequested || !error.isEmpty()) {
          endTask(false, m_stopRequested ? "已取消附加镜像校验" : error);
          return;
        }
        m_pages[m_page].images << image;
        display(m_pages[m_page].images, m_pages[m_page].directory);
        buildAndExecute();
      });
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
    return OugaProcessRunner::formatToolsError(
               toolPath("fastboot", ResourceExtractor::getFastbootPath()))
        .isEmpty();
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
    const QStringList required = criticalImages(m_device.platform);
    for (auto &image : planImages)
      image.selected = required.contains(baseName(image.name));
  }
  if (std::any_of(planImages.cbegin(), planImages.cend(),
                  [](const Partition &p) {
                    return p.selected && p.sha256.size() != 32;
                  })) {
    m_task = Task::PathCheck;
    updateBusy();
    log("正在并行计算所选镜像 SHA-256（文件夹加载时不校验）…");
    struct Hashed {
      QVector<Partition> images;
      QString error;
    };
    auto watcher = new QFutureWatcher<Hashed>(this);
    connect(watcher, &QFutureWatcher<Hashed>::finished, this,
            [this, watcher, taskGeneration] {
              const Hashed result = watcher->result();
              watcher->deleteLater();
              if (!continueTask(taskGeneration))
                return;
              if (!result.error.isEmpty()) {
                endTask(false, "镜像校验失败：" + result.error);
                return;
              }
              for (Partition &image : m_pages[m_page].images)
                for (const Partition &h : result.images)
                  if (h.path == image.path && h.sha256.size() == 32)
                    image.sha256 = h.sha256;
              log("所选镜像 SHA-256 计算完成");
              buildAndExecute();
            });
    watcher->setFuture(QtConcurrent::run([planImages] {
      Hashed result{planImages, {}};
      OugaPackage::hashImages(&result.images, &result.error);
      return result;
    }));
    return;
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
  setExecutionOverlay(true);
  updateBusy();
  m_service->execute(m_plan);
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
  else if (m_task != Task::PathCheck && m_task != Task::PayloadInspect &&
           m_task != Task::Arb)
    endTask(false, "已取消后续阶段；已产生的文件保留");
}
