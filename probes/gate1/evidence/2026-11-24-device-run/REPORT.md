# Gate 1 真机证据报告 — 2026-11-24

**结论：`NOT EVALUATED`（Gate 1 未通过评估）**

功能链路已在真机跑通，但拓扑不满足 Gate 1 的基准条件，因此本轮**不得**记为
Gate 1 通过。以下区分"已实测的功能事实"与"未满足的基准条件"。

---

## 1. 环境与拓扑（非基准）

| 项 | 实测值 | 来源 |
|---|---|---|
| Host | MacBook Air `Mac16,12`，Apple M4，16 GB | `environment/host.txt` |
| macOS | 27.3（build `26C5047e`） | `environment/host.txt` |
| Device | HUAWEI MatePad Pro `MRDI-W00` | `environment/hdc-device.txt` |
| 系统 | OpenHarmony 7.0.0.111（`MRDIL-W00 7.0.0.111(<redacted-serial>)`） | `environment/hdc-device.txt` |
| API | 26 | `environment/hdc-device.txt` |
| 面板 | 2800×1840 | 任务简报 |
| 链路 | **经 USB2.1 Hub，480 Mbps（USB 2.0 High-Speed）** | `environment/usb-tree.txt`、host JSON `device.speed` |
| 序列号 | 已脱敏为 `<redacted-serial>` | `environment/*` |

**这是 non-reference topology。** 规格要求 Gate 1 的 Reference Link 为设备直连
Mac 机身 USB 口、不经 Hub 或扩展坞，并记录实际协商速率。本轮经过 Hub 且协商为
USB 2.0，不满足基准条件。这正是结论固定为 `NOT EVALUATED` 的唯一原因——
不是功能失败。

## 2. 构建与安装

| 项 | 结果 |
|---|---|
| Host 构建 | `probes/gate1/scripts/build_host.sh`，真实 libusb + CTest 全 PASS |
| Device 构建 | hvigor `BUILD SUCCESSFUL`，arm64-v8a |
| HAP 安装 | `hdc install -r` 成功（使用本地签名材料，签名块未入库） |
| 静态边界检查 | `check_device_boundary.sh` PASS（C++ 未 close/shutdown `accessoryFd`） |

## 3. 双向传输（实测通过）

AOA `GET_PROTOCOL(51)` → `SEND_STRING(52)` → `START(53)` 后设备重枚举为
`18d1:2d00`，Host claim interface 0，端点 `0x81`/`0x01`，max packet 512，
bInterval 0，bulk。

| 分支 | Host OUT | 设备 bytes_read | Host IN | 设备 bytes_written | 回显帧 | 错帧 | 解析错误 |
|---|---|---|---|---|---|---|---|
| eventfd | 29,475,128 | **29,475,128** | 29,476,788 | **29,476,788** | 12,936 | 0 | 0 |
| owner-close | 82,082,023 | **82,082,023** | 65,835,680 | **65,835,680** | 28,895 | 0 | 0 |

两端字节数**逐字节相等**，`echo_mismatches=0`、`unmatched_echoes=0`、
`parse_errors=0`。这证明在 480 Mbps 链路上持续双向 Bulk 传输、帧重组与回显正确。

**关于 `bytes_read` 的说明**：早前 v12 轮次曾观察到 Host `out_bytes` 大于设备
`bytes_read`。本轮复核确认那是**统计快照时点差异**——Host JSON 中的
`device_stats` 是停止前的 STATS 响应快照，而设备侧停止后的最终统计与 Host
`out_bytes` 完全相等（见上表）。不存在丢字节。

## 4. 三种唤醒分支（真机实测）

| 分支 | wake_reason | 唤醒计数 | stop→wake | stop→worker exit | 结果 |
|---|---|---|---|---|---|
| poll/eventfd | `eventfd` | `eventfd_wakeups=1` | 1,410 µs | 1 ms | **PASS** |
| 官方 closeAccessory | `owner-close` | `owner_close_wakeups=1` | 3,884,836 µs | 3,884 ms | **PASS** |
| 定向 SIGUSR1 | 无 | `signal_wakeups=0` | — | — | **FAILED** |

eventfd 与 owner-close 均为干净的同轮证据：两端字节数完全一致，计数器
（`eventfd_wakeups`/`signal_wakeups`/`owner_close_wakeups`）互不污染，证明
`startProbe()` 的计数器重置生效。

### 4.1 eventfd（通过）

- `wake_reason=eventfd`，`stop_to_wake_us=1410`，`stop_to_worker_exit_ms=1`
- `read_errors=0`、`write_errors=0`、`parse_errors=0`
- `frames_read=12964 == frames_written=12964`（无滞留响应）
- 证据：`branch-eventfd.json/.log`、`device-eventfd-result.json`、`device-eventfd-prestop.json`、`hilog-gate1-eventfd.txt`

