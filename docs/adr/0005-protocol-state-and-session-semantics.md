# 以代际和两阶段提交管理候选协议会话

Status: proposed

DeskMate 的协议草案以 `session_nonce` 区分 Session，以 `video_stream_id` 区分视频代际，防止重连或布局切换后的陈旧消息被接受。`CLIENT_HELLO`、心跳和暂停属于会话级消息；视频、输入、恢复与配置提交必须同时匹配两个标识。随机标识不提供身份认证。

`STREAM_PREPARE(config_id)` 只校验 Surface 租约、硬件能力和缓冲资源，不破坏旧流；`STREAM_COMMIT(config_id, video_stream_id)` 才允许控制线程切换解码器。重复 `config_id` 返回当前状态，不重复销毁资源。切换或 IDR 输出完成以明确的 ACK/状态消息确认，不能由 Host 在发送 COMMIT 后自行假定成功。单 Surface 是否要求独占绑定、Flush/Reconfigure 是否足够，以及失败后的 Last Good Frame 保留能力由 Gate 3 验证；未验证前不承诺无损切换或旧配置可回退。

每次新恢复周期递增 `recovery_id`，同周期 150ms 重试复用原值；恢复 IDR 以标志位指示载荷前附有 4 字节小端回显 ID。暂停请求有 ACK，但 Host 立即停止采集和编码，不等待确认。初始心跳为 1 秒，3 秒无有效消息判定失联；后台冻结和超时的实测结果决定暂停连接是否能保留。
