# DeskMate V1 有线 Extended Display 与 Basic Input

Status: ready-for-agent

项目状态：**Design / Pending Hardware Verification**。本规格汇总已经确认的 Q1–Q27 决策；不表示硬件能力、候选协议或性能目标已经通过真机验证。

## 当前授权范围

Agent 当前仅可执行：

- 共享的 proposed 协议编解码、边界校验及互操作测试。
- 平台无关的 Session 状态机、优先级调度器和基于字节 I/O 接缝的契约测试。
- 面向 Gate 0–3 的独立最小验证探针（PoC Harness）及测量记录工具。

在 Reference Environment 的具体设备型号、系统 Build 和 Reference Link 信息补齐，且 Gate 0–3 通过之前，不得将协议转为 accepted，不得提前推进全量生产端实现或承诺后续排期。Gate 4A 只建立测量基线；V1 性能退出门禁在 Gate 0–3 与 4A 通过后生效。

## 问题陈述

Mac 用户希望将 MatePad 用作真正独立的有线 Extended Display，并在 Device 上直接操作 Mac 桌面。现有无线串流容易受到网络和排队抖动影响，而未经真机验证的 USB 方案可能错误假设 AOA、虚拟显示器、硬解码或手写笔能力可用。用户需要清晰、低延迟的画面和可靠的 Basic Input，同时不能因拔线、切后台、锁屏或权限变化而丢失窗口、卡死应用或残留资源。

## 解决方案

DeskMate 在 Host 创建独立 Extended Display，经硬件编码和 Wired Transport 将 Display Stream 送至 Device，由 Device 硬解码直接显示；Device 将限定范围的 Basic Input 回传 Host。首选 AOA，只有经验证的 USB Network Fallback 才能作为备选，绝不以 Wi-Fi 冒充 V1 产品链路。

V1 只在 Reference Environment 承诺 60Hz、原生分辨率和 HiDPI，分别测量 Software Pipeline Latency 与相对 Photon Latency。遇到流不连续时冻结 Last Good Frame 并恢复；遇到生命周期变化时明确暂停或结束 Session，防止窗口长期滞留及资源泄漏。

## 用户故事

1. 作为 Mac 用户，我希望用 USB 数据线把 MatePad 变为独立 Extended Display，以便获得不依赖 Wi-Fi 的额外桌面空间。
2. 作为 Mac 用户，我希望 Device 使用原生比例、原生像素及 HiDPI 显示，以便文字和界面清晰且无意外黑边。
3. 作为 Mac 用户，我希望默认获得 60Hz Display Stream，以便在已验证的基准环境下得到稳定体验。
4. 作为 Mac 用户，我希望系统只选择 Host 与 Device 共同支持的编码器、像素格式和显示参数，以便避免无法解码或静默错误降级。
5. 作为 Mac 用户，我希望看到明确的连接、准备、提交、暂停和错误状态，以便理解当前能否使用 Extended Display。
6. 作为 Mac 用户，我希望握手或建屏失败时得到明确原因，以便处理权限、能力或连接问题，而不是看到无限重连。
7. 作为 Device 用户，我希望单指可移动、点击和拖动 Host 光标，以便完成基本桌面操作。
8. 作为 Device 用户，我希望双指滚动 Host 内容，以便浏览文档和网页。
9. 作为 Device 用户，我希望可选使用基础键盘快捷键，以便完成有限快捷操作。
10. 作为 Device 用户，我希望布局切换后的旧触摸事件不会作用到新布局，以便避免误点击。
11. 作为 Mac 用户，我希望锁屏时自动停止视频和输入，以便避免锁屏期间发生反向控制。
12. 作为 Mac 用户，我希望辅助功能权限不可用时仍能观看画面但收到输入不可用提示，以便保留只读副屏价值。
13. 作为 Mac 用户，我希望录屏权限撤销或建屏失败时看到明确错误，以便修复设置而不是误以为仍在串流。
14. 作为 Device 用户，我希望短暂切后台后能够恢复显示，以便处理其他应用中的临时任务。
15. 作为 Mac 用户，我希望短暂暂停期间 Extended Display 宽限保留，以便窗口不会立即挤回主屏。
16. 作为 Mac 用户，我希望暂停超时、拔线或系统睡眠后 Extended Display 被清理，以便窗口不会滞留在不可见桌面。
17. 作为 Device 用户，我希望断流时保留 Last Good Frame 而非花屏或黑屏闪烁，以便获得可理解的短暂故障体验。
18. 作为 Device 用户，我希望恢复后从新 IDR 继续显示，以便避免参考帧链损坏造成持续花屏。
19. 作为 Mac 用户，我希望高优先级控制消息不必等待整个大视频帧发送，以便及时暂停、校时和恢复。
20. 作为用户，我希望网络共享模式在允许输入前完成配对、加密和明确授权，以便防止其他网络设备控制 Host。
21. 作为注重隐私的用户，我希望 V1 不访问麦克风、不采集音频，以便将权限限制在显示与输入所需范围。
22. 作为用户，我希望安装独立签名并公证的 Host 应用，以便通过 Gatekeeper 使用 DeskMate。
23. 作为维护者，我希望区分软件耗时与光子延迟，以便准确定位性能问题而不夸大体验。
24. 作为维护者，我希望拥有 Gate 0–4A 的可复现实测记录，以便知道哪些能力已证实、哪些仍是假设。

