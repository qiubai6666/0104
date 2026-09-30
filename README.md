# Orange Tools · Android 设备工具箱

基于 **Qt 6 Widgets** 的 Windows 桌面程序，使用 ADB、Fastboot、scrcpy 和 Payload 工具操作 Android 设备。程序入口是 `src/app/main.cpp`，项目入口是 `OrangeTools.pro`。应用显示名和生成的可执行文件名分别是 **Orange Tools** 和 **`Orange Tools.exe`**；文件名包含空格，命令行中请将完整路径放在引号里。

> 本文按当前源码说明功能和流程，不把未实现的功能或未经测量的性能提升当成承诺。刷写分区、Root 修复和安装模块有风险，请先备份并确认设备、镜像和工具来源。

## 1. 先看懂项目结构

```text
.
├── OrangeTools.pro             # qmake 工程：模块、编译选项、源码和资源清单
├── README.md                   # 使用、源码导航和开发说明（本文）
├── deploy.ps1                  # 部署 Release、精简可选依赖，可选择 UPX
├── compress-release.ps1        # 只压缩未签名主程序/MinGW 运行库，并解压校验
├── package-release.ps1         # 高压缩率 7z 下载包、完整性和逐文件 SHA-256 校验
├── resources.qrc               # 需要编进 exe 的工具资源清单
├── resources.pri               # 主程序/测试共用的资源清单及 zlib 压缩参数
├── app.rc / app.manifest        # Windows 图标、权限和兼容性配置
├── LICENSE                     # 本项目的 MIT 许可证
├── src/
│   ├── app/                    # 启动流程和全局配置
│   │   ├── main.cpp
│   │   └── version.h
│   ├── ui/                     # 窗口、交互和公共界面辅助函数
│   │   ├── menuwidget.cpp/.h
│   │   ├── passworddialog.cpp/.h
│   │   ├── deviceinfowindow.cpp/.h
│   │   ├── devicecheckwindow.cpp/.h
│   │   ├── repairwindow.cpp/.h
│   │   ├── payloadwindow.cpp/.h
│   │   ├── configwindow.cpp/.h
│   │   └── uihelper.cpp/.h
│   └── services/               # 与具体窗口无关的设备、进程和资源服务
│       ├── devicemanager.cpp/.h
│       ├── processmanager.cpp/.h
│       ├── resourceextractor.cpp/.h
│       └── integritychecker.cpp/.h
├── qiubai/                     # 外部工具、工具 DLL、脚本及图片
├── tests/                      # 不操作真实设备的 Qt Test 回归测试
│   ├── readabilitytests.pro
│   ├── readabilitytests.cpp
│   ├── deploymentsmoke.pro/.cpp # 只验证发布插件、PNG、绘制及 TLS 能力
│   └── check-deployment.ps1     # 隔离开发环境后运行发布检查并清理临时 exe
├── build/                      # 编译/测试时生成，可清理，Git 忽略
├── OrangeToolsApp/             # 部署后的完整程序目录，Git 忽略
└── dist/                       # 7z 下载包及 SHA-256 校验文件，Git 忽略
```

### 每个源码文件负责什么？

下表中的 `.cpp/.h` 表示：头文件声明接口和状态，源文件实现界面与行为。

