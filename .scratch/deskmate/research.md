# DeskMate 技术调研记录

调研日期：2026-11-24。

## 已确认的 API 存在性

- HarmonyOS API 14 起存在 `usbManager.openAccessory()`、`requestAccessoryRight()` 和 `closeAccessory()`；官方说明返回 `accessoryFd` 供文件 I/O 读写。这证明公开配件 API 存在，不证明特定华为平板能从 Mac 完成 AOA 重枚举或达到目标吞吐。
- macOS `CGEventField` 文档包含数位板压感和倾角字段。这不证明将字段写入合成事件后，目标绘图软件会将其识别为真实压感输入。
- Apple 文档将 `com.apple.developer.driverkit.family.hid.virtual.device` 描述为创建和管理虚拟 HID 设备的 entitlement。V1 不依赖这条需要额外审批的虚拟设备路线。

## 官方来源

- [HarmonyOS usbManager API](https://developer.huawei.com/consumer/cn/doc/harmonyos-references/js-apis-usbmanager/)
- [HarmonyOS API 14 usbManager API](https://developer.huawei.com/consumer/en/doc/harmonyos-references-V14/js-apis-usbmanager-V14/)
- [Apple CGEventField](https://developer.apple.com/documentation/coregraphics/cgeventfield)
- [Apple virtual HID entitlement](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.driverkit.family.hid.virtual.device)
- [Apple DriverKit entitlement requests](https://developer.apple.com/documentation/driverkit/requesting-entitlements-for-driverkit-development)
- [Apple NSWorkspace 睡眠通知](https://developer.apple.com/documentation/appkit/nsworkspace/willsleepnotification)
- [Apple ScreenCaptureKit](https://developer.apple.com/documentation/screencapturekit)
- [HarmonyOS 视频解码指南](https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/video-decoding/)
- [HarmonyOS API 14 视频解码指南](https://developer.huawei.com/consumer/en/doc/harmonyos-guides-V14/video-decoding-V14/)
- [HarmonyOS NativeWindow API 14](https://developer.huawei.com/consumer/en/doc/harmonyos-references-V14/_native_window-V14/)
- [HarmonyOS XComponent 生命周期](https://developer.huawei.com/consumer/en/doc/harmonyos-references/capi-oh-nativexcomponent-native-xcomponent-oh-nativexcomponent-callback/)
- [HarmonyOS NativeWindow 常见生命周期错误](https://developer.huawei.com/consumer/en/doc/harmonyos-guides/native-common-mistakes/)

## 尚未由公开文档证明的假设

- 目标 MatePad 的 AOA 请求 51/52/53 行为、重枚举结果和持续 Bulk 吞吐。
- USB 网络共享是否在目标平板的设置中可用，以及 macOS 是否自动识别其 NCM/ECM/RNDIS 接口。
- 120Hz 解码、Surface 输出和光学延迟。
- M-Pencil 事件经 `CGEvent` 注入后在指定绘图软件中的兼容性。

## AVCodec 已确认约束

- 官方文档明确 Surface 模式可用 `OH_VideoDecoder_FreeOutputBuffer` 丢弃输出，用 `RenderOutputBuffer` 提交显示并释放输出。
- `Flush` 会使此前回调报告的缓冲区索引失效；重启后必须重新发送 SPS/PPS。API 14 文档说明 Flush 后需要再次 Start。
- `Flush`、`Reset`、`Stop`、`Destroy` 必须从非回调线程调用，否则会等待已触发回调完成而形成死锁风险。这是生命周期状态机的确定约束，不再是待测假设。
- 文档说明支持 Surface 动态切换和低延迟配置，但目标设备的实际行为、耗时和黑屏情况仍须 Gate 3 实测。

## 生命周期补充

- 官方确认 `OH_NativeWindow_NativeObjectReference/Unreference` 可配对管理并发生命周期，但两个函数都不是线程安全的；代际检查和引用操作必须串行化。
- XComponent 系统持有 NativeWindow 引用，并在 `OnSurfaceDestroyed` 后减少引用；因此增持必须在 Surface 仍有效的创建/租约阶段完成，不能在销毁回调里补救。
- `active_transfers == 0` 是 libusb 清理的必要条件，不代表取消调用会同步完成回调；事件线程必须继续处理取消回调。

## 协议治理与未验证边界

- `docs/adr/0003-wire-protocol-and-priority-scheduling.md` 和 `docs/adr/0005-protocol-state-and-session-semantics.md` 在真机门禁及互操作通过前均为 proposed。
- 单 `OHNativeWindow` 的解码器生产者独占、COMMIT 时是否可保留 Last Good Frame、失败后的旧配置恢复均不得预设成立，归入 Gate 3。
- `proto_rev` 只解决草案联调错配；它既不替代线上协议 `version`，也不提供安全认证。
- Apple 公开 Core Graphics API 列表未提供 `CGVirtualDisplay`；Developer ID 公证不等于私有 API 的稳定性或分发许可，须单独立项验证。
- Gate 0 只验证特定 macOS Build 下的可行性，不声称 Apple 对私有虚拟显示器 API 提供长期兼容承诺。
- 性能测量使用加误差上界的保守通过判定；不得用点估计减误差作为通过依据。

## 验收测试约束

- Reference Link 为平板直连 Mac 机身接口，不使用扩展坞或 Hub；PoC 使用 `system_profiler SPUSBDataType` 记录实际协商速率。
- 高速摄影使用至少 240fps，同时拍摄 Mac 内置参考屏与 Device；参考屏锁定 60Hz，并记录两台显示器各自的实际模式。即使同为 60Hz，也不能假设 VSync 相位相同。
- 黑白或高对比色块跳变、文字滚动和高动态视频用于光学延迟测试；静态高密度桌面用于画质和稳态测试，不能测光学跳变延迟。
- 软件跨端时间戳通过四时间戳交换估算偏移、RTT、漂移和误差上界；`RenderOutputBuffer` 返回不等于光子输出。
- USB 网络共享先独立验证系统入口、macOS 接口驱动和链路稳定性。只有原生免驱 NCM/ECM 合格；RNDIS 路线不可用。