## 实现决策

### 产品边界

- 项目名称为 DeskMate；Host 是 macOS 电脑，Device 是 HarmonyOS NEXT MatePad。
- V1 仅支持单一 Reference Environment：具体 MatePad/HarmonyOS Build、Mac/macOS Build、记录过协商速率的直连线缆；不经过 Hub 或扩展坞。信息未补齐前不宣称兼容任何具体设备。
- V1 为独立 Extended Display，不是主屏镜像；不采用显示欺骗器作为静默备选。
- V1 仅承诺 Basic Input：单指鼠标等价控制、双指滚动和可选基础快捷键。压感、倾角、悬停、系统级多指手势属于后续独立预研。
- V1 不采集或传输音频，不申请麦克风权限，Host 显式关闭 ScreenCaptureKit 音频。音频消息类型保留但不受支持。
- V1 以 60Hz、原生分辨率和 HiDPI 验收；120Hz 与不超过 25ms 光子延迟是实验目标。

### Host

- 使用 CGVirtualDisplay 私有 API 创建 Extended Display，ScreenCaptureKit 采集，VideoToolbox 实时硬编码；优先 HEVC，兼容 H.264，禁止 B 帧。
- 优先有线 AOA；开发调试 TCP 仅验证单向视频，代码级拒绝输入消息，不属于 V1 产品链路。
- 使用单一逻辑发送调度器：控制、输入和时钟消息在分片边界优先于视频，视频初始最多两个在途 URB。
- 初始视频分片为 16–32KB，待发字节阈值 512KB，最老未发帧年龄 16–20ms；这些参数必须经 PoC 调整，不是性能保证。
- 拥塞时只丢弃尚未开始发送的旧 GOP 剩余帧，并请求新 IDR；健康连接下已开始的帧完整发送，STOPPING 时取消全部双向 transfer。
- 清理顺序为停止采集、失效编码器、取消并排空 libusb、销毁 Extended Display。事件循环持续至所有已提交 transfer 均回调结束。
- 锁屏停止采集、编码和输入；解锁重验资源。系统睡眠结束 Session；录屏权限撤销停流，辅助功能权限撤销只停输入；建屏失败保留控制连接并报告错误。
- 暂停后停止采集编码，Extended Display 初始保留 30–60 秒；超时销毁。独立 Developer ID 签名、公证分发，不进入 Mac App Store。

### Device

- ArkTS 负责界面、USB 授权和 USBAccessoryHandle 所有权；C++ NDK 负责字节 I/O、重组、AVCodec 和输入采集，不直接关闭 accessoryFd。
- 单一控制线程独占 Session 状态及解码器 Flush/Reset/Stop/Destroy；回调只投递带代际的命令，不在回调线程同步清理。
- SurfaceOwner 与 Session 分离，通过带代际和引用计数的 Surface Lease 使用 NativeWindow；引用操作串行化，Surface 销毁后立即停止送显。
- 解码器 Surface 输出不做 CPU 像素读取。正常输出提交渲染，恢复代际输出释放而不送显。
- 正常 I/O 清理先拒绝新工作、唤醒读写、join，再由 ArkTS 关闭 handle。若实测只能由 closeAccessory 唤醒，则执行拒绝新工作、所有者关闭唤醒、join、确认释放的分支。
- 不用 shutdown 取消 USB 字符设备 I/O；Gate 1 依序验证 poll/eventfd、无 SA_RESTART 的定向信号及官方关闭行为。
- 切后台暂停视频；后台连接及心跳能否维持由 Gate 1/3 实测，失联按断连处理。

