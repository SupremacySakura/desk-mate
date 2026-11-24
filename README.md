# DeskMate

DeskMate 是 macOS Host 与 HarmonyOS NEXT Device 之间的有线 Extended Display 与
Reverse Input 项目。当前仓库处于 **Design / Pending Hardware Verification**；
本目录中的协议与 Session 实现是 proposed V1 共享核心，不是已完成硬件验证的产品
协议。

## 当前范围

- 显式小端序列化的 32 字节候选协议头与 V1 消息类型。
- 流式拆包、粘包处理，以及在任何载荷分配前的头部与长度校验。
- 测试视频分片重组、分片乱序补齐、帧序号不连续检测和 Recovery IDR 前缀。
- 控制、输入与校时消息的非分片约束；音频与未知顶层类型被明确拒绝并丢弃。
- 基于 `ByteIo` 接缝的 Host/Device 控制回声与测试视频端到端契约测试。
- Host/Device 从 `CLIENT_HELLO` 到 `STREAM_COMMITTED` 和首个匹配 Stream Epoch
  的 IDR 的两端握手状态机。
- 编码器、像素格式和分辨率的双方能力交集协商；空交集与 `proto_rev` 不匹配会
  返回明确错误并终止 Session。
- 可注入的单调时钟与内存测试时钟；双方各握手阶段的 2 秒超时无需真实 sleep。
- PREPARE 幂等、非破坏性边界、COMMIT 失败、`DISPLAY_ERROR` 和首帧超时的确定性
  契约测试。
- Host 暂停立即停止采集/编码与输入意图，Device 返回 `PAUSE_ACK`；恢复通过新的
  `config_id` 重新执行 `PREPARE/READY/COMMIT/COMMITTED`。
- 1 秒心跳与 3 秒无有效消息失联检测，全部由可注入时钟驱动，不依赖真实 sleep。
- `STREAMING` 中的运行配置切换在成功提交前保留旧流；失败或超时回退，重复
  `config_id` 幂等。
- 0x05 四时间戳交换估算 Device 相对 Host 的时钟偏移、RTT、漂移与误差上界。
- 锁屏、睡眠、录屏权限撤销、辅助功能权限撤销和 `DISPLAY_ERROR` 各自公开独立的
  降级状态；Pause Grace Timeout 限制为 30–60 秒。
- Surface Lease 代际与引用计数、FD 单所有权，以及
  `STOPPING → 拒绝新工作 → 取消 I/O → 排空回调 → 释放` 的可观察契约。
- `HostSendScheduler` 的控制消息优先发送、已开始分片完整发送、拥塞时只丢弃新
  keyframe 之前的旧 GOP 未开始帧，以及未发送帧超龄后的恢复请求。
- `DeviceRecoveryGate` 的 Recovery Epoch、150 ms 同周期重试与 Last Good Frame
  门控；旧 GOP 的 P 帧不会进入解码器。恢复 IDR 会同时写入标志位和 4 字节小端
  `recovery_id` 前缀，调度器的两条入队路径都不会静默吞掉恢复请求。
- 重定向到新 Stream Epoch 时，解码器会丢弃候选流尚未完成的包并重新同步到下一个
  合法头部边界，不会把候选流的残留字节拼进旧流关键帧。
- 锁屏是覆盖握手、重配与 Streaming 的持久状态：任一阶段锁屏都会阻止采集/输入
  意图，且 `resume()` 返回 `DisplayUnavailable`；Device 在首个 IDR 前也接受 Pause。
- 回滚保留旧流并重新等待旧流 IDR；失败的候选 `config_id` 迟到的 ACK 会被忽略，
  不会打断后续重配或让 Device 对已回滚候选再发最终确认。
- Basic Input 的 28 字节显式小端载荷（触点/滚动/按键、归一化坐标与主键位）；
  Device 只在 `Streaming` 发送，Host 只在 `Streaming` 且输入意图有效时交付，
  陈旧 Stream Epoch 或锁屏后的输入不会进入注入队列。
- 合法与非法 golden vectors。

上述能力是 Host 侧共享核心与确定性契约测试；真实 USB 吞吐、编码器行为、
AVCodec Flush/重建、Surface 保留和光学延迟仍需对应 Verification Gate 的真机证据。

