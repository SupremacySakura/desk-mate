# 08: Gate 2A：USB 网络共享硬件探测

**What to build：** 维护者无需构建 Plan B 产品通道，即可独立确定 Reference Environment 是否提供 macOS 原生免驱的 USB 网络共享及其吞吐稳定性。

**Blocked by：** None (can start immediately; requires Reference Environment)

**Status:** ready-for-agent

- [ ] 验证 Device 存在无需开发者模式的用户可操作 USB 网络共享入口。
- [ ] 记录 macOS 识别的接口类型、绑定驱动和网络配置；只有原生免驱 NCM/ECM 合格，RNDIS 判负。
- [ ] 在 Reference Link 测量吞吐、RTT 和抖动，并记录测试工具及原始结果。
- [ ] 本探针不接收或注入 Reverse Input，不宣称产品安全通道已实现。
- [ ] 结果独立记录，不能从 AOA 成败推导 USB 网络共享可用性。

## Comments

- 2026-11-24：Gate 2A 探针已实现（`probes/gate2a`，含契约测试），但真机项未执行。
  现有 `evidence/2026-11-24-non-reference-dev/` 记录的是非基准 Hub 链路，
  不足以判定 NCM/ECM 免驱可用性。
- 需在 Reference Link（设备直连 Mac 机身 USB 口）上重新采集接口类型、绑定驱动、
  吞吐/RTT/抖动。
