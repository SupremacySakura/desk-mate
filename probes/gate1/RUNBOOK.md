# Gate 1 AOA 真机运行手册

本手册用于 Reference Environment 或明确标记的非基准开发环境。任何结果都先记录
拓扑，再记录行为；非直连链路不得写成 Gate 1 通过。

## 0. 冻结身份与拓扑

1. 用已知数据线把 Device 直连 Mac 机身 USB 口，不经过 Hub、扩展坞或显示器。
2. 在 Host 执行：

   ```sh
   probes/gate1/scripts/capture_environment.sh evidence/local/run-001-env
   ```

   必须保存 `host.txt`、`ioreg-usb.txt`、`usb-topology.txt`、
   `system-profiler-usb.*`、`hdc.txt` 和 `SHA256SUMS`。

3. 从 `ioreg-usb.txt`/`usb-topology.txt` 确认目标设备的父链。若出现 Hub，
   本轮固定标记为 `non-reference topology`。
4. 记录 Mac 型号、macOS build、Device 型号/序列号、HarmonyOS build、API、
   屏幕模式和线缆标识。没有实际读到的字段写 `not captured`，不得补猜。

## 1. 构建与安装

1. 构建 Host：

   ```sh
   probes/gate1/scripts/build_host.sh
   probes/gate1/host/build-real/gate1_host --help
   ```

2. 构建 Device：

   ```sh
   export DEVECO_SDK_HOME=/Applications/DevEco-Studio.app/Contents/sdk
   probes/gate1/scripts/build_device.sh
   probes/gate1/scripts/check_device_boundary.sh
   ```

3. 记录 HAP 和 `.so` 的 SHA-256。签名/安装使用实际 DevEco 或 hdc 命令：

   ```sh
   HDC=.../hdc
   "$HDC" list targets
   "$HDC" install -r device/entry/build/default/outputs/default/entry-default-signed.hap
   ```

   `entry-default-unsigned.hap` 被拒绝（例如 `no signature file`）是有效构建证据，
   但不是安装证据。没有匹配当前 UDID/bundle name 的签名材料时停止，不得改写其他
   项目的 profile，也不得把安装失败写成已执行授权。

## 2. 基线枚举

1. 保持 Device 在普通 HDC/文件传输模式，启动 HAP，界面应显示 `not authorized`。
2. Host 执行：

   ```sh
   DYLD_LIBRARY_PATH=/opt/homebrew/lib \
     probes/gate1/host/build-real/gate1_host list
   ```

   记录目标 VID:PID、bus、address 和 speed。此后所有命令使用这里的 bus/address。

## 3. AOA 重枚举与授权

1. Host 开始一轮完整运行：

   ```sh
   DYLD_LIBRARY_PATH=/opt/homebrew/lib \
     probes/gate1/host/build-real/gate1_host run \
     --bus N --address N \
     --handshake-timeout-ms 60000 \
     --run-seconds 30 \
     --heartbeat-ms 1000 \
     --stats-ms 2000 \
     --zlp-every 32 \
     --json evidence/local/eventfd-host.json
   ```

2. Host 打印 51/52/53 与重枚举后，在 Device 上依次点：
   `1. 授权并打开`，然后 `启动 eventfd`。
   若使用自动脚本，先把 `scripts/auto_device.sh` 推到 Device 并在 Host 启动 AOA
   前执行；脚本会在 AOA 后重新 `aa start`，并在每次点击前 dump layout：

   ```sh
   HDC=.../hdc
   "$HDC" file send probes/gate1/scripts/auto_device.sh /data/local/tmp/gate1_auto.sh
   "$HDC" shell chmod 755 /data/local/tmp/gate1_auto.sh
   "$HDC" shell sh /data/local/tmp/gate1_auto.sh
   ```

   执行前必须手动解锁 Device。开发者模式下 `aa start` 在锁屏时会返回
   `Error Code:10106102` 但进程退出码仍为 0；脚本会检查输出文本，在
   `/data/local/tmp/gate1_auto.log` 记录失败而不是误判为已在前台。

   Host 同时使用 `--handshake-initial-delay-ms 10000` 等待 Device 完成
   `openAccessory` 与 worker 启动，避免首个 `HELLO` 在 accessory 尚未就绪时打到
   OUT endpoint。
