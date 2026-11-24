# 07: Gate 3：AVCodec Surface 恢复

**What to build：** Device 最小探针可将 Annex-B 测试码流低延迟解码至 Surface，并在 Flush、Reset 或重建后安全恢复；维护者获得 Last Good Frame 和异步回调行为的真实证据。

**Blocked by：** None (can start immediately with test vectors; Host interoperability confirmation uses the sample produced by 05)

**Status:** ready-for-agent

- [ ] 记录 Device/HarmonyOS Build、支持的实际编解码器、分辨率和低延迟配置。
- [ ] Surface 输出无需 CPU 像素读取，Surface Lease 的引用操作串行化且不在销毁后访问旧窗口。
- [ ] 实测 Flush、Reset、销毁重建的状态、耗时、参数集重发、Surface 保留及残留回调行为。
- [ ] 所有阻塞解码器清理均来自非回调控制线程，旧 Recovery Epoch 输出被释放而不渲染。
- [ ] 实测恢复时 Last Good Frame 是否稳定保留，不承诺未经验证的无损重配或回退。
- [ ] 取得 05 的真实 Host 样本后，补做 Annex-B、参数集和色彩元数据互操作确认。
