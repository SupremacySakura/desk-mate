# HarmonyOS 系统能力调研

调研日期：2026-11-24。以当前官方公开文档为准；应用基线为 API 20 时，不能把 API 26 新增接口当作基线能力。`SysCap` 表示设备能力集，不等于对普通第三方应用开放；开放范围仍须以具体接口和权限文档为准。

## 结论速览

| 场景 | 可用性 | 关键条件 |
| --- | --- | --- |
| 应用退出后的定时提醒 | 可实现，但有平台管控 | 用户通知授权 + 代理提醒开放能力审批；可评估 Calendar Kit |
| 手机、平板、PC 间发现/协同 | 可实现发现、认证及数据类协同 | `DISTRIBUTED_DATASYNC` 用户授权；自由控制远端键鼠另有门槛 |
| 普通应用响应/注入键鼠 | 本应用事件可响应；系统输入注入强受限 | 注入仅限特定场景、PC/2in1；API 26 起需受限权限与手动授权 |
| 环境光、心率等感知 | 取决于实际硬件/设备类型 | 环境光可直接订阅；心率需健康数据权限 |
| 设置系统屏幕亮度/模式 | 常规手机、PC 应用不可直接实现 | 公开方案是自身前台窗口亮度；轻量穿戴是独立例外 |

## 1. 定时、后台代理提醒

`@kit.BackgroundTasksKit` 的 `reminderAgentManager.publishReminder` 支持倒计时、日历和闹钟；应用退后台或进程终止后由系统服务代理提醒。SysCap 为 `SystemCapability.Notification.ReminderAgent`，需要 `ohos.permission.PUBLISH_AGENT_REMINDER`，发布前还需取得通知授权。

- 手机、平板、PC/2in1 上存在管控，须在开发者中心申请代理提醒开放能力；官方建议也可采用 Calendar Kit 替代。智慧屏、智能穿戴无此管控限制。
- 申请仅限官方列出的应用类型及提醒场景，营销类场景不支持；申请需重新生成 Profile 并手动签名。因此不能把它当作无门槛普通能力。
- 提醒条数随 API 版本变化：API 26 起普通应用最多 64 个有效提醒，API 25 及以下最多 30 个；点击通知只能跳回发起提醒的本应用。