### 4.2 owner-close（通过）

严格按 A→B→C 顺序由 ArkTS 调用官方 API：

1. `A. 拒绝新工作` → native 置 `reject_new_work_`
2. `B. closeAccessory` → ArkTS 关闭 `USBAccessoryHandle`
3. `C. join` → worker 被 `POLLHUP` 唤醒并退出

- `last_poll_revents=0x20`（POLLHUP），`wake_reason=owner-close`
- `stop_to_wake_us=3884836`，`stop_to_worker_exit_ms=3884`
- C++ 未调用 `close(accessoryFd)`；句柄由 ArkTS 关闭（静态边界检查通过）
- 证据：`branch-owner-close.json/.log`、`device-owner-close-result.json`、`device-owner-close-prestop.json`、`hilog-gate1-owner-close.txt`

### 4.3 定向 SIGUSR1（失败，如实记录）

`SIGUSR1` 到达时设备侧 `read()` 直接返回 `errno=5 (EIO)`，`poll revents=0x1`，
worker 以 `exit_reason=read-error` 退出，`wake_reason` 为空、`signal_wakeups=0`。

```
Gate1Probe: read failed: errno=5 (I/O error) fd=30 mode=signal revents=0x1
Gate1Probe: worker exit: read-error
```

**结论：该平台不支持以定向 SIGUSR1 唤醒阻塞在 `accessoryFd` 上的 worker。**
这是真实平台行为，不是测试脚本缺陷。已实测的可复现事实是：信号确实送达
（`read()` 被中断并返回 EIO 而非 EINTR），但唤醒路径本身不成立，因为 worker
以错误路径退出而非按信号语义退出。

这也与 owner-close 的结果自洽：只有关闭 owner handle 才能可靠唤醒。按 RUNBOOK
要求，本项记为 `FAILED`，不得改写成 eventfd 结果。

## 5. 取消与清理（实测通过）

| 项 | eventfd | owner-close |
|---|---|---|
| `cancellation_requested` | true | true |
| `cancellation_ok` | true | true |
| `cancel_wait_ms` | 0 | 0 |
| `active_on_release` | 0 | 0 |
| `cleanup_safe` | true | true |
| 取消后回调 | in 337 µs / out 340 µs | in 361 µs / out 364 µs |

Host 同时取消 Bulk IN/OUT，事件循环持续到 `active_transfers == 0` 才释放接口。
`in_callback_after_cancel_us` / `out_callback_after_cancel_us` 记录了取消后仍到达的
回调延迟，说明取消与释放顺序安全。

## 6. 拆包、粘包与 ZLP

- **拆包/粘包**：设备 `parse_errors=0`，`frames_read` 与 Host `data_frames_sent`
  在同一量级且回显无错帧；帧头跨 chunk 与多帧粘包由 decoder 重同步逻辑处理
  （`FrameDecoder::resync_discarded_bytes()`）。单元测试覆盖头跨 chunk 与帧跨 chunk。
- **ZLP**：Host `zero_length_out_scheduled/completed` 相等（eventfd 101/101、
  owner-close 281/281）；设备 `zero_writes` 非零。**注意**：这只是零长度写尝试
  与完成的观测，**没有总线分析仪，不得声称"线上 ZLP 已证实"**。

## 7. 未执行项（not executed）

| 项 | 状态 | 原因 |
|---|---|---|
| 拔线重连 | not executed | 本轮未做受控拔线实验；AOA 结束后设备停在 accessory 模式需重新插拔，属环境恢复而非重连测试 |
| Device 后台冻结 | partially observed | `lifecycle_background` 在 signal/owner-close 轮次有计数（1/2），但未做受控后台切换实验 |
| 心跳丢失（受控） | not executed | owner-close 轮次 `heartbeat_loss_events=1` 是 Host 运行窗口长于设备停止所致，不是受控丢包实验 |
| Reference Link 直连 | not executed | 硬件为 Hub 拓扑，无法在本轮补齐 |
| 总线分析仪 ZLP | not executed | 无设备 |

## 8. 本轮修复的实现缺陷

真机测试暴露并修复了以下探针缺陷（均有代码与验证）：

1. **AOA `SEND_STRING` 编码错误**：原发 UTF-16LE，规范要求 UTF-8 + NUL。已改为
   `append_aoa_string`。设备现能读到完整 `DeskMate / DeskMate Gate 1 Probe / AOA wired duplex probe`。