## 构建与测试

环境要求：支持 C++20 的 `clang++` 与 GNU Make。共享核心没有第三方依赖，也不需要
CMake。

```sh
make test
```

`make` 会生成共享静态库 `build/libdeskmate-protocol.a`、协议测试程序
`build/deskmate-tests`、Session 测试程序 `build/deskmate-session-tests` 和
调度测试程序 `build/deskmate-scheduling-tests`。`make lib` 只生成静态库。

也可以分别运行测试：

```sh
./build/deskmate-tests testdata/golden
./build/deskmate-session-tests
./build/deskmate-scheduling-tests
```

当前主机套件为协议 21 项、Session 75 项、调度 15 项，共 111 项；全部使用内存
`ByteIo` 与 `ManualClock`，不依赖真实 sleep、USB 设备或编解码器。

清理构建产物：

```sh
make clean
```

## 目录

```text
core/include/deskmate/
  protocol.hpp          线上类型、头部/包/控制载荷编码与基础校验
  message_stream.hpp    流式包解码、视频重组与协议事件
  session.hpp           Host/Device 握手、能力协商、公开状态与边界结果
  scheduler.hpp         控制/视频优先发送、GOP 边界丢弃与恢复请求
  clock.hpp             单调时钟接缝、SteadyClock 与测试用 ManualClock
  byte_io.hpp           Session/调度器可复用的字节 I/O 接缝
core/src/               共享核心实现
core/tests/             无框架协议、Host/Device Session 与调度契约测试
testdata/golden/        独立于 C++ 编码器生成的合法/非法线上字节
```

`MessageStreamDecoder::feed` 接受任意切分的字节块并返回以下事件：

- `Message`：完整的控制、输入或校时消息。
- `VideoFrame`：完整重组的一帧视频；分片元数据来自原包头，载荷长度被归一化为
  整帧长度。
- `RejectedMessage`：可安全丢弃的单包错误，例如不支持类型、陈旧 Session 或流代际、
  分片重复/乱序/不连续。连接仍可继续。
- `StreamError`：无法继续可靠定界的致命错误。解码器进入 `failed()` 状态；调用方
  必须丢弃当前流状态并重连或显式 `reset()`，不得扫描后续字节猜测包边界。

## Session API

`session.hpp` 提供 `HostSession` 与 `DeviceSession`。构造时注入同一个
`ByteIo` 接缝的一对方向端和 `MonotonicClock`；`start()` 启动本地阶段，
`pump()` 非阻塞读取到 `WouldBlock` 并刷新待写字节。调用方交替 `pump()` 两端，
通过公开的 `state()`、`failure()`、`negotiated_stream()` 和
`DeviceSession::take_video_frames()` 观察结果。

生产适配层负责用 CSPRNG 生成非零 `HostSessionConfig::video_stream_id` 和
`DeviceSessionConfig::session_nonce`；随机标识只用于筛选陈旧消息，不作认证。

```cpp
deskmate::protocol::SteadyClock clock;
deskmate::protocol::HostSession host(host_io, clock, host_config);
deskmate::protocol::DeviceSession device(device_io, clock, device_config, &boundary);

host.start();
device.start();
host.pump();
device.pump();
// Host 在 state() == Streaming 后发送首个 keyframe。
```

状态顺序：

| Host | Device |
| --- | --- |
| `AwaitingClientHello` | `AwaitingPrepare` |
| `AwaitingReady` | `AwaitingCommit` |
| `AwaitingCommitted` | `AwaitingFirstIdr` |
| `Streaming` | `Streaming` |

建立 Session 后，Host 通过 `pause()`、`resume()`、`reconfigure()`、
`request_clock_sync()` 和 `handle_host_event()` 驱动暂停、运行配置切换、校时和
macOS 系统事件。`request_stop()` 只进入 `Stopping`；所有已提交回调排空后由
`complete_stop()` 进入 `Stopped`，或使用同步便捷方法 `stop()`。`VideoState` 与
`InputState` 分别表达锁屏、睡眠、录屏权限、辅助功能权限和显示错误的降级结果。

