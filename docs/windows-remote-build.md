# Windows Host 局域网远程编译指南

Windows 产物必须在 Windows 上编译（`build-portable.ps1` 依赖 `llvm-mingw` 的 Windows SDK 头文件与 UCRT，macOS 上无法交叉产出可用的 `DeskMateHost.exe`）。仓库所在的 Mac 通过局域网 SSH 驱动一台专用 Windows 构建机完成编译，本文记录该环境的全部事实与可复刻步骤。

> 一键脚本：`bash scripts/build-windows-remote.sh`（追加 `--verify` 会远程跑启动自检）。

---

## 1. 构建机信息（2026-11-27 实测）

| 项 | 值 |
| --- | --- |
| 局域网地址 | `192.168.43.4`，SSH 端口 `22` |
| 登录用户 | `Administrator` |
| 主机名 | `PC-20261122RWRQ` |
| 操作系统 | Windows `10.0.26300.9457` |
| 默认 Shell | PowerShell `7.7.0`（`cmd.exe` 亦可用） |
| 认证方式 | **已配置公钥免密**，`ssh -o BatchMode=yes` 可直接登录 |
| 仓库所在 Mac | 局域网 `192.168.43.5` |

**不需要密码。** 若某次提示要密码，说明构建机的 `administrators_authorized_keys` 丢了公钥，按第 6 节恢复。

### 远程工具链（路径固定，勿随意改动）

| 用途 | 路径 / 版本 |
| --- | --- |
| C/C++ 编译器 | `C:\dm\llvm-mingw-20261117-ucrt-x86_64\bin\clang.exe`（clang 23.1.6） |
| C++ 编译器 | `C:\dm\llvm-mingw-20261117-ucrt-x86_64\bin\clang++.exe` |
| 静态库工具 | `C:\dm\llvm-mingw-20261117-ucrt-x86_64\bin\llvm-ar.exe` |
| 资源编译 | `C:\dm\llvm-mingw-20261117-ucrt-x86_64\bin\llvm-windres.exe`（LLVM 23.1.6） |
| libusb 源码树 | `C:\dm\libusb-1.0.30\libusb\core.c` |
| 解包源码目录 | `C:\dm\src\`（每次全删重建） |
| 编译中间产物 | `C:\dm\buildwork\`（**必须在源码树外**） |
| 上传的源码包 | `C:\Users\Administrator\dm-src.zip` |
| 产物校验脚本 | `C:\Users\Administrator\verify-win.ps1` |

远程已装 `bsdtar 3.8.8`，可直接 `tar -xf` 解 zip。

---

## 2. 每次重建的完整流程

在仓库根目录执行（macOS 侧）：

### ① 打源码包

只打包 Windows 真正需要的目录，避免把 `build/`、`dist/`、`.git/` 送上网络：

```bash
cd /Users/shi/study/frontend/projects/desk-mate
rm -f /tmp/dm-src.zip
zip -q -r /tmp/dm-src.zip core apps/common apps/windows-host CMakeLists.txt \
  -x '*/build/*' -x '*.o'
```

### ② 上传

```bash
scp -o StrictHostKeyChecking=no /tmp/dm-src.zip \
  Administrator@192.168.43.4:C:/Users/Administrator/dm-src.zip
```

### ③ 解包 + 编译（一条 SSH 命令）

```bash
ssh -o StrictHostKeyChecking=no Administrator@192.168.43.4 \
  'cmd /c "rmdir /s /q C:\dm\src 2>nul & mkdir C:\dm\src & tar -xf C:\Users\Administrator\dm-src.zip -C C:\dm\src" && powershell -NoProfile -ExecutionPolicy Bypass -File C:\dm\src\apps\windows-host\build-portable.ps1 -ToolchainDir C:\dm\llvm-mingw-20261117-ucrt-x86_64 -LibusbSource C:\dm\libusb-1.0.30 -WorkDir C:\dm\buildwork 2>&1 | Select-Object -Last 14'
```

成功标志（最后一行）：

```
DeskMateHost.exe -> C:\dm\src\dist\windows\DeskMateHost.exe (3.0x MB)
```

耗时约 30–60 秒。

### ④ 取回产物

```bash
scp -o StrictHostKeyChecking=no \
  Administrator@192.168.43.4:C:/dm/src/dist/windows/DeskMateHost.exe \
  /Users/shi/study/frontend/projects/desk-mate/dist/windows/DeskMateHost.exe