| 文件 | 职责 | 阅读重点 |
| --- | --- | --- |
| `src/app/main.cpp` | 创建应用、检查资源、解包工具、验证密码、显示主菜单 | `main()`：理解整个启动顺序 |
| `src/app/version.h` | 应用名称、版本、默认密码、资源目录名和监控间隔等常量 | 修改配置前先看使用位置；并非每个常量都已被使用 |
| `src/ui/menuwidget.cpp/.h` | 屏幕右侧主菜单；开关各功能窗口；提取 IMG；退出清理 | `setupUI()`、`onButtonClicked()`、`extractImg()`、`cleanupAndExit()` |
| `src/ui/passworddialog.cpp/.h` | 启动密码框、错误提示、三次错误后的延迟退出 | `onOkClicked()`；界面构建在 `setupUI()` |
| `src/ui/deviceinfowindow.cpp/.h` | 投屏入口和设备信息卡片；启动 scrcpy；查询型号/系统；拖拽传文件 | `startScrcpy()`、`showDeviceInfo()`、`transferNextFile()`；文件前部是绘制图标的局部类 |
| `src/ui/devicecheckwindow.cpp/.h` | 显示 ADB/Fastboot 状态；重启/切换模式；打开命令行；刷写 boot/init_boot | `flashPartition()` → `waitForFastbootMode()` → `performFlash()` |
| `src/ui/repairwindow.cpp/.h` | 执行 USB 脚本、修复临时目录、批量安装 APK 和 Root 模块 | 各 `on…StepFinished()` 通过进程完成信号串联步骤；阶段由具名枚举表示 |
| `src/ui/payloadwindow.cpp/.h` | 接收 Payload 链接或路径，选择 boot/init_boot，调用 payload.exe 提取 | `onExtractClicked()`；顶部的局部复选框类负责绘制 |
| `src/ui/configwindow.cpp/.h` | 读取下载配置；下载/解压/启动工具；显示进度、处理重试 | `DownloadConfig` 表示一条配置；`downloadFile()`、`onFileDownloadFinished()` |
| `src/ui/uihelper.cpp/.h` | 统一菜单按钮样式、创建按钮、显示居中消息框 | `createMenuButtons()` 只管界面，各窗口自行连接业务信号 |
| `src/services/devicemanager.cpp/.h` | 共享设备监控、设备模式和信息缓存 | 单例；全模式/仅 ADB 的引用计数；轮询和暂停/恢复 |
| `src/services/processmanager.cpp/.h` | 写入进程 PID/名称，退出时终止记录的进程 | `recordProcess()`、`killAllRecordedProcesses()`、`killProcessByName()` |
| `src/services/resourceextractor.cpp/.h` | 从 Qt 嵌入资源提取工具，提供统一的工具和图片路径 | `extractResources()`、`getResourcePath()`、`getAdbPath()` |
| `src/services/integritychecker.cpp/.h` | 检查调试器和关键资源的存在性、可读性及非空性 | `verifyIntegrity()`；哈希函数存在，但启动时没有做可信哈希比对 |

### 建议的阅读顺序

1. **先看入口**：`main.cpp` → `MenuWidget::onButtonClicked()`，知道程序如何启动、按钮去哪里。
2. **再看公共服务**：`ResourceExtractor` → `DeviceManager` → `ProcessManager`，理解工具路径、设备状态和退出清理。
3. **最后选一个功能追踪**：从按钮处理函数跟到 `QProcess::start()`，再跟到 `finished` 信号对应的槽函数。
4. 看界面外观时找 `setupUI()` 和 `UIHelper`；看业务步骤时找动作函数和 `on…Finished()`，不必先读大段样式字符串。

## 2. 程序怎么运行？

### 启动与退出

```text
QApplication
  → 设置应用名称 / 组织 / 版本
  → IntegrityChecker::verifyIntegrity()
      检测到调试器，或关键嵌入资源缺失/为空：报错退出
  → ResourceExtractor::extractResources()
      重建用户数据目录中的 qiubai 文件夹并提取工具
      失败仅记录日志，启动流程仍继续
  → ProcessManager::clearPIDFile()
  → PasswordDialog
      取消 / 验证未通过：退出
      验证通过：显示 MenuWidget
  → QApplication 事件循环

点击主菜单“退出”
  → 结束记录的进程
  → 按名称终止 adb.exe / fastboot.exe
  → 等待 500 ms
  → 尝试删除运行时 qiubai 目录
  → QApplication::quit()
```

**退出清理属于主菜单“退出”按钮的流程**，并非所有关闭方式都有同样的清理保证。按名称终止进程也可能影响其他软件启动的同名 ADB/Fastboot 实例。

### 各部分的调用关系

```text
app/main
  ├── IntegrityChecker / ResourceExtractor / ProcessManager
  └── PasswordDialog → MenuWidget
                         ├── DeviceInfoWindow ─┐
                         ├── DeviceCheckWindow ├→ DeviceManager → 状态/信息信号 → 窗口
                         ├── RepairWindow      │
                         ├── PayloadWindow     └→ QProcess → adb/fastboot/scrcpy/payload
                         └── ConfigWindow → QNetworkAccessManager → 下载 → 解压/启动

界面共用 UIHelper；工具路径共用 ResourceExtractor。
```

当前业务命令仍由各功能窗口发起；`services/` 是共享基础能力，不代表所有业务已独立于 UI。

### 设备监控的关键规则

