# 采用显式帧协议和分片级优先调度

Status: proposed

DeskMate v1 候选协议使用 32 字节统一包头并逐字段显式序列化，不以 C 结构体内存布局作为线上格式。视频按序号重组，控制、输入和时钟包在分片边界优先调度；拥塞由待发字节数和最老帧年龄联合触发，而非按帧数猜测。16–32KB 分片、最多两个在途视频 URB、512KB 待发阈值和 16–20ms 帧龄是 PoC 初始参数，不是未测先承诺的性能保证。Gate 1–3 和两端互操作测试通过后才转为 accepted。

视频 `timestamp_us` 表示 Host 的原始采集时刻；编码耗时需另带测量字段或诊断元数据，不能覆盖采集时间戳。Device 生成 `session_nonce`，Host 生成 `video_stream_id`；两者都是非零 CSPRNG 值，配合 Session 状态降低陈旧消息混淆概率，不能宣称绝对无碰撞。草案期 `version` 固定为 1，`CLIENT_HELLO` 和 `STREAM_PREPARE` 携带单调递增的 `proto_rev`，不一致即拒绝。

## v1 包头

| 偏移 | 字段 | 编码 |
| --- | --- | --- |
| 0 | `magic` | 线上字节 `0x44 0x4D`（`DM`） |
| 2 | `version` | `uint8`，v1 为 1 |
| 3 | `type` | `uint8` |
| 4 | `flags` | `uint16` LE |
| 6 | `frag_idx` | `uint8`，从零开始 |
| 7 | `frag_total` | `uint8`，至少为一，v1 最多 64 |
| 8 | `session_nonce` | `uint32` LE，Device 生成并双向回显 |
| 12 | `video_stream_id` | `uint32` LE，Host 生成；无活动流为零 |
| 16 | `seq` | `uint32` LE，视频为帧序号，其余为消息序号 |
| 20 | `payload_len` | `uint32` LE，当前包载荷 |
| 24 | `timestamp_us` | `uint64` LE，视频为采集时刻 |

单包最大 65536 字节，视频单帧最多 64 个分片，控制/输入/校时包不分片且载荷不超过 4096 字节。接收端在任何载荷分配之前验证所有字段、帧级上限及当前状态；魔数、版本或分片字段异常触发流复位或重连，不盲目扫描后续字节。

会话级消息只匹配 `session_nonce`；视频、输入、恢复和提交消息同时匹配 `session_nonce` 与 `video_stream_id`。载荷中的 `proto_rev`、`config_id` 和 `recovery_id` 均须按各自消息语义验证。
