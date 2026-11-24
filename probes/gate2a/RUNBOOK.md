# Gate 2A 真机运行手册

## 0. 前置条件

- Reference Mac：`Mac16,12` / Apple M4 / 16 GB / macOS `27.3` `26C5047e`。
- Reference Device：MatePad Pro `MRDI-W00` / `59GYD25716204513` /
  OpenHarmony `7.0.0.111` / API 26。
- python3。
- `hdc`，用于只读读取 Device 参数与接口清单。
- 测量阶段需要 `iperf3` 以及 Device 上可由操作者显式启动的测试服务。

不要把 Wi-Fi、Hub、扩展坞或第三方 USB 网卡结果写成 Reference Link 结果。

## 1. 人工核验 Device 入口

1. 在 Device 上打开设置，找到 USB 网络共享 / 以太网共享相关入口。
2. 记录精确路径、操作者、UTC 时间和可见行为。
3. 确认该入口不需要开启“开发者模式”。
4. 把截图或其他可复核证据保存到最终 `evidence/<run>/` 目录内。
5. 从 `templates/device-entry.json` 复制并填写：

```json
{
  "status": "verified",
  "device_serial": "59GYD25716204513",
  "observed_at_utc": "2026-11-24T00:00:00Z",
  "operator": "name",
  "user_operable": true,
  "developer_mode_required": false,
  "path": "exact observed UI path",
  "steps": ["..."],
  "evidence": ["device-entry.png"],
  "notes": "observed behavior"
}
```

`device-entry.json` 和其中的 `evidence` 路径都必须以最终 `evidence/<run>/`
为基准，保存为相对路径。没有看到入口或无法排除开发者模式时，保持
`not_executed` 或填写真实失败原因。

## 2. 建立 Reference Link

1. 移除 Hub、扩展坞和 Thunderbolt dock。
2. 用同一根已记录线缆将 Device 直连 Mac 机身 USB 接口。
3. 在 Device 上启用刚核验的 USB 网络共享入口。
4. 在 Mac 上执行：

```sh
cd probes/gate2a
RUN=evidence/reference-$(date -u +%Y%m%dT%H%M%SZ)
mkdir -p "$RUN"
cp /path/to/device-entry.json "$RUN/device-entry.json"
cp /path/to/device-entry.png "$RUN/device-entry.png"
SERIAL=59GYD25716204513 make capture \
  EVIDENCE="$RUN" \
  DEVICE_ENTRY="$RUN/device-entry.json"
```

5. 检查：

```sh
REPORT=evidence/<run>/report.json
jq '{
  reference_link: .usb_topology.reference_link,
  hubs: .usb_topology.hubs,
  speed_mbps: .usb_topology.link_speed_mbps,
  interface: .device_network.interface,
  driver: .device_network.network_driver_bundle_id,
  service: .device_network.macos_network_service,
  decision: .decision
}' "$REPORT"
```

通过候选必须同时满足：

- `usb_topology.reference_link = true`；
- `device_network.interface` 非空；
- `device_network.driver_family` 是 `ncm` 或 `ecm`；
- `device_network.native_bundled_driver = true`；
- `device_network.network_driver_bundle_id` 是 Apple 原生 bundle；
- `device_network.macos_network_service` 已记录；
- `device_network.rndis_detected = false`。

如果看到 RNDIS、第三方驱动、Hub 或未关联网卡，停止，不要在该链路测吞吐。

## 3. Reference Link 测量

RTT 与抖动使用 macOS 自带 `ping`。吞吐使用 `iperf3`；Device 端测试服务只用于
测量，不是 DeskMate 产品通道，也不接收 Reverse Input。

1. 在 Device 上启动操作者提供的测试服务：

```sh
iperf3 -s -p 5201
```

2. 确认 Device USB 网卡 IP，并确认 Mac 路由确实走该 USB 接口：

```sh
route -n get <device-ip>
ipconfig getifaddr <interface>
```

3. 先生成计划，不执行：

```sh
python3 gate2a.py measure \
  --report "$REPORT" \
  --target <device-ip> \
  --interface <interface> \
  --output evidence/<run>/measurement
```

4. 仅在确认 Reference Link 后执行：

```sh
python3 gate2a.py measure \
  --report "$REPORT" \
  --target <device-ip> \
  --interface <interface> \
  --output evidence/<run>/measurement \
  --execute
```

工具会拒绝以下情况：

- 非直连 Reference Link；
- 非原生 NCM/ECM；
- 未人工核验 Device 入口；
- 路由不经过指定接口；
- 接口没有 IPv4 地址；
- 缺少 `iperf3`。

原始文件写入 `evidence/<run>/measurement/`：

- `ping.txt`
- `iperf-host-to-device.json`
- `iperf-device-to-host.json`
- `commands.json`
- `link-measurement.json`

`jitter_ms` 定义为相邻成功 ICMP RTT 样本的绝对差均值；报告同时保留 ICMP
summary 的 min/avg/max/stddev。

## 4. 合成最终报告

```sh
python3 gate2a.py compose \
  --raw-dir evidence/<run>/raw \
  --device-serial 59GYD25716204513 \
  --device-entry evidence/<run>/device-entry.json \
  --measurement evidence/<run>/measurement/link-measurement.json \
  --output evidence/<run>/report.json

python3 gate2a.py validate-report --report evidence/<run>/report.json
```

`compose` 要求上述输入都位于同一个 `evidence/<run>/` 下，会把 measurement
引用的 ping/iperf3 原始文件和 manual entry 截图加入 `artifacts`，并把报告内
所有证据路径统一写成相对 `evidence/<run>/` 的路径。`validate-report` 对
`passed` 报告会重新解析这些 raw 文件并逐项复算；报告值与 raw 不一致、路径
逃逸、缺失文件或哈希不符都会失败。

再按 `templates/REPORT.md` 写人类可读报告，并生成哈希：

```sh
cd evidence/<run>
find . -type f ! -name SHA256SUMS -print | LC_ALL=C sort | \
  while IFS= read -r file; do shasum -a 256 "$file"; done > SHA256SUMS
```

## 5. 决策语义

| Status | 含义 |
| --- | --- |
| `pending` | 入口未核验、无关联接口、非 Reference Link，或测量未执行 |
| `failed` | 已确认 RNDIS、仅第三方驱动，或已执行测量失败 |
| `passed` | 人工入口、原生 NCM/ECM、直连 Reference Link、双向测量和完整性校验全部满足 |

`passed` 只证明 Gate 2A 的硬件/驱动/链路前提，不证明 Plan B 安全通道、
配对、加密、Reverse Input 或产品传输实现。

## 6. 常见问题

- `system_profiler SPUSBDataType` 返回空：保留空结果，改用
  `ioreg -p IOUSB -w0 -l` 与 `ioreg -r -c IOUSBHostInterface -l`，不要伪造
  USB 清单。
- 同一 VID/PID 有多个设备：关联结果标为 ambiguous，必须重新直连并确认序列号。
- `en5` 等已有 USB 网卡看起来是原生 NCM：检查 `usb_serial` 是否等于目标 Device；
  不相等就不是 Gate 2A 接口。
- Device 只有 `hdc`/AOA 占用：记录 `owner` 与接口 class，保持 pending；不得从
  AOA 或 HDC 推断 NCM/ECM 可用。
