# DeskMate 工程架构与平台设计决策

本文档整理并归档了 DeskMate 已在代码库中落地的架构决策、核心协议边界、macOS / HarmonyOS 双端实现设计及已确认的平台 API 约束。更底层的 32 字节线缆格式与 Session API 细节见 [protocol.md](./protocol.md)，领域术语见 [GLOSSARY.md](../GLOSSARY.md)，早期演进决策记录见 [adr/](./adr/)。

---

## 1. 总体架构

DeskMate 将**传输与平台无关的协议核心（`core/`）**与**双端原生适配层（`apps/host`、`apps/device`）**彻底分离：

```text
macOS Host (apps/host)                         HarmonyOS Device (apps/device)
├─ SwiftUI Studio UI & 菜单栏浮窗               ├─ ArkTS 自适应 UI（平板 / 手机竖转横）
├─ DisplaySession / CGVirtualDisplay           ├─ UsbManager (USBAccessoryHandle 所有权)
├─ ScreenCaptureKit + VideoToolbox             ├─ XComponent 原生多点触控采集
├─ InputInjector（四模式触控状态机）            ├─ N-API + DeviceEngine (AVCodec HW 解码)
└─ Bridge.cpp（libusb 多链路 AOA 中心）         └─ FdByteIo（POLLIN 门控读写）
                 │                                              │
                 └────────── 共享 C++20 核心 (core/) ───────────┘
                     32B 显式小端头部 · HostSession / DeviceSession
                     优先级调度器 · GOP / IDR 恢复门控 · 四时间戳校时
```

### 核心设计原则

1. **有线直连优先**：基于 USB-C AOA（Android Open Accessory）全双工 Bulk 传输，不依赖 Wi-Fi 或云端中继。
2. **零第三方依赖的跨端接缝**：`core/` 只依赖标准 C++20，通过 `ByteIo`、`MonotonicClock`、`HostBoundary` 与 `DeviceBoundary` 四组抽象接口接入两端平台代码，并以此作为唯一确定性契约测试接缝。
3. **不采集、不传输音频**：V1 显式关闭 `ScreenCaptureKit` 音频捕获，不申请麦克风权限；协议头保留音频消息类型（`0x02`）并在解码阶段明确拒绝且不中断健康会话。
4. **独立扩展桌面，不做静默主屏回退**：Host 端绝不将主屏静默冒充为扩展屏输出，也不销毁非自身创建的外部显示器。

---

## 2. 共享核心（`core/`）

| 模块 | 职责与已实现行为 |
| --- | --- |
| **线缆编解码（`protocol.hpp/.cpp`）** | 显式小端 32 字节协议头（魔数 `DM` / `0x44 0x4D`、`version=1`、`session_nonce`、`video_stream_id`、`seq`、`timestamp_us`）；单包上限 64 KiB；控制/输入/校时消息禁止分片且载荷上限 4096 B；输入载荷固定 28 字节（支持多指触点帧标志与归一化坐标）。 |
| **流式拆包与重组（`message_stream`）** | 支持任意切片与粘包；视频最多 64 分片（最大 4 MiB/帧），检测乱序、重复与不连续；在任何大内存分配前完成头部合法性与长度校验。 |
| **会话状态机（`session`）** | `CLIENT_HELLO → STREAM_PREPARE → STREAM_READY → STREAM_COMMIT → STREAM_COMMITTED → 首个匹配 IDR`；按能力交集协商编码器/像素格式/分辨率；支持 `Pause/PauseAck`、`Resume`、两阶段运行中重配（失败回滚保留旧流）、1 秒心跳与 3 秒失联检测、锁屏/睡眠/权限撤销独立降级状态、以及 `0x05` 四时间戳 RTT/漂移估算。 |
| **优先级发送与恢复（`scheduler` / `recovery`）** | 控制、输入与校时消息在视频分片边界优先发送；拥塞（待发字节超限或未发帧年龄超限）时仅在完整 GOP 边界丢弃尚未开始发送的旧帧并请求新 IDR；接收端 `DeviceRecoveryGate` 维护 `recovery_id` 与 150 ms 重试去抖，旧代际 P 帧不进入解码器，故障期间冻结并保留 **Last Good Frame**。 |

---

## 3. macOS Host 端实现（`apps/host`）

### 3.1 多设备 USB-C AOA 中心（`Bridge.cpp`）

