#include "ougaflashplanner.h"
#include <QRegularExpression>
#include <algorithm>
#include <climits>
using namespace Ouga;
namespace {
bool fail(QString *e, const QString &s) {
  if (e)
    *e = s;
  return false;
}
struct Builder {
  Plan p;
  QString error;
  QString slot;
  bool mode = true;
  QMap<QString, QString> written;
  QSet<QString> rebuilt;
  QString target(const QString &name, const QString &requestedSlot) {
    return p.device.targetPartition(name, requestedSlot, &error);
  }
  void command(const QString &title, const QStringList &args,
               const QString &target = {}, bool missing = false) {
    Step s;
    s.title = title;
    s.arguments = args;
    s.target = target;
    s.userspace = mode;
    s.allowMissing = missing;
    p.steps << s;
  }
  void switchMode(bool userspace) {
    if (mode == userspace)
      return;
    Step s;
    s.kind = Step::ModeSwitch;
    s.title = userspace ? "进入 FastbootD 并确认原设备稳定"
                        : "进入普通 Fastboot 并确认原设备稳定";
    s.arguments = {"reboot", userspace ? "fastboot" : "bootloader"};
    s.userspace = userspace;
    p.steps << s;
    mode = userspace;
  }
  void wait(int ms, const QString &title) {
    Step s;
    s.kind = Step::Wait;
    s.title = title;
    s.waitMs = ms;
    s.userspace = mode;
    p.steps << s;
  }
  bool flashTarget(const Partition &img, const QString &t) {
    if (t.isEmpty())
      return false;
    if (written.contains(t)) {
      if (written[t] == img.path)
        return true;
      error = "两个来源映射同一目标：" + t;
      return false;
    }
    if (!rebuilt.contains(t) &&
        (!p.device.sizes.contains(t) ||
         quint64(img.expandedBytes) > p.device.sizes[t])) {
      error = "目标容量未知或镜像超容量：" + t;
      return false;
    }
    if (img.expandedBytes > LLONG_MAX - p.totalBytes) {
      error = "刷写总大小溢出";
      return false;
    }
    Step step;
    step.title = "写入 " + t;
    step.arguments = {"flash", t, img.path};
    step.target = t;
    step.image = img.path;
    step.sha256 = img.sha256;
    step.bytes = img.expandedBytes;
    step.userspace = mode;
    p.steps << step;
    written[t] = img.path;
    ++p.flashCount;
    p.totalBytes += img.expandedBytes;
    return true;
  }
  bool flash(const Partition &image, const QString &requestedSlot) {
    return flashTarget(image, target(image.name, requestedSlot));
  }
  bool both(const Partition &image) {
    const QString a = target(image.name, "a");
    if (a.isEmpty())
      return false;
    const QString b = target(image.name, "b");
    return !b.isEmpty() && flashTarget(image, a) &&
           (a == b || flashTarget(image, b));
  }
  bool flashImages(const QVector<Partition> &images,
                   bool bothPhysical = false) {
    for (const Partition &image : images)
      if (!(bothPhysical && !p.device.isLogical(image.name)
                ? both(image)
                : flash(image, slot)))
        return false;
    return true;
  }
  bool flashCritical(const QVector<Partition> &images, const QString &missing) {
    for (const QString &name : criticalImages(p.device.platform)) {
      auto image =
          std::find_if(images.cbegin(), images.cend(), [&](const Partition &i) {
            return baseName(i.name) == name;
          });
      if (image == images.cend()) {
        error = missing + name;
        return false;
      }
      if (!both(*image))
        return false;
    }
    return true;
  }
  bool deferModem() const {
    return p.options.mode != FlashMode::OnlyFastbootd &&
           ((p.device.platform == Platform::Qualcomm &&
             !p.device.partitions.contains("modem")) ||
            (p.options.mode == FlashMode::BothSlots &&
             !p.device.partitions.contains("modem")) ||
            p.options.mode == FlashMode::AfterSalesFastbootd ||
            (p.options.mode == FlashMode::Force && p.device.slot == "b"));
  }
  bool flashDeferredModem(const QVector<Partition> &images) {
    if (images.isEmpty())
      return true;
    if (p.device.platform == Platform::Qualcomm &&
        !p.device.partitions.contains("modem"))
      switchMode(false);
    const bool dual = p.device.platform == Platform::Qualcomm ||
                      p.options.mode == FlashMode::AfterSalesFastbootd ||
                      p.options.mode == FlashMode::BothSlots;
    for (const Partition &image : images)
      if (!(dual ? both(image) : flash(image, slot)))
        return false;
    return true;
  }
  bool selectAfterSalesSlot() {
    quint64 sizes[2] = {0, 0};
    bool found[2] = {false, false};
    for (auto it = p.device.sizes.cbegin(); it != p.device.sizes.cend(); ++it) {
      if (!p.device.isLogical(it.key()) ||
          (!it.key().endsWith("_a") && !it.key().endsWith("_b")))
        continue;
      const int index = it.key().endsWith("_a") ? 0 : 1;
      if (it.value() > quint64(LLONG_MAX) - sizes[index]) {
        error = "槽位容量溢出";
        return false;
      }
      sizes[index] += it.value();
      found[index] = true;
    }
    if (!found[0] || !found[1] || (!sizes[0] && !sizes[1])) {
      error = "无法读取双槽逻辑分区容量，不能猜测启动槽 A";
      return false;
    }
    slot = sizes[0] >= sizes[1] ? "a" : "b";
    return true;
  }
  void cow() {
    QStringList names = p.device.partitions.values();
    std::sort(names.begin(), names.end());
    for (const QString &n : names)
      if (n.contains("cow", Qt::CaseInsensitive) && safeName(n) &&
          p.device.sizes.contains(n))
        command("清理 COW " + n, {"delete-logical-partition", n}, n, true);
  }
  bool rebuild(const QVector<Partition> &images) {
    quint64 freed = 0, needed = 0;
    QSet<QString> deletes;
    for (const Partition &i : images)
      if (p.device.isLogical(i.name)) {
        for (const QString s : {"a", "b"}) {
          QString t = target(i.name, s);
          if (t.isEmpty())
            return false;
          deletes.insert(t);
        }
        QString t = target(i.name, slot);
        if (t.isEmpty())
          return false;
        rebuilt.insert(t);
        if (quint64(i.expandedBytes) > quint64(LLONG_MAX) - needed - 511) {
          error = "逻辑镜像总大小溢出";
          return false;
        }
        needed += (quint64(i.expandedBytes) + 511) / 512 * 512;
      }
    for (const QString &t : deletes) {
      if (!p.device.sizes.contains(t)) {
        error = "逻辑分区容量未知：" + t;
        return false;
      }
      if (p.device.sizes[t] > quint64(LLONG_MAX) - freed) {
        error = "逻辑释放容量溢出";
        return false;
      }
      freed += p.device.sizes[t];
    }
    quint64 used = 0;
    for (auto it = p.device.sizes.cbegin(); it != p.device.sizes.cend(); ++it)
      if (p.device.isLogical(it.key()) && !deletes.contains(it.key())) {
        if (it.value() > quint64(LLONG_MAX) - used) {
          error = "逻辑占用容量溢出";
          return false;
        }
        used += it.value();
      }
    if (needed > freed &&
        (!p.device.sizes.contains("super") ||
         p.device.sizes["super"] < 4 * 1024 * 1024 ||
         used > p.device.sizes["super"] - 4 * 1024 * 1024 ||
         needed > p.device.sizes["super"] - 4 * 1024 * 1024 - used)) {
      error = "逻辑分区重建容量不足/未知";
      return false;
    }
    QStringList sorted = deletes.values();
    std::sort(sorted.begin(), sorted.end());
    for (const QString &t : sorted)
      command("删除逻辑分区（不可逆） " + t, {"delete-logical-partition", t}, t,
              true);
    for (const Partition &i : images)
      if (p.device.isLogical(i.name)) {
        QString t = target(i.name, slot);
        command("重建逻辑分区 " + t,
                {"create-logical-partition", t,
                 QString::number((quint64(i.expandedBytes) + 511) / 512 * 512)},
                t);
      }
    return true;
  }
  bool finish() {
    switchMode(true);
    if (p.options.clearData) {
      for (const QString n : {"userdata", "metadata"}) {
        if (!p.device.partitions.contains(n) || !p.device.sizes.contains(n)) {
          error = "清除数据目标不存在或容量未知：" + n;
          return false;
        }
        command("清除数据（不可逆） " + n, {"erase", n}, n);
      }
      if (p.device.platform == Platform::Qualcomm)
        command("格式化用户数据（不可逆）", {"-w"}, "userdata");
    }
    if (p.options.autoReboot)
      command("重启系统", {"reboot"});
    return true;
  }
};
} // namespace
bool OugaFlashPlanner::build(const QVector<Partition> &images, const Device &d,
                             const Options &options, Plan *out,
                             QString *error) {
  if (error)
    error->clear();
  if (!out)
    return fail(error, "缺少计划输出");
  *out = Plan();
  switch (options.mode) {
  case FlashMode::Normal:
  case FlashMode::BothSlots:
  case FlashMode::Force:
  case FlashMode::OnlyFastbootd:
  case FlashMode::RepairFastbootd:
  case FlashMode::AfterSalesBootloader:
  case FlashMode::AfterSalesFastbootd:
    break;
  default:
    return fail(error, "未知刷写模式");
  }
  Builder b;
  b.p.device = d;
  b.p.options = options;
  b.p.images = images;
  b.mode = d.userspace;
  b.slot = d.slot;
  if (d.serial.trimmed().isEmpty() ||
      d.serial.contains(QRegularExpression("[\\s\\x00-\\x1f]")))
    return fail(error, "必须绑定有效序列号");
  if (!d.modeKnown || !d.unlockKnown || !d.unlocked)
    return fail(error, "设备模式/解锁状态未知或设备未解锁");
  if ((d.platform != Platform::Qualcomm && d.platform != Platform::MediaTek) ||
      d.product.isEmpty() || (d.slot != "a" && d.slot != "b"))
    return fail(error, "平台、机型或当前槽位未知");
  Platform package = options.packagePlatform;
  bool xbl = false, lk = false;
  for (const Partition &i : images) {
    xbl |= baseName(i.name) == "xbl";
    lk |= baseName(i.name) == "lk";
  }
  if (xbl && lk)
    return fail(error, "刷机包同时包含高通/联发科引导镜像");
  if (xbl || lk) {
    Platform detected = lk ? Platform::MediaTek : Platform::Qualcomm;
    if (package != Platform::Unknown && package != detected)
      return fail(error, "包平台声明与镜像冲突");
    package = detected;
  }
  if (package == Platform::Unknown)
    return fail(error, "无法从包判定平台，请确认机型并明确选择包平台");
  if (package != d.platform)
    return fail(error, "包平台与设备冲突");
  bool repair = options.mode == FlashMode::RepairFastbootd,
       af = options.mode == FlashMode::AfterSalesBootloader,
       ab = options.mode == FlashMode::BothSlots,
       force = options.mode == FlashMode::Force,
       only = options.mode == FlashMode::OnlyFastbootd;
  if (options.afterSuper && !af)
    return fail(error, "Super 检查点只能用于售后 Fastboot");
  if (!startsInFastbootd(options.mode) && d.userspace)
    return fail(
        error,
        "该操作需要普通 Fastboot，请先明确执行准备模式切换并重新读取设备");
  if (startsInFastbootd(options.mode) && !d.userspace)
    return fail(error, "请先准备 FastbootD 模式并重新读取完整分区表");
  if (only && (d.platform != Platform::Qualcomm || !d.userspace))
    return fail(error, "仅 FBD 只允许已在 FastbootD 的高通设备");
  if (!repair && options.clearData && d.platform == Platform::Qualcomm &&
      !options.formatToolsReady)
    return fail(error, "清除数据需要完整匹配的 "
                       "platform-tools（mke2fs、make_f2fs、mke2fs.conf）");
  if (options.arbDowngrade)
    return fail(error, "可信当前镜像比较发现 ARB 降级风险");
  if (!options.arbVerified)
    b.p.warnings << "ARB 未验证：无可信比较基准；镜像 ARB "
                    "不代表硬件熔断状态，不能保证整包不存在其他反回滚风险。";
  QVector<Partition> ps;
  QSet<QString> names;
  for (const Partition &i : images) {
    if (!i.selected)
      continue;
    QString n = baseName(i.name);
    if (!safeName(n) || blockedImageName(n))
      return fail(error, "不允许刷写该目标：" + n);
    if (names.contains(n))
      return fail(error, "重复归一化分区：" + n);
    if (i.path.isEmpty() || i.bytes <= 0 || i.expandedBytes <= 0 ||
        i.sha256.size() != 32)
      return fail(error, "镜像未经有效预检：" + n);
    names.insert(n);
    ps << i;
  }
  if (ps.isEmpty())
    return fail(error, "至少选择一个镜像");
  if (options.validateTable)
    for (const Partition &i : images) {
      if (baseName(i.name) == "super" && af)
        continue;
      if (b.target(i.name, d.slot).isEmpty())
        return fail(error, b.error);
    }
  if (ab) {
    if (options.targetSlot != "a" && options.targetSlot != "b")
      return fail(error, "AB 模式必须明确选择最终启动槽");
    b.slot = options.targetSlot;
  }
  if (force)
    b.slot = "a";
  if (only)
    b.slot = d.slot == "a" ? "b" : "a";
  if (needsAdditionalImages(options.mode))
    for (const QString n : {"my_company", "my_preload"})
      if (!names.contains(n))
        return fail(error, "此模式必须明确提供并选择匹配的 " + n + ".img");
  if (repair) {
    if (!b.flashCritical(ps, "修复 FastbootD 缺失必要镜像："))
      return fail(error, b.error);
    b.switchMode(true);
    b.p.options.clearData = false;
    b.p.options.autoReboot = false;
    b.p.warnings << "修复完成后停留 FastbootD；不清除数据，不重启系统。";
  } else if (af && !options.afterSuper) {
    auto super = std::find_if(ps.cbegin(), ps.cend(), [](const Partition &i) {
      return baseName(i.name) == "super";
    });
    if (super == ps.cend() || super->merged.isEmpty())
      return fail(error, "售后 Fastboot 需要 super.img 及已验证的合并分区清单");
    // Validate the initial post-super preview as well; checkpoint rebuilds
    // against fresh table.
    Options cont = options;
    cont.afterSuper = true;
    Plan tail;
    if (!build(images, d, cont, &tail, error))
      return false;
    if (b.target("super", d.slot) != "super")
      return fail(error, "售后 Super 阶段需要已确认存在的无槽 super 分区");
    b.command("擦除 Super（不可逆）", {"erase", "super"}, "super");
    if (!b.flash(*super, d.slot))
      return fail(error, b.error);
    b.wait(120000, "Super 完成后等待 120 秒");
    Step checkpoint;
    checkpoint.kind = Step::Checkpoint;
    checkpoint.title = "Super 后重新探测；确认新的分区映射及启动槽后才能继续";
    checkpoint.userspace = false;
    b.p.steps << checkpoint;
    b.p.steps += tail.steps;
    if (tail.totalBytes > LLONG_MAX - b.p.totalBytes)
      return fail(error, "刷写总大小溢出");
    b.p.totalBytes += tail.totalBytes;
    b.p.flashCount += tail.flashCount;
    b.slot = tail.options.targetSlot;
    b.p.warnings
        << "Super 写入后必须再次确认；后续计划可能因真实分区表改变而被阻止。";
  } else {
    QSet<QString> merged;
    if (af) {
      for (const Partition &i : ps)
        if (baseName(i.name) == "super")
          merged.unite(i.merged);
      if (merged.isEmpty())
        return fail(error, "缺少 Super 合并清单");
      if (!b.selectAfterSalesSlot() ||
          !b.flashCritical(ps, "售后关键镜像缺失："))
        return fail(error, b.error);
      b.switchMode(true);
    }
    QVector<Partition> active;
    const QStringList critical =
        af ? criticalImages(d.platform) : QStringList();
    for (const Partition &image : ps) {
      const QString name = baseName(image.name);
      if (name != "super" && !merged.contains(name) && !critical.contains(name))
        active << image;
    }
    if (!af && names.contains("super"))
      return fail(error, "Super 只能在售后 Fastboot 专用阶段刷写");
    // Match the reference's small-image-first order without consulting files.
    // Required additional images stay first; only inspected manifest data is
    // used.
    const bool additional = needsAdditionalImages(options.mode);
    std::sort(active.begin(), active.end(),
              [additional](const Partition &a, const Partition &b) {
                const QString an = baseName(a.name), bn = baseName(b.name);
                const auto extra = [](const QString &name) {
                  return name == "my_company" || name == "my_preload";
                };
                if (additional && extra(an) != extra(bn))
                  return extra(an);
                if (a.bytes != b.bytes)
                  return a.bytes < b.bytes;
                return an < bn;
              });
    const bool preSwitch = force && d.slot == "b";
    QVector<Partition> early, normal, modem;
    for (const Partition &image : active) {
      if (baseName(image.name) == "modem" && b.deferModem())
        modem << image;
      else if ((preSwitch || ab) && !d.isLogical(image.name))
        early << image;
      else
        normal << image;
    }
    if (only)
      b.wait(5000, "仅 FBD：切槽前 5 秒倒计时，可请求停止");
    // AB keeps the current slot active until all non-logical writes succeed.
    // Slotted modem is deferred on both platforms; slotless modem stays here.
    if (preSwitch || ab) {
      b.cow();
      if (!b.flashImages(early, ab))
        return fail(error, b.error);
    }
    const bool rebuild = force || only || (ab && b.slot != d.slot);
    if (b.slot != d.slot && (rebuild || af))
      b.command("切换活动槽（不可逆） " + b.slot, {"set_active", b.slot},
                b.slot);
    if (rebuild && !b.rebuild(active))
      return fail(error, b.error);
    if (!preSwitch && !ab)
      b.cow();
    if (!b.flashImages(normal, ab) || !b.flashDeferredModem(modem))
      return fail(error, b.error);
    if (!b.finish())
      return fail(error, b.error);
  }
  b.p.options.targetSlot = b.slot;
  b.p.summary =
      QString(
          "序列号 %1 | 机型 %2 | %3 | 当前槽 %4 → 最终槽 %5 | %6 次刷写 | %7")
          .arg(d.serial, d.product, platformText(d.platform), d.slot, b.slot,
               QString::number(b.p.flashCount), sizeText(b.p.totalBytes));
  *out = b.p;
  return true;
}
