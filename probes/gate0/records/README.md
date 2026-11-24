# Gate 0 records

这些 JSON 是本地环境快照，不是 Gate 0 通过证明。

- `local-pending.json`：dry-run 报告，`decision = pending`。
- `local-distribution-pending.json`：Developer ID/公证/Gatekeeper preflight，`status = pending`。
- `local-display-inventory.json`：只读显示清单；受限 agent 环境中可能为 `unavailable`。
- `local-live-2026-11-24.json`：Reference Host 上的第一次 live 尝试，`decision = failed`。
  虚拟屏创建成功、销毁成功（无幽灵副屏），但 macOS 27.3 只发布 1x 模式，
  所以 HiDPI 校验失败，采集与编码从未启动。这是失败证据，不是通过证据。

在 Reference Host 上完成 Screen Recording、live 显示/采集/编码、锁屏/睡眠重建和 Developer ID 公证后，应新增独立的 live、lifecycle 与 distribution 记录。不要把多个部分记录的“已执行”拼成没有真实 `passed` 证据的 Gate 0 结论。

## 2026-11-24 实测：macOS 27.3 不再发布 2x 虚拟屏模式

在 `Mac16,12` / macOS `27.3` (`26C5047e`) 上，`CGVirtualDisplaySettings.hiDPI = 1`
返回成功、`CGVirtualDisplay.hiDPI` 读回 `2`，但
`CGDisplayCopyAllDisplayModes` 只给出点/像素相等的模式，`NSScreen.backingScaleFactor`
为 `1.0`。已试过的组合（`maxPixelsWide` 1400/2800/5600、请求 1400x920 或
2800x1840、`hiDPI` 取 1 或 2、`isReference` 开或关）都没有产生
`1400x920 pt / 2800x1840 px`。`isReference = YES` 时虚拟屏甚至不会上线。

结论：本机无法用 `CGVirtualDisplay` 建立 HiDPI Extended Display。Gate 0 因此保持
`failed`（不是 `pending`），需要在仍支持该私有 API HiDPI 模式的 macOS 版本上重跑，
或改用其他已获许可的虚拟显示器实现。

顺带修正：`CGDisplayIsOnline` 对已销毁的显示 ID 返回 `-1`（非零即为真），旧探针
因此把每次正常销毁都报成「幽灵副屏」。清理判定已改为查询在线显示列表。
