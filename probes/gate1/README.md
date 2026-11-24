# DeskMate Gate 1：AOA 有线双向探针

状态：**开发验证工具已实现；Gate 1 尚未评估**。本目录不产生 Gate 1 通过结论。

本探针只回答 Gate 1 的传输问题：

- Host 能否对目标设备发送 AOA `GET_PROTOCOL(51)`、`SEND_STRING(52)` 和
  `START(53)`，观察重枚举并打开 accessory。
- 授权后 `accessoryFd` 上的持续双向 Bulk 字节流是否能维持心跳、数据回显和统计。
- 拆包、粘包、零长度读写及异常断流在两端如何表现。
- `poll/eventfd`、无 `SA_RESTART` 的定向信号、官方 `closeAccessory` 三种唤醒路径
  是否能退出 Device worker。
- Host 同时取消 IN/OUT 后，事件循环是否等到 `active_transfers == 0` 才释放资源。
- 拔线、Device 切后台和恢复时，I/O、心跳和清理的实际结果。

## 目录

- `host/`：macOS C++17 与 libusb 的 AOA、重枚举和异步双向探针。
- `device/`：HarmonyOS Stage 模型 HAP。ArkTS 独占 `USBAccessoryHandle`；C++ NDK
  只借用 `accessoryFd`，绝不关闭它。
- `scripts/build_host.sh`：真实 libusb 构建、测试；缺失依赖时可显式构建不可运行语法检查目标。
- `scripts/build_device.sh`：通过 `DEVECO_SDK_HOME` 调用 DevEco SDK 构建 unsigned HAP。
- `scripts/auto_device.sh`：在 Device 上自动等待 AOA accessory、授权/打开、启动
  eventfd worker、刷新统计并抓取 hilog；每次点击前重新 dump layout，必要时先
  `aa start` 把 Ability 拉回前台。
- `scripts/capture_environment.sh`：采集 Mac、USB 拓扑、协商速率和可选 hdc 设备信息。
- `scripts/check_device_boundary.sh`：静态检查 Device C++ 对 `accessoryFd` 的单所有者边界。
- `RUNBOOK.md`：真机执行顺序、三种唤醒分支和证据记录要求。
- `evidence/TEMPLATE.md`：每轮实测必须填写的证据模板。

## 先决条件

- Host：macOS、CMake 3.16+、C++17 编译器、libusb 1.0。
- Device：DevEco Studio 26.0 或兼容 SDK；API 26 / OpenHarmony 26.0.0。
- 真机安装必须使用与设备 UDID 和 bundle name 匹配的签名材料。unsigned HAP
  只能证明构建成功，不能安装，也不能作为授权或双向传输证据。

SDK 路径不写进产品代码。脚本按以下顺序寻找 DevEco（可用环境变量覆盖）：

1. `DEVECO_SDK_HOME`
2. `DEVECO_STUDIO_HOME`
3. `device/local.properties` 的 `sdk.dir`
4. 仅作为开发者便利默认值：`/Applications/DevEco-Studio.app/Contents`

hvigor 通常要求 `DEVECO_SDK_HOME` 指向 `.../sdk`；脚本也接受 `.../sdk/default`
并做规范化。

## 构建 Host

```sh
probes/gate1/scripts/build_host.sh
```

真实运行必须存在 `host/build-real/gate1_host`。若 libusb 缺失，脚本会明确失败并给出
安装/覆盖路径，不应把语法检查当成运行验证：

```sh
brew install libusb
# 或
LIBUSB_PREFIX=/path/to/libusb probes/gate1/scripts/build_host.sh
```

无 libusb 时可生成不可运行的语法检查目标：

```sh
probes/gate1/scripts/build_host.sh --syntax-only
```

`gate1_host_syntax` 使用 compile-only shim，执行只会失败；其产物不构成 runtime 证据。

## 构建 Device

```sh
export DEVECO_SDK_HOME=/Applications/DevEco-Studio.app/Contents/sdk
probes/gate1/scripts/build_device.sh
```

