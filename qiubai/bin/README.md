# 欧加线刷随应用提供的依赖

正式依赖已纳入 resources.qrc。欧加和其他页面共用 qiubai 根目录的一套 adb/fastboot、配套 DLL 及格式化辅助工具，均来自同一份 Google platform-tools 37.0.1；不再重复嵌入或提取 bin/platform-tools。7-Zip 和 lpmake 保留 bin 子目录。用户在 QSettings/Ouga 中明确配置的路径仍优先，不覆盖已有设置。

| 相对 qiubai 的目录 | 内容 | 来源及版本 |
| --- | --- | --- |
| bin/7zip | 7z.exe、7z.dll、License.txt | 7-Zip 官方 Windows x64 26.03，仅命令行及格式库，不安装系统软件 |
| 根目录 | Google 官方完整 Windows platform-tools，包括 fastboot/adb、DLL、mke2fs、make_f2fs、配置及 NOTICE | 37.0.1-15733141；未将不同版本的格式化工具混用 |
| bin/lpmake | lpmake.exe、LICENSE-AOSP.txt | 用户提供的 SMT 参考发行包 D:\Downloads\~8160665324498448780\exe\lpmake.exe；32 位 Windows，构建版本未知，未签名 |

lpmake 的 SHA-256 为 FF893ED7582E0FB9C3C8D482899C0FF8888E09CFE112E52ECD8B3135E2425334，与参考文件逐字节一致。附 AOSP 上游采用的 Apache-2.0 许可文本，但不能据此确认该预编译二进制的全部构建来源、第三方许可或数字签名；对外再发行前应另行核实。它依赖 Windows 的 Universal CRT，当前验证环境为 Windows 11 x64，未安装额外全局运行库。

7-Zip 来源：https://www.7-zip.org/a/7z2603-x64.exe
对应源码：https://www.7-zip.org/a/7z2603-src.7z
platform-tools 来源：https://dl.google.com/android/repository/platform-tools-latest-windows.zip
Apache-2.0 文本：https://www.apache.org/licenses/LICENSE-2.0.txt

manifest.json 的 schema 2、pathBase=qiubai 表明文件路径均相对于 qiubai 根目录（不再相对于 bin）。它记录实际取得的版本、源档案摘要及各文件 SHA-256。摘要是本次复制/完整性基线，不等同于第三方签名认证，来源的 latest 链接也不作为固定版本标识。

AOSP 风格的 lpmake --help 正常打印完整用法后退出 1；只对这一无写入能力查询作兼容。Super 生成及所有设备命令仍严格检查正常退出、零状态和失败输出。

本目录不安装 USB 驱动、不配置 ROM 服务地址、不另行提供 simg2img（现有代码已完成 Sparse 转换），也不添加下载/解锁/EDL 等新功能。所有实际工具验收仅涉及版本、压缩文件及本地小型 Super 生成，不访问真实手机。