3. Host 每秒重发 HELLO；看到 `HELLO_ACK` 后进入双向传输。
4. 记录 Host 每秒进度；至少确认：
   - `handshake_ok=true`；
   - `data_echoes_received > 0`；
   - `echo_mismatches=0`；
   - `heartbeat_acks > 0`；
   - `device_stats_seen=true`；
   - 结束时 `active_on_release=0`、`cleanup_safe=true`。
5. 一轮结束后在 Device 点 `停止并 join`，记录 native 返回 JSON 中的
   `stop_to_wake_us`、`stop_to_worker_exit_ms`、`wake_reason` 和错误计数。
   自动脚本结束后取回：

   ```sh
   "$HDC" file recv /data/local/tmp/gate1_auto.log evidence/local/
   "$HDC" file recv /data/local/tmp/gate1_auto_layout.json evidence/local/
   "$HDC" file recv /data/local/tmp/gate1_auto_stats.json evidence/local/
   "$HDC" file recv /data/local/tmp/gate1_auto_final.json evidence/local/
   "$HDC" file recv /data/local/tmp/gate1_auto_hilog.log evidence/local/
   ```

   若 Device 首次 `read()` 失败，记录 `last_read_errno`、`last_poll_revents` 和
   hilog 的 `read failed: errno=... fd=... mode=... revents=...`，不得只写
   “read-error” 而省略 errno。

## 4. 三种唤醒分支

每个分支都从新的授权/打开开始；不要把上一个 branch 的结果复用于下一个。

### 4.1 poll/eventfd

按第 3 节执行。预期 worker 由内部 eventfd 唤醒，`wake_reason=eventfd`，
`eventfd_wakeups>0`。记录实际微秒/毫秒值，不填理论值。

### 4.2 无 SA_RESTART 的定向 SIGUSR1

1. 重新授权并打开。
2. Device 点 `启动 signal`；Host 完成 HELLO 和双向数据后，点
   `信号唤醒并 join`。
3. 预期 `wake_reason=signal`、`signal_wakeups>0`。若 poll/read 不返回 `EINTR`
   或 join 超时，记录失败及 errno，不得改成 eventfd 结果。

### 4.3 官方 closeAccessory 唤醒

严格按 UI 顺序：

1. Device 点 `启动 owner-close`，Host 完成 HELLO 和双向数据。
2. 点 `A. 拒绝新工作`，保存返回 JSON。
3. 点 `B. closeAccessory`。此步只能由 ArkTS 调用官方 API。
4. 点 `C. join`。预期 `wake_reason=owner-close`，worker 退出且后续 join 成功。

原生代码不得自行 `close(accessoryFd)`。如果只有关闭 owner handle 才能唤醒，
这个顺序本身就是 Gate 1 需要的证据。

## 5. 拆分、粘包和 ZLP

- 拆分：协议 decoder 在独立单测中已覆盖“头跨 chunk”和“帧跨 chunk”；
  真机 Host 端报告 `in_bytes > in frames`，Device 端 `frames_read` 正确递增。
- 粘包：Host 的 OUT buffer 可包含多个帧；Device 必须仍得到期望的
  `frames_read`，`parse_errors=0`。
- ZLP：Host 记录 `zero_length_out_scheduled/completed`，Device 记录
  `zero_reads`/`zero_writes`。同时保存总线分析结果才可声称“线上 ZLP 已证实”；
  仅有 `write(fd, ..., 0) == 0` 时写成“zero-length write attempt observed”。

## 5.1 实测平台约束（2026-11-24，MatePad Pro / OpenHarmony 7.0.0.111）

以下行为在真机上实测确认，后续任何 Gate 1 运行都必须按此设计，否则会出现
"传输正常但 stop 永远 join 不回来"的假失败：

