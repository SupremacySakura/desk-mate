# 08: Gate 2A：USB 网络共享硬件探测

**What to build：** 维护者无需构建 Plan B 产品通道，即可独立确定 Reference Environment 是否提供 macOS 原生免驱的 USB 网络共享及其吞吐稳定性。

**Blocked by：** None (can start immediately; requires Reference Environment)

**Status:** ready-for-agent

- [ ] 验证 Device 存在无需开发者模式的用户可操作 USB 网络共享入口。
- [ ] 记录 macOS 识别的接口类型、绑定驱动和网络配置；只有原生免驱 NCM/ECM 合格，RNDIS 判负。
- [ ] 在 Reference Link 测量吞吐、RTT 和抖动，并记录测试工具及原始结果。
- [ ] 本探针不接收或注入 Reverse Input，不宣称产品安全通道已实现。
- [ ] 结果独立记录，不能从 AOA 成败推导 USB 网络共享可用性。
