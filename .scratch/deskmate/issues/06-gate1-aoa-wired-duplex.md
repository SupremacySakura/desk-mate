# 06: Gate 1：AOA 有线双向通道

**What to build：** 基准 Host 与 Device 可通过 AOA 完成授权、持续双向字节传输、取消和拔线重连；维护者获得 Reference Link、后台行为及确定清理能力的实测报告。

**Blocked by：** None (can start immediately; requires Reference Environment)

**Status:** ready-for-agent

- [ ] 记录 MatePad/HarmonyOS Build、Mac、线缆、直连拓扑及实际协商 USB 2.0/3.x 速率。
- [ ] 最小双端探针完成 AOA 请求、重枚举、配件授权和持续双向吞吐测试。
- [ ] 验证拆包、粘包、ZLP 和异常断流在 accessoryFd 读写中的实际表现。
- [ ] 依次验证 poll/eventfd、无 SA_RESTART 的定向信号、官方 closeAccessory 的唤醒行为；C++ 不直接关闭 accessoryFd。
- [ ] Host 同时取消 Bulk IN/OUT，事件循环持续至 active_transfers 为零后释放资源。
- [ ] 记录拔线重连和 Device 后台冻结对 I/O 与心跳的影响，不把 Session 状态机测试当作真实唤醒证据。
