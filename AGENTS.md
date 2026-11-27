# Repository Agent Guide

DeskMate 是 macOS / Windows Host 与 HarmonyOS NEXT / Android Client 之间的多设备有线扩展显示与反向触控项目。

## Key Documentation

- 项目总览与使用指南：[README.md](./README.md)（简体中文）· [README.en.md](./README.en.md)（English）
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
- Windows Host 打包（局域网远程构建）：`bash scripts/build-windows-remote.sh [--verify]`

## Remote Windows Build（Windows 端必须远程编译）

`DeskMateHost.exe` 只能在 Windows 上编译（`build-portable.ps1` 依赖 llvm-mingw 的 Windows SDK 头与 UCRT，macOS 无法交叉产出可用二进制）。仓库所在的 Mac 通过局域网 SSH 使用专用构建机，**无需用户再提供地址或密码**：

| 项 | 值 |
| --- | --- |
| 地址 | `192.168.43.4:22`，用户 `Administrator`，已配置公钥免密 |
| 主机名 / 系统 | `PC-20261122RWRQ` / Windows `10.0.26300`（PowerShell 7.7） |
| 工具链 | `C:\dm\llvm-mingw-20261117-ucrt-x86_64`（clang 23.1.6） |
| libusb 源码 | `C:\dm\libusb-1.0.30` |
| 解包目录 / 中间产物 | `C:\dm\src`（每次全删重建）/ `C:\dm\buildwork`（**必须在源码树外**） |
| 产物校验脚本 | `C:\Users\Administrator\verify-win.ps1` |

一条命令即可完成「打包源码 → 上传 → 远程全量编译 → 取回 exe → 重打 zip → 刷新 sha256」：

```bash
bash scripts/build-windows-remote.sh --verify
```

完整流程、SSH 调用规范（单引号包裹、`cmd /c`、中文乱码判读）、故障恢复与硬约束见 [docs/windows-remote-build.md](./docs/windows-remote-build.md)。改动 `apps/windows-host` 或 `core/` 后若需要 Windows 产物，直接跑上述命令，不要尝试在 macOS 上交叉编译。

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
- **禁止重复安装说明**：严禁在每次 Release 描述中重复粘贴软件安装、WinUSB / Zadig 驱动、Parsec VDD 虚拟屏、macOS 签名/权限或通用排障教程；所有安装注意事项统一维护在 [README.md](./README.md) / [README.en.md](./README.en.md)，Release 正文末尾仅保留一行指引链接即可。
- **维护流程**：发布新版本前先更新 [CHANGELOG.md](./CHANGELOG.md)，GitHub Release 正文直接复用对应版本的 `CHANGELOG.md` 段落。

## Conventions

- 修改 `core/` 线缆结构或控制 opcode 时，须同步检查 `core/tests/` 与 `testdata/golden/`。
- 不要提交 `apps/harmonyos-client/build-profile.json5` 中的个人签名配置（`signingConfigs`）或构建产物（`build/`、`.hvigor/`、`dist/`）。
