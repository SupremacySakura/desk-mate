# 04: 优先发送与 GOP 恢复

**What to build：** Host 在大视频帧传输期间及时处理高优先级消息，发生拥塞或断流时以完整 GOP 边界恢复并保留 Last Good Frame；异常退出则立即取消在途工作。

**Blocked by：** 02: 首次握手到可显示状态

**Status:** ready-for-human

- [x] 控制、输入和校时消息可在视频分片边界优先调度，视频在途深度受配置限制。
- [x] 最大待发字节数与最老帧年龄均可触发旧 GOP 清理和新 IDR 请求。
- [x] 健康连接下已开始帧完整发送，未开始旧 GOP 帧被丢弃；不按帧数猜测拥塞。
- [x] 新恢复周期更新 recovery_id，同周期重试复用原值；旧代际输出被门控并冻结 Last Good Frame。
- [x] 进入 STOPPING 后立即取消尚未完成的在途工作，不等待完整帧保护。
- [x] 使用字节 I/O 与确定性时钟验证以上行为，不模拟编解码内部实现。

## Comments

- 实现完成。优先级调度、在途深度限制、按字节/帧年龄触发 GOP 清理、recovery_id 代际门控与 STOPPING 立即取消均由 `core/tests/scheduling_tests.cpp` 覆盖。