2. **阻塞 `write()` 导致 join 永久卡死**：Host 停止排空 Bulk IN 后设备 OUT 端点被
   NAK，`write()` 永久阻塞，`eventfd` 无法打断。已改为 `O_NONBLOCK` + 输出缓冲 +
   `POLLOUT` + 128 KB 高水位背压。修复前 `frames_read - frames_written = 1` 且
   `join timed out`；修复后两者相等且 `stop_to_worker_exit_ms=1`。
3. **计数器跨轮次污染**：`startProbe()` 未清零统计，导致上一分支的
   `signal_wakeups`/`owner_close_wakeups` 残留到下一分支。已在 `start()` 重置全部
   计数器与 decoder。
4. **帧解码器一次失步即永久失效**：已加入重同步（丢弃到下一个 magic），
   `process_input` 不再 `decoder_.reset()`。
5. **`write()` 对 EINTR 无条件重试**：会掩盖真实错误，已改为可中止。
6. **Host OUT 按字节切片跨帧**：已改为每帧一次 bulk OUT，payload 上限
   `out_capacity - 16`。

## 9. 平台约束（写入 RUNBOOK）

1. **`accessoryFd` 的 `read()` 是"按消息"语义**：缓冲小于 Host 单次 OUT 时，
   剩余字节被丢弃。必须使用足够大的缓冲（本轮自适应到 16 KB）。
2. **64 KB 读请求返回 `EINVAL`**，而 `poll()` 仍报 `POLLIN`。读缓冲需自适应下调。
3. **`poll(accessoryFd)` 不遵守 timeout**：51 秒运行中 `poll_timeouts=0`，静默期
   计数器完全冻结。因此 `poll()` 的 250 ms 超时不可依赖。
4. **定向 SIGUSR1 不能唤醒阻塞的 `accessoryFd` 读**：`read()` 返回 `EIO` 而非
   `EINTR`，worker 走错误退出路径。
5. **只有官方 `closeAccessory` 能可靠唤醒 worker**（`POLLHUP`）。
6. **AOA 重枚举会把 Ability 切到后台**，`aa start` 需要多轮重试；自动脚本必须
   等到真实流量出现后再执行停止，不能用固定 sleep。

## 10. 结论

- **Gate 1 结论：`NOT EVALUATED`。** 拓扑为 Hub + USB 2.0，非 Reference Link。
- 传输功能层面：AOA 授权、重枚举、持续双向 Bulk、帧重组、取消与清理均已在真机
  实测通过，两端字节数逐字节相等。
- 唤醒层面：`eventfd` 与官方 `closeAccessory` 两条路径实测通过；定向 `SIGUSR1`
  实测失败，已如实记录。
- 要在基准环境判定 Gate 1，需在设备直连 Mac 机身 USB 口、记录实际协商速率的
  条件下重跑本报告第 3–6 节的完整矩阵，并补齐第 7 节的未执行项。

## 11. 证据清单

| 文件 | 内容 |
|---|---|
| `environment/` | Host/Device 身份、USB 拓扑、HDC 采集（序列号已脱敏） |
| `branch-eventfd.json/.log` | eventfd 分支 Host 侧完整结果 |
| `device-eventfd-result.json` | eventfd 分支设备侧停止后统计（权威） |
| `device-eventfd-prestop.json` | eventfd 分支传输中统计快照 |
| `hilog-gate1-eventfd.txt` | eventfd 分支设备日志 |
| `branch-owner-close.json/.log` | owner-close 分支 Host 侧完整结果 |
| `device-owner-close-result.json` | owner-close 分支设备侧停止后统计（权威） |
| `device-owner-close-prestop.json` | owner-close 分支传输中统计快照 |
| `hilog-gate1-owner-close.txt` | owner-close 分支设备日志 |
| `branch-signal.json/.log` | 另一轮 signal 握手超时记录（**不是**唤醒轮证据） |
| `eventfd-host-v2..v12.*` | 逐轮排查记录（编码、读缓冲、ZLP、重同步、帧对齐） |
| `read-only-v1.log` | IN 方向隔离实验 |

**注意（signal 分支证据链）**：本目录**没有** signal 分支的 Host 侧成功轮
JSON，也没有单独的 `device-signal.log` / `hilog-gate1-signal.txt`。仓库内的
`branch-signal.json/.log` 是另一轮失败记录（握手 150 s 超时、0 字节），**不能**
当作 signal 唤醒的证据。signal 分支唯一可核对的设备侧记录在
`hilog-gate1-owner-close.txt` 第 2 行：同一应用进程（PID 41495）在 owner-close
轮开始前，`stop signal` 返回 `last_read_errno=5`、`exit_reason=read-error`、
`signal_wakeups=0`。因此该分支结论按「实测失败、缺少同轮 Host 配对工件」记录，
不声称有完整证据链。
