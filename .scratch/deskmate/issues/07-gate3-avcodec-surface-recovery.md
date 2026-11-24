# 07: Gate 3：AVCodec Surface 恢复

**What to build：** Device 最小探针可将 Annex-B 测试码流低延迟解码至 Surface，并在 Flush、Reset 或重建后安全恢复；维护者获得 Last Good Frame 和异步回调行为的真实证据。

**Blocked by：** None (can start immediately with test vectors; Host interoperability confirmation uses the sample produced by 05)

**Status:** needs-info

- [x] 记录 Device/HarmonyOS Build、支持的实际编解码器、分辨率和低延迟配置。
- [x] Surface 输出无需 CPU 像素读取，Surface Lease 的引用操作串行化且不在销毁后访问旧窗口。
- [x] 实测 Flush、Reset、销毁重建的状态、耗时、参数集重发、Surface 保留及残留回调行为。
- [x] 所有阻塞解码器清理均来自非回调控制线程，旧 Recovery Epoch 输出被释放而不渲染。
- [x] 实测恢复时 Last Good Frame 是否稳定保留，不承诺未经验证的无损重配或回退。
- [ ] 取得 05 的真实 Host 样本后，补做 Annex-B、参数集和色彩元数据互操作确认。

## Comments

- 2026-11-24 真机实测（MatePad Pro MRDI-W00 / OpenHarmony 7.0.0.111 / API 26）：
  - 640x360 彩条经 `OH_VideoDecoder` + Surface 输出成功渲染；恢复套件 `suite.state=complete`。
  - Flush/Reset/销毁重建、Surface Lease 串行化、Last Good Frame 冻结与恢复均通过；
    `no_cpu_pixel_readback` 成立（仅 `RenderOutputBuffer`）。
  - 平台在 flush 期间报告一次 `AVCODEC_JANK_FAULT` 69.58 ms，无崩溃、无 fault-log。
  - 限制：Surface 保留只证明 lease 有效与 `SetSurface` 成功，不证明合成器扫描输出；
    低延迟 configure key 被接受，但不证明解码调度延迟；外部截图是人工视觉证据。
  - 未完成：Annex-B Host 样本互操作，等待 issue 05 产出真实样本。
- 结论：设备侧功能通过；因缺少 Host 样本互操作与 Reference Link，Gate 3 整体仍待收口。