### 候选协议

- 协议状态为 proposed；Gate 1–3 和两端互操作通过后才可转 accepted。草案期 version 为 1，CLIENT_HELLO 和 STREAM_PREPARE 携带单调递增 proto_rev。
- 使用 32 字节统一头并显式小端序列化，不以 C 结构体内存布局作为线上格式；magic 为 DM，线上字节 0x44 0x4D。
- 头字段依次是 2B magic、1B version、1B type、2B flags、1B frag_idx、1B frag_total、4B session_nonce、4B video_stream_id、4B seq、4B payload_len、8B timestamp_us。
- 视频 timestamp_us 是 Host 原始采集时刻，不得被编码时间覆盖。session_nonce 由 Device 生成，video_stream_id 由 Host 生成，均为非零 CSPRNG 值，不作为认证凭证。
- 单包最多 65536 字节；视频单帧最多 64 分片；控制、输入和校时包不分片且载荷不超过 4096 字节。分配前检查所有边界和当前状态，不盲目扫描坏流。
- 会话级消息只校验 session_nonce；视频、输入、恢复和提交消息同时校验 session_nonce 与 video_stream_id。
- 使用 PREPARE/READY/COMMIT/COMMITTED 两阶段切换。PREPARE 只做非破坏性检查与预分配；Device 回 COMMITTED 后 Host 才发送新流 IDR。config_id 重复时幂等返回状态。
- 单 Surface 独占、切换保留 Last Good Frame、Flush/Reconfigure 行为和旧配置回退能力必须由 Gate 3 验证，不预设成立。
- 新恢复周期递增 recovery_id，同周期 150ms 重试复用原 ID；恢复 IDR 回显 recovery_id，旧代际 P 帧不进入解码器。
- Host 等待 HELLO、READY、COMMITTED 各 2 秒；Device 等待 PREPARE、COMMIT 和首个新流 IDR 各 2 秒。暂停有 ACK，但 Host 不等待 ACK 才停编。
- 初始心跳 1 秒，3 秒无有效消息判失联。能力无交集和 proto_rev 不匹配明确报错并终止；未知或不支持的消息类型报错并丢弃，不自动断开健康 Session。

### 安全

- USB Network Fallback 只在 Gate 2 验证系统入口、macOS 原生免驱 NCM/ECM 和稳定性后启用；RNDIS 不合格。
- Socket 输入仅在绑定 USB 点对点接口或 loopback、完成一次性配对码确认、对端公钥持久化与加密、用户明确开启后允许。
- AOA 的 VID/PID、配件身份和随机标识只做一致性校验，不称为密码学认证。

### 验证门禁

- Gate 0：独立验证虚拟显示器创建、HiDPI、持续采集、锁屏/睡眠恢复及 Developer ID 签名公证。失败即停止 Extended Display 路线。
- Gate 1：验证 AOA 请求与重枚举、配件授权、持续双向吞吐及 accessoryFd 取消/唤醒；同时验证后台 I/O 行为。
- Gate 2：独立验证 USB Network Fallback；不得从 AOA 成败推导其可用性。
- Gate 3：验证 AVCodec 低延迟、Flush/Reset/Destroy 的 Surface 行为、耗时、回调隔离和 Last Good Frame 保留。
- Gate 4A：验证四时间戳校时、软件打点和光学测量可复现，不设性能阈值。
- V1 性能退出门禁：软件管线 P95 不超过 15ms；相对光子延迟 P95 不超过 40ms，35ms 为优化目标。降低原生分辨率以达标必须另经 ADR 审批。

## 测试决策

### 唯一跨端接缝