`SessionLimits::stage_timeout_ns` 默认 2 秒。Host 等待 HELLO、READY、
COMMITTED；Device 等待 PREPARE、COMMIT、首个新流 IDR。Host 不以本地首 IDR
计时器抢先回退，而是等待 Device 的明确 `PROTOCOL_ERROR` 或后续常规失联检测，
避免 Device 已收到候选 IDR 时两端对“是否切换”作出相反判断。超时通过
`SessionFailure` 暴露；重配超时回退旧流，初始握手超时终止 Session。测试使用
`ManualClock::advance_ns()` 推进时间，不调用 sleep。

Host 的 `pause()` 先关闭采集/编码与输入意图，再发送 `PAUSE`，不等待 ACK。
`resume()` 要求新的 `config_id` 和非零新 `video_stream_id`；Host 发送 `RESUME`
并重新走完整配置切换。`STREAMING` 中的 `reconfigure()` 使用两阶段确认：

1. Device 在 `STREAM_COMMIT` 后保留旧流并返回第一次 `STREAM_COMMITTED`（4 字节
   stream id）；
2. Host 收到第一次确认后才发送候选 Stream Epoch 的视频，并在收到候选首个
   keyframe 前继续保留旧协商结果；
3. Device 收到候选 keyframe 后才释放旧流并返回第二次 `STREAM_COMMITTED`
   （5 字节，末尾 stage byte 为 `2`）；
4. Host 只在第二次确认后切换 `negotiated_stream()` 并恢复采集/输入意图。

重配失败或超时回滚旧流，重置解码器并等待旧流的新 IDR；健康 Session 不因回滚或
重复的确认消息终止。锁屏期间即使握手或重配完成，Host 也不会恢复采集/输入意图；
`resume()` 会返回 `DisplayUnavailable`。

Pause Grace Timeout 通过 `SessionLimits::pause_grace_timeout_ns` 配置，允许
30–60 秒，默认 30 秒。超时只销毁一次 Extended Display，并报告
`PauseGraceExpired`。

协商规则：

- Host 按自身列表顺序选择第一个双方都支持的编码器、像素格式和分辨率。
- 任一类别没有交集时返回 `NoCommonCapabilities` 并终止 Session。
- `proto_rev` 与本地 `protocol_revision` 不同时返回
  `UnsupportedProtocolRevision` 并终止 Session。
- 同一 `config_id`、同一 profile 的重复 PREPARE 只重发 READY，不重复调用
  `DeviceBoundary::prepare_stream`，也不提交流。
- `DeviceBoundary::commit_stream` 返回失败时 Device 发送 `PROTOCOL_ERROR`，
  不发 `STREAM_COMMITTED`。

Host 只有在收到匹配的 `STREAM_COMMITTED` 后进入 `Streaming`；
`send_video_frame` 在该状态前返回 `NotStreaming`，首个视频帧必须设置
`keyframe=true`，否则返回 `IdrRequired`。Device 在 COMMIT 后只接受同时匹配
Session Nonce 与 Stream Epoch 的视频，并在首个 keyframe 前忽略 P 帧。

## 线上格式

所有多字节整数均为显式小端，不使用 C/C++ 结构体内存布局。固定头为 32 字节：

| 偏移 | 字段 | 编码 |
| ---: | --- | --- |
| 0 | `magic` | `0x44 0x4D`（`DM`） |
| 2 | `version` | `uint8`，当前为 `1` |
| 3 | `type` | `uint8` |
| 4 | `flags` | `uint16` |
| 6 | `frag_idx` | `uint8`，从零开始 |
| 7 | `frag_total` | `uint8`，`1..64` |
| 8 | `session_nonce` | `uint32`，非零 |
| 12 | `video_stream_id` | `uint32`；视频和输入必须非零 |
| 16 | `seq` | `uint32`；视频为帧序号，其余为消息序号 |
| 20 | `payload_len` | `uint32` |
| 24 | `timestamp_us` | `uint64`；视频为 Host 原始采集时刻 |

约束如下：

- 单包载荷最多 `65536` 字节；控制、输入与时钟载荷最多 `4096` 字节。音频属于
  不受支持类型，接收端仍先执行通用头校验，然后丢弃整个包。
