# Gate 1 非基准开发环境记录

日期：2026-11-24（Asia/Shanghai）

## 结论

- Gate 1：**NOT EVALUATED**
- 本轮拓扑：**non-reference topology / not a Gate 1 pass**
- 原因：Device 经 USB2.1 Hub，协商为 USB 2.0 High-Speed；没有匹配的签名材料，
  HAP 未能安装/授权；双工、三种唤醒、后台、拔线和重连均未执行。
- 可复用产物：Host 真实 libusb 探针可构建并运行；Device ArkTS/C++ HAP
  **可构建但不可用当前材料安装**。

本记录不把 AOA 重枚举、Host 打开 accessory 或协议单测结果写成 Gate 1 通过。

## 环境

| 项目 | 值 | 来源 |
| --- | --- | --- |
| Mac | MacBook Air `Mac16,12`，Apple M4，16 GB | `sysctl` / `system_profiler SPHardwareDataType` |
| macOS | 27.3，build `26C5047e` | `sw_vers` |
| Device | HUAWEI MatePad Pro `MRDI-W00` | 操作者现场记录 |
| Serial | `59GYD25716204513` | `ioreg` 与 `hdc` 现场记录 |
| HarmonyOS | 7.0.0.111，API 26，tablet | 操作者现场记录 |
| Display | 2800×1840，active 60Hz；支持 120/144Hz | 操作者现场记录 |
| DevEco | 26.0；SDK root `/Applications/DevEco-Studio.app/Contents/sdk/default` | 本机安装 |
| OpenHarmony SDK | platform 26.0.0；native 26.0.0.105 | 本机安装 |
| hvigor | 6.26.8 | DevEco bundled |
| libusb | Homebrew `/opt/homebrew/opt/libusb` | build/runtime |

采集时 hdc 已无法识别目标，故 HarmonyOS/API/显示参数未能由 `hdc` 重新读取；
这些字段只按现场操作者记录，标记为未由本轮脚本复核。

## USB 拓扑和协商速率

`probes/gate1/scripts/capture_environment.sh` 采集。`device-topology.txt` 原始内容：

```text
serial=59GYD25716204513
matched_line=    |       "kUSBSerialNumberString" = "59GYD25716204513"
ancestor_path:
  +-o Root
    +-o AppleT8132USBXHCI@01000000
      +-o USB2.1 Hub@01100000
      | +-o "HDC Device"@01130000
```

关键属性：

- 父 Hub：Bridgesil `USB2.1 Hub`，VID:PID `35d6:2510`，`UsbLinkSpeed=480000000`。
- Device：采集时 `idVendor=0x18D1`、`idProduct=0x2D00`（AOA accessory），
  `UsbLinkSpeed=480000000`，`Device Speed=2`（High-Speed），bus 1 / address 7。
- 因此链路经过 Hub，且只协商到 480 Mbps；不满足 Reference Link 的机身直连要求。

`system_profiler SPUSBDataType -json` 在本机返回了空数组：

```json
{
  "SPUSBDataType" : [

  ]
}
```

文本模式同样为 0 字节。脚本保留了原始状态文件和 ioreg 回退证据；没有把空输出
补写成设备速率。

## 构建与测试

Host：

```text
probes/gate1/scripts/build_host.sh
-- libusb found: host probe enabled
100% tests passed, 0 tests failed out of 1
RUNTIME PROBE: .../host/build-real/gate1_host
```

依赖缺失降级：

```text
probes/gate1/scripts/build_host.sh --syntax-only
100% tests passed, 0 tests failed out of 1
SYNTAX ONLY: .../host/build-syntax/gate1_host_syntax is deliberately non-runnable.
```

Device：

```text
export DEVECO_SDK_HOME=/Applications/DevEco-Studio.app/Contents/sdk
probes/gate1/scripts/build_device.sh
BUILD SUCCESSFUL
WARN: No signingConfig found for product default
```

产物：

- `entry/build/default/outputs/default/entry-default-unsigned.hap`
  SHA-256 `a388d26d2fe5af2dd6b2d65b5fef2f898a50e6ae7b3ee8f1caa6384c8059efa8`
- `entry/build/default/intermediates/libs/default/arm64-v8a/libgate1_probe.so`
  SHA-256 `77aa4542c17b1e8ebd9bd118ce950a94daf4670bd1a1afd59519bcf40d376efe`
  （ELF 64-bit ARM aarch64）