chmod 755 /Users/shi/study/frontend/projects/desk-mate/dist/windows/DeskMateHost.exe
```

### ⑤ 重新打 zip + 刷新校验和

```bash
cd /Users/shi/study/frontend/projects/desk-mate/dist/windows
rm -f DeskMate-v<ver>-windows-x64.zip
zip -q -9 DeskMate-v<ver>-windows-x64.zip DeskMateHost.exe README.txt
shasum -a 256 DeskMateHost.exe > /tmp/win.sha
shasum -a 256 DeskMate-v<ver>-windows-x64.zip >> /tmp/win.sha
```

`<ver>` 取自 `apps/windows-host/include/deskmate/windows/updater.hpp` 的 `kWindowsHostVersion`（当前 `1.5.0`）。归档完成后记得刷新 `dist/release/SHA256SUMS`。

---

## 3. 产物验证（强烈建议）

```bash
ssh -o StrictHostKeyChecking=no Administrator@192.168.43.4 \
  'powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\Administrator\verify-win.ps1'
```

期望输出：

```
size_bytes=<与本地 zip 内 exe 一致>
sha256=<与本地 exe 一致>
file_version=1.5.0.0
product_version=1.5.0.0
description=DeskMate Windows Host
pe_machine=0x8664
pe_subsystem=2
pe_opt_magic=0x020B
launch=running_after_8s
```

`launch=EXITED` 表示程序启动即退出，**必须停下来排查**，不要继续打包发布。

---

## 4. SSH 调用规范（避免踩坑）

1. **整条远程命令用单引号包住**。否则本地 zsh 会把 `&&`、`|` 拆成多段，既不原子也会触发多次审批。
2. **远程优先用 `cmd /c "..."` 包一层**：`&&`、`2>nul`、`&` 在 cmd 下语义明确；PowerShell 7 虽然也支持 `&&`，但重定向与引号规则不同，容易出意外。
3. **只要结果不要噪音**，末尾接 `| Select-Object -Last N`。
4. **中文输出在 SSH 回传时是乱码**（GBK/UTF-8 不一致）。判断成功不要 grep 中文，认英文标记，例如 `DeskMateHost.exe ->`、`BUILD SUCCESSFUL`。
5. 构建耗时较长时，本地用 `yield_time_ms: 30000` 启动再轮询，别一次等太久。
6. 在 Codex 沙箱里，`ssh` / `scp` 需要 `sandbox_permissions: "require_escalated"`（规则 `["ssh"]` / `["scp"]`）。

---

## 5. 两条硬约束

- **`-WorkDir` 必须写 `C:\dm\buildwork`**（源码树外）。漏掉的话 `.o` 会生成到 `apps/windows-host/build/`，下次打包源码时被一起上传，包体积和编译时间都会失控。
- **远程 `C:\dm\src` 每次 `rmdir /s /q` 全删重建**，不要试图增量编译。zip 覆盖后文件时间戳会错乱，容易链到旧对象，产生难以解释的链接错误。

---

## 6. 故障恢复

### 提示要密码 / 公钥失效

把本机公钥重新追加到构建机的管理员授权文件：

```bash
ssh-copy-id -i ~/.ssh/id_ed25519.pub Administrator@192.168.43.4
```

若 `ssh-copy-id` 不可用，手动把 `~/.ssh/id_ed25519.pub` 的内容追加到：

```
C:\ProgramData\ssh\administrators_authorized_keys
```

并确保该文件仅 `Administrators` 与 `SYSTEM` 可读写。

### 远程环境是否完好

```bash
ssh Administrator@192.168.43.4 \
  'cmd /c "C:\dm\llvm-mingw-20261117-ucrt-x86_64\bin\clang.exe --version & dir C:\dm\libusb-1.0.30\libusb\core.c"'
```

### 本地是否真的需要重编

没输出就不用重建：

```bash
cd /Users/shi/study/frontend/projects/desk-mate
find apps/windows-host core apps/common -newer dist/windows/DeskMateHost.exe \
  \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.rc' \) -print
```

### 编译报错找不到头文件 / 链接失败

先确认是否漏了 `-WorkDir`、或没有全删 `C:\dm\src`；再确认 `C:\dm\libusb-1.0.30\libusb\core.c` 与工具链路径存在（见第 6 节检查命令）。

---

## 7. 与其他平台打包的关系

Windows 只是四端之一。完整打包顺序与产物命名见根目录 [AGENTS.md](../AGENTS.md) 的「Local Build & Packaging Output Convention」；本机 macOS / Android / HarmonyOS 的构建命令见 [AGENTS.md](../AGENTS.md) 的「Build & Test Commands」与 [apps/windows-host/README.md](../apps/windows-host/README.md)。
