# 02: 首次握手到可显示状态

**What to build：** Host 与 Device 从 CLIENT_HELLO 走到 COMMITTED 和首个新流 IDR，维护者可确定性验证能力交集、协议修订、各阶段超时和明确错误，而无需真实编解码器。

**Blocked by：** 01: 候选协议端到端回声

**Status:** ready-for-agent

- [ ] Host/Device 按 HELLO、PREPARE、READY、COMMIT、COMMITTED、首个有效 IDR 的次序推进 Session。
- [ ] Host 在 COMMITTED 前不发送新流 IDR；Device 只接受匹配 Session Nonce 和 Stream Epoch 的消息。
- [ ] 编码器、像素格式和分辨率只从双方能力交集选择；空交集和 proto_rev 不匹配明确报错并结束 Session。
- [ ] 使用确定性时钟验证双方各自的 2 秒阶段超时，无真实 sleep。
- [ ] PREPARE 的非破坏性边界、config_id 幂等、COMMIT 失败及首帧超时均可通过最小边界结果桩验证。