- `DeviceManager` 只有一个实例，以 `DEVICE_CHECK_INTERVAL`（默认 1000 ms）轮询设备。
- 投屏窗口申请 **仅 ADB** 监控；设备检测窗口申请 **ADB + Fastboot** 监控；后者优先。
- 窗口销毁时释放对应引用，所有引用归零后停止轮询。仅隐藏或最小化不等同于释放监控。
- ADB 设备信息按“设备代号 → 当前槽位 → 解锁状态”的顺序异步查询；Fastboot 查询读取其标准错误输出。
- 刷写分区时暂停监控，完成/失败后恢复，避免 Fastboot 操作冲突。
- `isDeviceConnected()` 返回**缓存的设备模式**，不主动运行新的检测。主菜单本身不申请监控；使用修复或提取 IMG 前可先打开投屏/设备检测窗口确认设备。

### 多步骤操作为什么有多个槽函数？

`QProcess::start()` 启动外部命令后，界面继续处理事件。命令结束触发 `finished`，槽函数读取结果，再启动下一步，例如：

```text
提取 IMG：ls 按时间排序 → 取最新 .img → adb pull → 打开桌面 IMG 目录
USB 修复：push usb.sh → su 执行脚本 → 删除手机上的脚本
APK 安装：Push → InstallWithSuC → 必要时 InstallWithSuS → DeleteTemporaryFile
模块安装：识别 Root 管理器 → Push → Install → DeleteTemporaryFile → 下一个模块
          全部模块安装成功后，自动重启设备
刷写分区：确认镜像 → 必要时切到 Fastboot → 暂停监控 → flash
          成功后自动重启并恢复监控；失败时恢复监控并提示
```

修复窗口一次复用一个 `repairProcess`，因此操作期间禁用菜单按钮。APK/模块的枚举状态在 `repairwindow.h` 中，避免只看“0/1/2/3”猜当前步骤。

## 3. 功能与使用方法

| 菜单 | 实际行为 | 使用前提 |
| --- | --- | --- |
| 投屏 | 打开设备信息卡片并使用 scrcpy 投屏；卡片支持拖入本地文件传到 `/sdcard/文件名` | USB 调试已开启，设备已授权 |
| 秋白工作室 | 打开修复菜单：USB 修复、修复 TMP、安装 APK、安装模块（全部成功后自动重启） | 多数操作使用 `su`，依赖 Root 权限及相应管理器 |
| PAYLOAD | 输入链接或本地路径，选择 boot/init_boot，用 payload.exe 提取到桌面 IMG | 输入是否受支持由外部 Payload 工具决定 |
| 提取IMG | 从手机 `/sdcard/Download/*.img` 中拉取按时间排序的最新一个到桌面 IMG | 已检测到连接设备且目录可读；不是直接读取手机分区 |
| 设备检测 | 查看模式/设备信息、选择重启方式、打开 CMD、刷写 boot/init_boot | 模式与命令必须匹配；刷写前确认镜像兼容性 |
| 配置 | 前六项按下载配置获取工具；另有 GeekFlashTool、网盘、NDM 入口 | 网络可用，只使用可信下载来源 |
| 联系作者 | 使用系统默认程序打开 Neil.jpg；失败时尝试 file URL | 图片已成功提取 |
| 退出 / 收起 | 清理退出 / 最小化主窗口及可见子窗口 | 收起不会退出程序 |

首次运行需要密码：当前默认值是 **`123456...`（包括末尾三个英文句点）**，统一定义在 `src/app/version.h` 的 `DEFAULT_PASSWORD` 中。密码为本地硬编码检查，并不是强安全认证。

### 文件放在哪里？

| 位置 | 内容与生命周期 |
| --- | --- |
| 仓库 `qiubai/` | 编译输入：工具、依赖 DLL、脚本及图片；不要和运行时目录混淆 |
| `%LOCALAPPDATA%/qiubai` | 每次启动重建并提取资源；也存放 `PID.txt`、下载配置及下载工具；点击“退出”尝试删除 |
| `%LOCALAPPDATA%/Neil.jpg` | 单独提取的作者图片，与运行时 qiubai 目录同级；退出清理不删除它 |
| 系统桌面下的 `IMG/` | IMG 拉取和 Payload 提取输出；程序退出时保留 |
| `OrangeToolsApp/` | 发布包，包含主 exe、Qt DLL、运行库、插件和 `qt.conf` |

数据路径由 `QStandardPaths` 获取。工具资源**不是跨启动缓存**：当前实现每次启动都会尝试删除旧的运行时 qiubai 目录。

## 4. 构建与部署

### 编译准备