静态所有权检查：

```text
PASS: Device ownership/wake static boundary checks
```

检查拒绝 Device C++ 对 `accessoryFd` 调用 `close()`/`shutdown()`，并要求 ArkTS
存在授权、打开、关闭以及 A/B/C owner-close 次序。

## 安装与授权

- HAP 未配置 `signingConfig`，构建输出为 unsigned。
- 本机已有两个有效 debug profile，bundle 分别为
  `com.example.myapplication` 与 `com.supremacy.simpleledger`；均不是本探针的
  `com.deskmate.gate1probe`。没有证据表明它们的 device ID 包含本轮设备。
- 本轮没有改写或复用这两个不相关 profile。
- 工作会话早前对 unsigned HAP 的真实安装尝试返回：

  ```text
  code:9568320 error: no signature file
  ```

- 当前 Device 已处于 AOA 模式，`hdc list targets -v` 返回 `[Empty]`；
  `hdc -t 59GYD25716204513 shell bm get -u` 返回
  `[Fail]Not match target founded, check connect-key please`。
- 因此 `requestAccessoryRight`、`openAccessory`、持续双向授权均未执行。

**可安装性结论：源码和 unsigned HAP 构建成功；当前机器上的材料不能产出可安装 HAP。**

## AOA 与 Host 运行观察

工作会话早前观察过请求 51/52/53 被接受、设备重枚举为 `18d1:2d00`、Host 打开
interface 0（Bulk IN `0x81`、OUT `0x01`）。该次 stdout 未保留为仓库内原始文件，
不作为正式 Gate 1 请求证据；当前 ioreg 中的 `18d1:2d00` 与该观察一致。

随后使用现有 AOA accessory 做了一次明确非通过的 Host 探测：

```text
gate1_host probe --vid 0x18d1 --pid 0x2d00 --handshake-timeout-ms 5000
[open] accessory 18d1:2d00 bus=1 address=7 speed=480 Mbps interface=0 ep_in=0x81 ep_out=0x1
[open] claimed interface 0, bulk IN=0x81 OUT=0x1
[error] handshake OUT failed: LIBUSB_ERROR_TIMEOUT
handshake_ok=false
out_bytes=0
in_bytes=0
active_on_release=0
cleanup_safe=true
Gate 1 conclusion: NOT EVALUATED
```

这说明 Host 能打开并 claim 当前 accessory，但 Device 未安装探针，因而没有 reader
消费 `HELLO`；Bulk OUT 在 3 秒后超时。它不证明 authorization、双工、心跳或取消。

## 未执行项

| 项目 | 状态 | 原因 |
| --- | --- | --- |
| Reference Environment / Reference Link | NOT EXECUTED | 当前设备经 Hub，USB 2.0 High-Speed |
| 正式 AOA 51/52/53 原始捕获 | NOT CAPTURED | 当前已处于 AOA 模式；原始会话 log 未保留 |
| HAP 安装 | FAILED / NOT EXECUTED | unsigned；无匹配 bundle+UDID 的签名材料 |
| `requestAccessoryRight` / `openAccessory` | NOT EXECUTED | HAP 未安装 |
| HELLO_ACK、持续双工、回显 | NOT EXECUTED | 同上；当前 handshake OUT 超时 |
| poll/eventfd 唤醒 | NOT EXECUTED | 无真机 HAP |
| 无 SA_RESTART 定向信号唤醒 | NOT EXECUTED | 无真机 HAP |
| 官方 closeAccessory 唤醒 | NOT EXECUTED | 无真机 HAP |
| ZLP/拆分/粘包的 Device 真机表现 | PARTIAL | 仅协议单测；无 accessoryFd 真机数据 |
| 同时取消 IN/OUT 与 `active_transfers` 归零 | NOT EXECUTED | 未进入双工传输阶段 |
| 拔线、后台冻结、心跳失联、重连 | NOT EXECUTED | 无安装授权与双工阶段 |

## 复现文件

- `host-build.log`
- `device-build.log`
- `device-boundary.log`
- `host-list.log`
- `hdc-list.log`
- `hdc-query.log`
- `device-topology.txt`
- `probe-no-app.log`
- `probe-no-app.json`

这些文件由对应命令直接产生；`SHA256SUMS` 在提交前生成。
