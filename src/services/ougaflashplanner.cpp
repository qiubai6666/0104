#include "ougaflashplanner.h"
#include "ougapackage.h"
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <algorithm>
#include <climits>
using namespace Ouga;
namespace {
bool fail(QString *error, const QString &message) {
  if (error) *error = message;
  return false;
}
struct Builder {
  Plan p;
  QString error, slot;
  bool mode = true;
  QString target(const QString &name, const QString &requestedSlot) {
    return p.device.targetPartition(name, requestedSlot, &error);
  }
  void command(const QString &title, const QStringList &args,
               const QString &target = {}, bool missing = false,
               bool continueOnFailure = false) {
    Step step;
    step.title = title;
    step.arguments = args;
    step.target = target;
    step.userspace = mode;
    step.allowMissing = missing;
    step.continueOnFailure = continueOnFailure;
    p.steps << step;
  }
  void switchMode(bool userspace) {
    if (mode == userspace) return;
    Step step;
    step.kind = Step::ModeSwitch;
    step.title = userspace ? "重启到FastbootD" : "重启到Fastboot";
    step.arguments = {"reboot", userspace ? "fastboot" : "bootloader"};
    step.userspace = userspace;
    p.steps << step;
    mode = userspace;
  }
  void wait(int ms, const QString &title) {
    Step step;
    step.kind = Step::Wait;
    step.title = title;
    step.waitMs = ms;
    step.userspace = mode;
    p.steps << step;
  }
  bool flash(const Partition &image, const QString &requestedSlot,
             bool continueOnFailure = true) {
    QString destination = target(image.name, requestedSlot);
    const bool missing = destination.isEmpty();
    if (missing) {
      // The reference records an unresolved target as a failed write and
      // continues the ordinary image loop, rather than rejecting the package.
      destination = baseName(image.name) + "_" + requestedSlot;
      error.clear();
    }
    return flashTarget(image, destination, continueOnFailure, missing);
  }
  bool flashTarget(const Partition &image, const QString &destination,
                   bool continueOnFailure = true, bool missing = false) {
    if (image.bytes > LLONG_MAX - p.totalBytes) {
      error = "刷写总大小溢出";
      return false;
    }
    Step step;
    step.title = "[Flashing] " + QFileInfo(image.path).fileName() + " → " +
                 destination + ".img";
    step.arguments = {"flash", destination, image.path};
    step.target = destination;
    step.image = image.path;
    step.sha256 = image.sha256;
    step.bytes = image.bytes;
    step.userspace = mode;
    step.continueOnFailure = continueOnFailure;
    step.targetMissing = missing;
    p.steps << step;
    ++p.flashCount;
    p.totalBytes += image.bytes;
    return true;
  }
  bool both(const Partition &image, bool continueOnFailure = true) {
    const QString a = target(image.name, "a");
    const QString b = target(image.name, "b");
    return flash(image, "a", continueOnFailure) &&
           (!a.isEmpty() && a == b ? true : flash(image, "b", continueOnFailure));
  }
  void cow() {
    // DeleteCowPartitions reads a fresh getvar table at this execution point.
    Step step;
    step.kind = Step::CowCleanup;
    step.title = "解析COW快照分区";
    step.userspace = mode;
    p.steps << step;
  }
  bool rebuild(const QVector<Partition> &images) {
    QStringList names;
    for (const Partition &image : images) {
      const QString name = baseName(image.name);
      if (logicalName(name) && !names.contains(name)) names << name;
    }
    // The reference removes both existing sides for selected logical bases,
    // then recreates only the requested/target slot.
    const bool ab = p.options.mode == FlashMode::BothSlots;
    if (ab) {
      // AB sends both delete commands for each selected base, accepting missing
      // partitions at the command boundary rather than omitting the attempt.
      for (const QString &name : names)
        for (const QString &suffix : {QString("a"), QString("b")}) {
          const QString destination = name + "_" + suffix;
          command("删除逻辑分区 " + destination,
                  {"delete-logical-partition", destination}, destination, true);
        }
    } else for (const QString &suffix : {QString("a"), QString("b")}) {
      for (const QString &name : names) {
        const QString destination = name + "_" + suffix;
        if (p.device.partitions.contains(destination))
          command("删除逻辑分区 " + destination,
                  {"delete-logical-partition", destination}, destination, true);
      }
    }
    for (const QString &name : names) {
      const QString destination = name + "_" + slot;
      command("创建逻辑分区 " + destination,
              {"create-logical-partition", destination,
               p.options.mode == FlashMode::BothSlots ? "0" : "40"}, destination);
    }
    return true;
  }
  bool additional(const QVector<Partition> &images) {
    for (const QString name : {"my_company", "my_preload"}) {
      auto image = std::find_if(images.cbegin(), images.cend(),
          [&](const Partition &value) { return baseName(value.name) == name; });
      if (image == images.cend()) {
        error = "缺少必须刷写的双my镜像：tmp\\" + name + ".img";
        return false;
      }
      if (!flash(*image, slot)) return false;
    }
    return true;
  }
  bool critical(const QVector<Partition> &images) {
    bool found = false;
    for (const QString &name : criticalImages(p.device.platform)) {
      auto image = std::find_if(images.cbegin(), images.cend(),
          [&](const Partition &value) {
            return (p.options.mode == FlashMode::AfterSalesBootloader
                ? value.name.toLower() : baseName(value.name)) == name;
          });
      if (image == images.cend()) {
        if (p.options.mode == FlashMode::RepairFastbootd) {
          Step message;
          message.kind = Step::Message;
          message.title = "警告: 未找到 " + name + " 镜像文件，跳过";
          message.userspace = mode;
          p.steps << message;
        }
        continue;
      }
      found = true;
      // Repair and sale FB do not run inside a full-package session:
      // FlashPartitionToSlot constructs both explicit suffixed targets.
      if (!flashTarget(*image, name + "_a")) return false;
      if (p.options.mode == FlashMode::AfterSalesBootloader)
        wait(500, {});
      if (!flashTarget(*image, name + "_b")) return false;
    }
    return found || p.options.mode == FlashMode::AfterSalesBootloader ||
           p.options.mode == FlashMode::RepairFastbootd;
  }
  void finish() {
    const bool afterSales = p.options.mode == FlashMode::AfterSalesBootloader ||
                            p.options.mode == FlashMode::AfterSalesFastbootd;
    if (p.options.mode == FlashMode::RepairFastbootd) return;
    if (afterSales && !p.options.clearData && !p.options.autoReboot) return;
    const int beforeSwitch = p.steps.size();
    switchMode(true);
    if (afterSales && p.steps.size() != beforeSwitch)
      p.steps.last().target = "post-actions";
    // Full packages erase FRP before the optional wipe; sale packages erase
    // it only as the last part of a requested wipe.
    if (!afterSales)
      command("擦除FRP", {"erase", "frp"}, "frp", false, true);
    if (p.options.clearData) {
      Step wipe;
      wipe.kind = Step::ClearData;
      wipe.title = "清除手机数据";
      wipe.userspace = true;
      p.steps << wipe;
      if (afterSales)
        command("擦除FRP", {"erase", "frp"}, "frp", false, true);
    }
    if (p.options.autoReboot)
      command("自动重启设备", {"reboot"}, {}, false, true);
  }
};
bool extra(const Partition &image) {
  const QString name = baseName(image.name);
  return name == "my_company" || name == "my_preload";
}
} // namespace
bool OugaFlashPlanner::compatibleTable(const QVector<Partition> &images,
                                        const Device &device, QString *error) {
  for (const Partition &image : images) {
    if (!image.selected || image.path.trimmed().isEmpty() ||
        image.path.contains("(缺失)", Qt::CaseInsensitive) || !QFileInfo::exists(image.path)) continue;
    const QString name = baseName(image.name);
    if (logicalName(name) || name == "modem") continue;
    if (!device.partitions.contains(name) && !device.partitions.contains(name + "_a") &&
        !device.partitions.contains(name + "_b"))
      return fail(error, "分区表效验失败，刷写固件包与设备分区表不匹配，请检查刷机包是否正确，若刷机包正确，可关闭分区表效验功能后，重新点击开始线刷按钮.");
  }
  return true;
}
QVector<Step> OugaFlashPlanner::postActions(const Device &device,
                                          const Options &options) {
  Builder b;
  b.p.device = device;
  b.p.options = options;
  b.mode = device.userspace;
  b.finish();
  return b.p.steps;
}
bool OugaFlashPlanner::build(const QVector<Partition> &sourceImages, const Device &device,
                             const Options &options, Plan *out, QString *error) {
  if (error) error->clear();
  if (!out) return fail(error, "缺少计划输出");
  *out = Plan();
  Builder b;
  const QVector<Partition> images = options.mode == FlashMode::AfterSalesBootloader &&
      !options.packageDirectory.isEmpty()
      ? OugaPackage::saleBootloaderSources(options.packageDirectory) : sourceImages;
  b.p.device = device;
  b.p.options = options;
  b.p.images = images;
  b.mode = device.userspace;
  b.slot = device.slot;
  const bool repair = options.mode == FlashMode::RepairFastbootd;
  const bool afterFb = options.mode == FlashMode::AfterSalesBootloader;
  const bool afterFbd = options.mode == FlashMode::AfterSalesFastbootd;
  const bool independent = repair || afterFb || afterFbd;
  if (repair || (afterFb && !options.packageDirectory.isEmpty()) ||
      (independent && device.platform == Platform::Unknown)) {
    const bool lk = std::any_of(images.cbegin(), images.cend(),
        [afterFb](const Partition &image) {
          return (afterFb ? image.name.toLower() : baseName(image.name)) == "lk" &&
              (!afterFb || QFileInfo(image.path).dir().dirName().compare("IMAGES", Qt::CaseInsensitive) == 0);
        });
    b.p.device.platform = lk ? Platform::MediaTek : Platform::Qualcomm;
  }
  const Platform platform = b.p.device.platform;
  if (device.serial.trimmed().isEmpty() ||
      device.serial.contains(QRegularExpression("[\\s\\x00-\\x1f]")))
    return fail(error, "必须绑定有效序列号");
  if (!device.modeKnown || (!independent && (!device.unlockKnown || !device.unlocked)))
    return fail(error, "设备模式/解锁状态未知或设备未解锁");
  if (!independent && ((platform != Platform::Qualcomm && platform != Platform::MediaTek) ||
      (device.slot != "a" && device.slot != "b")))
    return fail(error, "为了本次的刷写安全，请确保你的设备为A/B槽位设备");
  if ((afterFb || afterFbd) && b.slot.isEmpty()) b.slot = "a";
  const bool ab = options.mode == FlashMode::BothSlots;
  const bool force = options.mode == FlashMode::Force;
  const bool only = options.mode == FlashMode::OnlyFastbootd;
  if (!repair && !afterFb && !afterFbd && !ab && !force && !only &&
      options.mode != FlashMode::Normal)
    return fail(error, "未知刷写模式");
  if (options.afterSuper && !afterFb)
    return fail(error, "Super续刷仅适用于售后Fastboot模式");
  if (device.userspace != startsInFastbootd(options.mode))
    return fail(error, "设备尚未进入所需的刷写模式");
  if (only && platform != Platform::Qualcomm)
    return fail(error, "仅FastbootD线刷不适用于联发科设备，已安全终止任务.");
  QVector<Partition> selected;
  QSet<QString> names;
  for (const Partition &image : images) {
    if (!image.selected && !repair) continue;
    const QString name = independent && !repair ? image.name.toLower() : baseName(image.name);
    if (!safeName(name) || name == "payload")
      return fail(error, "分区名称无效：" + name);
    if (names.contains(name)) {
      if (repair) continue;
      if (!independent) return fail(error, "检测到重复刷写目标：" + name);
    }
    if (image.path.isEmpty() || !QFileInfo::exists(image.path) ||
        image.path.contains("(缺失)", Qt::CaseInsensitive)) continue;
    names.insert(name);
    selected << image;
  }
  if (selected.isEmpty() && !afterFb && !repair) return fail(error, "分区表中没有已勾选的镜像文件，请先勾选需要刷写的分区.");
  if (options.validateTable && !independent && !compatibleTable(selected, device, error)) return false;
  if (ab) {
    if (options.targetSlot != "a" && options.targetSlot != "b")
      return fail(error, "请选择启动槽位");
    b.slot = options.targetSlot;
  }
  if (force) b.slot = "a";
  if (only) b.slot = device.slot == "a" ? "b" : "a";
  if (repair) {
    if (!b.critical(selected)) return fail(error, b.error);
    b.switchMode(true);
    b.p.steps.last().title = "重启设备到FastbootD";
    b.p.options.clearData = false;
    b.p.options.autoReboot = false;
  } else if (afterFb && !options.afterSuper) {
    // Reference FB always flashes IMAGES/super.img, regardless of its checkbox.
    auto super = std::find_if(images.cbegin(), images.cend(),
        [](const Partition &image) { return baseName(image.name) == "super"; });
    if (super == images.cend()) return fail(error, "未找到super.img文件");
    if (super->path.isEmpty() || !QFileInfo::exists(super->path))
      return fail(error, "镜像文件无效：super");
    b.command("擦除super分区", {"erase", "super"}, "super", false, true);
    if (!b.flashTarget(*super, "super", false)) return fail(error, b.error);
    b.wait(120000, "单次传输数据较大，等待设备冷却中...");
    Step step;
    step.kind = Step::Checkpoint;
    step.title = "设备冷却完成";
    step.userspace = false;
    b.p.steps << step;
  } else {
    QVector<Partition> active, additional, modem;
    const bool needsExtra = needsAdditionalImages(options.mode);
    for (const Partition &image : (afterFb ? images : selected)) {
      const QString name = afterFb || afterFbd ? image.name.toLower() : baseName(image.name);
      if (needsExtra && extra(image)) additional << image;
      else if (afterFb && (name == "super" || (baseName(name) == name && logicalName(name)) || name == "userdata" ||
                          name == "metadata" || name == "persist")) continue;
      else if (afterFbd && name.contains("modem")) modem << image;
      else active << image;
    }
    std::stable_sort(active.begin(), active.end(),
        [](const Partition &a, const Partition &b) { return a.bytes < b.bytes; });
    if (afterFb) {
      if (!b.critical(images)) return fail(error, b.error);
      b.switchMode(true);
      Step slot;
      slot.kind = Step::SaleSlot;
      b.p.steps << slot;
      b.cow();
      Step table;
      table.kind = Step::SaleTable;
      table.userspace = true;
      b.p.steps << table;
      const QStringList critical = criticalImages(platform);
      for (const Partition &image : active) {
        const QString name = image.name.toLower();
        if (critical.contains(name) ||
            !device.partitions.contains(name + "_" + b.slot)) continue;
        if (!b.flashTarget(image, name + "_a") ||
            !b.flashTarget(image, name + "_b")) return fail(error, b.error);
      }
      Step step;
      step.kind = Step::Reprobe;
      step.title = "判断最佳启动槽位";
      step.userspace = true;
      b.p.steps << step;
    } else {
      for (int i = active.size() - 1; i >= 0; --i) {
        const Partition &image = active.at(i);
        const QString imageBase = baseName(image.name);
        const bool modemLike = imageBase == "modem";
        const bool slotted = !device.partitions.contains(imageBase);
        if (modemLike &&
            ((ab && slotted) || (force && device.slot == "b") ||
             (slotted && platform == Platform::Qualcomm && !only))) {
          modem.prepend(image);
          active.removeAt(i);
        }
      }
      const bool preSwitch = force && device.slot == "b";
      if (preSwitch || ab) {
        b.cow();
        for (const Partition &image : active)
          if (!logicalName(image.name) &&
              !(ab ? b.both(image) : b.flash(image, "a")))
            return fail(error, b.error);
      }
      if (preSwitch || ab) {
        Step guard;
        guard.kind = Step::PreSwitchGuard;
        b.p.steps << guard;
      }
      if (only) b.wait(5000, "仅FastbootD线刷适用于没有Fastboot的高通骁龙设备，天玑处理器设备请勿勾选...");
      if (force || only || (ab && b.slot != device.slot)) {
        if (b.slot != device.slot)
          b.command("切换槽位：" + device.slot.toUpper() + "槽 → " + b.slot.toUpper() + "槽位",
                    {"set_active", b.slot}, b.slot);
        QVector<Partition> rebuildImages;
        if (ab) {
          for (const Partition &image : selected)
            if (!extra(image)) rebuildImages << image;
          for (const QString name : {"my_company", "my_preload"})
            for (const Partition &image : additional)
              if (baseName(image.name) == name) rebuildImages << image;
        } else {
          rebuildImages = active;
          rebuildImages += additional;
        }
        if (!b.rebuild(rebuildImages)) return fail(error, b.error);
        if (!preSwitch && !ab) b.cow();
      }
      if (afterFbd) {
        Step slot;
        slot.kind = Step::SaleSlot;
        b.p.steps << slot;
      }
      if (!preSwitch && !ab && !(force || only || (ab && b.slot != device.slot))) b.cow();
      if (afterFbd) {
        Step table;
        table.kind = Step::SaleTable;
        table.userspace = true;
        b.p.steps << table;
      }
      if (needsExtra && !b.additional(additional)) return fail(error, b.error);
      for (const Partition &image : active) {
        if (afterFbd && !device.partitions.contains(image.name.toLower() + "_" + b.slot)) continue;
        if ((preSwitch || ab) && !logicalName(image.name)) continue;
        if (!(afterFbd ? b.flashTarget(image, image.name.toLower() + "_" + b.slot)
                       : b.flash(image, b.slot))) return fail(error, b.error);
      }
      if (!modem.isEmpty()) {
        if (afterFbd || platform == Platform::Qualcomm) {
          const int beforeSwitch = b.p.steps.size();
          b.switchMode(false);
          if (afterFbd && b.p.steps.size() != beforeSwitch)
            b.p.steps.last().target = "sale-modem";
        }
        for (const Partition &image : modem) {
          Partition source = image;
          if (afterFbd) source.name = "modem";
          const QString modemBase = baseName(source.name);
          const bool slotlessModem = device.partitions.contains(modemBase) &&
              !device.partitions.contains(modemBase + "_a") &&
              !device.partitions.contains(modemBase + "_b");
          const bool dual = ab || afterFbd ||
              (platform == Platform::Qualcomm && !slotlessModem);
          const int first = b.p.steps.size();
          if (afterFbd) {
            // Sale FBD sends every modem-like image to modem_a/modem_b,
            // even on MTK or when the reported table only contains modem.
            if (!b.flashTarget(source, "modem_a") ||
                !b.flashTarget(source, "modem_b")) return fail(error, b.error);
          } else if (!(dual ? b.both(source) : b.flash(source, b.slot))) {
            return fail(error, b.error);
          }
          if (dual && !ab && b.p.steps.size() == first + 2) {
            b.p.steps[first].bytes = image.bytes / 2;
            b.p.steps[first + 1].bytes = afterFbd ? image.bytes / 2
                                                               : image.bytes - image.bytes / 2;
            b.p.totalBytes -= image.bytes;
          }
        }
      }
    }
    Step postActions;
    postActions.kind = Step::PostActions;
    postActions.userspace = b.mode;
    b.p.steps << postActions;
    b.finish();
  }
  if (options.mode == FlashMode::Normal || force || only || afterFbd) {
    // CreateOujiaFlashPlan counts each selected source and mandatory auxiliary
    // image once, including sources without a matching sale target.
    b.p.totalBytes = 0;
    for (const Partition &image : selected) {
      if (image.bytes > LLONG_MAX - b.p.totalBytes)
        return fail(error, "刷写总大小溢出");
      b.p.totalBytes += image.bytes;
    }
  }
  b.p.options.targetSlot = b.slot;
  if (!independent && b.p.totalBytes <= 0)
    return fail(error, "刷写任务镜像总大小无效");
  b.p.summary = QString("%1 | %2 | %3 | %4 次刷写 | %5")
       .arg(b.p.device.serial, platformText(platform), b.slot,
           QString::number(b.p.flashCount), sizeText(b.p.totalBytes));
  *out = b.p;
  return true;
}
