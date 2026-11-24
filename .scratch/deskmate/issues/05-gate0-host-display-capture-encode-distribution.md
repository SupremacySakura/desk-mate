# 05: Gate 0：Host 虚拟屏、采集编码与公证

**What to build：** 在基准 Mac 上，用户可创建 HiDPI Extended Display 并持续采集、实时编码为 Device 可消费的 Annex-B 码流；维护者获得锁屏/睡眠清理和独立签名分发的真实可行性报告。

**Blocked by：** None (can start immediately; requires Reference Mac)

**Status:** ready-for-agent

- [ ] 记录 Mac 型号、macOS Build 和私有虚拟显示器 API 的实际可用性。
- [ ] 创建原生分辨率、HiDPI Extended Display，并持续采集；锁屏/睡眠后的销毁重建不残留幽灵副屏。
- [ ] 最小 VideoToolbox 探针以实时模式、无 B 帧生成 HEVC 和 H.264 Annex-B 样本，包含所需参数集及色彩元数据。
- [ ] 将真实码流样本和元数据提供给 Gate 3，不等到端到端集成才发现互操作问题。
- [ ] 采集显式关闭音频；记录 Developer ID 签名、公证及 Gatekeeper 启动结果。
- [ ] 报告明确通过或失败；失败时不以主屏镜像替代。

## Comments

- 2026-11-24：Gate 0 探针已实现（`probes/gate0`，`make test`/`make dry-run` 通过），
  但真机项全部未执行。`records/local-pending.json` 明确 `decision=pending`：
  - 缺少 Screen Recording 权限；
  - 缺少 Developer ID Application 身份，签名/公证未执行；
  - 受限 agent 进程中 `CGGetOnlineDisplayList` 返回空集合，无法创建/验证
    Extended Display 或做 live 采集；
  - 未产生真实 HEVC/H.264 Annex-B 样本。
- 因此 issue 07 的 Annex-B Host 互操作仍被本任务阻塞。
- 需要人在已登录图形桌面的 Reference Host 上授予录屏权限并运行 live 流程。
