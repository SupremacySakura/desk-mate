# DeskMate Gate 0 独立探针

Status: **failed on 2026-11-24 (macOS 27.3 publishes only 1x virtual-display modes)**

这个目录只实现任务单 05 的 Gate 0 探针，不修改规格、ADR 或其他任务单。它不会把“主屏镜像”当作 Extended Display 的回退路径，也不会在未满足前置条件时声称 Gate 0 通过。

## 已确认的 Reference Environment

- Host: MacBook Air，`Mac16,12`，Apple M4，16 GB，macOS `27.3`，build `26C5047e`，`arm64`
- Device: `MRDI-W00`，API 26，面板原生像素 `2800x1840`

探针不硬编码这些值。它从 `system_profiler`、`sw_vers` 和 `uname` 读取实际值，并且只有在显式传入匹配的 `--reference-model` 与 `--reference-build` 时才把本机标为 Reference Host。即使 Reference Host 匹配，Screen Recording、真实 live 证据、锁屏/睡眠证据与 Developer ID 证据仍必须分别满足。

## 交付内容

- 默认只读的环境报告：Mac 型号、macOS build、架构、工具链、私有 API 静态可用性。
- 可选的 `CGVirtualDisplay` Extended Display 创建与 HiDPI 模式校验；模式不匹配、主屏或镜像状态都会失败。
- 可选的 ScreenCaptureKit 持续视频采集；只绑定实际创建的虚拟显示，不退回主屏。
- 明确关闭音频：`capturesAudio = false`，不注册音频输出，metadata 记录 `audioRequested = false`。
- VideoToolbox HEVC 与 H.264 实时编码；`AllowFrameReordering = false`，输出 Annex-B 与参数集/色彩元数据。
- 锁屏/睡眠观察器与可手工复核的清理步骤。
- Developer ID 签名、Apple 公证、staple 和 Gatekeeper 检查脚本，默认 dry-run。
- `tests/contract.sh`：不创建显示器、不请求 GUI 权限的可执行安全契约。

## 构建与安全检查

```bash
cd probes/gate0
make test
make dry-run
```

`make test` 会构建 CLI、可签名 `.app` 包装，并验证：

- 默认模式是 dry-run；
- live 缺少任一显式 opt-in 时退出；
- 主屏回退参数被拒绝；
- Annex-B 长度前缀转换与非法长度拒绝；
- 只读显示清单不改变显示状态；
- Reference Host 使用实际检测值对照；
- Developer ID 脚本默认不执行签名/公证，并拒绝非 Developer ID 身份；
- 二进制不静态导入私有 `CGVirtualDisplay` 类符号，所需 rotation selector 仍会被探测。
- VideoToolbox callback context 由 encoder 持有并在 finish/deinit 排空后才失效。

## 当前本地记录

在本次 agent 环境生成了以下非通过记录：

- `records/local-pending.json`：完整 dry-run 报告
- `records/local-distribution-pending.json`：签名/公证 preflight
- `records/local-display-inventory.json`：只读显示清单

本次实测环境明确缺少：

- Screen Recording 权限；
- 任何 Developer ID Application 身份；
- WindowServer 显示访问：受限 agent 进程中的 `CGGetOnlineDisplayList` 返回空集合，因此不能完成显示清单或 live 证据；
- `VTCopyVideoEncoderList` 只列出 `24-bit RGB`、`32-bit ARGB`、`JPEG`，没有列出 H.264/HEVC。该列表只是提示，不能替代 live `VTCompressionSession` 证据，也不能据此断言硬件编码器缺失。

因此 `decision` 必须是 `pending`。上述记录不是 Gate 0 通过证据。

## 在 Reference Host 上执行

### 1. 授予 Screen Recording

1. 在已登录图形桌面的 Terminal 中运行探针。
2. 首次可使用显式权限请求：

```bash
./build/Gate0Probe.app/Contents/MacOS/Gate0Probe \
  --live --allow-create-display --allow-screen-capture \
  --request-screen-recording-permission
```

3. 在 System Settings > Privacy & Security > Screen & System Audio Recording 中允许 Terminal（或实际承载该二进制的 app）。
4. 完全退出并重新打开 Terminal，使 TCC 决策生效。
5. 先用以下命令确认权限，而不创建任何显示：

```bash
./build/gate0-probe --dry-run --json \
  --reference-model Mac16,12 --reference-build 26C5047e | \
  jq '{permission: ([.prerequisites[] | select(.id == "screen-recording-permission")][0]), decision}'
```

如果权限仍为 false，停止；不要继续声称 live 测试。

### 2. 创建 HiDPI Extended Display 并采集编码

```bash
mkdir -p artifacts records
./build/Gate0Probe.app/Contents/MacOS/Gate0Probe \
  --live \
  --allow-create-display \
  --allow-screen-capture \
  --logical-width 1400 \
  --logical-height 920 \
  --scale 2 \
  --fps 60 \
  --duration 20 \
  --codec both \
  --output "$PWD/artifacts" \
  --reference-model Mac16,12 \
  --reference-build 26C5047e \
  --json \
  --report "$PWD/records/live-$(date -u +%Y%m%dT%H%M%SZ).json"
```

`1400x920 @ scale 2` 对应 `2800x1840` backing pixels。live 模式强制 `--scale 2`。

通过条件必须同时满足：

