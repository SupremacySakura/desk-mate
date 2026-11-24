# Gate 2A：USB 网络共享硬件探测报告

Status: **pending | failed | passed**

生成时间：`YYYY-MM-DDTHH:MM:SSZ`

## 范围

- 只验证 USB 网络共享入口、macOS 原生免驱 NCM/ECM 识别与 Reference Link 测量。
- 不构建 Plan B 产品通道。
- 不接收、注入或测试 Reverse Input。
- 不从 AOA 成败推导 USB 网络共享能力。

## Reference Environment

| 项目 | 实测值 | 来源 |
| --- | --- | --- |
| Host |  | `sysctl` / `sw_vers` |
| macOS |  | `sw_vers` |
| Device |  | Device UI / `hdc` |
| Device 系统与 API |  | Device UI / `hdc` |
| Device 序列号 |  | Device UI / `hdc` |

## Device 用户入口

- 状态：`not executed | verified | failed`
- 精确 UI 路径：
- 是否需要开发者模式：
- 操作者与时间：
- 截图或手工证据：

未实际看到入口时不得填写 `verified`。

## macOS 识别结果

| 项目 | 实测值 |
| --- | --- |
| USB Vendor / Product |  |
| VID / PID |  |
| 关联 USB 序列号 |  |
| 协商速率 |  |
| 网络接口 |  |
| 驱动 bundle / class |  |
| 驱动族 | `ncm / ecm / rndis / none` |
| macOS 网络服务 |  |
| 原生免驱 |  |

RNDIS 无论是否工作都必须判负。

## USB 拓扑与 Reference Link

- HDC / Device 祖先路径：
- 路径中的 Hub：
- 树中观察到的其他 Hub：
- 是否直连 Mac 机身接口：
- 是否为 Reference Link：
- 非 Reference 原因：

## 测量

状态：`not executed | executed | failed`

| 指标 | 原始文件 | 结果 |
| --- | --- | --- |
| RTT min/avg/max |  |  |
| RTT jitter |  |  |
| Packet loss |  |  |
| Host → Device throughput |  |  |
| Device → Host throughput |  |  |

只有 Reference Link 上产生的原始 ping/iperf JSON 才算 Gate 2A 证据。Hub、Wi-Fi、
扩展坞或非关联网卡结果仅作排除记录。

## 决策

- Status:
- 理由：
- 未执行项：
- 本次结论不代表 Plan B 安全通道、Reverse Input 或产品链路已实现。

## 命令与原始证据

```sh
# 填入实际命令
```

原始文件与 SHA-256 见报告 JSON 的 `artifacts` 与同目录 `SHA256SUMS`。
