# DeskMate Gate 2A 独立探针

Status: **pending / not executed**

本目录只实现任务单 08。它验证 MatePad 是否存在用户可操作、无需开发者模式的
USB 网络共享入口，以及 macOS 是否以原生免驱 NCM/ECM 识别该链路。它不构建
Plan B 产品通道，不接收或注入 Reverse Input，也不从 AOA 结果推导可用性。

## 判定规则

以下条件全部满足才可能通过 Gate 2A：

1. Device UI 入口已被人工观察并记录，证明用户可操作；
2. 同一入口不要求开发者模式；
3. macOS 将 Device 识别为 USB 网络接口；
4. 驱动必须属于原生免驱 NCM 或 ECM。当前允许的 Apple bundle 是
   `com.apple.driver.usb.cdc.ncm`、`com.apple.driver.usb.cdc.ecm` 与
   `com.apple.DriverKit.AppleUserECM`；
5. 网卡与目标 Device 序列号或同一 USB 复合设备明确关联；
6. Device 直连 Mac 机身接口，不经过 Hub、扩展坞或 Thunderbolt dock；
7. 在 Reference Link 上完成 ping 与双向 iperf3，并保留原始日志；
8. 报告通过 `validate-report`。

`validate-report` 会打开报告引用的 artifacts 与测量原始文件并重新计算 SHA-256；
绝对路径、`..`、指向 evidence root 外的 symlink、文件缺失、内容被改动或占位
哈希都不会通过。`passed` 报告还必须从 `raw/host.json`、`raw/device.json`、
`raw/ioreg-*.txt`、ping 与双向 iperf3 原始输出重新解析并逐项核对拓扑、驱动、
计数、RTT、抖动与吞吐；只用真实哈希包装伪造数值不会通过。手动入口证据必须
以 evidence root 相对路径归档在 `artifacts` 中并匹配其 SHA-256。

任何 RNDIS 接口都直接失败，即使它已经有 IP 或能传输数据。第三方 NCM/ECM
驱动、未关联的 USB 网卡、Hub 路径和 Wi-Fi 都不能算 Gate 2A 证据。

## 安全边界

- `capture` 只运行只读命令：`sw_vers`、`sysctl`、`ioreg`、`system_profiler`、
  `networksetup`、`ifconfig` 与 `hdc` 查询。
- 不启用共享，不修改网络服务，不安装 kext/dext，不执行 `sudo`。
- `measure --execute` 只有在报告已经证明原生 NCM/ECM、直连 Reference Link
  和人工 Device 入口时才运行。
- 原始网络与 IORegistry 输出会遮蔽 MAC，网络地址输出会遮蔽 IP；报告仍保留
  接口名、状态、驱动和网络服务。
- 本探针没有输入消息、Socket 产品通道、配对或注入代码。

## 命令

```sh
cd probes/gate2a
make test
make validate

SERIAL=59GYD25716204513 make capture
```

`make capture` 默认写入
`evidence/2026-11-24-non-reference-dev/`。可用 `EVIDENCE=` 指定新目录。

从归档原始证据重新生成报告：

```sh
python3 gate2a.py compose \
  --raw-dir evidence/<run>/raw \
  --device-serial <serial> \
  --device-entry evidence/<run>/device-entry.json \
  --measurement evidence/<run>/measurement/link-measurement.json \
  --output evidence/<run>/report.json
```

`device-entry.json`、其入口截图与 `measurement` 必须位于同一个
`evidence/<run>/` 下；报告和测量里的文件路径均相对该目录。`compose` 会把
measurement 引用的 ping/iperf3 原始文件以及手动入口证据一并加入 `artifacts`。
这些输入仅在真实人工核验和真实 Reference Link 测量完成后提供。

## 关键文件

- `gate2a.py`：采集、拓扑/网卡关联、报告合成、测量与校验 CLI。
- `tests/test_contract.py`：RNDIS 拒判、第三方驱动拒绝、Hub 非 Reference、
  未关联网卡排除、报告完整性和测量安全门。
- `RUNBOOK.md`：真机操作和测量步骤。
- `templates/device-entry.json`：人工入口记录模板。
- `templates/REPORT.md`：报告模板。
- `evidence/2026-11-24-non-reference-dev/`：本次真实非 Reference 环境记录。

## 当前记录

2026-11-24 的实机证据结论为 `pending`：

- Host：`Mac16,12`，Apple M4，16 GB，macOS `27.3` / `26C5047e`。
- Device：`MRDI-W00`，`HUAWEI MatePad Pro`，序列号
  `59GYD25716204513`，OpenHarmony `7.0.0` / API `26`。
- 当前 Device 由 `hdc` 占用 HDC Interface，USB 协商 `480 Mbps`。
- HDC 祖先路径含 Bridgesil `USB2.1 Hub`；同一扩展坞树中还枚举了
  GenesysLogic `USB2.1 Hub`，但它不是当前 HDC 路径的祖先。
- 没有与 Device 关联的 NCM/ECM/RNDIS 网络接口，也没有 IP 配置。
- 现有 `en5` 是 Realtek USB 网卡，由原生 NCM 驱动支持并绑定网络服务
  `USB 10/100/1G/2.5G LAN`；其序列号不同，已明确排除。
- Device 设置入口未人工核验；真实共享、吞吐、RTT 与抖动均未执行。

因此该记录不是 Gate 2A 通过证据，也不是 Plan B 安全通道实现。
