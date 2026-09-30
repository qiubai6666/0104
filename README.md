# FloatingWindow - Android设备管理工具

<div align="center">

![Version](https://img.shields.io/badge/version-1.0.0-blue.svg)
![Qt](https://img.shields.io/badge/Qt-6.8.0-green.svg)
![Platform](https://img.shields.io/badge/platform-Windows-lightgrey.svg)
![License](https://img.shields.io/badge/license-MIT-orange.svg)

基于 Qt 6 的现代化 Android 设备管理工具，提供投屏、修复、Payload 解包等功能。

</div>

## ✨ 特性

- 🚀 **高性能** - ADB Server 预启动，首次投屏速度提升 5-6 倍
- 📦 **单文件分发** - 所有资源嵌入 exe，无需安装
- 🎨 **现代化 UI** - 无边框设计，视觉美观
- 🔧 **智能管理** - 自动资源提取和进程清理
- 🛡️ **稳定可靠** - 完善的错误处理机制

## 🎯 主要功能

| 功能 | 说明 |
|------|------|
| 📱 设备投屏 | 基于 scrcpy 的高性能投屏 |
| 🔧 设备修复 | 一键修复常见设备问题 |
| 📦 Payload 解包 | 提取 Android OTA 包内容 |
| 🖼️ IMG 提取 | 提取系统镜像文件 |
| 🔍 设备检测 | 查看设备详细信息 |
| ⚙️ 配置管理 | 在线下载和更新配置 |
| 👤 联系作者 | 快速联系开发者 |

## 🚀 快速开始

### 编译环境

- Qt 6.8.0 或更高版本
- MinGW 13.1.0 或 MSVC 2022
- Windows 10/11 64-bit

### 编译步骤

```bash
# 使用 qmake
qmake FloatingWindow.pro
make

# 或使用 Qt Creator
# 直接打开 FloatingWindow.pro 并点击运行
```

### 首次运行

1. 运行程序后输入密码（默认：**123456**）
2. 程序会自动提取资源到 `%LOCALAPPDATA%/qiubai`
3. 自动启动 ADB Server（首次可能需要几秒钟）

## 📖 使用说明

### 投屏功能

1. 连接 Android 设备到电脑
2. 点击"投屏"按钮
3. 投屏窗口会显示在屏幕左上角

### 修复工具

提供多种设备修复选项：
- 重启设备
- 进入 Recovery 模式
- 进入 Fastboot 模式
- 解锁 Bootloader
- 等等...

### Payload 解包

1. 点击"PAYLOAD"按钮
2. 选择 payload.bin 文件
3. 选择输出目录
4. 等待解包完成

## 🏗️ 项目结构

```
FloatingWindow/
├── main.cpp                    # 程序入口
├── menuwidget.cpp/h           # 主菜单界面
├── deviceinfowindow.cpp/h     # 设备信息窗口
├── repairwindow.cpp/h         # 修复工具窗口
├── payloadwindow.cpp/h        # Payload 解包窗口
├── devicecheckwindow.cpp/h    # 设备检测窗口
├── configwindow.cpp/h         # 配置管理窗口
├── passworddialog.h           # 密码验证对话框
├── processmanager.cpp/h       # 进程管理器
├── resourceextractor.cpp/h    # 资源提取器
├── version.h                  # 版本信息
├── FloatingWindow.pro         # Qt 项目文件
├── resources.qrc              # 资源文件配置
├── app.rc                     # Windows 资源文件
└── qiubai/                    # 工具资源文件夹
    ├── adb.exe
    ├── scrcpy.exe
    ├── fastboot.exe
    └── ...（共 28 个文件）
```

## 🔧 技术实现

### 资源嵌入

所有工具文件通过 Qt 资源系统嵌入到 exe 中：
- 首次运行自动提取到 `%LOCALAPPDATA%/qiubai`
- 退出时自动清理临时文件
- 支持资源更新和版本管理

### 性能优化

- **ADB Server 预启动**：程序启动时异步启动 ADB Server
- **超时优化**：设备检测超时从 2000ms 降低到 500ms
- **异步处理**：所有耗时操作不阻塞 UI 线程
- **智能缓存**：资源提取后缓存使用

### 进程管理

- 记录所有启动的子进程 PID
- 程序退出时自动清理所有子进程
- 防止进程泄漏和资源占用

## 📊 性能数据

| 项目 | 优化前 | 优化后 | 提升 |
|------|--------|--------|------|
| 第一次投屏 | 3-6秒 | 0.5-1秒 | **5-6倍** |
| 设备信息查询 | 2-10秒 | 0.5-2.5秒 | **4倍** |
| 程序启动 | 1-2秒 | 1-2秒 | 无变化 |

## 🔐 安全性

- 密码验证保护
- 错误次数限制（3次）
- 进程隔离
- 资源完整性检查

## 🛠️ 开发指南

### 修改密码

编辑 `passworddialog.h` 中的 `checkPassword` 函数：

```cpp
bool checkPassword(const QString &password) {
    return password == "你的新密码";
}
```

### 添加新功能

1. 在 `menuwidget.cpp` 中添加按钮
2. 创建对应的窗口类
3. 在 `.pro` 文件中添加源文件
4. 实现功能逻辑

### 更新资源文件

1. 将新文件放入 `qiubai/` 文件夹
2. 在 `resources.qrc` 中添加文件引用
3. 重新编译项目

## 📝 更新日志

### v1.0.0 (2025-11-16)

- ✨ 初始版本发布
- 🚀 实现核心功能
- ⚡ 性能优化
- 📦 资源嵌入
- 🔧 进程管理

## 🤝 贡献

欢迎提交 Issue 和 Pull Request！

## 📄 许可证

本项目采用 MIT 许可证 - 详见 [LICENSE](LICENSE) 文件

## 👨‍💻 作者

QiuBai Team

## 🙏 致谢

- [Qt](https://www.qt.io/) - 跨平台应用框架
- [scrcpy](https://github.com/Genymobile/scrcpy) - Android 投屏工具
- [ADB](https://developer.android.com/studio/command-line/adb) - Android 调试桥

---

<div align="center">

**如果这个项目对你有帮助，请给一个 ⭐ Star！**

</div>
