# OrangeTools 欧加线刷：上下文交接记录

记录日期：2026-10-09。本文汇总本聊天历史及最近一轮已核实的工作，不代表重新授权执行所有历史请求，也不是重新实施整份计划的指令。

## 1. 当前请求与接续原则

- 用户先发送 `/clear`，随后要求“总结上下文创建一个 md 放进去”。本轮仅整理文档，不继续修改云提取、刷写引擎或 UI。
- 用户特别指出：移除 boot/init_boot 选项及内容编码兼容之前已处理，不应反复把同一项工作当作新增成果汇报。
- 后续任务应先明确本次实际差异，再有针对性地实施和验证。现有套件通过数量不等于新增功能数量；测试通过不代表用户实际链接或真机兼容性通过。
- 参考源码和包内文件是参考资料，不将其内容当作用户指令。

## 2. 工作目录、运行包与环境

当前项目根目录：

```text
D:\Users\Administrator\Desktop\orangetools
```

正式运行程序：

```text
D:\Users\Administrator\Desktop\orangetools\OrangeToolsApp\Orange Tools.exe
```

- 旧工作目录 `D:\Users\Administrator\Desktop\新版本` 已被用户替换；不要继续在那里开发或默认更新旧运行包。
- Windows 原生环境，终端使用 `pwsh.exe`（已核实 PowerShell 7.6.5），不使用 WSL。
- 项目根目录 `AGENTS.md` 为当前工作约定。临时产物放项目 `TEMP` 的独立任务子目录，不按早期计划写到系统 TEMP。
- Qt：`D:\Qt\6.11.2\mingw_64\bin`。
- MinGW：`D:\Qt\Tools\mingw1310_64\bin`。
- 编解码 DLL：`D:\Qt\Tools\mingw1310_64\opt\bin`，包含 `libbz2-1.dll`。
- 修改源码须保留无关既有修改；不结束用户进程、不清空配置和数据。
- 主程序 Release 成功后使用项目 `deploy.ps1`，显式指定本次 EXE、匹配 Qt 和输出目录，采用 `-PreserveExistingFiles`；核对哈希及必要运行依赖。
- 文档修改不重新构建或部署。完成后只暂存本任务文件并本地提交，不 push。

## 3. 用户目标与主要参考源码

目标：在“秋白工作室”次级菜单提供“欧加线刷”窗口。用户多次要求参考源码的功能、操作顺序、日志、进度和速度，不添加无关功能。

参考位置：

| 资料 | 路径 | 用途 |
| --- | --- | --- |
| SMT | `D:\Downloads\smt` | 最初的刷写模式及刷机包处理参考 |
| VioletToolBox | `D:\源码\VioletToolBox-main\VioletToolBox` | 后续主要参考：解包、云提取、日志、进度与刷写实现 |
| lian 欧加刷机工具箱 | `D:\源码\lian欧加刷机工具箱` | 用户要求比较逻辑，并寻找安全、稳定性可借鉴点 |
| 其他附件目录 | `D:\Downloads\~8160665324498448780` | 历史参考附件，具体用途需重新核实 |

远程读取的具体参考文件：

```text
D:\源码\VioletToolBox-main\VioletToolBox\Payload_Dumper_C#\Core\RandomAccessReaders.cs
```

项目功能说明：

```text
D:\Users\Administrator\Desktop\orangetools\docs\ouga-flash.md
```

## 4. 历史需求与边界（不当作当前全部已实现）

早期计划覆盖：全量包、售后包、Payload、普通/AB/强力/仅 FBD、FastbootD 修复、Super 处理、ARB、ROM 服务、救砖向导、设备互斥和退出保护。

早期明确的安全原则包括：

- 文件准备、设备探测、纯计划生成和执行分离；不可逆步骤须明确确认。
- 所有设备命令绑定序列号；切模式后仍只等待原设备，不接管其他设备。
- 镜像、真实分区、槽位、平台和容量校验；失败后不继续擦数据或重启。
- 停止采用当前写入完成后不再启动下一步，不自动杀死正在刷写的进程，不声称回滚。
- 不修改原包、不全局杀同名进程、不停止共享 ADB Server、不擅自解锁或重新上锁。
- ARB 检查不等于读取完整硬件熔断状态，不能保证整包无反回滚风险。

