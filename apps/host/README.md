# DeskMate Host (macOS)

产品端 Host 应用：把一块 **Extended Display** 采集、硬件编码，经 AOA 有线链路
送到 HarmonyOS 平板，并把平板回传的 Basic Input 注入回该显示器。

## 构建

```sh
cd apps/host
make app      # 生成 build/DeskMate.app
make test     # 打包/参数/链接/脱敏契约检查
```

依赖：Xcode 命令行工具、`libusb`（`brew install libusb`，默认前缀
`/opt/homebrew`，可用 `LIBUSB_PREFIX` 覆盖）。

## 运行

```sh
./build/DeskMate.app/Contents/MacOS/DeskMate --list-displays
./build/DeskMate.app/Contents/MacOS/DeskMate            # 借用 display 41
./build/DeskMate.app/Contents/MacOS/DeskMate --dry-run  # 只采集编码，不连 USB
```

参数：

| 参数 | 默认 | 说明 |
|---|---|---|
| `--display-id N` | `41` | 借用一块已存在的 Extended Display |
| `--create` | 关 | 用 `CGVirtualDisplay` 自建（见下方限制） |
| `--width/--height` | `2800x1840` | 采集像素尺寸 |
| `--fps` | `60` | 采集帧率 |
| `--codec` | `hevc` | `hevc` 或 `h264` |
| `--dry-run` | 关 | 不建立 USB Session |

应用是菜单栏常驻（`LSUIElement`），标题 `DM ●` 表示采集运行中，`DM !` 表示有错误，
悬停可看状态 JSON。

## 显示来源

macOS 27.3 上 `CGVirtualDisplay` 只发布 1x 模式（`hiDPI=1` 会被接受但所有模式的
点尺寸等于像素尺寸）。因此默认借用外部工具创建的真正 2x 虚拟屏——本机用
BetterDisplay 创建的 `id 41`（`1920x1280 pt / 3840x2560 px @ 120Hz`）。

**应用绝不销毁不是自己创建的显示器**：借用模式下退出只做在线检查。`--create`
保留给能发布 HiDPI 模式的环境，失败会明确报错而不是静默降到 1x。

## 链路

1. 找到 HDC 设备 `12d1:5000`，发 AOA `GET_PROTOCOL(51)` / `SEND_STRING(52)` /
   `START(53)`，等待 `18d1:2d00` 重新枚举。
2. claim 接口 0，用 bulk IN/OUT 端点作为 `ByteIo`，交给共享核心 `HostSession`。
3. 握手：`CLIENT_HELLO → STREAM_PREPARE → STREAM_READY → STREAM_COMMIT →
   STREAM_COMMITTED`，之后才发送首个 IDR。
4. 采集回调的每帧经 VideoToolbox 编码为 Annex-B（关键帧前拼 VPS/SPS/PPS），
   通过 `send_video_frame()` 入调度器。
5. 平板回传的 `Input` 消息经 `take_input_events()` 取出，用 `CGEvent` 注入到该
   显示器的 bounds。

握手失败会重建 Session 重试，最多 5 次。退出应用时会把平板 `set_configuration(0)`
并 reset，让它回到正常 HDC 模式，不需要手动拔插。

## 权限

- **屏幕录制**：必需。缺失时应用直接报错退出，不做静默降级。
- **辅助功能**：只影响输入注入。缺失时仍可看画面，但会记录警告。

## 已知限制

- V1 只承诺单指鼠标等价控制 + 双指滚动；键盘与压感尚未接入产品端。
- 采集尺寸与平板面板不一致时由设备端缩放，产品默认 1:1。
- 本仓库的 Gate 1 结论仍是 `NOT EVALUATED`（经 Hub、480 Mbps）；此应用不改变
  该基准结论。