官方来源：[代理提醒开发指南](https://developer.huawei.com/consumer/cn/doc/harmonyOS-guides/agent-powered-reminder)、[reminderAgentManager API](https://developer.huawei.com/consumer/cn/doc/harmonyos-references/js-apis-reminderagentmanager/)。

## 2. 跨设备、分布式与外设

`@kit.DistributedServiceKit` 的 `distributedDeviceManager`（不要与 Driver Kit 的同名 `deviceManager` 混淆）支持发现周边设备、建立可信关系、查询在线可信设备和上下线监听，SysCap 为 `SystemCapability.DistributedHardware.DeviceManager`，须申请 `ohos.permission.DISTRIBUTED_DATASYNC`，并由用户动态授权。

- 跨设备拖拽是较低门槛的系统协同入口：组件实现拖入/拖出后，由系统完成键鼠穿越和跨设备数据传递。条件是支持的平板或 2in1、同一华为账号、Wi-Fi 与蓝牙开启、同一局域网并开启“键鼠穿越”。这不是应用接管任意设备输入的通用接口。
- 系统键鼠穿越能力与“应用向另一台设备自由注入键鼠”不能画等号。应用若接收远端输入并操作本机，应归入远程登录器被控端等特定场景，走输入事件注入权限评估。
- `@kit.DriverDevelopmentKit` 的 `deviceManager` 能查询、绑定和解绑真实外接设备及其驱动，SysCap 为 `SystemCapability.Driver.ExternalDevice`，须 `ohos.permission.ACCESS_EXTENSIONAL_DEVICE_DRIVER`，新版本绑定/解绑使用 `ohos.permission.ACCESS_DDK_DRIVERS`。后者是受限 ACL 权限，要求目标驱动服务端已上架或一并上架且能力匹配。它解决外设驱动通信，不是分布式设备协同 API。

官方来源：[分布式设备管理指南](https://developer.huawei.com/consumer/cn/doc/harmonyOS-guides/devicemanager-guidelines/)、[跨设备拖拽](https://developer.huawei.com/consumer/cn/doc/best-practices/bpta-distribute-drag-cast/)、[外设管理 API](https://developer.huawei.com/consumer/cn/doc/harmonyos-references/js-apis-driver-devicemanager/)、[受限权限列表](https://developer.huawei.com/consumer/cn/doc/harmonyOS-guides/restricted-permissions/)。

## 3. 键鼠输入与控制

普通 ArkUI 组件可以通过 `onMouse` 处理外接鼠标或触控板在本组件上的点击、移动等事件，SysCap 为 `SystemCapability.ArkUI.ArkUI.Full`；这不赋予全局键鼠监听或控制权。

- API 26 新增 `@kit.InputKit` 的 `inputEventClient`，可创建键盘、鼠标和触控控制器。SysCap 为 `SystemCapability.MultimodalInput.Input.InputSimulator`，需 `ohos.permission.CONTROL_DEVICE`，且仅 PC/2in1 可正常调用。
- `CONTROL_DEVICE` 是 `system_basic` 的受限 ACL 权限，授权方式为 `manual_settings`。官方只列出远程登录器被控端、需要辅助操控设备的智能体两类特殊场景，还须 AGC 审核，不能假设桌面宠物自动符合。
- API 20 的 Native 输入注入还需用户注入授权；API 26 的简化路径不能回溯为 API 20 的普遍能力。如果产品基线固定 API 20，应单独做真机验证，不能仅引用新接口。

官方来源：[鼠标事件](https://developer.huawei.com/consumer/cn/doc/HarmonyOS-references/ts-universal-mouse-key/)、[输入事件注入 API](https://developer.huawei.com/consumer/cn/doc/HarmonyOS-references/js-apis-inputeventclient/)、[Native 输入管理](https://developer.huawei.com/consumer/cn/doc/HarmonyOS-references/capi-oh-input-manager-h/)、[CONTROL_DEVICE 权限](https://developer.huawei.com/consumer/cn/doc/harmonyOS-guides/restricted-permissions/)。

## 4. 环境光、心率与其他传感器

`@kit.SensorServiceKit` 的 `sensor` 提供传感器查询和订阅，SysCap 为 `SystemCapability.Sensors.Sensor`。环境光 `AMBIENT_LIGHT` 返回 lux；心率 `HEART_RATE` 的订阅/单次读取需 `ohos.permission.READ_HEALTH_DATA`。

- API 是否列出某传感器不代表目标 PC、手机或穿戴设备一定有该硬件；应先查询设备传感器并设计无硬件降级。
- 心率是健康敏感数据，不应因桌面宠物的轻量展示需求而默认索取权限。
- 轻量设备文档中的 `@system.sensor` 使用 `SystemCapability.Sensors.Sensor.Lite`，应与标准 `@ohos.sensor` 分开评估。

官方来源：[传感器 API](https://developer.huawei.com/consumer/cn/doc/harmonyos-references/js-apis-sensor/)、[传感器概览](https://developer.huawei.com/consumer/cn/doc/HarmonyOS-guides/sensor-overview/)。

## 5. 屏幕亮度与常亮

普通公开方案是 `window.setWindowBrightness()`：仅影响自己的主窗口，并且主窗口前台、获焦时生效；退后台失效。官方明确说明当前没有直接设置系统屏幕亮度的接口，传入 `-1.0` 只是恢复为系统亮度。`setWindowKeepScreenOn()` 可保持当前前台窗口常亮，后台自动释放。

`@system.brightness` 虽提供亮度及模式读写，但属于 `SystemCapability.PowerManager.DisplayPowerManager.Lite`：Lite Wearable 长期维护；其他设备从 API 7 起不再维护，替代能力仅对系统应用开放。因此不能据此宣称手机或 PC 第三方应用可改全局亮度。

官方来源：[亮度与常亮控制](https://developer.huawei.com/consumer/cn/doc/HarmonyOS-Guides/brightness-control/)、[@system.brightness](https://developer.huawei.com/consumer/cn/doc/HarmonyOS-references/js-apis-system-brightness/)。

## 建议

对普通第三方桌面宠物，优先采用应用内交互、窗口亮度、通知/日历提醒和明确的跨设备拖拽；不要承诺全局亮度、自由远控键鼠或任意健康数据读取。若远程控制是核心功能，应先确认设备为 PC/2in1、最低 API 版本、远程登录器场景分类及 ACL/手动授权是否可获批，再安排真机验证。