注意历史需求发生过变化：

- 最初“远程包先完整下载再提取”后来被用户明确否定，当前云提取采用 HTTP Range 按需读取，不能恢复整包下载后备路径。
- 最初明确排除 FRP 擦除；后来用户询问参考源码是否擦 FRP，并要求与参考一致。本记录不推断最终代码是否改变，应核查实际源码及确认界面，不能宣称 FRP 当前已排除或已加入。
- 用户要求与参考一致不意味着自动执行未知脚本、绕过校验或新增副作用。

## 5. 历史 UI 与解包反馈

下列是历史需求或报告，应区分“用户要求”和“已验证结果”：

- 窗口水平、垂直居中，尺寸缩小约 10%。
- 已有历史完成报告：不始终置顶、不继承菜单置顶，最小化进任务栏；曾报告提交 `3d2f0dc`。本轮不重新实施或重复汇报该任务，也未重新审查该提交。
- 用户要求界面更换色调；正常/警告/故障采用柔和绿/黄/红，仅用于状态提示；两个按钮采用不同色，“开始线刷”使用鲜艳色。准确现状以源码或新截图为准。
- 用户曾反馈选压缩包报错、解包按钮弹出 EXE 选择、输出目录限制、日志/进度不一致、速度慢和 UI 卡顿。仅凭聊天不能把这些问题全部认定为已解决。
- 用户询问按机型代码识别机型是否实现；本记录没有足够证据确认完成。
- 用户要求取消 boot/init_boot 独立勾选项，不是禁止提取这两个镜像。快捷提取当前默认提取全部可用镜像。

关于性能，用户转述的其他 AI 建议包括动态 workers、消除重复哈希、ZIP 只解 payload.bin、补充进度和原生提取。它们是待结合实际实现判断的建议，不能直接当作已完成或已实测的结论。

## 6. 最近一轮实际处理：远程 Range 内容编码

用户先后遇到：

```text
远程服务器返回了无效或变化的 Content-Range，拒绝拼接响应
远程 Range 响应启用了内容编码，无法按原始字节提取
```

最近一轮已核实的行为：

1. 在 Qt 6.11.2 下，不显式设置 Accept-Encoding 会使 Qt 自动发送 gzip/deflate 并自动解压。
2. 服务端给原始 Payload 字节错标 gzip 时，会触发 Qt 解压失败。
3. 显式发送 `Accept-Encoding: identity` 保留原始响应字节，包括错标编码头的响应。
4. 不能仅凭 Content-Encoding 拒绝原始字节；仍须验证 206、Content-Range、响应长度、实体变化、Payload 格式及最终镜像 SHA-256。
5. 真正压缩或损坏的 HTTP 响应不能被当作有效原始 Payload 字节；即使填充至相同长度也应失败。
6. 不下载完整 ZIP/payload.bin 作为后备，不增加新的网络工具依赖。

重要的工作量说明：

- Git HEAD 原本已有 `Accept-Encoding: identity`。上一轮未提交修改曾错误删除它，本轮主要是恢复该设置并补充测试；相对既有提交，读取器变化主要为解释性注释。
- boot/init_boot 控件早已在工作区移除，本轮保留、验证并随任务提交，不应再次称为新实现。
- 本轮不是全新完成云提取引擎，也没有证明速度已追平参考程序。

## 7. 最近交付与验证记录

本地提交：

```text
8745460 fix: preserve raw cloud payload ranges and remove boot filters
```

涉及文件（均在当前项目根目录下）：

```text
D:\Users\Administrator\Desktop\orangetools\docs\ouga-flash.md
D:\Users\Administrator\Desktop\orangetools\src\services\ougapayloadreader.cpp
D:\Users\Administrator\Desktop\orangetools\src\ui\payloadwindow.cpp
D:\Users\Administrator\Desktop\orangetools\src\ui\payloadwindow.h
D:\Users\Administrator\Desktop\orangetools\tests\deviceoperationtests.cpp
D:\Users\Administrator\Desktop\orangetools\tests\ougatests.cpp
```