- Windows；Qt 6 的桌面开发组件；与该 Qt kit 匹配的编译器。
- 本机验证环境：**Qt 6.11.2 / MinGW 13.1 64-bit**。
- 先确认 `resources.qrc` 中每个文件都存在。
- `qiubai/` 中的工具 exe/DLL 是编译输入，通过 `.gitignore` 的目录白名单纳入 Git；提交并推送完整项目后，正常克隆会带齐现有工具。`build/`、`OrangeToolsApp/`、`dist/` 中的构建产物和发布包仍然忽略。替换工具包时应使用可信、配套的文件，并保留和遵守各工具随附的许可证。

当前资源清单匹配本地工具包中的 `SDL3.dll`、`avcodec-62.dll`、`avformat-62.dll`、`avutil-60.dll`、`swresample-6.dll`。不要混用其他版本的 DLL；替换工具包时同时更新资源清单和完整性检查中的关键文件名。

### 工具资源与 Git 推送

`resources.qrc` 当前需要 23 项工具资源。仅提交资源清单不会上传其中引用的文件；工具 exe/DLL 也必须纳入提交。之前全局 `*.exe`、`*.dll` 规则误将这些输入文件排除，已针对 `/qiubai/*.exe` 和 `/qiubai/*.dll` 添加例外，不放开其他目录。

修改或更换工具文件后，将对应的 `qiubai/` 文件与资源清单变更一起提交，再推送。可用以下只读命令核对：

```powershell
git ls-files -- qiubai
git status --short --untracked-files=all -- qiubai
```

正常情况下，现有工具 exe/DLL 都应出现在已跟踪文件清单中，而不是被忽略；新增文件仍需 `git add` 后提交。Git 推送的是提交记录，不会自动上传未提交文件。不要用全项目强制添加来绕过忽略规则，也不要把构建目录或下载发布包混进源码提交。

### 用 Qt Creator

打开根目录 `OrangeTools.pro`，选择匹配的 Qt kit，使用独立构建目录进行 Release 构建。

### 用 PowerShell（从项目根目录执行）

下面的路径是本机示例，换机器后请改为实际安装路径：

```powershell
$qtBin = 'D:\QT\6.11.2\mingw_64\bin'
$compilerBin = 'D:\QT\Tools\mingw1310_64\bin'
$env:PATH = "$qtBin;$compilerBin;$env:PATH"

New-Item -ItemType Directory -Path '.\build\release-check' -Force | Out-Null
Push-Location '.\build\release-check'
try {
    & "$qtBin\qmake.exe" '..\..\OrangeTools.pro' 'CONFIG+=release' 'CONFIG-=debug'
    if ($LASTEXITCODE -ne 0) { throw 'qmake failed' }
    & "$compilerBin\mingw32-make.exe" -j2
    if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
} finally {
    Pop-Location
}
```

产物为 `build/release-check/release/Orange Tools.exe`。**编译成功不等于已经部署：直接双击编译输出仍可能缺少 Qt DLL。默认请先执行下面的部署命令，再运行 `OrangeToolsApp/Orange Tools.exe`。**仓库根目录里旧的 `Makefile*`、`.qmake.stash` 是本地生成文件，不是源码入口；目录调整后请重新运行 qmake，不要沿用旧 Makefile。

### 打包给其他电脑运行

```powershell
# 默认部署 build/release-check/release/Orange Tools.exe
.\deploy.ps1

# 使用其他构建目录时，指定同一个 Qt kit
.\deploy.ps1 -ExecutablePath '.\release\Orange Tools.exe' -QtBinPath 'D:\QT\6.11.2\mingw_64\bin'
```

脚本调用 `windeployqt`，复制 Qt DLL、编译器运行库、Windows 平台插件及其他需要的插件，并检查关键部署文件。默认不部署下方清单里的可选组件，并按明确清单清理旧部署目录中的残留文件；不会按文件名猜测或批量删除其他 DLL。运行/分发时保持 **整个 `OrangeToolsApp` 文件夹**完整，不能只发送一个 exe。无需把 DLL 放进 Windows 系统目录。

### 发布包体积、依赖精简与 UPX

以下为发布目录的文件大小合计（**MiB = 1,048,576 字节**，不含构建中间文件；不是运行时内存占用）：

| 阶段 | 完整发布目录 |
| --- | ---: |
| 原始部署包 | 99.57 MiB |
| zlib 压缩资源、不部署软件 OpenGL | 63.36 MiB |
| 再精简当前代码未使用的 9 项依赖 | 57.74 MiB |
| 再用 UPX 压缩未签名主程序/MinGW 运行库 | **56.36 MiB（13 个文件）** |

