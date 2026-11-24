# Gate 0 records

这些 JSON 是本地环境快照，不是 Gate 0 通过证明。

- `local-pending.json`：dry-run 报告，`decision = pending`。
- `local-distribution-pending.json`：Developer ID/公证/Gatekeeper preflight，`status = pending`。
- `local-display-inventory.json`：只读显示清单；受限 agent 环境中可能为 `unavailable`。

在 Reference Host 上完成 Screen Recording、live 显示/采集/编码、锁屏/睡眠重建和 Developer ID 公证后，应新增独立的 live、lifecycle 与 distribution 记录。不要把多个部分记录的“已执行”拼成没有真实 `passed` 证据的 Gate 0 结论。
