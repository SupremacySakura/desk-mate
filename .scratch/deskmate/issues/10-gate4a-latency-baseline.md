# 10: Gate 4A：端到端延迟测量基线

**What to build：** 维护者通过真实 Wired Transport 展示 Host Extended Display 的同步测试图案，产出可复现的校时、软件分段耗时和相对光子延迟基线，而不提前宣称性能达标。

**Blocked by：** 02: 首次握手到可显示状态；03: 暂停、恢复、重配与生命周期；04: 优先发送与 GOP 恢复；05: Gate 0：Host 虚拟屏、采集编码与公证；07: Gate 3：AVCodec Surface 恢复；以及 06: Gate 1：AOA 有线双向通道，或 09: Gate 2B：Plan B 安全通道实现

**Status:** needs-info

- [x] 真实 Session 经过优先调度、分片传输、Host 实时编码和 Device Surface 解码完成端到端亮屏。
- [ ] 记录完整 Reference Environment、Reference Link、60Hz 模式、码率、分片和调度参数。
- [ ] 四时间戳校时和分阶段打点可落盘，报告偏移、RTT、漂移、误差上界和 Software Pipeline Latency 分布。
- [ ] 每种光学负载至少采集 100 个非同步伪随机间隔的有效样本，报告 P50/P95/P99、样本数和误差。
- [ ] 光子结果按加误差上界通过、减误差下界超标、中间不确定的三态规则报告；不确定时升级测量方法。
- [ ] 本任务只建立 Gate 4A 基线，不执行 15ms/40ms V1 性能退出门禁。
- [ ] Gate 0–3 通过且两端互操作收口后，协议才可转为 accepted；本任务完成本身不替代任何硬件门禁。
- [ ] 后续另行拆分 V1 Reverse Input 注入及 15ms/40ms 性能调优，不在本批次隐含交付。

## Comments

- 2026-11-24：blocked。端到端亮屏依赖 Gate 0（未执行 live 采集/编码）、Gate 1
  （拓扑非基准，`NOT EVALUATED`）与 Gate 3 的 Annex-B Host 互操作（等待 05）。
  在这些门禁收口前不做延迟基线测量，避免把非基准链路数据当成性能结论。
- 2026-11-24（更新）：端到端亮屏这一项已实测通过——真实 Session 经优先调度、
  12 KiB 分片、Host 实时编码、设备 Surface 解码后平板出画面（HEVC 与 H.264
  均验证），据此勾选第一项。
  - 其余各项仍未完成：链路是 USB2.1 Hub 而非 Reference Link，因此没有 60Hz
    基准模式、没有四时间戳校时落盘、没有光学采样，也没有任何延迟数值。
  - 状态保持 `needs-info`。**本任务不产出性能结论**；已知的帧率偏低（约 9 fps）
    属于待优化项，不是 Gate 4A 基线。
