# Repository Agent Guide

DeskMate 是 macOS Host 与 HarmonyOS NEXT Device 之间的多设备有线扩展显示与反向触控项目。

## Key Documentation

- 项目总览与使用指南：[README.md](./README.md) · [README.zh-CN.md](./README.zh-CN.md)
- 工程架构与双端约束：[docs/architecture.md](./docs/architecture.md)
- 线缆协议与 Session API 规范：[docs/protocol.md](./docs/protocol.md)
- 测试与真机排障记录：[docs/verification-notes.md](./docs/verification-notes.md)
- 领域术语表：[GLOSSARY.md](./GLOSSARY.md)
- 架构决策记录：[docs/adr/](./docs/adr/)

## Build & Test Commands

- 共享 C++20 核心单元测试：`make test`
- macOS Host 编译与契约/多链路测试：`make -C apps/host test`

## Conventions

- 修改 `core/` 线缆结构或控制 opcode 时，须同步检查 `core/tests/` 与 `testdata/golden/`。
- 不要提交 `apps/device/build-profile.json5` 中的个人签名配置（`signingConfigs`）或构建产物（`build/`、`.hvigor/`）。
