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
- 合法与非法 golden vectors。

当前实现不包含暂停/恢复、运行中重配、心跳、帧级优先调度或 GOP 恢复策略。这些
属于后续任务，不能由本目录的实现推断为已完成。

## 构建与测试

环境要求：支持 C++20 的 `clang++` 与 GNU Make。共享核心没有第三方依赖，也不需要
CMake。

```sh
make test
```

`make` 会生成共享静态库 `build/libdeskmate-protocol.a`、协议测试程序
`build/deskmate-tests` 和 Session 测试程序 `build/deskmate-session-tests`。
`make lib` 只生成静态库。

也可以分别运行测试：

```sh
./build/deskmate-tests testdata/golden
./build/deskmate-session-tests
```

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
  clock.hpp             单调时钟接缝、SteadyClock 与测试用 ManualClock
  byte_io.hpp           Session/调度器可复用的字节 I/O 接缝
core/src/               共享核心实现
core/tests/             无框架协议测试与 Host/Device Session 契约测试
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

## Session 握手 API

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

`SessionLimits::stage_timeout_ns` 默认 2 秒。Host 等待 HELLO、READY、
COMMITTED；Device 等待 PREPARE、COMMIT、首个新流 IDR。超时通过
`SessionFailure` 暴露并终止 Session。测试使用 `ManualClock::advance_ns()` 推进
时间，不调用 sleep。

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

## Golden vectors

`testdata/golden/manifest.txt` 每行格式为：

```text
name | expectation | expected-payload-hex | packet-hex
```

`expectation` 可为 `message`、`video`、`unsupported`、`incomplete` 或
`error:<ErrorCode>`。测试逐字节喂入解码器，因此 golden 同时覆盖部分头、部分载荷、
粘包与错误发生时机。向量字节由协议字段独立构造，测试不会用被测编码器生成期望值。

## 已知限制

- 协议仍为 proposed；Gate 1–3 与真实两端互操作通过前不得转为 accepted。
- 控制 opcode 与载荷前缀是本任务为后续实现定义的草案契约；若真机或 Session
  设计需要调整，必须先修订 proposed 协议与 `proto_rev`。
- 本仓库不证明 AOA、`accessoryFd`、AVCodec、虚拟显示器或任何性能指标。
- `ByteIo` 只有接口；测试中的 Host/Device 内存管道是契约 harness，不是生产传输层。
- 分片不连续产生 `RequestRecovery` 事件，但选择 IDR、暂停/恢复及 Last Good Frame
  属于后续 Session/调度任务。
- 当前只实现首次握手。暂停/恢复、心跳失联、运行中重配、Pause Grace Timeout 和
  系统权限变化尚未实现；不同 `config_id` 的 PREPARE 在首次握手期会被拒绝。
- `DeviceBoundary` 是最小结果桩，不证明 Surface Lease、解码器 Flush/Reconfigure
  或旧流回退能力；这些仍由 Gate 3 验证。
- 首个 IDR 之后不实现优先级调度、GOP 边界丢弃、恢复 IDR 重试或 Last Good Frame
  门控。
