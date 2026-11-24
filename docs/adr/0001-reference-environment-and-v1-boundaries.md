# 以单一基准环境约束 V1 范围

Status: accepted

DeskMate V1 只在一台明确的 MatePad、其指定 HarmonyOS 版本和一台明确的 Mac 上承诺扩展显示、Basic Input 及 USB 有线传输；其他设备、120Hz 和专业手写笔能力均为未验证或后续预研。AOA 是首选传输，USB 网络共享仅作为必须通过同一真机验证的有线备选，Wi-Fi 不满足产品核心约束。此取舍避免在硬件和系统能力未知时扩大兼容性及输入承诺。

V1 在 Reference Link、60Hz 和原生分辨率下以软件管线 P95 不超过 15ms、相对光子延迟 P95 不超过 40ms 验收，35ms 是优化目标；120Hz 和不超过 25ms 仅为实验目标。具体设备标识及链路协商速率仍待 PoC 记录，不得将缺省标识视为已完成基准冻结。

项目状态为 **Design / Pending Hardware Verification**。Gate 0 验证虚拟显示器与独立分发前提；AOA 链路、USB 网络共享备选和 AVCodec 恢复行为构成 Gate 1–3；Gate 4A 只建立可复现的测量基线，之后才启用 V1 性能退出门禁。在缺少设备标识和门禁证据时，不承诺 Phase 2 及后续排期。
