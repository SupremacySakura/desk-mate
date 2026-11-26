# DeskMate 验证记录与真机排障总结

本文档汇总了 DeskMate 共享核心契约测试、产品端到端真机调试中定位并解决的关键问题，以及 `probes/` 目录下正式硬件验证探针（Gate 0–4A）的当前状态。

---

## 1. 自动化契约测试（已通过）

### 共享 C++20 核心（根目录 `Makefile`）

运行命令：`make test`

- **协议测试（`core/tests/protocol_tests.cpp`，21 项）**：32 字节显式小端编解码、分片重组/乱序检测、长度与分片边界校验、非视频类型分片拒绝、音频等不支持类型安全丢弃、`testdata/golden/` 跨端 Golden Vectors 逐字节校验。
- **会话与生命周期测试（`core/tests/session_tests.cpp`，75 项）**：基于内存 `ByteIo` 与确定性 `ManualClock` 验证 `CLIENT_HELLO → STREAM_COMMITTED → 首 IDR` 握手、能力交集与 `proto_rev` 校验、`PREPARE` 幂等、运行中配置切换及超时回滚、`Pause/Resume`、1s/3s 心跳失联、0x05 四时间戳校时、锁屏/睡眠/权限撤销独立降级状态及清理顺序。
- **调度与恢复测试（`core/tests/scheduling_tests.cpp`，15 项）**：控制/输入消息在视频分片边界插队调度、字节数与帧龄双阈值拥塞丢帧（完整 GOP 边界）、`REQUEST_IDR` 与 `recovery_id` 去抖复用、Last Good Frame 门控及 `STOPPING` 立即取消。

### macOS Host 端测试（`apps/macos-host/Makefile`）

运行命令：`make -C apps/macos-host test`

- **应用包契约校验（`apps/macos-host/tests/contract.sh`）**：校验 `Info.plist` 元数据、CLI 参数合法性、必须链接框架（`libusb-1.0`、`ScreenCaptureKit`、`VideoToolbox`）、无探针/私有符号与密钥材料泄漏、禁止 AOA 退出阶段使用 `set_configuration`。
- **USB 桥接与多设备单元测试（`apps/macos-host/tests/usb_byte_io_test.cpp`，6 项）**：
  - `UsbByteIo` 写入超时部分发送进度保留与后缀续传
  - `UsbByteIo` 读取超时进度保留
  - 闲置 USB IN 轮询不饿死 60 fps 视频发送
  - 调度器在 288 KiB 关键帧场景维持 120 fps
  - 单设备拉黑与显式断开策略
  - **多设备并发链路（`DeviceLink`）隔离**：策略拉黑 / `dm_bridge_disconnect_link` 按链路精准断开互不干扰、按 `link_id` 隔离视频与输入队列、基于 `port_path` 区分无序列号并发设备身份。

---

## 2. 真机联调定位并修复的关键问题

在 Mac（Apple Silicon）与 HarmonyOS NEXT 平板/手机的真实 USB-C AOA 联调中，排查并修复了以下核心工程瓶颈：

| 问题现象 | 根因定位 | 已落地的修复方案 |
| --- | --- | --- |
| 发送 `CLIENT_HELLO` 后读写线程冻结 | 鸿蒙 USB 配件驱动的 `read()` 在无数据时忽略 `O_NONBLOCK` 直接在内核阻塞，占用单线程循环 | `FdByteIo` 改用 `poll(POLLIN)` 检测单次可读后再读，并用 `eventfd` 负责停止唤醒（见 `apps/harmonyos-client/entry/src/main/cpp/fd_byte_io.hpp`） |
| 推流 2–3 秒后报 `invalid_wire_data` 断流 | Host 原 32 KiB 视频分片超过鸿蒙配件驱动 16 KiB 读缓冲上限，尾部数据被驱动截断丢弃 | 固定视频分片载荷上限为 **12 KiB**（见 `Bridge.cpp`） |
| 会话进入 `streaming` 但屏幕一直黑屏 | `AVCodec` `Start()` 后将全部输入 buffer 池交给回调暂存，而喂帧只在 `OnNeedInputBuffer` 触发，首帧晚到时触发死锁 | `Push()` 帧入队后立即主动调用 `PumpFrames()` 驱动投喂（见 `video_decoder.cpp`） |
| 60/120 Hz 高帧率跌落至 0–8 fps 与写入超时 | ① `UsbByteIo` 在 `LIBUSB_ERROR_TIMEOUT` 时丢弃部分已写入字节，重传整块导致死循环；② 同期 IN 阻塞与 UI 定时器抢占了 TX/IDR 恢复 | ① 超时后按实际 `transferred` 推进偏移仅重传后缀；② 引入独立 `rx_loop()` 全双工读线程 + 条件变量唤醒；③ `33ms` 独立关键帧请求轮询与 `recovery_id` 请求合并；④ 帧龄阈值调优与 `App Nap` 抑制 |
| 手机端在横竖屏切换或返回设置后黑屏 | 切换界面或旋转重新触发 `XComponent` 尺寸变化时销毁重建解码器 | `OnSurfaceChanged` 在同一 `OHNativeWindow` 下保留解码器并动态重读尺寸；返回首页保留 Surface 租约 |
| 多指拖动无响应、妙控板模式指针跳变 | 早期单点 ArkTS 触摸映射丢失多指拓扑；妙控板相对坐标在回退模式未跟踪系统光标位置 | 在 C++ 原生层通过 `OH_NativeXComponent` 采集多指状态；Host 端按设备通道构建 4 模式状态机与长按锁存拖拽 |

