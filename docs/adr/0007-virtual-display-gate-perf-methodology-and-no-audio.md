# 以虚拟显示器门禁和保守测量约束 V1

Status: proposed

Gate 0 最先独立验证 Reference Environment 中 `CGVirtualDisplay` 的自定义分辨率、HiDPI、ScreenCaptureKit 持续采集、锁屏/睡眠后的销毁重建，以及 Developer ID 签名、Apple 公证和 Gatekeeper 启动。分发仅限独立 Developer ID 应用，不进入 Mac App Store；公证通过不代表私有 API 的稳定性或未来许可。Gate 0 失败即停止当前 Extended Display 路线，不以主屏镜像或硬件显示欺骗器静默替代。

每种延迟负载至少采集 100 个有效跳变，以伪随机、非同步间隔打破相机和显示器整数倍频锁相，报告 P50/P95/P99、样本量及保守误差上界 `epsilon`。设光子 P95 点估计为 `L`：`L + epsilon <= 40ms` 为通过，`L - epsilon > 40ms` 为不达标，其余为不确定，须以更高帧率或光电二极管复测。240fps 的 4.17ms 帧间隔只是初始量化量级，不能在未分析曝光、双屏响应和子帧插值误差时直接当作完整误差上界。

V1 不采集或传输音频，不申请麦克风权限；ScreenCaptureKit 显式关闭音频采集。协议 `type = 0x02` 保留但不受支持，收到后返回 `UNSUPPORTED_MSG_TYPE` 并丢弃，不因单个不支持的消息终止健康 Session。macOS 权限面板可能固定显示“录屏与系统录音”，权限引导须说明 DeskMate 仅采集视频。
