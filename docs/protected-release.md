# 离线保护 Release

## 保护范围

此功能提高静态逆向成本，不是授权系统，也不是不可破解的加密。Qt 元对象、UI 文案、导入函数、设备协议和内置脚本仍可能被查看。运行时解码后明文仍在内存中；默认密码没有改变。本次不新增反调试或反虚拟机措施（原有完整性检查行为保持不变）。UPX 和 SFX 是压缩/包装，不算安全保护。

## 推荐入口（不打包）

在 PowerShell 7 中运行：

```powershell
& 'D:\Users\Administrator\Desktop\新版本\build-protected-release.ps1' `
  -QtBinPath 'D:\Qt\6.11.2\mingw_64\bin' `
  -CompilerBinPath 'D:\Qt\Tools\mingw1310_64\bin' `
  -Jobs 4
```

入口在项目 `TEMP` 创建独立目录，保存输入快照、普通/保护 Release、测试报告、性能对比和私有符号。确认链接、字符串扫描、回归及匹配依赖验证全部通过后，才更新 `OrangeToolsApp`，保留未知文件、配置和数据。不创建压缩包，也不自动提交源码；任务执行者按 AGENTS.md 在审阅验证后提交。

使用现有打包流程时，显式加 `release.ps1 -ProtectedRelease`，其余参数保持原来用法；没有此开关仍是普通发布。已有打包入口会继续验证 UPX 候选，私有符号不进入候选运行包。

## 构建行为

- `OrangeTools.pro` 的 `CONFIG+=protected_release` 仅在 Windows MinGW 的 Release 分支生效。Debug 不增加 LTO 或字符串混淆。
- 业务代码使用 `-O2 -flto -g`，链接阶段去掉原来的提前 strip。ASCII 驱动器前缀映射避免中文工程路径作为映射参数时被工具链误解析。
- 巨型 QRC 数据采用 Qt `resources_big` 两阶段生成。资源占位对象使用 `-fno-lto -g0`；业务代码仍参与 LTO，所有资源保持内嵌，资源内容不因此加密。
- 链接使用 `-save-temps=obj` 将 LTO 中间文件保留在构建输出旁，规避本机 MinGW 13 对中文绝对临时文件路径的解析问题。中间文件只能在任务 TEMP 内，不分发。
- 匹配工具链 `objcopy --only-keep-debug` 提取私有 DWARF，`strip --strip-all` 仅处理主 EXE，再添加只包含符号文件名的 GNU debuglink。Qt DLL/第三方程序不 strip。
- 保护构建失败就停止，没有普通构建回退。直接调用 qmake 得到的带调试信息 EXE 不可发布，必须先完成 `Finalize-ProtectedExecutable`。部署识别保护 Makefile 时要求匹配的 `ProtectionManifestPath`。

## 敏感字符串清单

唯一清单为 `src/app/sensitivestrings.h` 的六个 `ORANGE_SENSITIVE_STRING` 条目：默认密码、密码校验成功/退出内部诊断、完整性验证开始/成功诊断和原有调试器警告。普通构建返回相同 QString；保护构建通过 C++11 兼容的 constexpr 编码和 volatile 运行时读取解码。`DEFAULT_PASSWORD` 保留原有使用名称，现返回 QString。

新增条目必须采用单行普通 UTF-8 字符串字面量，并补充测试/清单数量断言。主 EXE 扫描每条的 UTF-8 与 UTF-16LE 完整明文，以及绝对源目录路径。Qt 所需类名/信号槽元数据不要求删除。扫描只证明完整静态明文未出现，不证明无法动态恢复。

## 发布审计与符号保存

- `deploy.ps1` 拒绝本次新生成/内容变更的源码、工程文件、调试文件、对象文件、静态库、Makefile 和构建日志。既有残留只警告、不删除；用户自己的 `.log` 也会被报告为需人工确认的残留。
- `licenses` 下依法需要随包提供的第三方源文件可保留；调试信息、对象和日志即使在该目录也禁止。保留现有许可证收集，不以保护名义移除来源/义务材料。
- `private-symbols/OrangeTools.debug`、`protection.json` 及全部源码/构建 TEMP 都是私有材料，不能分发。manifest 记录最终 EXE SHA-256、符号 SHA-256、工具链和通过扫描的条目名。请将对应文件备份到私有存储后再清理 TEMP；符号文件可能包含源码信息，不能公开。
- 崩溃分析必须使用对应 SHA-256 的 EXE 和符号；LTO 优化会使部分变量不可见。路径映射后的相对源码位置需要在调试器中配置源码搜索路径。

## 验证与限制

- 脚本安全测试覆盖合法许可证/运行脚本保留、源码/符号/日志拒绝、旧文件保留、越界/TEMP 复用拒绝、两种编码的明文检测和 manifest 哈希不匹配。
- 普通与保护配置均运行字符串和 UI/资源测试：密码正确、空输入、错误重试、三次拒绝、键盘提交和取消；完整性验证与资源释放仅使用显式 TEMP 路径，核对关键资源哈希，不执行内置设备工具。
- 普通与保护配置还分别运行进程、设备信息、设备操作、投屏、欧加依赖/界面/网络和小米测试；设备操作使用替身或本地回环服务。截图输出隔离到 TEMP；自启动工具替身的 marker 路径以 Base64 传递，避免 Windows 环境变量编码损坏中文路径，业务协议不改变。部署探针验证 Windows 插件、Qt Widgets、图片、TLS、Concurrent、SVG 和 bzip2，以及所有 PE 导入和 Qt 文件哈希/签名。
- 对比报告中的 StartupChainTests 是隔离 UI/资源测试套件耗时，不是主程序首次启动耗时。自动测试不会启动真实主程序并写入用户 AppData，也不会连接真实手机或执行刷机。实际设备操作仍需用户在设备上验收。

- 本机禁用 8.3 短文件名时，小米旧 BAT 回归会临时分配一个空闲 ASCII 盘符，只映射已核实的任务 TEMP 测试目录；数据仍存于项目 TEMP，不改变现有盘符，结束后按精确目标移除映射。没有空闲盘符或清理失败时停止，不放宽业务中的安全路径检查。
- 欧加异步解包回归发生进度断言失败时，报告会保留完整的界面准备日志，方便定位外部工具失败原因；仍要求原有完整进度和结果断言通过。