以 Session 契约作为唯一跨端测试接缝：输入为控制消息、字节 I/O、确定性单调时钟和最小化外部事件/结果；观察公开状态、输出消息和外部行为。替换字节 I/O，不模拟采集器、编码器或解码器内部管线，不新增平行抽象层。

时钟用于确定性触发 2 秒握手超时、1/3 秒心跳、恢复去抖及 Pause Grace Timeout，不依赖真实 sleep。最小结果桩只注入 DISPLAY_ERROR、STREAM_COMMITTED/ERROR、SUSPENDED、锁屏/睡眠和权限变化等边界结果。字节 I/O 可构造拆包、粘包、非法包头、重复/陈旧消息、拥塞、EIO 和 EOF。

### 外部行为测试

- 协议：合法字段往返、跨端 golden vectors、所有长度和分片边界、非法 magic/version/type/flags/session/video ID、截断与粘连、最大帧限制，以及分配前拒绝异常输入。
- 握手：能力交集、空交集、proto_rev 不匹配、每阶段超时、PREPARE 幂等、COMMITTED 前无新流视频、COMMIT 失败及首个 IDR 超时。
- 会话：暂停 ACK 与立即停编、恢复、重复消息、过期消息、心跳失联、后台与 Host 系统事件、权限独立降级。
- 调度与恢复：高优先级分片插队、字节/帧龄拥塞、健康态在途帧完整、GOP 切换、REQUEST_IDR 重试复用 ID、冻结 Last Good Frame、旧输出被门控。
- 生命周期：STOPPING 拒绝新工作、双向取消、回调后释放、FD 单所有者、Surface Lease 代际及重复销毁安全。
- 安全：调试 TCP 永远拒绝输入；Socket 未绑定安全接口、未认证或未经用户允许时不能注入输入。

字节 I/O 契约测试仅证明协议、状态机、调度和错误语义，不证明 CGVirtualDisplay、AOA、accessoryFd、AVCodec 或延迟达标。硬件结论只能来自 Gate 0–3、Gate 4A 和 V1 性能门禁的实测记录。当前仓库没有实现或既有测试先例。

### 延迟与画质

- Software Pipeline Latency 从 Host 采集回调到 Device 请求解码器输出；使用四时间戳校时并记录 RTT、漂移和误差上界。
- 相对 Photon Latency 用至少 240fps 高速相机同时拍摄 Mac 内置参考屏和 Device；参考屏锁定 60Hz，记录实际显示模式和 Reference Link。
- 每种负载至少 100 个有效跳变，使用非同步伪随机间隔避免 240fps/60Hz 锁相，报告 P50/P95/P99、N、误差上界和三态结论。
- 点估计为 L、误差为 epsilon：L+epsilon 不超过 40ms 为通过；L-epsilon 大于 40ms 为不达标；其余为不确定，升级高帧率或光电二极管测量。
- 高对比跳变、文字滚动和高动态视频用于延迟；静态高密度桌面用于清晰度、色彩和稳定性，不用于光学跳变测量。

## 范围之外

- Wi-Fi 产品化传输、跨广域网远程桌面或多 Device 同时连接。
- 主屏镜像、HDMI/USB-C 显示欺骗器、Mac App Store 分发。
- 120Hz、音频、麦克风、M-Pencil 压感/倾角/悬停、系统级多指手势和完整键盘映射。
- 未指定设备的兼容承诺、自动适配所有华为 VID/PID、RNDIS 支持。
- Gate 0–4A 通过前的性能承诺、全量生产端实现及 Phase 2 后排期。

## 补充说明

- 详细背景与官方资料记录于本功能的 research 文档；长期决策以仓库 ADR 和 GLOSSARY 为准。
- 若 Gate 0 失败，停止 Extended Display 路线；若 Gate 1 失败，仅触发 Gate 2 的条件式备选评估；若 Gate 2 也不可行，暂停有线产品路线。
- 若实现揭示协议字段或硬件行为需要改变，先修订 proposed 协议、proto_rev 和相关 ADR，不以实现静默覆盖本规格。

## Comments

- 已确认 Session 为唯一跨端测试接缝，时钟和最小边界结果注入属于该接缝，不新增平行抽象。
