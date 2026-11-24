# 03: 暂停、恢复、重配与生命周期

**What to build：** 已建立的 Session 可暂停、恢复、无损准备运行中配置切换并响应 Host 系统事件；维护者可验证四时间戳校时和确定性资源清理顺序。

**Blocked by：** 02: 首次握手到可显示状态

**Status:** done

- [x] 暂停立即触发停编且有 ACK；恢复重新协商，心跳 1 秒与静默 3 秒失联使用确定性时钟验证。
- [x] STREAMING 中 PREPARE/READY/COMMIT/COMMITTED 的失败或超时保留旧流，重复 config_id 不重复清理资源。
- [x] 0x05 四时间戳交换计算并报告时钟偏移、RTT、漂移和误差上界。
- [x] 锁屏、睡眠、录屏权限撤销、辅助功能权限撤销和 DISPLAY_ERROR 产生各自明确的公开状态。
- [x] Pause Grace Timeout、Surface Lease 代际、FD 单所有者及 STOPPING 至释放的顺序通过边界结果验证。
- [x] 本任务不宣称真实阻塞 accessoryFd 已可唤醒，该结论由 Gate 1 提供。

## Comments

- 实现完成。暂停/恢复重协商、运行中配置切换保留旧流、四时间戳校时、公开错误状态与清理顺序均由 `core/tests/session_tests.cpp` 覆盖。真实阻塞 `accessoryFd` 的唤醒结论由 Gate 1 提供，本任务不作该声明。
- 2026-11-24：全部验收项勾选，代码已合入 `main`（fast-forward 至 `6a9937e`）；`make test` 全绿。
