#include "ougaflashplanner.h"
#include "ougapackage.h"
#include <QRegularExpression>
#include <algorithm>
#include <climits>
#include <limits>
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
  QString target(const QString &name, const QString &s) {
    QString b = baseName(name);
    bool plain = p.device.partitions.contains(b),
         a = p.device.partitions.contains(b + "_a"),
         bb = p.device.partitions.contains(b + "_b");
    QString has = p.device.variables.value("has-slot:" + b);
    if (plain && (a || bb)) {
      error = "分区表有槽/无槽映射冲突：" + b;
      return {};
    }
    if (has == "no" && (a || bb)) {
      error = "has-slot 与分区表冲突：" + b;
      return {};
    }
    if (plain && has != "yes")
      return b;
    QString t = b + "_" + s;
    if ((a || bb || has == "yes") && p.device.partitions.contains(t))
      return t;
    error = "实际目标不存在：" + t;
    return {};
  }
  bool logical(const QString &n) {
    QString b = baseName(n);
    return logicalName(b) || p.device.logical.contains(b) ||
           p.device.logical.contains(b + "_a") ||
           p.device.logical.contains(b + "_b");
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
  bool flash(const Partition &img, const QString &s) {
    QString t = target(img.name, s);
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
  bool both(const Partition &p) { return flash(p, "a") && flash(p, "b"); }
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
      if (logical(i.name)) {
        for (const QString &s : {"a", "b"}) {
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
      if (logical(it.key()) && !deletes.contains(it.key())) {
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
      if (logical(i.name)) {
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
      for (const QString &n : {"userdata", "metadata"}) {
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
  if (d.platform == Platform::Unknown || d.product.isEmpty() ||
      (d.slot != "a" && d.slot != "b"))
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
       only = options.mode == FlashMode::OnlyFastbootd,
       afterD = options.mode == FlashMode::AfterSalesFastbootd;
  if ((repair || af) && !options.afterSuper && d.userspace)
    return fail(
        error,
        "该操作需要普通 Fastboot，请先明确执行准备模式切换并重新读取设备");
  if (!(repair || af) && !d.userspace)
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
    if (!safeName(n) || n == "frp" || n == "misc" || n == "userdata" ||
        n == "metadata")
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
  if (ab || force || only)
    for (const QString &n : {"my_company", "my_preload"})
      if (!names.contains(n))
        return fail(error, "此模式必须明确提供并选择匹配的 " + n + ".img");
  if (repair) {
    QStringList required = OugaPackage::repairNames(d.platform);
    for (const QString &n : required)
      if (!names.contains(n))
        return fail(error, "修复 FastbootD 缺失必要镜像：" + n);
    for (const Partition &i : ps)
      if (required.contains(baseName(i.name)) && !b.both(i))
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
      quint64 a = 0, bb = 0;
      bool foundA = false, foundB = false;
      for (auto it = d.sizes.cbegin(); it != d.sizes.cend(); ++it)
        if (b.logical(it.key())) {
          if (it.key().endsWith("_a")) {
            if (it.value() > quint64(LLONG_MAX) - a)
              return fail(error, "槽位容量溢出");
            a += it.value();
            foundA = true;
          } else if (it.key().endsWith("_b")) {
            if (it.value() > quint64(LLONG_MAX) - bb)
              return fail(error, "槽位容量溢出");
            bb += it.value();
            foundB = true;
          }
        }
      if (!foundA || !foundB || (!a && !bb))
        return fail(error, "无法读取双槽逻辑分区容量，不能猜测启动槽 A");
      b.slot = a >= bb ? "a" : "b";
      QStringList critical = OugaPackage::repairNames(d.platform);
      for (const QString &n : critical)
        if (!names.contains(n))
          return fail(error, "售后关键镜像缺失：" + n);
      for (const Partition &i : ps)
        if (critical.contains(baseName(i.name)) && !b.both(i))
          return fail(error, b.error);
      b.switchMode(true);
    }
    QVector<Partition> active;
    for (const Partition &i : ps)
      if (baseName(i.name) != "super" && !merged.contains(baseName(i.name)))
        active << i;
    if (!af && names.contains("super"))
      return fail(error, "Super 只能在售后 Fastboot 专用阶段刷写");
    if (only)
      b.wait(5000, "仅 FBD：切槽前 5 秒倒计时，可请求停止");
    if (force && d.slot == "b") {
      b.cow();
      for (const Partition &i : active)
        if (!b.logical(i.name) && baseName(i.name) != "modem" &&
            !b.flash(i, "a"))
          return fail(error, b.error);
    }
    bool rebuild = force || only || (ab && b.slot != d.slot);
    if (b.slot != d.slot && (rebuild || af))
      b.command("切换活动槽（不可逆） " + b.slot, {"set_active", b.slot},
                b.slot);
    if (rebuild && !b.rebuild(active))
      return fail(error, b.error);
    if (!(force && d.slot == "b"))
      b.cow();
    // The additional images are part of the same validated manifest, but
    // deliberately first.
    std::stable_sort(active.begin(), active.end(),
                     [](const Partition &a, const Partition &bb) {
                       auto rank = [](const Partition &p) {
                         QString n = baseName(p.name);
                         return n == "my_company" || n == "my_preload" ? 0 : 1;
                       };
                       return rank(a) < rank(bb);
                     });
    for (const Partition &i : active) {
      QString n = baseName(i.name);
      if (n == "modem" && !only &&
          ((d.platform == Platform::Qualcomm &&
            !d.partitions.contains("modem")) ||
           afterD))
        continue;
      if (ab && !b.logical(n)) {
        if (!b.both(i))
          return fail(error, b.error);
      } else if (!b.flash(i, b.slot))
        return fail(error, b.error);
    }
    if (!only)
      for (const Partition &i : active)
        if (baseName(i.name) == "modem" && !b.written.contains("modem")) {
          if (d.platform == Platform::Qualcomm &&
              !d.partitions.contains("modem"))
            b.switchMode(false);
          if (d.platform == Platform::Qualcomm || afterD || ab) {
            if (!b.both(i))
              return fail(error, b.error);
          } else if (!b.flash(i, b.slot))
            return fail(error, b.error);
        }
    if (!b.finish())
      return fail(error, b.error);
  }
  b.p.options.targetSlot = b.slot;
  b.p.partitionCount = ps.size();
  b.p.summary =
      QString(
          "序列号 %1 | 机型 %2 | %3 | 当前槽 %4 → 最终槽 %5 | %6 次刷写 | %7")
          .arg(d.serial, d.product, platformText(d.platform), d.slot, b.slot,
               QString::number(b.p.flashCount), sizeText(b.p.totalBytes));
  *out = b.p;
  return true;
}