- **设备识别与切换**：后台 `discovery_main` 持续扫描多个 USB 端口上的 HDC（`12d1:5000`）与 AOA（`18d1:2d00..2d05`）设备，利用 `serial` 与 USB 物理端口路径（`port_path`）联合缓存设备身份，避免无序列号设备并发重枚举时身份冲突；对未授权或被拦截的设备在本地阻断而不影响其他端口设备。
- **独立全双工链路（`DeviceLink`）**：每台设备拥有独立的 `libusb_device_handle`、独立 `rx_loop()` 读线程（QOS `USER_INTERACTIVE`）与 `link_worker` 发送泵线程、独立 `HostSession` 与视频/输入队列。
- **关键驱动兼容策略**：
  - 视频分片载荷上限固定为 **12 KiB**（含 32 B 头部低于 HarmonyOS 配件驱动 16 KiB 读缓冲上限，避免截断丢包）。
  - 首次 AOA 握手阶段超时放宽至 **20 秒**，为设备端弹出「允许访问 USB 配件」系统提示留出操作窗口；握手超时时在原链路重建 `HostSession`，不释放并重 claim 接口（避免 `LIBUSB_ERROR_BUSY`）。
  - `UsbByteIo::write()` 在 `LIBUSB_ERROR_TIMEOUT` 时依据 `transferred` 实际进度推进发送偏移，仅重试未发完的后缀。
  - 退出与断开仅执行 `libusb_release_interface` + `libusb_close`，**严禁**调用 `set_configuration(0)` 或 USB Reset（会导致设备滞留在 `18d1:2d00`）。

### 3.2 多块虚拟显示器与采集编码（`DMVirtualDisplay.m`、`DisplaySession.swift`、`CapturePipeline.swift`、`VideoEncoder.swift`）

- **多虚拟屏实例**：每台设备连接时根据 `device_id` 生成唯一非零 `serialNum` 与显示名称（`DeskMate · <设备名>`），通过 `DisplayConfigCoordinator` 串行执行 `CGVirtualDisplay` 创建、销毁与全局布局事务。
- **非重叠拓扑排布**：支持**右侧、左侧、上方、下方**四个方位，并记忆每台已配对设备的默认方位；同侧接入多块副屏时通过单次原子 `CGBeginDisplayConfiguration` 自动向外顺延或并排对齐，确保副屏之间与主屏互不重叠。
- **低延迟采集与硬编码**：按 `displayID` 隔离 `ScreenCaptureKit` 回调队列，关闭音频捕获，经 Apple `VideoToolbox`（`HEVC` / `H.264`、RealTime、禁止 B 帧、按需强制 IDR 拼装 VPS/SPS/PPS）输出 Annex-B 码流；关键帧请求轮询独立以 30 Hz 运行，避免被 UI 定时器阻塞。

### 3.3 四模式反向触控与权限控制（`TouchInputMode.swift`、`InputInjector.swift`、`DMVirtualHID.m`）

- 每条 `DeviceChannel` 拥有独立的 `TouchInputEngine` 状态机并映射到对应显示器的全局 CoreGraphics `bounds`，实现多台设备各自在自身扩展屏幕内的独立操作。
- 支持四种模式：**原生屏幕触控（实验性）**、**虚拟妙控板（实验性）**、**兼容手势映射（默认）**、**仅单点触控**；前两者在签名具备 `IOHIDUserDevice` Virtual HID 权限时启用原生通路，否则自动无感回退到 CoreGraphics / 相对触控板手势映射。
- 提供辅助功能「一键获取」（弹窗注册并直达系统设置）与「重置旧授权」（`tccutil reset Accessibility`）。

### 3.4 桌面工作台界面与生命周期（`HostWindow.swift`、`MenuBarPopover.swift`、`StartupServiceManager.swift`、`PairedDeviceStore.swift`）

- 采用 **Alabaster Studio** 视觉体系（主窗口包含交互式多副屏拓扑布局、在线副屏列表与各自方位/FPS 卡片、已配对设备管理、启动行为/登录项设置与运行诊断日志；菜单栏提供轻量快控浮窗）。
- 监听系统锁屏/解锁与睡眠/唤醒通知（`NSWorkspace` / `DistributedNotificationCenter`），异常或停止服务时立即停止编码并销毁由 DeskMate 创建的虚拟显示器，防止窗口滞留或残留幽灵副屏。

---

## 4. HarmonyOS 客户端实现（`apps/device`）