- 只有视频可分片；视频单帧最多 64 片，每片至少 1 字节，整帧最多 4 MiB。
- 控制、输入与时钟必须 `frag_idx = 0`、`frag_total = 1` 且 `flags = 0`。
- 视频允许 `keyframe`（`0x0001`）与 `recovery IDR`（`0x0002`）标志；其他位非法。
- Recovery IDR 的帧载荷前 4 字节是 `recovery_id` 的小端回显；重组后由解码事件
  单独暴露，不留在 `payload` 中。

### 消息类型

| `type` | 名称 | 支持状态 |
| ---: | --- | --- |
| `0x01` | Video | 支持；允许分片 |
| `0x02` | Audio | V1 保留但不支持；报 `UnsupportedMessageType` 并丢弃 |
| `0x03` | Input | 支持；不可分片，需匹配非零 `video_stream_id` |
| `0x04` | Control | 支持；不可分片 |
| `0x05` | Clock | 支持；不可分片 |

其他顶层类型按 `UnsupportedMessageType` 丢弃，不终止健康 Session。

### 控制载荷草案

控制消息载荷采用 10 字节前缀，后接 opcode 专属 body：

| 偏移 | 字段 | 编码 |
| ---: | --- | --- |
| 0 | `opcode` | `uint8` |
| 1 | reserved | `uint8`，必须为 0 |
| 2 | `proto_rev` | `uint32` |
| 6 | `config_id` | `uint32` |
| 10 | body | 剩余字节 |

当前 opcode：`CLIENT_HELLO=0x01`、`STREAM_PREPARE=0x02`、`STREAM_READY=0x03`、
`STREAM_COMMIT=0x04`、`STREAM_COMMITTED=0x05`、`PAUSE=0x06`、`PAUSE_ACK=0x07`、
`RESUME=0x08`、`HEARTBEAT=0x09`、`REQUEST_IDR=0x0A`、`PROTOCOL_ERROR=0x0B`。
`CLIENT_HELLO` 与 `STREAM_PREPARE` 必须携带非零 `proto_rev`；单调递增检查属于
Session 层，本实现的握手只接受与本地 `kProtocolRevision` 相同的修订。

### Session 控制 body

所有 Session body 都是显式小端，并受 4096 字节控制载荷上限约束。本实现没有修改
32 字节包头或既有 opcode 数值；新增的是 body 契约和独立 Session API。

能力 body：

| 偏移 | 字段 | 编码 |
| ---: | --- | --- |
| 0 | `encoding_version` | `uint8`，当前为 1 |
| 1 | `encoder_count` | `uint8`，最多 8 |
| ... | `encoders` | `uint8`，`H264=1`、`HEVC=2` |
| ... | `pixel_format_count` | `uint8`，最多 8 |
| ... | `pixel_formats` | `uint8`，`NV12=1`、`P010=2`、`BGRA8=3` |
| ... | `resolution_count` | `uint8`，最多 8 |
| ... | `resolutions` | 每项 `uint16 width`、`uint16 height`、`uint16 refresh_hz` |

`STREAM_PREPARE` 与 `STREAM_READY` 的 body 是 9 字节 profile：

| 偏移 | 字段 |
| ---: | --- |
| 0 | `encoding_version`，当前为 1 |
| 1 | `encoder` |
| 2 | `pixel_format` |
| 3 | `width`，LE `uint16` |
| 5 | `height`，LE `uint16` |
| 7 | `refresh_hz`，LE `uint16` |

`STREAM_COMMIT` 与 `STREAM_COMMITTED` 的 body 是 4 字节非零
`video_stream_id`；包头 `video_stream_id` 必须与 body 相同。`PROTOCOL_ERROR`
body 是 1 字节 `SessionFailure` 数值，见 `session.hpp`。

Input body 固定为 28 字节，全部显式小端：

