# 项目工作约定

仅适用于本项目及本任务创建的项目 TEMP 子目录；只读问答不触发部署或提交。

## 目录职责与根目录白名单

- 根目录只允许以下条目：
  - 工程输入：`OrangeTools.pro`、`resources.pri`、`resources.qrc`、`app.rc`、`app.manifest`、`openlist.pri`、`ouga.pri`、`ougacodecs.pri`、`protected-release.pri`、`xiaomi.pri`
  - 文档与许可：`README.md`、`AGENTS.md`、`LICENSE`、`.gitignore`、`.gitattributes`
  - 发布入口：`deploy.ps1`、`release.ps1`、`package-release.ps1`、`package-single-exe.ps1`、`compress-release.ps1`、`build-protected-release.ps1`
  - 目录：`src/`、`assets/`、`tests/`、`scripts/`、`docs/`、`third_party/`、`qiubai/`、`build/`、`OrangeToolsApp/`、`dist/`、`TEMP/`（版本库 `.git/` 除外）
- 根目录不得新增其他文件或目录。确需新增，先同步更新本节、`scripts/check-workspace.ps1` 和 README 目录树。
- 构建产物只进 `build/<task-id>/`；部署只写 `OrangeToolsApp/`；发布包只写 `dist/`；临时/一次性文件只进 `TEMP/<task-id>/`；可复用脚本只进 `scripts/`；维护文档只进 `docs/`；测试夹具只进 `tests/fixtures/`。
- 根目录及源码目录禁止出现：exe/DLL/插件、Makefile、`qt.conf`、`licenses` 运行包、截图、HTML 预览、下载物、一次性报告、源码压缩包。
- 功能任务不得顺手大规模移动/改名既有文件；目录整理必须作为独立任务并先确认。既有残留只在用户确认的独立整理任务中处理。

## 临时测试：只放项目 TEMP

- 临时文件统一存放在 `D:\Users\Administrator\Desktop\orangetools\TEMP`，每次任务在其中新建并记录独立子目录；核实实际路径未通过链接越界，不复用其他任务目录。临时脚本、工程副本、日志、截图、报告及构建产物全部放在其中，不写入项目其他目录。
- 将工作目录、输出和缓存指向该子目录，使用源码和 `.pro` 的绝对路径；无法避免写入源码或程序目录时在 TEMP 隔离副本中运行。该 TEMP 不可用则说明，不自行改用其他位置。
- 发布脚本自身使用的系统 TEMP 快照是例外，但不得把构建产物写入工作树。
- 一次性脚本、报告、截图、预览只放 `TEMP/<task-id>/`；任务结束删除本任务目录。确需保留证据时，全项目最多保留最近 1 个任务目录，并在回复中说明路径与原因。
- 新建目录、文档、脚本使用小写 kebab-case；日期用 `YYYYMMDD`；源码文件名沿用现有风格；新增文件名使用 ASCII，不使用中文或空格。
- 正式源码、回归测试及夹具、配置和交付物保留在约定位置。

## 修改后的结构与目录整理

- 修改代码时检查重复代码、过大的函数、无效注释和临时实现；只进行不改变功能、公共接口和业务逻辑的安全重构。
- 新增文件放入正确目录；日志、截图、测试副本、缓存和临时脚本统一放入 `D:\Users\Administrator\Desktop\orangetools\TEMP`，禁止写入项目根目录。
- 修改完成后检查项目根目录和本次任务产生的文件；仅处理能够确认由本次任务创建且已无用途的临时文件。
- 无法确认归属或用途的文件保留，不自动删除；不整体清空 `build`、`dist`、`OrangeToolsApp`、依赖目录或用户数据。
- 结构问题存在较高修改风险时，只记录并报告，不强行重构。

## 任务收尾自检

- 有文件变更的任务结束前运行 `pwsh -NoProfile -File scripts/check-workspace.ps1`；失败先修复再提交。
- 再运行 `git status --short --untracked-files=all`，确认没有白名单外新增、构建产物、临时文件、日志或下载物。
- 不提交生成物；不使用 `git clean -fdx`。

## 编译成功后更新 OrangeToolsApp

- 主应用 Release 编译成功后，用 `deploy.ps1` 将本次程序及匹配依赖更新至项目内 `OrangeToolsApp`，明确传入 `-ExecutablePath`、`-QtBinPath` 和 `-OutputDirectory`，不取旧产物或混用 Debug/Release。仅有 Debug 时先编译 Release，不能完成则说明未更新。
- 文档修改、仅编译测试或编译失败时不更新。只覆盖部署文件，保留配置、数据、日志及用途不明文件；核实目标无链接越界，文件占用时停止并报告，不强行结束用户进程或清空目录。
- 更新后核实本次产物（未额外处理时可比对 SHA-256）、必要依赖并完成适用验证；报告结果，失败时说明可能不完整，不宣称成功或继续分发。不默认打包，保留最新运行包。

## 每次任务完成后自动本地提交

- 完成必要验证和适用部署后，审阅差异并创建一次说明清晰的本地提交；只改文档不额外编译。只读、无变更或用户要求不提交时跳过。
- 仅按明确路径或差异块暂存本任务改动，不用 `git add .`、`git add -A` 或 `git commit -a`。不夹带已有/其他任务改动，不提交临时文件、缓存、密钥或用户数据，不强制添加忽略的运行包。
- 混合改动无法隔离、已有暂存内容无法排除、验证/部署失败或提交受阻时保留现场并说明；不改动他人暂存、不跳过钩子或绕过限制。不自动 push、切换分支、修改远程/身份、amend 或覆盖历史，不创建空提交。
- 提交后核对内容及剩余状态，最终回复报告短哈希与摘要；未提交则如实说明原因。
