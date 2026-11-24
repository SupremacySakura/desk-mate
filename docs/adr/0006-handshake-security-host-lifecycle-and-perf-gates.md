# 以限时握手和分层门禁约束主机生命周期

Status: proposed

DeskMate 只从双方能力交集中选择编码器、像素格式和显示配置。Host 负责等待 `CLIENT_HELLO`、`READY`、`STREAM_COMMITTED` 的各 2 秒初始超时；Device 负责等待 `STREAM_PREPARE`、`STREAM_COMMIT` 和匹配新流的首个有效 IDR 的各 2 秒初始超时。无能力交集或不支持的 `proto_rev` 先尽力返回明确错误再终止 Session。未知顶层类型和 V1 不支持的音频消息返回 `UNSUPPORTED_MSG_TYPE` 并丢弃；只有消息结构非法或状态机明确禁止时才终止或回到 IDLE。Host 收到 `STREAM_COMMITTED` 后才发送新流 IDR。

调试 TCP 只承载单向视频，代码级拒绝输入消息，不作为 V1 产品链路。USB 网络共享若成为 Plan B，只允许绑定 USB 点对点接口或 loopback，启用输入前必须完成一次性配对码确认、对端公钥持久化、加密传输和用户显式授权。AOA 的设备字段和 Session Nonce 只做一致性校验，不构成密码学认证。

Mac 系统睡眠直接结束 Session；锁屏停止采集、编码及输入注入，解锁后重验资源。录屏权限撤销停止视频并返回错误，辅助功能权限撤销只关闭输入注入并保留视频。虚拟屏创建失败保留控制通道并报告 `DISPLAY_CREATE_FAILED`，禁止自动重连形成失败循环。

Gate 4A 仅验证四时间戳校准、软件分阶段打点及至少 240fps 摄影测量可复现，不设性能通过阈值。Gate 1–3 和 4A 通过后才启用 60Hz 原生分辨率下软件 P95 不超过 15ms、相对光子延迟 P95 不超过 40ms 的 V1 性能退出门禁；降低原生分辨率以达标属于产品范围变更，必须另经 ADR 确认。