### Windows Host 90/120 Hz 丢帧定位（2026-11-26）

30/60 Hz 下被帧预算掩盖的问题在 90 Hz（11.1 ms）/ 120 Hz（8.33 ms）下会连锁放大，按根因定位到以下六处，均已修复：

| 问题现象 | 根因定位 | 已落地的修复方案 |
| --- | --- | --- |
| 高刷下 `captureDropped` 暴涨 | `encoder_loop()` 每轮无条件 `wait_for_encoder(4)`，且异步 MFT 只靠 `GetEvent(NO_WAIT)` 轮询 | 实现 `IMFAsyncCallback` + `BeginGetEvent()`，事件到达即 `SetEvent(work_event_)`；无待处理帧时改为阻塞等待 |
| `encoded` 正常但 `dropped` 高、`live_fps` 掉 | `link_worker_loop()` 在未检查 `wakeup` 时 `ResetEvent(wait_event)`，吞掉刚到达的唤醒并睡满 5 ms；`has_pending_output()` 未统计 `video_queue_` | 锁内消费 `wakeup`（原子）；`has_pending_output()` 同时检查视频队列；写阻塞时按 1 ms 重试 |
| 90/120 Hz 下 USB 吞吐抖动 | `libusb_bulk_transfer` 同步发包，零流水线，单次超时卡 50 ms（等于 6 帧） | 改为异步 API：常驻 4 路 Bulk IN + 6 槽位 Bulk OUT，独立事件线程 |
| 90 Hz 限帧失效、CPU 空转 | `capture_wait_event_` 为 Manual-Reset 且 `signal_work()` 每帧 `SetEvent`，从未 `ResetEvent` | 改为 Auto-Reset，仅 `stop()` 置位；限帧改用 `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` 定时器 |
| 复杂画面突发大帧撞穿 64 分片预算 | `CODECAPI_AVEncCommonBufferSize` 按 bit 计算（`bitrate/4`），实际单位是字节，VBV 窗口被放宽 8 倍 | 改为 `bitrate/32`（≈250 ms）；GOP 从 2 s 放宽到 30 s 安全网，依靠按需 IDR |
| 120 Hz 下显存分配抖动 | 每帧 `CreateTexture2D` + `CreateVideoProcessorOutputView` 约 5–6 MB | 预建 6 块 NV12 纹理 + 输出视图环形池，按 FIFO 跟踪在途槽位，仅保留最新一帧供静态画面重发 |

同批修复的边界问题：丢 P 帧后立即置 `drop_until_keyframe_` 并请求 IDR（不再继续发送残缺 GOP）；MFT 静默降帧（120→90→60→30）会在日志与诊断中显式提示。

### Windows Host 异步化后的断链与残余卡顿（2026-11-26 第二轮）

第一轮异步化上线后出现「跑几秒突然断链 + 仍然卡顿」，定位到 WinUSB 端点语义与唤醒/资源池竞态，均已修复：