本轮减少 **7.01 MiB（11.06%）**，其中依赖精简节省 5.62 MiB，UPX 节省 1.38 MiB。与原始包相比总共减少 **43.22 MiB（43.40%）**。UPX 后主程序为 23.09 MiB；由于嵌入资源已经用 zlib 压缩，主 exe 的 UPX 收益只有 54 KiB，主要收益来自 MinGW 运行库。

#### 哪些依赖不再默认部署？

| 文件 | 当前可以不部署的依据 |
| --- | --- |
| `opengl32sw.dll` | 可选软件 OpenGL；当前没有 OpenGL/Qt Quick 界面，已在上一轮精简 |
| `D3Dcompiler_47.dll` | 可选着色器编译器；普通 Widgets 绘制未使用，精简后的绘制检查通过 |
| `Qt6Svg.dll`、`iconengines/qsvgicon.dll`、`imageformats/qsvg.dll` | 源码没有 SVG 加载；当前自绘图标不使用 SVG |
| `imageformats/qgif.dll`、`qico.dll`、`qjpeg.dll` | 源码没有 Qt GIF/ICO/JPEG 解码；PNG 解码仍可用，Neil.jpg 通过系统程序打开，Windows 程序图标由 RC 资源提供 |
| `generic/qtuiotouchplugin.dll` | 当前没有 TUIO 输入功能 |
| `tls/qcertonlybackend.dll` | 当前实际加载 Schannel 后端；移除后 TLS 能力检查通过 |

**仍保留** Qt Core/Gui/Widgets/Network、3 个 MinGW 运行库、Windows 平台插件、Windows 样式、网络信息插件、Schannel TLS 插件和 `qt.conf`。ADB、Fastboot、scrcpy、Payload 等 23 项嵌入资源全部保留；没有改动外部工具的 DLL 或许可证。

默认精简配置对应当前源码，不适用于任意 Qt 项目。以后增加 GIF/JPEG/ICO/SVG 图片、TUIO 或 OpenGL/Qt Quick 功能时，需要重新评估部署配置。恢复本轮可选依赖，或连同软件 OpenGL 一并恢复：

```powershell
.\deploy.ps1 -IncludeOptionalDependencies
.\deploy.ps1 -IncludeOptionalDependencies -IncludeSoftwareOpenGL
```

`resources.pri` 继续让主程序和测试共用 zlib 级别 9、阈值 0 的资源配置；读取资源时解压，启动提取流程不变。未测量编译、启动耗时或 UPX 的运行时内存影响。

#### 使用 UPX，以及恢复未加壳版本

本机使用官方 UPX 5.2.1 发布包，下载 ZIP 的 SHA-256 已与官方发布资产提供的摘要核对。UPX 是构建工具，不放进发布目录；下例路径只适用于本机，其他电脑请改为实际安装路径：

```powershell
$upx = 'C:\Users\Administrator\.codex\tools\upx-5.2.1\upx-5.2.1-win64\upx.exe'
.\deploy.ps1 -QtBinPath 'D:\QT\6.11.2\mingw_64\bin' -UseUpx -UpxPath $upx

# 恢复未加壳的精简发布包，保留在 build 中的原版不会被 UPX 修改
.\deploy.ps1 -QtBinPath 'D:\QT\6.11.2\mingw_64\bin'
```

`compress-release.ps1` 只处理未签名的主程序和 3 个 MinGW 运行库，保留重定位、导出、图标及 Windows 资源；先 `upx -t`，再解压比较代码、数据、资源等 PE 段的 SHA-256，通过后才替换发布文件。UPX 会重建 PE 头及导入表，因此解压后的整个文件不保证与原版逐字节相同；不要把该校验描述为“整个文件哈希不变”。

本机 Qt DLL/插件的数字签名均验证有效，**没有强制压缩这些签名文件**。原版 `build/release-check/release/Orange Tools.exe` 及其运行库保留未加壳状态。若安全软件拦截 UPX 版，优先重新部署未加壳版本；不要为加壳包关闭安全防护。没有启动主程序、连接手机或访问网络验证下载功能。

### 更小的下载包（7z）

若目标是缩小下载大小，而不是解压后的运行目录，使用 `package-release.ps1`。它把整个 `OrangeToolsApp/` 打成高压缩率的固实 7z 包，不改动程序或 DLL，也不删除运行依赖；不打包源码、工具输入目录、Git 或 build 中间文件。

本机验证结果：

