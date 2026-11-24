# Gate 2A：USB 网络共享硬件探测（非 Reference 环境）

Status: **pending / not executed**

生成时间：`2026-11-24T04:57:11Z`

## 结论

本次没有启用真实 USB 网络共享，也没有执行吞吐、RTT 或抖动测量。当前 Device
由 `hdc` 占用 HDC Interface，没有与目标序列号关联的 NCM/ECM/RNDIS 接口，
USB 仅协商 `480 Mbps`，且 HDC 路径包含 USB Hub。因此本次记录不能解除 Gate 2A，
也不能作为 Plan B 产品通道或 Reverse Input 已实现的证据。

## 范围

- 只做只读 Host/Device 环境、USB 拓扑、网络接口与驱动识别。
- 不启用 Device 共享入口。
- 不构建 Socket 产品通道。
- 不接收或注入 Reverse Input。
- 不使用 AOA 成败推导 USB 网络共享能力。

## Reference Environment 实测

| 项目 | 实测值 | 来源 |
| --- | --- | --- |
| Host | `Mac16,12`，Apple M4，16 GB | `sysctl` |
| macOS | `27.3`，build `26C5047e`，`arm64` | `sw_vers` / `uname` |
| Device | `MRDI-W00`，`HUAWEI MatePad Pro` | `hdc` 只读参数 |
| Device 序列号 | `<redacted-serial>` | `hdc` / USB serial |
| Device 系统 | `MRDIL-W00 7.0.0.111(<redacted-serial>)` | `const.product.software.version` |
| OpenHarmony / API | `7.0.0` / `26` | `hdc` 只读参数 |
| USB 连接 | `USB` / `Connected` | `hdc list targets -v` |

## Device 用户入口

状态：`not executed`

没有人工在 Device UI 上核验 USB 网络共享入口，也没有证明该入口不需要开发者模式。
本次记录不填写入口路径、截图或“用户可操作”结论。复核步骤见 `RUNBOOK.md`。

## macOS USB 与网络识别

| 项目 | 实测值 |
| --- | --- |
| 目标 USB 设备 | `HDC Device` |
| Vendor | `HISILICON` |
| VID / PID | `4817` / `20480`（`0x12D1` / `0x5000`） |
| USB 序列号 | `<redacted-serial>` |
| 协商速率 | `480 Mbps` |
| USB owner | `pid 67816, hdc` |
| Device 暴露接口 | `HDC Interface`，class `255`，subclass `80`，protocol `1` |
| Device 网络接口 | 无 |
| Device 驱动族 | `none` |
| Device RNDIS | 未发现 |
| Device macOS 网络服务 | 无 |

Device `ifconfig -a` 中未出现 `usb0`、`ncm*` 或 `rndis*` 接口。当前 HDC
Interface 是厂商私有接口，不能当作 NCM/ECM，也不能从 HDC/AOA 占用推断 USB
网络共享可用。

Host 上另有 `en5`：

- 厂商/型号：Realtek `USB 10/100/1G/2.5G LAN`；
- 驱动：`AppleUSBNCMControl` / `com.apple.driver.usb.cdc.ncm`；
- macOS 网络服务：`USB 10/100/1G/2.5G LAN`；
- USB 序列号：`<redacted-serial>`；
- 协商速率：`5000 Mbps`。

该接口虽然是原生免驱 NCM，但不属于目标 Device，已按序列号排除。

## USB 拓扑

HDC Device 的 IORegistry 祖先路径：

```text
AppleT8132USBXHCI@01000000
└── USB2.1 Hub@01100000 (Bridgesil)
    └── HDC Device@01130000 (HISILICON, 480 Mbps)
```

同一扩展坞 USB 树中还枚举了：

- GenesysLogic `USB2.1 Hub@01140000`；
- Bridgesil `USB3.2 Hub@01200000`；
- GenesysLogic `USB3.1 Hub@01240000`。

因此环境中确实同时存在 GenesysLogic 与 Bridgesil Hub，但当前 HDC 路径的直接
Hub 祖先是 Bridgesil `USB2.1 Hub`，GenesysLogic Hub 不是 HDC 的祖先。

无论按路径 Hub 还是按实际 480 Mbps 连接，当前链路都不是 Reference Link。

## 测量

状态：`not executed`

| 指标 | 结果 |
| --- | --- |
| 吞吐 | 未执行 |
| RTT | 未执行 |
| 抖动 | 未执行 |
| Packet loss | 未执行 |

原因：

1. 没有与 Device 关联的原生免驱 NCM/ECM 网络接口；
2. 当前 USB 路径包含 Hub 且只协商 480 Mbps，不是 Reference Link；
3. Device USB 网络共享入口未人工核验。

未来测量命令与原始文件要求见 `RUNBOOK.md`。Wi-Fi、`en5` 或 Hub 链路数值不能
代替 Reference Link 结果。

## 决策

Status: **pending**

未通过项：

- Device 用户入口未核验；
- 无目标 Device 的 NCM/ECM 网络接口；
- 当前路径非 Reference Link；
- 吞吐、RTT、抖动未执行；
- 原生免驱与 macOS 网络服务尚未在目标接口上得到实测。

本记录不证明：

- USB 网络共享在 MatePad 上不存在或存在；
- Plan B 的对端认证、加密、Socket 通道或 Reverse Input；
- AOA 能力；
- V1 产品链路可行性。

## 已执行命令

全部命令只读，未使用 `sudo`：

```sh
sw_vers
sysctl -n hw.model
sysctl -n hw.memsize
sysctl -n machdep.cpu.brand_string
uname -m
system_profiler SPUSBDataType -json
ioreg -p IOUSB -w0 -l
ioreg -r -c IOUSBHostInterface -l
ioreg -r -c AppleUSBNCMControl -l
ioreg -r -c AppleUSBECMControl -l
networksetup -listallhardwareports
networksetup -listallnetworkservices
ifconfig -a
hdc list targets -v
hdc -t <redacted-serial> shell param get const.product.model
hdc -t <redacted-serial> shell param get const.product.name
hdc -t <redacted-serial> shell param get const.product.software.version
hdc -t <redacted-serial> shell param get const.product.os.dist.version
hdc -t <redacted-serial> shell param get const.ohos.apiversion
hdc -t <redacted-serial> shell ifconfig -a
```

`system_profiler SPUSBDataType -json` 返回空 `SPUSBDataType` 数组；USB 结论来自
IORegistry。原始命令、返回码与输出见 `raw/commands.json` 和同目录日志。

## 原始证据

- `report.json`：机器可读完整报告，`validate-report` 已通过；
- `raw/ioreg-usb.txt`：USB 设备树；
- `raw/ioreg-interfaces.txt`：USB 接口描述；
- `raw/ioreg-ncm.txt` / `raw/ioreg-ecm.txt`：NCM/ECM 驱动实例；
- `raw/networksetup-*.txt`：macOS 网络硬件端口与服务；
- `raw/hdc-*.txt`：Device 版本与接口清单，IP/MAC 已遮蔽；
- `raw/commands.json`：每条命令、参数与返回码。

文件 SHA-256 见同目录 `SHA256SUMS`。
