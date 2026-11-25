# 更新日志 (Changelog)

本仓库所有版本说明统一遵守以下规范：
1. **以简体中文为主**，按差异分类清晰列出改动。
2. **仅记录相对上一个版本的变更差异**（新增 / 优化 / 修复 / 注意事项）。
3. **不在每个版本说明中重复粘贴安装步骤与驱动教程**；首次安装、WinUSB / Parsec VDD 驱动及 macOS 权限配置请查阅 [README.zh-CN.md](./README.zh-CN.md)。

---

## [v1.4.0] - 2026-11-25

### ✨ 新增功能
- **macOS & Windows 画面帧率切换选项**：在桌面端「概览 → 快速控制」与「设置 → 虚拟显示器与输入源」新增「画面帧率」切换开关，支持 `自动（跟随协商）`（默认）以及 `30 Hz` / `60 Hz` / `90 Hz` / `120 Hz` 手动档位；Windows 的 `90 Hz` / `120 Hz` 标记为**实验性**，推荐追求稳定时使用 `30 Hz` / `60 Hz`。
- **Oink 风格多屏拓扑排列**：macOS 与 Windows Host 均升级为按真实比例绘制的多屏幕拓扑画布，完整标出主屏、物理外接屏与 DeskMate 虚拟屏，支持拖拽自动磁吸对齐与拖拽换序。
- **Windows 旧版本清理按钮**：Windows 自动更新覆盖后会自动清理解锁后的旧版残留，同时在「设置 → 软件更新」新增「一键清理旧文件」按钮。
- **项目与 Release 规范文档**：在 [AGENTS.md](./AGENTS.md)、`.github/release.yml` 与本文件落实本地构建产物统一输出 `dist/<platform>/` 及「中文差异化 Release Notes」编写规范。

### ⚡ 性能与体验优化
- **Windows 端图标对齐**：为 Windows 可执行文件与主窗口嵌入应用图标（`AppIcon.ico`），与 macOS / Android / HarmonyOS 客户端图标保持统一。
- **Windows 协商刷新率对齐 macOS**：默认握手阶段开放 `120 / 90 / 60 Hz` 能力协商，不再将 Windows Host 初始能力上限锁定在 `60 Hz`。

### 🐛 问题修复
- **修复 Windows 端高频丢帧**：
  - 在 DXGI Desktop Duplication 采集后立即 GPU 拷贝并释放桌面锁（`ReleaseFrame`），避免等待读回与 Media Foundation 编码期间阻塞下一帧采集；
  - 修复虚拟屏首帧黑屏即永久触发 CPU 色彩转换回退的问题，保留 D3D11 GPU `VideoProcessor` 加速并复用输入/输出视图；
  - 对齐 Media Foundation 编码器低延迟参数（长 GOP 避免周期性大帧冲击 USB 通道、按分线性帧率缩放控制码率、开启 `QualityVsSpeed` 实时优先），并提升采集与 USB 收发线程优先级。
- **修复 Windows GOP 断裂与首帧恢复问题**：发送队列积压时不再抽取旧 P 帧并发送后续 P 帧；保留编码器不支持强制 IDR 时的正常首帧提交，避免手机端持续丢帧或反复重连。
- **清理仓库根目录残留打包产物**：移除误留在根目录的构建包并明确 `.gitignore` 与打包脚本统一输出路径。

安装与驱动说明请见 [README.zh-CN.md](./README.zh-CN.md)。

---

## [v1.3.0] - 2026-11-25

相对 `v1.2.0` 的主要差异：

### ✨ 新增功能
- **应用内 Ed25519 签名自动更新（macOS & Windows）**：在主窗口概览与设置页加入版本检查、镜像加速前缀、签名校验与一键原地替换升级能力。

---

## [v1.2.0] - 2026-11-25

相对 `v1.1.0` 的主要差异：

### ✨ 新增功能
- **Windows Host 图形工作台与完整有线副屏链路**：新增 Windows Direct2D Alabaster Studio 界面、WinUSB / UsbDk AOA 直连、DXGI + Media Foundation 硬件编码、SendInput/原生触控注入及 Parsec VDD / MttVDD 自动虚拟扩展屏。

---

## [v1.1.0] - 2026-11-25

相对 `v1.0.0` 的主要差异：

### ✨ 新增功能
- **Android Client（APK）**：新增与 HarmonyOS 端对齐的 Android USB AOA 副屏客户端（MediaCodec 硬解 + 多点触控回传）。
- **工程目录整理**：规范跨平台公共代码与文档结构。

---

## [v1.0.0] - 2026-11-25

### ✨ 首次发布
- **macOS Host & HarmonyOS NEXT Client**：实现通过 USB AOA 有线连接将鸿蒙平板/手机扩展为 Mac 独立副屏，支持 HEVC/H.264 硬编硬解、12 KiB 低延迟分包调度与 4 种反向触控/手势输入模式。