产物：

- `device/entry/build/default/outputs/default/entry-default-unsigned.hap`
- `device/entry/build/default/intermediates/libs/default/arm64-v8a/libgate1_probe.so`

构建成功只证明 ArkTS 与 native ABI 可编译。未签名 HAP 安装失败必须原样记录。

## 运行边界

Host 命令：

```sh
gate1_host list
gate1_host run --bus N --address N \
  --handshake-timeout-ms 60000 \
  --run-seconds 30 \
  --json evidence/local/run.json
```

`run` 依次执行 AOA 51/52/53、等待重枚举、打开 accessory、claim interface、
等待 `HELLO_ACK`、双向 Bulk、统计、同时取消 IN/OUT 和归零清理。握手期间每秒重发
`HELLO`，给操作者留出在平板上完成授权和启动 native worker 的时间。
真实 AOA 重枚举可能把 Ability 切到后台；自动脚本在每次点击前 dump layout，并在
目标按钮不可见时执行 `aa start -a EntryAbility -b com.deskmate.gate1probe`。
如果 Device 的 `openAccessory`/worker 启动晚于 Host claim，使用
`--handshake-initial-delay-ms 10000` 让首个 `HELLO` 延后发出；该延迟只改变握手
开始时间，不改变 `--handshake-timeout-ms` 的 `HELLO_ACK` 等待预算。
如果探针报告 `cleanup_safe=false`，即使 JSON 写入失败，Host 也不会关闭仍可能被
回调引用的 handle/context；进程只能由操作者确认后再结束。

Device 界面按顺序提供：

1. 授权并打开；
2. `eventfd` 启动/停止并 join；
3. 定向 `SIGUSR1` 启动/唤醒并 join；
4. `owner-close` 的 A 拒绝新工作、B `closeAccessory`、C join；
5. native 统计和生命周期日志。

Device C++ 可以 `close(event_fd_)`，但不得对 `accessoryFd` 调用 `close()` 或
`shutdown()`。`scripts/check_device_boundary.sh` 会拒绝越界写法。
`EntryAbility.onDestroy` 通过共享 `gate1Probe` 同步停止 worker、join 后由 ArkTS
关闭 `USBAccessoryHandle`；若 join 失败则保留句柄而不由 native 关闭。

Device 的 read/poll 错误会在 hilog 中记录数值 `errno`、fd、mode 和 poll
`revents`；`stats_json()` 同时输出 `last_read_errno` 与 `last_poll_revents`。
自动脚本把原始布局和日志写到：

- `/data/local/tmp/gate1_auto.log`
- `/data/local/tmp/gate1_auto_layout.json`
- `/data/local/tmp/gate1_auto_stats.json`
- `/data/local/tmp/gate1_auto_final.json`
- `/data/local/tmp/gate1_auto_hilog.log`

通过 `hdc file recv` 取回这些文件后再写入证据目录；脚本本身不伪造证据。

## 证据规则

- 未经 Hub/扩展坞直连，且未记录实际协商速率的链路，只能标记为
  **non-reference topology / not a Gate 1 pass**。
- 本工具固定输出 `gate1_conclusion: not_evaluated`。维护者必须把双端原始记录、
  拓扑、构建与安装结果、三种唤醒分支、拔线和后台行为填入证据模板后人工判定。
- HAP 未安装、未授权、未建立 `HELLO_ACK` 或未完成双向回显时，不得用 AOA
  重枚举成功推导 Gate 1 通过。
- 拆分/粘包由帧重组器证明；ZLP 必须同时记录 Host 零长度 IN 完成与 Device
  零长度写尝试。没有总线分析仪时，报告不得把“写 0 字节返回 0”升级为“线上 ZLP 已证实”。
- 拔线和后台是独立观测项，不能用 Session 状态机单测替代真机唤醒证据。

完整操作顺序见 `RUNBOOK.md`。
