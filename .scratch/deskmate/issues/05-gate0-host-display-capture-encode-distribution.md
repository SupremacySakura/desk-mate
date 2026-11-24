# 05: Gate 0：Host 虚拟屏、采集编码与公证

**What to build：** 在基准 Mac 上，用户可创建 HiDPI Extended Display 并持续采集、实时编码为 Device 可消费的 Annex-B 码流；维护者获得锁屏/睡眠清理和独立签名分发的真实可行性报告。

**Blocked by：** None (can start immediately; requires Reference Mac)

**Status:** needs-info

- [x] 记录 Mac 型号、macOS Build 和私有虚拟显示器 API 的实际可用性。
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

- 2026-11-24（第二次，已在真实登录桌面执行 live）：**Gate 0 `failed`，不是 pending。**
  - 环境：`Mac16,12` / macOS `27.3` (`26C5047e`) / Xcode `27.2` (`27B52`)。
  - Screen Recording 预检通过，`CGVirtualDisplay` 可创建、可销毁，销毁后无幽灵副屏。
  - **阻塞点**：该 macOS 版本不再为 `CGVirtualDisplay` 发布 2x 模式。
    `hiDPI = 1` 返回成功、`CGVirtualDisplay.hiDPI` 读回 `2`，但
    `CGDisplayCopyAllDisplayModes` 只有点/像素相等的模式，`NSScreen.backingScaleFactor = 1.0`。
    已试 `maxPixelsWide` 1400/2800/5600 × 请求 1400x920 或 2800x1840 ×
    `hiDPI` 1/2 × `isReference` 开/关，均未产生 `1400x920 pt / 2800x1840 px`；
    `isReference = YES` 时虚拟屏直接不上线。
  - 因此采集与编码从未启动，无 Annex-B 样本，issue 07 的互操作仍阻塞。
  - 证据：`probes/gate0/records/local-live-2026-11-24.json`、`records/README.md`。
  - 附带修复：旧清理判定用 `CGDisplayIsOnline`，它对已销毁 ID 返回 `-1`（真值），
    把每次正常销毁都误报成幽灵屏；已改为查询在线显示列表。
  - **需要决策**：改用仍支持该私有 API HiDPI 的 macOS 版本，或改用其他已获许可的
    虚拟显示器实现。这不是继续写代码能解决的问题。

- 2026-11-24（第三次，attach 外部 HiDPI 显示）：**显示/采集/编码首次全部通过，
  但 Gate 0 结论仍为 `pending`。**
  - 新增 `--attach-display ID`：借用第三方创建的显示，探针**绝不创建、绝不销毁**
    它；运行后检查该 ID 仍在线。`destroy()` 对 attached 显示只做在线校验。
  - 目标显示由 BetterDisplay 4.0.4 提供：id 41，`1920x1280 pt / 3840x2560 px @
    120 Hz`，非主屏、非镜像，是真正的 2x HiDPI。这证明「macOS 27.3 的
    `CGVirtualDisplay` 只有 1x」是**该私有 API 的限制**，不是系统缺少 HiDPI 能力。
  - 实测（`records/local-live-attached-2026-11-24.json`）：
    - `live-display-and-capture: passed`，`capture: 3840x2560`，97 帧，`started`；
    - HEVC 97 帧 / 2 关键帧 / VPS+SPS+PPS / 硬件编码 / 无 B 帧 / 709 色彩；
    - H.264 97 帧 / 2 关键帧 / SPS+PPS / 硬件编码 / 无 B 帧 / 709 色彩；
    - `stillOnlineAfterRun: true`，探针没有销毁外部显示。
  - 仍未通过：`developer-id`、`notarization-and-gatekeeper`、`lock-sleep-recovery`。
  - 因此 `decision` 保持 `pending`：显示由第三方工具创建，探针只证明「采集+编码+
    不破坏外部显示」这一半；HiDPI 显示本身的**产品化创建**仍未解决。
  - 真实 Annex-B 样本已产出（HEVC 3.7 MB、H.264 772 KB），可用于 Gate 3 互操作；
    样本本身未入库，只记录路径与 SHA-256。