| 内容 | 大小 |
| --- | ---: |
| 解压后的完整发布目录 | 56.36 MiB（13 个文件） |
| `dist/OrangeTools-Portable.7z` 下载包 | **32.22 MiB** |
| 下载体积减少 | **24.14 MiB / 42.83%** |

需先安装 7-Zip；脚本优先从 PATH、安装注册表和常规安装目录查找，不会联网下载或安装任何工具。在项目根目录执行：

```powershell
.\package-release.ps1

# 若未自动找到，可显式指定本机的 7-Zip 路径（不要重复执行已有输出的命令）
# .\package-release.ps1 -SevenZipPath 'D:\Program Files\7-Zip\7z.exe'

# 下次发布另存一个新文件；脚本拒绝覆盖/增量更新旧包，避免混入旧文件
# .\package-release.ps1 -ArchivePath '.\dist\OrangeTools-Portable-v2.7z'
```

脚本使用 7z 最高压缩级别、固实压缩、64 MiB 字典和 2 个压缩线程。本机产物实际使用 LZMA2 / LZMA / BCJ2。压缩后执行完整性检查，再直接读取每个文件解压后的字节流，与源文件的 SHA-256 比对；不生成解压测试副本，不启动主程序，也不操作设备。最后生成同名 `.7z.sha256` 校验文件。

**发给别人的是 `dist/OrangeTools-Portable.7z`**，可同时提供校验文件。接收者应使用支持 7z 的软件完整解压，再打开 `OrangeToolsApp/Orange Tools.exe`；不要在压缩包内部直接运行，不要只解压一个 exe。解压后的占用仍是 56.36 MiB，功能和原发布目录一致。源码或工具包有更新时，先重新编译、部署，再打包；打包不会自动编译。

### 双击提示缺少 Qt6Gui.dll 等运行库

先确认没有只复制一个 exe，或直接运行尚未部署的编译输出。不要从不明网站下载单个 DLL；使用本项目的 `deploy.ps1`，由编译时同一个 Qt kit 的 `windeployqt` 部署运行库和插件。

默认部署后运行 `OrangeToolsApp/Orange Tools.exe`，分发整个 `OrangeToolsApp/`。如果仅为本地调试，需要在原 Release 输出目录直接双击运行，也可以在项目根目录执行：

```powershell
.\deploy.ps1 -ExecutablePath '.\build\release-check\release\Orange Tools.exe' -QtBinPath 'D:\QT\6.11.2\mingw_64\bin' -OutputDirectory '.\build\release-check\release'
```

这会将运行库和插件部署到该 Release 目录，不启动主程序。构建目录还包含 `.o`、MOC/RCC 生成源码等中间文件，因此对外分发仍应使用独立的 `OrangeToolsApp/`。

## 5. 回归测试

`tests/readabilitytests.cpp` 验证 Orange Tools 应用名、公共菜单按钮的顺序/布局/样式、密码框外观、默认密码、空密码、错误后重试及三次错误退出；另逐项读取资源清单，将内嵌资源的长度和 SHA-256 与原始文件比对，确认压缩后内容不变。主程序和测试使用同一份 `resources.pri`。测试仅在内存中读取/解压资源，不提取到运行目录、不启动主程序、不调用设备工具、不访问网络。

在第 4 节设置好 `$qtBin`、`$compilerBin` 和 PATH 后执行：

```powershell
New-Item -ItemType Directory -Path '.\build\readability-tests' -Force | Out-Null
Push-Location '.\build\readability-tests'
try {
    & "$qtBin\qmake.exe" '..\..\tests\readabilitytests.pro' 'CONFIG+=release' 'CONFIG-=debug'
    if ($LASTEXITCODE -ne 0) { throw 'Test qmake failed' }
    & "$compilerBin\mingw32-make.exe" -j2
    if ($LASTEXITCODE -ne 0) { throw 'Test build failed' }
    & '.\release\ReadabilityTests.exe' -platform offscreen
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
} finally {
    Pop-Location
}
```

### 发布目录加载检查

`tests/deploymentsmoke.cpp` 创建隐藏的 Widgets 界面并抓取绘制结果，读取 PNG，检查 Windows 平台插件及 Schannel TLS 1.2 能力。不启动主程序、不解包资源、不显示窗口、不联网、不操作设备。