| 问题现象 | 根因定位 | 已落地的修复方案 |
| --- | --- | --- |
| 突发断链、`bulkErr` 增长 | 异步 IN 超时 `write_timeout_ms_*4`（120 Hz 仅 100 ms）、OUT 超时 `3000/fps`（25 ms）；WinUSB 上任一 URB 超时都会 `WinUsb_AbortPipe()` 中止整条管道 | Bulk IN 超时固定 `0`（仅 `cancel()` 取消）；Bulk OUT 固定 `2000 ms`，`set_stream_fps()` 不再按帧率缩放 |
| 断链后读通道永久变少 | 回调内同步调用 `libusb_clear_halt()`（EP0 控制传输重入 + 重置 DATA0/DATA1）；`CANCELLED` 分支不重新挂载 | 删除回调内两处 `libusb_clear_halt()`；STALL/ERROR 干净失败重连；非 `stop()` 的 `CANCELLED` 重新 `submit_in_transfer()` |
| 大关键帧直接杀会话 | `write()` 对超过 16384 字节的缓冲区返回 `Error`，而 `ByteIo` 支持部分写 | 截断为 `min(size, 16384)` 提交并返回 `{Ok, chunk}`，由 core 继续写剩余部分 |
| 槽位释放后发送线程仍白睡 1–2 ms | `consume_write_blocked()` 先清标志，`handle_out_complete()` 末尾再读永远是 `false` | 新增独立 `out_wake_needed_`，只由 OUT 完成回调 `exchange(false)` 消费 |
| 偶发花屏/编码停顿 | NV12 池在 `drain_output()` 之前回收槽位，且优先抢占 `cached_slot_`；1:1 `ProcessInput`/`HaveOutput` 假设在 MFT 内部跳帧时漏光槽位 | 改为 COM 引用计数 + 在途集合双重判定，轮转查找空闲槽；`drain_output()` 先于回收；槽位连续 1 s 不可用才强制复用并在 `enc` 中显示 `poolReuse=` |
| 高刷抖动、偶发跳帧 | `DXGI_ERROR_WAIT_TIMEOUT` 分支在 `AcquireNextFrame` 之后又 `wait_until_us()`，睡过刚落地的帧 | 删除该分支内的等待 |
| 极端时序下 MFT 事件丢失 | `EventBridge` 只存单个 `IMFAsyncResult*`，新回调覆盖未处理结果 | 改为 `std::deque<IMFAsyncResult*>`；空闲等待改 50 ms 看门狗，避免 `BeginGetEvent` 失败后永久挂起 |
| 120 Hz 首挂仍落到 60 Hz | Parsec VDD 的 `HKLM\SOFTWARE\Parsec\vdd\0..4` 是全局模式槽位，驱动只在加载与 `VDD_IOCTL_ADD` 时读取 | 自定义模式改回在 `VDD_IOCTL_ADD` **之前**写 `...\vdd\0`，失败时日志提示管理员权限 |
| UI 每 250 ms 卡顿 | `snapshot_ui_state()` 每次都 `probe_status()`，枚举显示器并开关 VDD 设备句柄 | `probe_status()` 结果缓存 30 s，服务启停/重试/显示变化/虚拟屏增删时失效 |

---

## 3. 正式硬件门禁探针状态（`probes/`）

仓库中的 [`probes/`](../probes) 目录保留了早期的独立验证探针与原始记录，各正式门禁（Gate 0–4A）在独立基准报告口径下的状态如下（产品端端到端链路已在日常实机中可用，但不替代下述独立形式化门禁）：

| 门禁 | 关注范围 | 探针与记录状态 |
| --- | --- | --- |
| **Gate 0** | HiDPI 虚拟屏创建、锁屏/睡眠清理、Developer ID 签名与公证 | 探针 `make test` 通过；早期 macOS 私有 API 2x 校验曾记录于 `probes/gate0/records/`；Developer ID 公证分发尚未执行 |
| **Gate 1** | USB AOA 直连拓扑（非 Hub）、持续双向吞吐、拔线重连与后台冻结注入 | `probes/gate1` 完成真机 AOA 重枚举、双向 Bulk 与 `eventfd` 唤醒实测；因实测经 USB 2.1 Hub，正式基准结论暂记为 `NOT EVALUATED` |
| **Gate 2A / 2B** | USB Network Fallback（NCM/ECM）备选通道 | Gate 2A 探针校验通过；AOA 主路线已跑通，Plan B（Gate 2B）标记为 `wontfix` |
| **Gate 3** | `AVCodec` 低延迟、Flush/Reset、Last Good Frame 独立基准探针 | 探针已完成 640×360 解码与冻结恢复记录；Annex-B 硬解已在产品端 `apps/harmonyos-client` 跑通 |
| **Gate 4A** | 跨端四时间戳与 240fps+ 高速摄影光学延迟基线 | 尚未执行正式高速摄影光学打点采集，目前暂不宣称光学延迟数值达标 |