- `display.created = true`；
- `display.actualMode` 同时报告 `1400x920 pt` 与 `2800x1840 px`；
- `display.isMainDisplay = false` 且 `display.isMirroring = false`；
- `capture.started = true` 且 `capture.framesReceived > 0`；
- `capture.audioEnabled = false`、`audioRequested = false`、`audioOutputAdded = false`；
- HEVC 与 H.264 artifact 的 `failure = null`；
- `noBFramePropertyVerified = true`、`outputPTSMonotonic = true`；
- 参数集和三项色彩元数据存在；
- `display.destroyed = true`。

只要虚拟显示没有出现在 ScreenCaptureKit 的 display 列表中，探针就失败；它不会使用 `CGMainDisplayID` 代替目标显示。

### 3. 验证样本

每次 live run 在 `--output` 目录生成：

```text
gate0-hevc-<UTC timestamp>.h265
gate0-hevc-<UTC timestamp>.json
gate0-h264-<UTC timestamp>.h264
gate0-h264-<UTC timestamp>.json
```

JSON metadata 包含：

- codec、宽高、像素格式、frame rate；
- 实时编码与 `AllowFrameReordering=false` 的请求和 readback；
- 是否使用硬件编码器的 readback；
- 请求的色彩元数据、format description extensions；
- VPS/SPS/PPS 或 SPS/PPS 的类型、长度和 hex；
- 每帧 PTS、时长、字节数、是否 keyframe。

可做基本检查：

```bash
xxd -l 64 artifacts/gate0-hevc-*.h265
jq '{codec, width, height, allowFrameReorderingReadback, hardwareAccelerationReadback, parameterSets, frames: (.frames|length)}' artifacts/gate0-hevc-*.json
```

Annex-B 文件的起始 NAL 应为 `00 00 00 01`。这些样本应直接提供给 Gate 3，不要在端到端集成时才验证互操作。

## 锁屏与睡眠

探针注册以下观察器：

- DistributedNotificationCenter：`com.apple.screenIsLocked`、`com.apple.screenIsUnlocked`
- NSWorkspace：`willSleepNotification`、`didWakeNotification`、`screensDidSleepNotification`、`screensDidWakeNotification`

收到锁屏或睡眠事件后，探针停止采集、结束编码器并释放虚拟显示，然后退出。它不自动重建；必须按以下步骤手工完成“销毁后重建且无幽灵屏”的 Gate 0 证据。

1. 用 `--duration 300` 启动 live 探针。
2. 在 System Settings > Displays 中记录新 Extended Display。
3. 锁屏（Control-Command-Q），等待探针退出。
4. 检查 report：`lifecycle.eventsObserved` 包含 `screenIsLocked`，`display.destroyed = true`。
5. 解锁后运行：

```bash
./build/gate0-probe --list-displays --json
```

6. 确认没有残留的 `2800x1840` 显示；若 `--list-displays` 返回 `unavailable`，从正常的图形登录会话重试。
7. 再启动一次短 live run，确认出现且只出现一个新的 Extended Display。
8. 重复睡眠/唤醒测试。系统可能在 `willSleep` 到达前挂起进程；唤醒后必须确认探针完成清理，或手工确认显示已经消失。
9. 若进程被强制终止，重新运行只读 display inventory，确认 CoreGraphics 中没有幽灵显示。

## Developer ID 签名、公证与 Gatekeeper

默认 preflight 不改变任何文件：

```bash
make app
./scripts/distribution-check.sh \
  --app "$PWD/build/Gate0Probe.app" \
  --dry-run --json \
  --report "$PWD/records/local-distribution-pending.json"
```

执行签名前，维护者需要：

1. 在钥匙串中安装有效的 `Developer ID Application: ...` 身份；
2. 使用 `xcrun notarytool store-credentials <profile>` 建立公证凭据；
3. 在 Reference Host 上运行显式执行：

```bash
./scripts/distribution-check.sh \
  --app "$PWD/build/Gate0Probe.app" \
  --identity "Developer ID Application: Example (TEAMID)" \
  --notary-profile "<profile>" \
  --execute
```

脚本会执行 hardened-runtime 签名、严格验证、提交公证、staple、staple 验证、生成最终 zip，并运行 `spctl --assess --type execute`。默认拒绝签名非 `dev.deskmate.gate0-probe` bundle；只有显式 `--allow-any-app` 才可绕过。

没有 Developer ID、没有 accepted 公证结果、或 Gatekeeper 拒绝时，分部结果必须保持 `pending` 或 `failed`，不能写成通过。

## 决策语义

| Decision | 含义 |
|---|---|
| `pending` | 未执行、缺权限、缺 Reference Host 证据，或签名/锁屏证据仍缺失 |
| `failed` | 已执行的显示、采集、编码或清理检查失败 |
| `passed` | 所有 Gate 0 前置条件均在 Reference Host 上留下真机证据 |

dry-run 永远是 `pending`。live run 即使核心显示/采集/编码都成功，只要 Developer ID 与锁屏/重建证据尚未汇总，仍应是 `pending`。

## 隐私与安全边界

- 不采集音频，不申请麦克风权限。
- 不访问主屏作为替代采集目标。
- 默认 dry-run；`--live` 还必须同时提供 `--allow-create-display` 与 `--allow-screen-capture`。
- 私有 `CGVirtualDisplay` API 的静态存在不代表兼容、许可或未来可用性；只在特定 macOS build 上记录实测。
- 探针不会联系 Apple 公证服务，除非显式运行 distribution 脚本的 `--execute`。