```powershell
New-Item -ItemType Directory -Path '.\build\deployment-smoke' -Force | Out-Null
Push-Location '.\build\deployment-smoke'
try {
    & "$qtBin\qmake.exe" '..\..\tests\deploymentsmoke.pro' 'CONFIG+=release' 'CONFIG-=debug'
    if ($LASTEXITCODE -ne 0) { throw 'Smoke qmake failed' }
    & "$compilerBin\mingw32-make.exe" -j2
    if ($LASTEXITCODE -ne 0) { throw 'Smoke build failed' }
} finally {
    Pop-Location
}
.\tests\check-deployment.ps1
```

检查脚本临时把测试程序复制到发布目录，隔离 Qt 开发环境的 PATH/插件设置，测试结束后删除临时 exe 并恢复环境变量。它不会执行 `Orange Tools.exe`，也不会向 Windows 系统目录复制 DLL。

最终静态导入检查覆盖发布包的 12 个 PE 模块，共 121 项 DLL 导入，未发现缺失。对已加壳的 4 个模块检查对应的加壳前原版，其余 Qt 模块直接检查发布文件；这是静态依赖检查，不能代替动态插件和外部工具的功能联调。检查报告保存在 `build/dependency-check-results.txt`。

本轮精简包和 UPX 包的加载检查均通过；使用 UPX 发布运行库和精简插件运行回归测试，结果为 **11 passed / 0 failed**，23 项嵌入资源的大小/SHA-256 一致。TLS 检查仅验证本地后端和协议能力，没有进行真实 HTTPS 握手。真实投屏、刷写、Root 修复、网络下载和 Payload 解包仍需要人工联调；测试通过不代表已验证所有外部工具或手机型号。


## 6. 常见修改应该改哪里？

| 想改什么 | 位置与注意事项 |
| --- | --- |
| 应用名称 / exe 文件名 | `src/app/version.h` 的 `APP_NAME` 和 `OrangeTools.pro` 的 `TARGET`；同时调整 `deploy.ps1` 的默认路径 |
| 默认密码 / 版本 / 监控间隔 | `src/app/version.h`；密码错误次数在 `passworddialog.cpp` 的 `MaxPasswordAttempts` |
| 主菜单标题或顺序 | `MenuWidget::setupUI()` 中的按钮文本；同步调整头文件 `MenuAction` 的顺序和动作分支 |
| 菜单按钮高度、颜色、消息框 | `UIHelper`；三个纵向菜单共用 `createMenuButtons()` |
| 设备连接检测和公共设备信息 | `DeviceManager`；保持监控引用成对申请/释放 |
| 投屏参数 / 文件拖拽 | `DeviceInfoWindow::startScrcpy()` / `transferNextFile()` |
| 新修复或安装步骤 | `RepairWindow`；给阶段命名，处理成功/失败，并恢复禁用按钮 |
| 下载地址 / 下载文件类型 | `ConfigWindow`；配置描述使用 `DownloadConfig`，下载过程仍由 reply 对应的状态表管理 |
| 加一个功能窗口 | 在 `src/ui/` 添加 .cpp/.h，在 `OrangeTools.pro` 注册，然后在主菜单接入；不要忘记关闭和最小化流程 |
| 更新内嵌工具 | 更新 `qiubai/`、`resources.qrc`；关键工具名变化时同步更新 `IntegrityChecker::getCriticalFiles()`；重新编译 |
| 调整发布包体积 | `resources.pri` 控制资源压缩；`deploy.ps1` 控制可选依赖；`compress-release.ps1` 只对指定未签名文件做 UPX；修改后运行发布加载检查 |

### 下载配置的格式

运行时配置文件为 `%LOCALAPPDATA%/qiubai/download_config.txt`，不存在时从 `ConfigWindow::downloadConfig()` 中配置的地址下载。程序跳过第一行标题，后续每行是：

```text
编号|类型|文件名|下载URL
1|exe|ExampleTool.exe|https://example.invalid/ExampleTool.exe
2|zip|AnotherTool.exe|https://example.invalid/AnotherTool.zip
```

示例地址仅展示格式，不是实际下载源。编号 1–6 对应六个动态按钮；类型 `exe` 下载后直接打开，类型 `zip` 使用 PowerShell 解压并查找指定 exe。下载后会执行文件，必须确认配置和下载来源可信。

## 7. 当前实现的边界

