# Repository Agent Guide

DeskMate 是 macOS / Windows Host 与 HarmonyOS NEXT / Android Client 之间的多设备有线扩展显示与反向触控项目。

## Key Documentation

- 项目总览与使用指南：[README.md](./README.md) · [README.zh-CN.md](./README.zh-CN.md)
- 工程架构与双端约束：[docs/architecture.md](./docs/architecture.md)
- 线缆协议与 Session API 规范：[docs/protocol.md](./docs/protocol.md)
- 测试与真机排障记录：[docs/verification-notes.md](./docs/verification-notes.md)
- 领域术语表：[GLOSSARY.md](./GLOSSARY.md)
- 架构决策记录：[docs/adr/](./docs/adr/)

## Build & Test Commands

- 共享 C++20 核心单元测试：`make test`
- 跨平台通用层、Windows 契约与 Android fd 测试：`bash apps/common/tests/run_platform_tests.sh`
- macOS Host 编译与契约/多链路测试：`make -C apps/macos-host test`
- Android 配件 fd 契约测试：`bash apps/common/tests/run_android_io_test.sh`

## Local Build & Packaging Output Convention（本地打包产物目录规范）

**严禁将任何本地编译或打包产物放在仓库根目录（例如不要在根目录生成 `.apk`、`.zip`、`.dmg`、`.hap` 或 `.exe`）！** 所有本地编译后的最终安装包须统一输出到 `dist/<platform>/` 下（该目录已在 `.gitignore` 中忽略），中间编译缓存留在各端 `build/` 目录：

| 平台 | 最终安装包输出目录 | 产物文件名约定 | 中间构建目录 |
| --- | --- | --- | --- |
| **Windows Host** | `dist/windows/` | `DeskMateHost.exe`、`DeskMate-v<ver>-windows-x64.zip` | `build/` 或 `apps/windows-host/build/` |
| **macOS Host** | `dist/macos/` | `DeskMate.app`、`DeskMate-v<ver>-macos-<arch>.dmg`、`DeskMate-v<ver>-macos-<arch>.zip` | `apps/macos-host/build/` |
| **Android Client** | `dist/android/` | `DeskMate-v<ver>-android-debug.apk` / `DeskMate-v<ver>-android.apk` | `apps/android-client/app/build/` |
| **HarmonyOS Client** | `dist/harmonyos/` | `DeskMate-v<ver>-harmonyos-signed.hap` / `DeskMate-v<ver>-harmonyos-unsigned.hap` | `apps/harmonyos-client/entry/build/` |
| **Release 汇总（可选）** | `dist/release/` | `update-manifest.json`、`*.sha256` | — |

说明：
- `make -C apps/macos-host dmg`、`bash apps/android-client/package_apk.sh` 与 `apps/windows-host/build-portable.ps1` 已默认同步输出到上述 `dist/<platform>/` 目录。
- 在 HarmonyOS（DevEco Studio / `hvigorw`）本地构建 `.hap` 后，如需归档交付包，请将产物拷贝至 `dist/harmonyos/`，切勿留在根目录。

## Release Notes 编写规范（发布说明规范）

- **以简体中文为主**：Release 标题、分类与条目统一采用简洁清晰的中文。
- **仅写与上个版本的变更差异**：每个版本只记录相对上一版本的**新增功能**、**性能与体验优化**、**问题修复**与**破坏性变更/迁移说明**，写清改动内容与解决的问题。
- **禁止重复安装说明**：严禁在每次 Release 描述中重复粘贴软件安装、WinUSB / Zadig 驱动、Parsec VDD 虚拟屏、macOS 签名/权限或通用排障教程；所有安装注意事项统一维护在 [README.zh-CN.md](./README.zh-CN.md) / [README.md](./README.md)，Release 正文末尾仅保留一行指引链接即可。
- **维护流程**：发布新版本前先更新 [CHANGELOG.md](./CHANGELOG.md)，GitHub Release 正文直接复用对应版本的 `CHANGELOG.md` 段落。

## Conventions

- 修改 `core/` 线缆结构或控制 opcode 时，须同步检查 `core/tests/` 与 `testdata/golden/`。
- 不要提交 `apps/harmonyos-client/build-profile.json5` 中的个人签名配置（`signingConfigs`）或构建产物（`build/`、`.hvigor/`、`dist/`）。
