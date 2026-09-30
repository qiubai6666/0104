#!/bin/bash

if [ "$(id -u)" != "0" ]; then
echo "请使用ROOT权限执行！！" 1>&2
exit 1
fi
setprop persist.security.adbinput 1
file="/data/data/com.miui.securitycenter/shared_prefs/remote_provider_preferences.xml"
if sed -i 's/<boolean name="security_adb_install_enable" value="false" \/>/<boolean name="security_adb_install_enable" value="true" \/>/g' $file; then
    echo "开启成功"
    pkill -f com.miui.securitycenter
else
    echo "开启失败"
fi