### 4.1 USB 配件通道与生命周期

- **所有权边界**：ArkTS 层持有 `usbManager.USBAccessoryHandle` 并负责权限弹窗（`requestAccessoryRight`）、打开配件获取 `fd` 及最终 `closeAccessory()`；C++ N-API 层（`FdByteIo`）仅借用 `fd` 进行读写，不擅自关闭 `fd`。
- **`POLLIN` 阻塞读修复**：实测 HarmonyOS 配件字符设备在无数据时 `read()` 会忽略 `O_NONBLOCK` 直接阻塞在内核，因此 `FdByteIo` 严格通过 `poll()` 检查 `POLLIN` 后才执行单次非阻塞读；停止阶段采用 `eventfd` 唤醒并等待线程退出（`STOPPING → 拒绝新工作 → 唤醒并取消 I/O → join → 归还 ArkTS 关闭`）。

### 4.2 硬件解码与 Surface 租约（`video_decoder.cpp`、`device_engine.cpp`）

- **零拷贝直出**：采用 `OH_VideoDecoder`（`AVCodec`）Surface 模式直接解码并渲染到 `XComponent` 绑定的 `OHNativeWindow`，不做 CPU 像素回读。
- **非回调线程控制**：`Flush`、`Reset`、`Stop`、`Destroy` 均从单一非回调控制线程串行调用，避免在编解码回调线程同步阻塞形成死锁；`Push()` 入队后立即触发 `PumpFrames()`，解决启动瞬间输入 buffer 池未调度导致的黑屏死锁。
- **Surface 租约保活**：对 `OH_NativeWindow_NativeObjectReference/Unreference` 实行串行代际管理，在页面切换（首页 ⇄ 设置 ⇄ 全屏画面）期间保留已建好的解码 Surface，不意外销毁；若系统重建 Surface，自动重发关键帧恢复。

### 4.3 多点触控与手机/平板自适应界面（`napi_init.cpp`、`Index.ets`、`DeviceLayout.ets`）

- **原生多点触控采集**：在 C++ 端注册 `XComponent` 回调，直接采集多指触点并打包为协议帧发送，在 `OnSurfaceChanged` 时保留同指针解码器并在每次触摸时动态重读 `XComponent` 尺寸，保证横竖屏切换后归一化坐标依然准确。
- **全端响应与自动转屏**：使用 `< 600vp` 紧凑布局断点同时支持平板双栏与手机单栏底部导航；手机端接入前锁定**竖屏**，进入副屏渲染页面自动切换为**横屏自适应（`AUTO_ROTATION_LANDSCAPE`）**并适配挖孔/状态栏安全区。

---

## 5. 已确认的官方平台参考与边界

- **HarmonyOS**：
  - [usbManager API](https://developer.huawei.com/consumer/cn/doc/harmonyos-references/js-apis-usbmanager/)（`openAccessory`、`requestAccessoryRight`、`closeAccessory`）
  - [视频解码指南（AVCodec）](https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/video-decoding/)
  - [NativeWindow API](https://developer.huawei.com/consumer/en/doc/harmonyos-references-V14/_native_window-V14/) & [XComponent 回调](https://developer.huawei.com/consumer/en/doc/harmonyos-references/capi-oh-nativexcomponent-native-xcomponent-oh-nativexcomponent-callback/)
- **macOS**：
  - [Apple ScreenCaptureKit](https://developer.apple.com/documentation/screencapturekit) & `VideoToolbox`
  - [Apple CGEventField](https://developer.apple.com/documentation/coregraphics/cgeventfield)
  - [Apple Virtual HID DriverKit Entitlement](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.driverkit.family.hid.virtual.device)（需额外开发者审批，未授权时自动走兼容回退）
  - [Apple NSWorkspace Sleep Notifications](https://developer.apple.com/documentation/appkit/nsworkspace/willsleepnotification)
- **已验证约束提醒**：
  - `CGVirtualDisplay` 为 macOS 私有 API，DeskMate 通过运行时动态类查寻调用而非链接私有符号；不同 macOS 小版本对其 HiDPI 模式发布行为存在差异，如遇特定版本限制可切换为借用外部已存在的扩展显示器。
  - `ServiceManagement` 登录项与辅助功能（TCC）依赖稳定的应用签名；开发重编译覆盖 `.app` 后可使用内置「重置旧授权」重新授予权限。