| 偏移 | 字段 | 编码 |
| ---: | --- | --- |
| 0 | `kind` | `uint8`，`TOUCH_DOWN=1`、`TOUCH_MOVE=2`、`TOUCH_UP=3`、`TOUCH_CANCEL=4`、`SCROLL=5`、`KEY=6` |
| 1 | `pointer_id` | `uint8` |
| 2 | `flags` | `uint16`；bit0 为主键按下 |
| 4 | `x` | `float32`，归一化到 Device 显示宽度 `0..1` |
| 8 | `y` | `float32`，归一化到 Device 显示高度 `0..1` |
| 12 | `dx` | `float32`，滚动增量（显示宽度的倍数） |
| 16 | `dy` | `float32`，滚动增量（显示高度的倍数） |
| 20 | `keycode` | `uint32`，仅 `KEY` 使用且必须非零 |
| 24 | reserved | `uint32`，必须为 0 |

`x`/`y`/`dx`/`dy` 必须为有限值；保留位非零、长度不符或未知 `kind` 会被丢弃而不
终止 Session。

0x05 校时 body 固定为 28 字节：

| 偏移 | 字段 | 编码 |
| ---: | --- | --- |
| 0 | `encoding_version` | `uint8`，当前为 1 |
| 1 | `kind` | `uint8`，`REQUEST=1`、`RESPONSE=2` |
| 2 | reserved | 2 字节，必须为 0 |
| 4 | `t0` | Host 发送时刻，LE `uint64` ns |
| 12 | `t1` | Device 收到时刻，LE `uint64` ns |
| 20 | `t2` | Device 回复时刻，LE `uint64` ns |

Host 在收到响应时记录本地 `t3`。`ClockSyncEstimator` 以
`offset = ((t1 - t0) + (t2 - t3)) / 2` 估算 Device 相对 Host 的时钟偏移，
`RTT = (t3 - t0) - (t2 - t1)`，误差上界为 `RTT / 2`；连续样本同时报告
`drift_ppb`。这些是软件时钟契约，不代表 Gate 4A 的光学延迟结论。

## Golden vectors

`testdata/golden/manifest.txt` 每行格式为：

```text
name | expectation | expected-payload-hex | packet-hex
```

`expectation` 可为 `message`、`video`、`unsupported`、`incomplete` 或
`error:<ErrorCode>`。测试逐字节喂入解码器，因此 golden 同时覆盖部分头、部分载荷、
粘包与错误发生时机。向量字节由协议字段独立构造，测试不会用被测编码器生成期望值。

## 已知限制

- 协议仍为 proposed；Gate 0–4A 与真实两端互操作通过前不得转为 accepted。
- 控制 opcode 与载荷前缀是本任务为后续实现定义的草案契约；若真机或 Session
  设计需要调整，必须先修订 proposed 协议与 `proto_rev`。
- 本仓库不证明 AOA、`accessoryFd`、AVCodec、虚拟显示器或任何性能指标。
- `ByteIo` 只有接口；测试中的 Host/Device 内存管道是契约 harness，不是生产传输层。
- 分片不连续产生 `RequestRecovery` 事件；本仓库验证 Host 调度、Recovery IDR
  请求重试与 Last Good Frame 的软件契约，但不证明真实链路吞吐或硬件解码器在
  高负载下的表现。
- Pause/resume、心跳、重配与系统事件只验证 Session 状态机和资源契约，不证明真实
  `accessoryFd` 可被唤醒、后台心跳可维持或 AVCodec 的 Flush/Reconfigure 行为。
- `SurfaceOwner`/`SurfaceLease` 与 `BorrowedAccessoryFd` 是可注入的资源边界桩；
  `BorrowedAccessoryFd` 只借出 fd，不提供任何关闭路径（ArkTS 独占
  `USBAccessoryHandle`）。HarmonyOS NativeWindow 引用和真实 USB 句柄生命周期
  仍由 Gate 1/3 验证。
- Device 是否能在 Gate 3 条件下保留旧流、恢复旧配置或无损切换 Surface 尚未实证；
  本实现的回退仅是控制面契约，回滚后仍要求旧流重新提供 IDR。
- 重配最终确认与初始确认共用 `STREAM_COMMITTED` opcode，靠 body 长度区分：4 字节
  为初始确认，5 字节且末尾 stage byte 为 `2` 为候选首个 keyframe 后的最终确认。
  两种确认都可重传；Host 在候选 IDR 前收到重复的初始 `STREAM_COMMIT` 只回初始
  确认，不回最终确认。

