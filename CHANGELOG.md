# 更新日志 (Changelog)

本仓库所有版本说明统一遵守以下规范：
1. **以简体中文为主**，按差异分类清晰列出改动。
2. **仅记录相对上一个版本的变更差异**（新增 / 优化 / 修复 / 注意事项）。
3. **不在每个版本说明中重复粘贴安装步骤与驱动教程**；首次安装、WinUSB / Parsec VDD 驱动及 macOS 权限配置请查阅 [README.zh-CN.md](./README.zh-CN.md)。

---

## [v1.5.0] - 2026-11-26

相对 `v1.4.0` 的主要差异：

### ✨ 新增功能
- **运行时刷新率重协商（macOS / Windows）**：握手默认按 `120 / 90 / 60 Hz` 协商，连接后切换「画面帧率」不再需要重连——Host 会用新的 config id 发起会话内重协商，并等待设备以 IDR 确认后继续推流；请求高于设备能力时自动按协商上限输出并在 UI 明示。
- **Android 客户端跟随协商刷新率**：解码进入 `attached` 后按协商结果设置窗口 `preferredRefreshRate`，并在协商变化时热更新；`AMediaCodec` 同时下发 `operating-rate` 提示，便于硬解在 90/120 Hz 下保持低延迟。
- **Windows Host 帧率档位转为正式支持**：`90 Hz` / `120 Hz` 不再标记「实验性」，选择后即时生效。

### ⚡ 性能与体验优化
- **Windows Host 90/120 Hz 高刷链路重构**：编码线程不再每帧硬睡 4 ms，异步 MFT 改为 `BeginGetEvent` 事件驱动；USB 传输从同步 `libusb_bulk_transfer` 改为异步 transfer 流水线（4 路常驻 Bulk IN + 6 槽位 Bulk OUT），显著降低高刷下的帧间抖动与突发延迟。
- **Windows 采集限帧与显存分配**：限帧改用高精度可等待定时器，修复 `capture_wait_event_` 长期处于 Signaled 导致 90 Hz 限帧失效、CPU 空转的问题；NV12 纹理与输出视图改为 6 槽位环形池复用，消除每帧约 5–6 MB 的显存分配抖动。
- **Windows UI 刷新降频**：主窗口与托盘弹层隐藏时，UI 状态刷新由 250 ms 放宽到 1 s，减少后台无谓重绘。

### 🐛 问题修复
- **修复 Windows 高刷丢帧的唤醒竞态**：链路工作线程在清空唤醒事件前未检查 `wakeup`，会吞掉编码器刚提交的帧唤醒并额外睡 5 ms；`has_pending_output()` 也未统计待发送视频队列。现已改为锁内原子消费唤醒并同时检查视频队列。
- **修复 Windows 码控参数单位错误**：`CODECAPI_AVEncCommonBufferSize` 单位应为字节，原 `bitrate/4` 实际把 VBV 窗口放宽到约 2 秒，复杂画面会突发超大 P 帧撞穿 64 分片预算；现改为 `bitrate/32`（约 250 ms），并将周期性 GOP 由 2 秒放宽到 30 秒，依靠按需 IDR 恢复。
- **修复丢 P 帧后继续发送残缺 GOP**：发送队列满丢弃非关键帧后立即暂停后续 P 帧并请求 IDR，直到下一个关键帧才恢复发送，避免设备端持续解码失败与恢复风暴。
- **修复异步 USB 化后 90/120 Hz 突发断链**：异步 Bulk IN/OUT 的超时曾按帧率缩到 100 ms / 25 ms，而 WinUSB 上任一 URB 超时都会 `WinUsb_AbortPipe()` 中止整条端点；现固定为 IN 永不超时、OUT 2 秒，并移除回调内的同步 `libusb_clear_halt()`（EP0 重入且会重置数据翻转位），改由干净失败重连恢复。
- **修复大关键帧写入直接杀会话**：`write()` 遇到超过单次传输上限的缓冲区不再返回错误，改为按 16 KiB 分段提交并返回实际字节数，由 core 续写剩余部分。
- **修复 USB 槽位释放后发送线程仍空睡**：新增独立唤醒标志，Bulk OUT 完成回收槽位时必然唤醒链路线程，不再依赖已被消费的 `write_blocked_`。
- **修复 NV12 显存池覆写在途纹理 / 槽位泄漏**：改用 COM 引用计数 + 在途集合双重判定空闲，先 `drain_output()` 再回收槽位，MFT 内部跳帧也不会漏光 6 个槽位；连续 1 秒不可用时才强制复用并在诊断的 `enc` 中显示 `poolReuse=`。
- **修复采集超时分支多睡一轮**：`DXGI_ERROR_WAIT_TIMEOUT` 后不再重复等待到下一提交点，避免睡过刚到达的桌面帧。
- **修复 MFT 异步事件在极端时序下丢失**：事件桥由单指针改为队列，编码线程空闲等待加 50 ms 看门狗，避免 `BeginGetEvent` 失败后永久挂起。
- **修复 Parsec VDD 自定义分辨率写入时机**：`HKLM\SOFTWARE\Parsec\vdd\0..4` 是全局模式槽位，驱动只在加载与 `VDD_IOCTL_ADD` 时读取，现改回在 `VDD_IOCTL_ADD` 之前写入，权限不足时在日志中明确提示；虚拟屏未能应用目标刷新率时会显式告警。
- **修复 Windows UI 定时器反复探测虚拟屏驱动**：`probe_status()` 结果缓存 30 秒，仅在服务启停、重试、显示变化与虚拟屏增删时失效，避免每 250 ms 枚举显示器与开关 VDD 设备句柄。
- **修复设备侧分辨率重配置被误拒**：当 Host 接受设备分辨率时，重配置校验改为同时参考设备上报的能力集，不再用 Host 固定能力表把合法的高刷/异分辨率配置挡掉。

安装与驱动说明请见 [README.zh-CN.md](./README.zh-CN.md)。

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