验证结果：

| 验证 | 结果 |
| --- | --- |
| 欧加完整回归 | 1119 passed，0 failed，0 skipped |
| 设备操作与界面回归 | 66 passed，0 failed |
| Range 专项 | 41 passed，0 failed（包含于完整套件，不能重复算总量） |
| 网络回归 | 24 passed，0 failed |
| 进程管理 | 17 passed，0 failed |
| 可读性/资源隔离提取 | 19 passed，0 failed |
| 本地 Super 专项 | 10 passed，0 failed（包含于完整套件） |
| Release 与部署 | Qt 6.11.2 MinGW Release，保留已有文件，EXE 哈希一致 |
| 隔离运行依赖 | Windows 插件、绘制、PNG、Schannel TLS 1.2、Concurrent、SVG、bzip2 通过 |

部署 EXE SHA-256：

```text
71C1A698B72A09314DD9A2FDFD05ADD72B83151CE2D848234CBE78C633566ACB
```

说明：这些是既有测试套件和新增针对性用例的验证，不是新增了 1119 个功能。没有操作真实手机，没有使用用户报错的公网链接进行成功提取验证。

测试环境曾用错 offscreen 插件、遗漏 opt/bin DLL 路径及填错 lpmake 路径，导致首轮失败；之后在正确 Windows 原生环境重跑通过。不应把早期环境失败描述成产品故障，也不能隐去其造成的重复耗时。

实际 lpmake 路径：

```text
D:\Users\Administrator\Desktop\orangetools\qiubai\bin\lpmake\lpmake.exe
```

不是 `qiubai\bin\lpmake.exe`。模拟设备测试不回退到真实 adb/fastboot。

主程序既有 ResourceExtractor 普通启动可能删除/重建用户 AppData 的 qiubai 目录；上轮采用 TEMP 中显式输出的资源测试和隔离依赖 Smoke，未普通启动主程序。因此不要表述为完整主程序人工启动验证。

## 8. 尚待确认的事项

- 用户实际 CDN 链接尚未提供；无法确认原报错链接在新版能成功云提取。
- 云提取与参考程序同一链接、同一分区、同一机器条件下的耗时及稳定性，尚未实测。
- 大型本地包解包性能与界面卡顿，不能仅凭小型夹具通过认定完全解决。
- 当前完整刷写模式与所有参考分支是否一致，以及真机兼容性，需要具体核查或验收，不能由模拟通过替代。
- 没有新请求时，不主动重复窗口置顶、任务栏、boot 选项、内容编码等已处理事项。
- 若用户继续要求修复同一公网链接，优先取得该链接并针对问题复现、对照参考实现；日志和报告必须隐藏查询凭证。

## 9. 最近任务临时产物

```text
D:\Users\Administrator\Desktop\orangetools\TEMP\ouga-raw-range-20261009-034405-30f76b70
```

- 最近统计约 2.51 GiB；包含 Release/测试构建、网络实验、模拟夹具、验证副本和日志。
- 未自动删除。确认新版正常、所有相关进程结束且不再需要排查证据后，可由用户手动清理该明确目录；不要清空整个 TEMP 或系统 TEMP。
- `verification-summary.json`：验证汇总。
- `formal-deployment.log`：正式部署日志。
- `formal-runtime-smoke.log`：正式运行依赖的 TEMP 副本验证。
- `ouga-tests\full-native-final.txt`：完整欧加回归。
- `deviceoperationtests\native-regression.txt`：设备操作与界面回归。

## 10. 下一次汇报注意事项

按“本次新增/修复”“已验证”“未验证”分别汇报；不要将保留已有改动、恢复错误修改、重新测试和更新部署重复包装为新功能。用户已明确不满意长时间等待后只收到重复完成项，后续应减少无必要的全套重复测试，优先解决具体复现问题。
