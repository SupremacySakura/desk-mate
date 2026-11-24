# 09: Gate 2B：Plan B 安全通道实现

**What to build：** 只有 AOA 被判不可行且 USB 网络共享探测通过后，才为 Host 与 Device 提供经配对、加密和用户授权的 USB Network Fallback；未经授权的网络对端不能发送 Reverse Input。

**Blocked by：** 06: Gate 1：AOA 有线双向通道（AOA 不可行结论）；08: Gate 2A：USB 网络共享硬件探测（通过）

**Status:** ready-for-agent

- [ ] 仅当两个条件均满足时开始实施；否则记录不适用而不创建替代产品路径。
- [ ] Socket 只绑定 USB 点对点接口或 loopback，不暴露于任意局域网接口。
- [ ] 一次性配对码确认、对端公钥持久化及加密传输在启用输入前完成。
- [ ] 输入默认关闭，用户明确开启后才接受 Basic Input；未配对或授权撤销时拒绝输入。
- [ ] 安全通道仍使用同一 Session 契约，不新增平行跨端抽象。
