# DeskMate

DeskMate 是在 macOS 主机与平板之间提供有线扩展显示及反向输入的产品。

## Language

**Host**:
运行 macOS、创建虚拟显示器并发送画面的电脑。
_避免使用_：服务端、发送端

**Device**:
运行 HarmonyOS NEXT、显示 Host 画面并采集用户输入的平板。
_避免使用_：客户端、接收端

**Extended Display**:
macOS 桌面中一块具有独立显示标识和桌面区域的虚拟显示器，不是 Host 主屏的镜像。
_避免使用_：副屏、镜像屏、投屏

**Display Stream**:
从 Host 的 Extended Display 采集、编码并传输到 Device 的视频画面。
_避免使用_：音视频流、投屏音频

**Reverse Input**:
由 Device 采集并发送给 Host 的触控、手写笔或键盘输入。
_避免使用_：反向控制、触控回传

**Basic Input**:
V1 承诺的单指鼠标等价控制、双指滚动和可选基础键盘快捷键；不包含压感、倾角、悬停或系统级多指手势。

**Reference Environment**:
V1 唯一承诺支持的一台具体 MatePad 及其系统版本和一台具体 Mac 及其系统版本；其他环境均为未验证。
_避免使用_：兼容设备、支持机型

**Software Pipeline Latency**:
从 Host 的采集回调收到一帧，到 Device 请求解码器将该帧输出上屏的单帧耗时；它不包含显示扫描和光子响应，也不是输入到显示的延迟。

**Photon Latency**:
从 Host 上测试图案发生物理变化，到 Device 屏幕显示对应变化的端到端光学延迟。DeskMate V1 验收使用高速摄影测得 Device 相对 Mac 内置参考屏的延迟。
_避免使用_：端到端耗时、软件延迟、编码延迟、绝对光子延迟

**Reference Link**:
Reference Environment 中 Device 通过记录过实际协商速率的线缆直连 Mac 机身接口的 USB 链路，不经过扩展坞或 Hub。

**Verification Gate**:
必须在 Reference Environment 通过真机证据解除的一项设计假设；未通过前不得把依赖它的能力列为 V1 承诺或承诺后续排期。

**Display Feasibility Gate**:
Gate 0，独立验证私有虚拟显示器、采集和独立签名分发的前提；失败即停止 Extended Display 路线，不静默改为镜像。

**Measurement Baseline Gate**:
Gate 4A，验证跨端时钟校准、软件打点及光学测量可复现；它不设性能通过阈值，也不代替 V1 性能退出门禁。

**Last Good Frame**:
Device 最近一次完整接收并成功解码输出的画面；流不连续或解码失败时保持显示该画面，直到有效恢复帧可用。
_避免使用_：隐藏输出、黑屏、冻结任意帧

**AOA Session**:
Host 与 Device 通过 Android Open Accessory 协议建立的一次有线连接；建立 AOA Session 不等于已经具备显示或输入能力。
_避免使用_：USB 连接、配件模式

**Session**:
Host 与 Device 的一次逻辑连接及其资源所有权边界；拔线、应用销毁或致命错误都会终止 Session，重连创建新 Session，不复用旧资源。
_避免使用_：长连接、自动恢复

**Surface Lease**:
Session 对 `SurfaceOwner` 所持 NativeWindow 的一次有代际、带引用计数的借用；Surface 销毁不会立即释放仍被租约持有的对象，但会立即停止显示。
_避免使用_：窗口指针、永久 Surface

**Pause Grace Timeout**:
Device 暂停显示后，Host 暂时保留 Extended Display 的有限宽限期；超时销毁显示器，避免用户窗口滞留在不可见桌面。
_避免使用_：保活、心跳超时

**Stream Epoch**:
一次视频流配置的唯一标识，由 Host 通过密码学安全随机数生成；碰撞概率非零，需与 Session 和协商状态共同校验。
_避免使用_：会话 ID、持久序号、彻底隔离 ID

**Session Nonce**:
Device 为一次 Session 生成的非零随机标识；双方用它筛选当前连接的消息，不将其用作认证凭证。

**Config Generation**:
一次显示参数准备的单调递增标识 `config_id`；重复准备或提交同一代配置必须幂等。
_避免使用_：视频流 ID、恢复 ID

**Recovery Epoch**:
Device 每次进入恢复流程时递增的本地代际，用于区分恢复前后的异步回调和输出，不替代 Stream Epoch。

**Wired Transport**:
仅通过 Host 与 Device 之间的 USB 数据线承载 Display Stream 和 Reverse Input 的连接方式；Wi-Fi 不属于 Wired Transport。

**USB Network Fallback**:
AOA 不可用时待验证的备选 Wired Transport，通过 USB 网络共享建立本地 Socket 连接；只有 Reference Environment 提供用户可操作的系统入口且 macOS 原生免驱识别为 NCM 或 ECM 时才成立，RNDIS 不合格。
_避免使用_：无线降级、TCP 兜底、RNDIS 备选
