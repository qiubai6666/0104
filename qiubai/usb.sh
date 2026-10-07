#!/system/bin/sh
# 小米/红米安全中心的 USB 安装配置；必须由已授权的 Root Shell 执行。
fail() {
    echo "USB修复失败：$*" >&2
    exit 1
}

[ "$(id -u)" = "0" ] || fail "请先授予 Shell ROOT 权限。"
file="/data/data/com.miui.securitycenter/shared_prefs/remote_provider_preferences.xml"
[ -f "$file" ] || fail "找不到安全中心配置文件，当前系统可能不支持此修复。"
grep -q '</map>' "$file" || fail "安全中心配置格式无效。"

setprop persist.security.adbinput 1 || fail "无法开启 USB 调试（安全设置）。"
[ "$(getprop persist.security.adbinput)" = "1" ] || fail "USB 调试（安全设置）校验失败。"
# 先停止安全中心，避免它用内存中的旧配置覆盖修改结果。
am force-stop com.miui.securitycenter || fail "无法停止安全中心，请检查 ROOT 授权。"

set_boolean() {
    key="$1"
    value="$2"
    if grep -q "name=\"$key\"" "$file"; then
        sed -i "/name=\"$key\"/s/value=\"[^\"]*\"/value=\"$value\"/" "$file" || fail "无法修改 $key。"
    else
        sed -i "/<\/map>/i\\    <boolean name=\"$key\" value=\"$value\" />" "$file" || fail "无法添加 $key。"
    fi
    # 不以 sed 的退出码代替验证：未匹配到任何内容时 sed 也会返回成功。
    line=$(grep "name=\"$key\"" "$file") || fail "缺少 $key 配置。"
    echo "$line" | grep -q '<boolean ' || fail "$key 不是有效的布尔配置。"
    echo "$line" | grep -q "value=\"$value\"" || fail "$key 配置校验失败。"
    if echo "$line" | grep -q "value=\"[^\"]*\""; then
        invalid=$(echo "$line" | grep -v "value=\"$value\"")
        [ -z "$invalid" ] || fail "$key 存在未更新的重复配置。"
    fi
}

set_boolean security_adb_install_enable true
set_boolean permcenter_install_intercept_enabled false
# 桌面端仅在收到此标记且进程正常退出时报告成功。
echo "USB_FIX_OK"
echo "USB 安装已开启，安装拦截已关闭。请重新打开设置查看；未刷新时重启手机。"
