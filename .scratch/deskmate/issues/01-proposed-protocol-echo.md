# 01: 候选协议端到端回声

**What to build：** 维护者可通过字节 I/O 在两端交换 DeskMate 候选协议消息，包括控制消息回声与测试视频载荷的分片重组；接收端在分配前拒绝非法或不受支持的输入，并以 golden vectors 证明互操作。

**Blocked by：** None (can start immediately)

**Status:** done

- [x] 两端使用显式序列化交换带 DM 魔数、proto_rev 和会话/视频代际的 32 字节候选协议消息。
- [x] 测试视频载荷完成分片、乱序/不连续检测与完整重组；控制、输入和校时消息拒绝分片。
- [x] 拆包、粘包、非法长度、零分片、超上限分片和异常载荷在任何大分配前得到确定性处理。
- [x] 音频等不支持类型返回明确错误并丢弃，不使健康 Session 断开。
- [x] 提供覆盖合法和非法消息的跨端 golden vectors；测试只观察协议外部行为。

## Comments

- 实现完成。协议编解码、分片重组、非法输入拒绝、golden vectors 均由 `core/tests/protocol_tests.cpp` 覆盖，`make test` 全绿。
- 2026-11-24：全部验收项勾选，代码已合入 `main`（fast-forward 至 `6a9937e`）；`make test` 全绿。
