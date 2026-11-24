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
open ./build/DeskMate.app
./build/DeskMate.app/Contents/MacOS/DeskMate --dry-run  # 只采集编码，不连 USB
```

参数：

| 参数 | 默认 | 说明 |
|---|---|---|
| `--display-id N` | 已保存的选择，或唯一可用的扩展显示器 | 借用一块已存在的 Extended Display |
| `--create` | 关 | 用 `CGVirtualDisplay` 自建（见下方限制） |
| `--width/--height` | `2800x1840` | 采集像素尺寸 |
| `--fps` | `60` | 采集帧率 |
| `--codec` | `hevc` | `hevc` 或 `h264` |
| `--dry-run` | 关 | 不建立 USB Session |

从 Finder 打开应用后，主窗口会给出连接或权限提示。设置可选择扩展显示器（主屏和镜像屏不会列出）；会话已启动时，新的选择会在重开应用后生效。连接诊断和菜单栏都可复制状态。主窗口的「正在发送画面」不代表平板已经出画。

桥接日志每 5 秒输出一行
`state=… rx=…B tx=…B frames=… bulkErrors=… profile=…`，因此一次运行就能判断
平板是否真的发出了 `ClientHello`（`rx` 是否从 0 增长）。

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

握手阶段给 20 秒（首次接 AOA 时平板会弹「允许访问 USB 配件」，2 秒的协议默认值
必然不够）。超时只重建 `HostSession`，**不释放已 claim 的 USB 链路**：重新 claim
会返回 `LIBUSB_ERROR_BUSY`，断开链路又会逼平板重新授权，两种情况都无法收敛。

退出时只做 `libusb_release_interface` + `libusb_close`。**不做** `set_configuration(0)`
或 `libusb_reset_device`——实测两者都不能把平板送回 HDC 模式，还会把它留在
`18d1:2d00`；恢复方式是拔插 USB。

### 平板的 AOA 前台问题（必读）

平板切到 accessory 模式时，ArkTS 的 `setInterval` 会被挂起。设备端 `AccessoryLink`
正是靠定时器轮询 `getAccessoryList()`，因此切换后轮询停止、`openAccessory()` 不再
执行，Host 只会看到 `rx=0 tx=0` 和 `hello_timeout`。

`probes/gate1` 的解法是在 AOA 之后用设备本地 `aa start` 把应用重新拉到前台。产品端
把这个动作做成了脚本，**必须在切换前启动**：

```sh
make device-foreground \
  HDC=/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc \
  BUNDLE=com.deskmate.gate3
```

脚本在平板后台每 5 秒 `aa start` 一次，共 12 轮，覆盖整个切换窗口。日志在
`/data/local/tmp/deskmate_fg.log`。

## 权限

- **屏幕录制**：必需。缺失时主窗口会引导打开系统设置；授权后可重试，如系统要求则重新打开 DeskMate。
- **辅助功能**：只影响反向输入。授权并在 DeskMate 设置中开启开关后，平板才可操作 Mac；不授权仍可接收画面。

## 已知限制

- V1 只承诺单指鼠标等价控制 + 双指滚动；键盘与压感尚未接入产品端。
- 采集尺寸与平板面板不一致时由设备端缩放，产品默认 1:1。
- 本仓库的 Gate 1 结论仍是 `NOT EVALUATED`（经 Hub、480 Mbps）；此应用不改变
  该基准结论。
