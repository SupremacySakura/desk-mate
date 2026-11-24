# 06: Gate 1：AOA 有线双向通道

**What to build：** 基准 Host 与 Device 可通过 AOA 完成授权、持续双向字节传输、取消和拔线重连；维护者获得 Reference Link、后台行为及确定清理能力的实测报告。

**Blocked by：** None (can start immediately; requires Reference Environment)

**Status:** needs-info

- [ ] 记录 Reference Link 直连拓扑及实际协商速率（本轮为 USB2.1 Hub / 480 Mbps，非直连，线缆标识未采集）。
- [x] 最小双端探针完成 AOA 请求、重枚举、配件授权和持续双向吞吐测试。
- [x] 验证拆包、粘包、ZLP 和异常断流在 accessoryFd 读写中的实际表现。
- [x] 依次验证 poll/eventfd、无 SA_RESTART 的定向信号、官方 closeAccessory 的唤醒行为；C++ 不直接关闭 accessoryFd。
- [x] Host 同时取消 Bulk IN/OUT，事件循环持续至 active_transfers 为零后释放资源。
- [ ] 记录拔线重连和 Device 后台冻结对 I/O 与心跳的影响，不把 Session 状态机测试当作真实唤醒证据。

**结论：`NOT EVALUATED`。** 功能链路已在真机跑通，但链路为 USB2.1 Hub / 480 Mbps，
不是 Reference Link，按证据规则不得判定 Gate 1 通过。详见
`probes/gate1/evidence/2026-11-24-device-run/REPORT.md`。

## Comments

- 2026-11-24 真机实测（MatePad Pro MRDI-W00 / OpenHarmony 7.0.0.111 / API 26）：
  - AOA 51/52/53、重枚举、授权、持续双向 Bulk 全部通过；两端 `bytes_read`/`bytes_written`
    与 Host `out_bytes`/`in_bytes` 逐字节相等。
  - `poll/eventfd` 唤醒通过：`wake_reason=eventfd`，`stop_to_wake_us=1410`，exit 1 ms。
  - 官方 `closeAccessory` 唤醒通过：`wake_reason=owner-close`（`POLLHUP`），exit 3884 ms。
  - 定向 `SIGUSR1` 唤醒失败：`read()` 返回 `errno=5 (EIO)`，worker 走 `read-error`
    退出，`signal_wakeups=0`。该平台不支持此唤醒路径。
  - 并行取消 IN/OUT 通过：`active_on_release=0`、`cleanup_safe=true`。
  - 拓扑为 USB2.1 Hub、480 Mbps，**非 Reference Link**，故结论固定 `NOT EVALUATED`。
  - 未执行：受控拔线重连、受控后台冻结、心跳丢失注入、总线分析仪 ZLP。
- 阻塞原因：需在设备直连 Mac 机身 USB 口并记录实际协商速率的基准环境下重跑完整矩阵。