## 验证状态

2026-11-24 实测结果（Host 为 MacBook Air `Mac16,12`，Device 为 MatePad Pro
`MRDI-W00`）：

- 根 `make clean && make test`：协议、Session、调度全部通过。
- Gate 0：探针 `make test` 通过；live 在真机上 **failed**。Screen Recording 预检
  通过、虚拟屏可创建可销毁（无幽灵副屏），但 macOS 27.3 不再为该私有 API 发布
  2x 模式，HiDPI 校验失败，采集与编码从未启动。详见
  `probes/gate0/records/README.md`。
- Gate 1：Host 真实 libusb 构建与 CTest 通过，Device unsigned HAP 编译通过。
  真机 AOA 授权、重枚举、持续双向 Bulk、取消清理均实测通过，两端字节数逐字节
  相等；`eventfd` 与官方 `closeAccessory` 唤醒通过，定向 `SIGUSR1` 实测失败。
  **结论 `NOT EVALUATED`**：链路为 USB2.1 Hub / 480 Mbps，不是 Reference Link。
  详见 `probes/gate1/evidence/2026-11-24-device-run/REPORT.md`。
- Gate 2A：`make test` 39/39、`validate` 与 `SHA256SUMS` 校验通过。真机只做了
  只读拓扑识别，没有启用 USB 网络共享，也没有吞吐测量，结论 `pending`。
- Gate 3：Host 侧检查与记录校验通过；真机 AVCodec 解码 640x360 彩条、Last Good
  Frame 冻结/恢复、注入卡顿 `69.58 ms` 均有记录。Annex-B Host 样本互操作已由
  下方产品端到端实测确认（功能层面可解码上屏），但 Gate 3 整体仍因缺 Reference
  Link 保持 `needs-info`。
- 产品端到端（`apps/host` + `apps/device`，非探针）：2026-11-24 真机实测
  **平板成功显示 Mac 画面**，HEVC 与 H.264 均可上屏；反向触控在 Mac 上生效。
  这一条只证明「成品链路能出画面」，不改变上面任何硬件门禁的结论。
  - 卡点一：设备端 `read()` 在无消息时会阻塞在驱动内（`O_NONBLOCK` 不生效），
    必须以 `POLLIN` 作为「一次消息读取」的许可，否则 worker 发完 `CLIENT_HELLO`
    就冻住。见 `apps/device/entry/src/main/cpp/fd_byte_io.hpp`。
  - 卡点二：Host 视频分片原为 32 KiB，超过设备 16 KiB 读缓冲上限，超出部分被驱动
    丢弃，表现为流起来 2–3 秒后 `invalid_wire_data`。现固定 12 KiB。
  - 卡点三（黑屏根因）：解码器 `Start()` 后立即把整个输入 buffer 池交给回调，此时
    首帧往往还没到；这些 buffer 被暂存，而喂帧只在 `OnNeedInputBuffer` 里触发。
    池被扣住后编解码器不再回调，形成死锁——HEVC 与 H.264 都表现为「会话
    `streaming`、Surface 已绑定、但一帧都没解码」。修复：`Push()` 入队后立即
    `PumpFrames()`。见 `apps/device/entry/src/main/cpp/video_decoder.cpp`。
  - 已知未解决：帧率偏低（Host 编码约 25 fps，实际送出约 9 fps、约 470 KB/s，
    `bulkErrors=0`、`framesDroppedBacklog=0`）。瓶颈在发送/消费速率，不在丢包或
    解码；未做性能优化，也不构成性能结论。

仍未执行、因此不得声称通过的硬件门禁：

- Gate 0：HiDPI Extended Display 创建、持续采集、编码、锁屏/睡眠清理、
  Developer ID 签名与公证。
- Gate 1：Reference Link 直连拓扑、受控拔线重连、受控后台冻结、心跳丢失注入。
- Gate 2A：Reference Link 上的 Device 用户入口、NCM/ECM 识别、ping 与双向
  iperf3 原始证据。
- Gate 4A：跨端时钟校准、软件打点和高速摄影光学延迟基线。