1. **`accessoryFd` 的 `read()` 是"按消息"语义**。当读缓冲小于 Host 单次 Bulk OUT
   时，多余字节被直接丢弃，不会留到下次 `read()`。缓冲必须足够大（本轮自适应
   到 16 KB）。
2. **64 KB 读请求返回 `EINVAL`**，而 `poll()` 仍然报 `POLLIN`。读缓冲需要按
   `EINVAL` 自适应下调；不要假设 `poll` 可读就代表 `read` 能成功。
3. **`poll(accessoryFd)` 不遵守 timeout**。51 秒运行中 `poll_timeouts=0`，无流量
   期间计数器完全冻结。不要依赖 `poll` 的毫秒超时做周期性任务。
4. **定向 `SIGUSR1` 不能唤醒阻塞在 `accessoryFd` 上的 worker**。实测 `read()` 返回
   `errno=5 (EIO)` 而不是 `EINTR`，worker 会走 `read-error` 退出路径，
   `signal_wakeups=0`。该唤醒分支在本平台判定为 `FAILED`，不得改写成 eventfd 结果。
5. **只有官方 `closeAccessory` 能可靠唤醒 worker**，表现为 `POLLHUP`
   （`last_poll_revents=0x20`）。
6. **写方向必须非阻塞**。Host 停止排空 Bulk IN 后，设备 OUT 端点被 NAK，阻塞
   `write()` 会永久挂住 worker 且 eventfd 无法打断。探针使用 `O_NONBLOCK` +
   输出缓冲 + `POLLOUT` + 128 KB 高水位背压。
7. **AOA 重枚举会把 Ability 切到后台**，`aa start` 往往需要多轮重试。自动脚本
   必须等到设备统计出现真实流量（`frames_read > 0`）后再执行停止，不能用固定
   sleep，否则 stop 会落在握手完成之前。
8. **`hdc file recv` 在设备 Offline 时仍返回退出码 0**。取证据前必须先删除本地
   目标文件，并校验重新拉取的文件非空，否则会把旧文件误当成本轮证据。

## 6. 后台与心跳

1. `eventfd` 或 `signal` 模式持续运行时，记录一次 Device `Native stats`。
2. 按 Home 切到后台，保持至少 10 秒；记录 Host 是否出现
   `no valid inbound frame for at least ... ms`、`heartbeat_loss_events`、
   Device `lifecycle_background`/`poll_timeouts`。
3. 回到前台，再看是否恢复以及在哪个 seq 恢复。若系统冻结进程、读失败或心跳
   永久失联，照实记录，不推断 Session 状态机行为。
4. 保存 Host JSON 和 Device 统计截图/文字；记录前后台时间戳。

## 7. 拔线与重连

1. 在双向传输中直接拔线。Host 应报告 `NO_DEVICE`/取消错误；如有 transfer
   未回调，记录 `active_on_release` 和 `cleanup_safe=false`，不得强行释放。
2. Device 记录 poll/read 的 errno、`read_errors`、`wake_reason` 和 worker 是否退出。
3. 重新插线，确认普通枚举、重新授权、AOA 51/52/53、`HELLO_ACK` 和双向回显
   都在新 Session 中重新发生。旧 handle/worker 不得复用。

## 8. 完成记录

复制 `evidence/TEMPLATE.md` 到 `evidence/<date>-<environment>/REPORT.md`，填写：

- 拓扑是否为 Reference Link，以及原始命令证据；
- 实际协商速率和是否经过 Hub/扩展坞；
- 构建、签名、安装、授权每一步的真实结果；
- 三种唤醒分支的原始 JSON；
- 并行取消、`active_transfers` 归零、ZLP 表现；
- 拔线、后台、重连；
- 未执行项及原因。

结论只能是 `PASS`、`FAIL`、`NOT EVALUATED` 之一。直连拓扑、Reference Environment、
真实安装授权和完整矩阵缺任一项时，不得写 `PASS`。