- 密码是源码中的普通字符串，反调试和“资源非空”检查都不能当作完整的安全防护。
- `verifyExtractedResources()`、哈希辅助函数存在，但 `main()` 当前只调用嵌入资源检查。
- 主启动流程没有独立的 ADB Server 预启动步骤。大部分命令是异步的，但配置窗口初次等待配置使用嵌套事件循环，部分进程清理使用短暂 `waitForFinished()`，不能称为“所有操作都完全非阻塞”。
- 当前命令没有统一用 `-s` 选择设备，建议一次只连接一台设备。
- 连接判断依赖共享缓存；部分流程只监听 `finished`，外部工具启动失败时的 `errorOccurred` 处理并不完整。
- 资源提取的返回值表示“至少一个文件成功”，不是“所有文件都成功”；后续工具可用性需要实际检查。
- 调试器会被启动检查拒绝。此次整理保留该行为；如果要进行调试，请先明确如何调整检查策略。

## 8. 本次可读性整理

- 源码按 `app / ui / services` 分组，qmake 路径同步调整。
- 密码窗口实现从头文件拆到 .cpp，默认密码统一使用配置，保留原有实际密码。
- 菜单按钮创建和消息框居中共用辅助代码；主菜单重复的窗口开关逻辑合并。
- 主菜单/修复菜单使用具名动作；APK、模块和 Root 管理器使用具名枚举状态。
- 下载配置由“反复拼接/拆分字符串”改为结构体；移除主菜单中空的兼容函数与无用状态。
- 校正资源清单中与本地工具包不一致的旧 DLL 名及不存在的资源项，并增加独立回归测试。

## 9. 哪些文件可以清理？

清理只针对可重新生成的产物和确认未引用的文件，不按“代码中没有出现文件名”删除 DLL：外部工具和 Qt 插件可能动态加载它们。

| 可清理项目 | 原因 |
| --- | --- |
| `build/` | 本地编译/测试的 Makefile、MOC/RCC 生成源码、目标文件、exe、日志，以及 `readability-review` 中的临时源码副本；可按第 4、5 节重新生成 |
| `build/upx-check/Orange Tools.packed.exe`、`Orange Tools.restored.exe` | 本轮 UPX 验证的临时副本，合计约 46.23 MiB；不参与发布，自动删除被环境拦截，可手动删除 |
| 根目录 `Makefile`、`Makefile.Debug`、`Makefile.Release`、`.qmake.stash` | qmake 的旧生成文件和工具链缓存；项目应使用独立的 build 目录构建 |
| `OrangeToolsApp/_deployment_smoke.exe` | 临时部署检查程序；不是正式程序的运行依赖 |
| `qiubai/disconnected.png` | 源码、资源清单和脚本均未引用 |
| `qiubai/scrcpy.png` | 源码、资源清单和脚本均未引用，且与保留的 `icon.png` 内容完全相同 |

**不要删除** `src/`、`tests/`、工程/资源/部署配置、`.git/`、许可证、`qiubai/` 中的工具及 DLL，或部署包必需的 Qt 运行库和插件。`OrangeToolsApp/` 保留为现有可运行包，但不会随源码修改自动更新；如需最新版本，请先按第 4 节重新编译和部署。

下面是手动清理命令，仅在项目根目录执行。它会删除 `build/` 内的编译程序和测试报告；需要保留这些产物时先另行保存。不存在的目标会跳过。执行前先确认清理清单，不要使用 `git clean -fdx`，它会连同忽略的发布包、构建输出及其他未跟踪文件一起删除。

```powershell
$projectRoot = (Resolve-Path -LiteralPath '.').Path
if (-not (Test-Path -LiteralPath (Join-Path $projectRoot 'OrangeTools.pro') -PathType Leaf)) {
    throw 'Run from the project root.'
}
$cleanupTargets = @(
    'build', '.qmake.stash', 'Makefile', 'Makefile.Debug', 'Makefile.Release',
    'OrangeToolsApp\_deployment_smoke.exe',
    'qiubai\disconnected.png', 'qiubai\scrcpy.png'
)
$workspacePrefix = $projectRoot.TrimEnd('\') + '\'
foreach ($relativePath in $cleanupTargets) {
    $target = [IO.Path]::GetFullPath((Join-Path $projectRoot $relativePath))
    if (-not $target.StartsWith($workspacePrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Cleanup target outside project: $target"
    }
    if (Test-Path -LiteralPath $target) {
        Remove-Item -LiteralPath $target -Recurse -Force -ErrorAction Stop
    }
}
```

这份清单表示“已核实可清理”，不代表文件已经自动删除。清理不触碰手机或 `%LOCALAPPDATA%` 中的运行数据，也不改变 `resources.qrc` 中的 23 项内嵌资源。

本项目自身源码采用 MIT 许可证；Qt、ADB、scrcpy、Payload 及其他分发工具遵循各自许可证。
