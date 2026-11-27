# HarmonyOS 应用市场发布指南（DeskMate）

> 适用范围：`apps/harmonyos-client`（HarmonyOS NEXT / API 26）
> 核对日期：2026-11-27
> 华为政策与 AGC 页面会更新，提交前请以页面提示为准。

## 1. 先看结论

当前工程已完成正式上架包改造，可打出 Release `.app` 包，且 `targetSdkVersion` 为 `26.0.0`，满足 AGC 自动签名条件。发布参数已确认如下：

- **上架名称**：`DeskMate`（如 AGC 创建应用时提示重名，再按页面提示调整）
- **正式包名**：`com.supremacy.deskmate`（已从探针包名 `com.deskmate.gate3` 替换，并同步脚本与文档；`probes/gate3` 保持原包名不变）
- **首发设备**：`tablet` + `phone`（平板与手机）
- **版本号**：`versionName: "1.6.0"` / `versionCode: 1006000`
- **合规与隐私**：已新增首次启动隐私同意门禁（同意前不枚举 USB 配件）与设置页「关于与隐私」页面；无网络权限，本次不申请软著（提交 AGC 承诺函）且无需 APP 备案（选「单机 APP」）
- **商店图标与演示视频**：已生成 `dist/harmonyos/store/app_icon_1024.png`（1024×1024 无透明底 PNG），并已备好 `dist/pad.mp4`（平板）与 `dist/phone.mp4`（手机）使用演示视频
- **待在 AGC 侧手动补齐**：真实手机与平板商店截图、自测 `.docx`（附 Host 下载链接）、AGC 版权承诺函与提审填写

## 2. 发布前 3 个已定决策

### 2.1 上架名称（已定：`DeskMate`）

- 首次上架名称定为 **`DeskMate`**。
- AGC 应用名称限 15 个中文字符或 30 个英文字符，不能堆砌“扩展屏/投屏/副屏”等通用关键词。
- 创建 AGC 应用时如提示重名或被商标投诉，再决定是否加前缀/后缀，并同步修改：
  - `apps/harmonyos-client/AppScope/resources/base/element/string.json`
  - `apps/harmonyos-client/entry/src/main/resources/base/element/string.json`
  - `apps/harmonyos-client/entry/src/main/ets/deskmate/PrivacyContent.ets` 及应用内标题

### 2.2 正式包名（已定：`com.supremacy.deskmate`）

- 已替换为：**`com.supremacy.deskmate`**（`apps/harmonyos-client/AppScope/app.json5:3`）。
- 包名创建 AGC 应用后不可修改，只影响安装/升级/签名，不影响用户看到的应用名称。
- 已同步修改位置：
  - `apps/harmonyos-client/AppScope/app.json5`
  - `apps/harmonyos-client/README.md`
  - `apps/macos-host/tools/device-foreground.sh`
  - `apps/macos-host/README.md`
- `probes/gate3` 保持 `com.deskmate.gate3` 不变，与正式包彻底隔离。

### 2.3 首发支持设备（已定：`tablet` + `phone`）

- `entry/src/main/module.json5:7-10` 声明 `tablet` 和 `phone`，本次首发同时支持平板与手机。
- 审核材料中已备好平板演示视频 `dist/pad.mp4`（2800×1840，约 79 秒，31 MB）与手机演示视频 `dist/phone.mp4`（890×1920，约 46 秒，13 MB），请在 AGC 上同时准备手机与平板两套真机截图。

## 3. 代码与打包改动清单

### 3.1 修改正式包名（已完成）

已更新 `apps/harmonyos-client/AppScope/app.json5` 为 `com.supremacy.deskmate`，并同步辅助脚本与文档。若设备上装有旧探针/测试包，安装新包前无需卸载旧 `gate3` 探针，两者已解耦。

### 3.2 增加「关于与隐私」入口（已完成）

已在 `apps/harmonyos-client/entry/src/main/ets/deskmate/PrivacyContent.ets` 与 `Index.ets` 中落地设置页「关于与隐私 → 关于 DeskMate 与隐私说明」页面，包含：

- 应用名称、版本号与包名（通过 `bundleManager.getBundleInfoForSelfSync` 动态读取，带兜底值）
- 本地隐私政策完整 7 条条款（不依赖联网即可查看）
- 开发者名称与联系邮箱（请确认 `PrivacyContent.ets` 中的 `DEVELOPER_NAME` / `CONTACT_EMAIL` 与华为开发者账号一致）
- 一句话承诺：画面与触控仅在本地 USB 连接中实时处理，不联网、不上传、不存储

### 3.3 首次启动隐私同意（已完成）

已在 `Index.ets` 中新增首次启动「隐私保护提示」门禁弹窗（`PersistentStorage` 持久化 `deskMatePrivacyAccepted`）：

- 展示隐私摘要、本地处理说明与「查看完整隐私政策」展开入口
- 只有用户点击「同意并继续」后才调用 `this.accessoryLink.start()` 启动 USB 配件枚举与会话轮询
- 点击「不同意」时停留在门禁页并保持停止状态，不枚举 USB 配件、不请求 USB 配件权限

### 3.4 准备 1024×1024 商店图标（已完成）

已扩展 `scripts/generate-app-icons.py` 支持 `--only-harmony-store --harmony-store-icon`，并生成无圆角、无透明通道（RGB PNG）的 1024×1024 商店图标：

```text
dist/harmonyos/store/app_icon_1024.png
```

### 3.5 打 Release `.app` 包与正式签名（已完成）

AGC 上传的是**已签名 Release `.app`**，不是 `.hap`，也不是未签名包。实测直接上传未签名 `.app` 会报错 **AGC 错误码 `991`（非法软件包）**；另外 DevEco Studio 的「Automatically generate signature（自动签名）」生成的只是**调试证书**，不能用于应用市场上架。

正式发布签名流程：

1. **生成密钥与 CSR（仓库外部目录）**：在 DevEco Studio 执行 `Build → Generate Key and CSR`，将 `.p12` 与 `.csr` 存放在仓库外（如 `~/deskmate-signing/`）。
2. **在 AGC 申请发布证书与发布 Profile**：
   - 在「证书管理」上传 `.csr`，类型选**发布证书**，下载 `.cer`。
   - 在「Profile 管理」为 `com.supremacy.deskmate` 申请**发布**类型 Profile，勾选上述发布证书，下载 `.p7b`。
3. **配置签名并绑定 Product（关键坑点）**：
   - 仅在 `apps/harmonyos-client/build-profile.json5` 的 `app.signingConfigs` 中写入证书材料**不够**，还必须在 `app.products[0]`（`"name": "default"`）中显式绑定 `"signingConfig": "default"`；否则 `hvigor` 会报 `WARN: No signingConfig found for product default` 并仍输出 `-unsigned.app`。
4. **编译并本地验签**：

```sh
cd apps/harmonyos-client

export DEVECO_SDK_HOME=/Applications/DevEco-Studio.app/Contents/sdk
export JAVA_HOME=/Applications/DevEco-Studio.app/Contents/jbr/Contents/Home
export PATH="$JAVA_HOME/bin:$PATH"

/Applications/DevEco-Studio.app/Contents/tools/hvigor/bin/hvigorw \
  assembleApp --mode project -p product=default -p buildMode=release --no-daemon

# 本地验签确认
/Applications/DevEco-Studio.app/Contents/jbr/Contents/Home/bin/java -jar \
  /Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/lib/hap-sign-tool.jar \
  verify-app -inFile build/outputs/default/harmonyos-client-default-signed.app
```

5. **归档与提交前清理**：
   - 将 `harmonyos-client-default-signed.app` 归档至 `dist/harmonyos/DeskMate-v1.6.0-harmonyos-signed.app` 并上传 AGC。
   - **提交 git 前务必还原签名配置**：`git checkout -- apps/harmonyos-client/build-profile.json5`，保持仓库内 `"signingConfigs": []`，切勿将 `.p12`/`.cer`/`.p7b` 或密文口令入库。

打包内 `module.json` 关键字段为：

```json
{
  "debug": false,
  "buildMode": "release",
  "versionName": "1.6.0",
  "versionCode": 1006000,
  "bundleName": "com.supremacy.deskmate"
}
```

### 3.6 版本号（已定：`1.6.0` / `1006000`）

首次上架使用 `versionCode 1006000` / `versionName 1.6.0`；后续每次提交必须递增 `versionCode`（例如 1.6.1 用 `1006001`），不能重复。

## 4. 账号、资质与合规

### 4.1 华为开发者账号

在华为开发者联盟注册并完成实名认证（个人或企业）。个人开发者可以发布免费应用；如果以后要做付费/企业主体，建议一开始就用企业账号，避免迁移。

### 4.2 软件著作权 / 电子版权（本次不申请）

华为官方《应用资质审核要求》把软件著作权列为**非必选资质**。本次发布不申请软著与电子版权，直接在 AGC 版权信息页上传页面提供的**版权承诺函**即可。

### 4.3 APP 备案（本次不申请，选「单机 APP」）

DeskMate 鸿蒙端没有申请网络权限，所有画面和触控都走本地 USB，不依赖服务器、账号或互联网，属于单机应用。AGC 备案信息中选择「单机 APP」即可，官方现行口径是单机应用无需填写备案号。

前提是不要加入以下功能，否则会被视为联网应用：

- 检查更新、遥测/统计、崩溃上报
- 账号登录、云同步
- 任何 HTTP/WebSocket 请求

隐私声明托管本身不算业务联网，官方 FAQ 对此有说明。

### 4.4 隐私政策

AGC 提供隐私声明托管；没有自己的网站时优先用托管。注意：

- 隐私政策中的开发者名称、应用名称必须与 AGC 提交信息一致。
- 隐私标签选择「不收集个人信息」；第三方 SDK、广告、账号都选无。
- 政策要明确写出：USB 配件信息仅用于建立本地连接，画面/触控仅在内存中实时处理，不落盘、不上传、不共享。

可直接参考第 9 节的隐私政策模板。

### 4.5 版权承诺函

AGC 版权信息页通常要求上传承诺函（模板由页面提供）。如果上传软著/电子版权，再按页面要求补充证书；权利人不是开发者本人/本公司时，补充授权书。

## 5. 商店素材

### 5.1 应用图标

- 216×216 或 1024×1024 PNG
- 正方形，建议无圆角、无透明边
- 文件不超过 3 MB
- 不要包含华为 Logo、设备边框、应用市场角标

### 5.2 截图

官方素材规范（以 AGC 上传栏提示为准）：

- 手机：3-10 张，竖屏 `9:16`（最低 `1080×1920`）或横屏 `16:9`（最低 `1920×1080`）；PNG/JPG/JPEG 单张 ≤ 5 MB，WEBP ≤ 200 KB（不可为动图）
- 平板：3-10 张，3:2（1920×1280）或 2:3（1280×1920）
- 必须是真实应用界面，不要加设备外框、水印、虚假宣传文案
- 如果同时声明手机+平板，建议两套都准备

实操处理经验（v1.6.0 实测）：

1. **全面屏手机截图比例换算**：真机竖屏截图（如 `1320×2848`，约 `9:19.5`）比 `9:16` 更修长，不要直接拉伸，应优先收窄页面下半部分的纯色留白带以凑齐 `1080×1920`。
2. **横屏投屏图混排进 9:16 竖屏槽位**：若将横屏扩展桌面截图（如 `2848×1320`）与竖屏图一起上传到 `9:16` 槽位，建议等比缩放到 `1080` 宽并居中置于深色背景（`#111214`）画布上、上下加 1px 分割线，避免旋转 90° 或裁掉左上/右上角控件。
3. **始终从原始高分辨率图直接缩放**：不要对已经缩小过的截图做二次放大，否则文字边缘发虚。

建议截图内容：

1. 首页「连接数据线即可扩展」状态
2. 扩展桌面全屏画面（最能体现价值）
3. 设置页（编码、性能浮窗）
4. 诊断页或关于/隐私页

### 5.3 介绍视频与自测文件

这是本应用最重要的审核材料。华为官方允许在「审核信息」中上传最多 5 个 `.docx` / `.mp4`，总大小不超过 500 MB，用于说明需要特殊设备/环境才能使用的功能。

`dist/` 目录下已准备好手机与平板两段实拍使用视频，可直接作为审核视频上传：

- `dist/pad.mp4`（平板端使用演示，2800×1840，约 79 秒，31 MB）
- `dist/phone.mp4`（手机端使用演示，890×1920，约 46 秒，13 MB）
- 另建议补充 1 个 `.docx` 自测说明（测试环境、步骤、预期结果、Host 下载地址）

视频脚本参考：

1. 电脑端 DeskMate Host 已启动
2. USB-C 线连接电脑和平板
3. 平板打开 DeskMate，系统弹出 USB 配件授权，点击允许
4. 电脑端选择「扩展」模式
5. 平板出现扩展桌面，把电脑窗口拖到平板
6. 在平板上点击、拖动、双指滚动，电脑端有响应
7. 打开设置/诊断，退出全屏再返回
8. 结尾说明：不联网、不收集/上传屏幕内容

审核备注建议：

> 本应用为电脑端 DeskMate 的配套接收端，需配合 macOS/Windows 主机和 USB-C 数据线使用；完整演示见自测视频。应用不申请网络权限，不收集个人信息；USB 授权仅用于建立本地配件连接。

### 5.4 给审核员的 Host 软件

审核员通常没有你的电脑环境。建议把 Windows 便携版放到一个稳定、国内可访问的链接（华为云 OBS、Gitee Release 等），在自测 `.docx` 中给出下载地址和版本号。

**不要把下载链接放进鸿蒙应用内**，避免被认定为引导用户绕过应用市场下载外部应用。

## 6. AGC 填写顺序

1. 登录 AGC，完成开发者实名认证。
2. 「我的应用」→「创建应用」→ 选择 HarmonyOS → 填写应用名称、包名、设备类型、分类。
3. 进入「应用信息」→「准备提交」→ 上传 Release `.app`。
4. 填写应用信息：名称、图标、截图、简介、详细介绍、分类、标签、年龄分级。
5. 填写 AI 能力声明：**不涉及**。
6. 填写隐私标签/隐私政策：不收集个人信息；使用华为隐私托管或自建 URL。
7. 填写版权信息：承诺函；有软著/电子版权则上传。
8. 填写备案信息：单机 APP。
9. 上传审核信息：自测 `.docx` + 演示 `.mp4`，填写审核备注。
10. 填写版本说明（参考第 8 节）。
11. 提交审核；审核通过后选择手动发布或定时发布。

常用字段建议：

| 字段 | 建议 |
| --- | --- |
| 应用类型 | 应用（不是元服务） |
| 运行平台 | HarmonyOS |
| 设备类型 | 手机 + 平板（`phone` + `tablet`） |
| 分类 | 实用工具 / 效率（避免选需要额外资质的远程控制类目） |
| 年龄分级 | 3+ |
| 广告 | 无 |
| 内购/数字商品 | 无 |
| 账号/登录 | 无 |
| AI 功能 | 不涉及 |
| 隐私标签 | 不收集个人信息 |
| 备案 | 单机 APP |

## 7. 审核风险与应对

### 7.1 核心功能依赖外部 Host

这是最大风险。应用单独打开只能看到连接引导，审核员无法验证扩展屏和反向触控。**必须**用自测视频 + `.docx` 证明完整链路，并提供可运行的 Host 版本。

### 7.2 宣传与实测不一致

`README.md:5` 写的是“低延迟、高刷新率”，但 `apps/harmonyos-client/README.md:64` 记录“实测约 9 fps 尚未优化”。如果这个数据仍属实：

- 商店文案不要写“高刷/低延迟/120Hz”，改为“有线连接、稳定传输、反向触控”。
- 先优化再上架，否则用户评价和审核都可能质疑性能。
- 如果 9 fps 已过时，请更新验证记录，并用实测数据支撑文案。

### 7.3 手机兼容性

如果首发只测了平板，建议先只声明 `tablet`。手机布局代码存在不代表真机通过；审核员用手机测试时，USB 授权、横屏、全屏返回都可能出问题。

### 7.4 名称与商标

`DeskMate` 已有同名产品。上架前：

- 在 AGC 创建应用页试填名称，确认是否被占用。
- 公开查一下商标状态；如果存在冲突，换一个更独特的名字。
- 如果上传软著，确保软著名称/简称与最终上架名一致。

### 7.5 隐私同意顺序

如果保留当前“启动即枚举 USB”的逻辑，审核备注要写明不收集个人信息；更稳妥的做法是加首次启动隐私同意，把 `accessoryLink.start()` 放到同意之后。

### 7.6 不要在应用内引导下载 Host

可以说明“需配合电脑端 DeskMate 使用”，但不要把 GitHub/网盘下载按钮放进应用。Host 下载地址放在自测文档和项目官网/README 中。

## 8. 商店文案（可复制）

### 8.1 一句话简介（AGC 限制 17 字以内含标点）

- 推荐（16 字）：
  ```text
  一根数据线，把平板手机变电脑副屏
  ```
- 备选：
  - `有线扩展电脑副屏，支持反向触控`（15 字）
  - `USB有线直连，鸿蒙变身电脑副屏`（16 字）

### 8.2 应用介绍

```text
DeskMate 是一款有线扩展显示与反向触控工具，只需一根支持数据传输的 USB-C 数据线，即可将你的鸿蒙平板或手机变成 macOS / Windows 电脑的第二块显示器，轻松拓展桌面工作空间。

【核心功能】
• USB 有线扩展桌面：画面经由 USB 本地直连传输，不依赖 Wi-Fi 或局域网环境，连接稳定、抗干扰。
• 反向触控操作：支持在平板或手机屏幕上直接点击、拖动与双指滚动，像操作触控屏一样自然控制电脑窗口。
• 平板与手机双形态适配：平板端提供宽屏桌面控制台；手机端连接前保持竖屏便于设置，进入扩展画面后自动切换横屏并支持左右翻转。
• 硬件解码与画面设置：支持自动 / HEVC (H.265) / H.264 编码偏好切换，内置实时性能浮窗（帧率、码率、编码格式与丢帧统计）以及连接诊断页。
• 即插即用体验：连接数据线并确认系统 USB 配件授权后自动建立会话，全屏模式下可通过左上角折叠工具栏或系统返回随时切换回主页，界面支持跟随系统深浅色切换。

【使用前提】
1. 电脑端需配合安装并运行 DeskMate 桌面主机端（支持 macOS / Windows）。
2. 请使用支持数据传输功能的 USB-C 数据线连接电脑与鸿蒙设备。
3. 需在 HarmonyOS 6.1.1 及以上系统设备上运行。

【隐私与安全说明】
DeskMate 严格遵循本地化处理原则：不申请网络权限，无账号登录、无广告、无第三方统计 SDK，不收集、不上传、不存储您的屏幕画面、触控数据或任何个人信息，所有音视频与触控数据仅在本地 USB 连接的内存中实时处理。
```

### 8.3 版本说明

```text
DeskMate 首个 HarmonyOS 正式版本（1.6.0）：

• 支持 HarmonyOS 平板与手机通过 USB-C 作为 macOS / Windows 电脑的有线扩展副屏
• 支持 HEVC / H.264 硬件解码与反向触控（点击、拖动、双指滚动）
• 支持手机出画自动横屏、系统深浅色自适应、实时性能浮窗与连接诊断
• 内置首次启动隐私提示与本地隐私说明页，全程无网络权限、不收集个人信息
```

## 9. 隐私政策模板

> 应用内 `PrivacyContent.ets` 已内置以下条款；提交 AGC 前请确认开发者名称与联系邮箱与您的华为开发者账号一致。

```text
DeskMate 隐私政策

更新日期：2026-11-27
开发者：SupremacySakura
应用名称：DeskMate

1. 我们收集的信息
DeskMate 不收集、不上传、不存储您的个人信息。应用不申请网络权限，不接入广告、统计分析或第三方 SDK。

2. 本地处理的数据
为实现有线扩展屏功能，应用会通过 USB 配件连接接收电脑端传输的画面，并将您在手机或平板上的触控坐标回传给电脑端。上述数据仅在本地内存中实时处理，不写入本地存储，不通过互联网传输，不与我们或任何第三方共享。

3. 设备与权限
应用会在您同意本隐私提示后访问 USB 配件列表并请求 USB 配件授权，用于建立本地有线连接。授权由系统弹窗完成，您可以随时拒绝或断开 USB 数据线。应用不申请位置、通讯录、相册、麦克风、摄像头等权限。

4. 数据共享与跨境传输
我们不共享、不转让、不公开披露您的数据，也不进行数据跨境传输。

5. 未成年人保护
应用不面向未成年人单独收集信息。

6. 您的权利
您可以随时断开 USB 或卸载应用以停止数据处理；如有疑问，请联系 3118654731@qq.com。

7. 政策更新
如本政策发生变更，我们会在应用内或应用市场页面同步更新。
```

## 10. 自测说明 `.docx` 模板

```text
DeskMate HarmonyOS 客户端自测说明

应用名称：DeskMate
包名：com.supremacy.deskmate
版本：1.6.0（versionCode 1006000）

一、测试环境
电脑：Windows 11 x64 / macOS [版本]
Host：DeskMate Host 1.6.0，下载地址：[可访问链接]
设备：华为平板 / 华为手机，HarmonyOS [版本]
线缆：支持数据传输的 USB-C 线

二、前置条件
1. 电脑端 DeskMate Host 已安装并启动。
2. 手机或平板已安装本应用。
3. 首次启动同意“隐私保护提示”。
4. 设备通过 USB-C 连接电脑。

三、测试步骤与预期结果
1. 打开 DeskMate，首次启动阅读隐私提示并点击“同意并继续”。
2. 首页显示“连接数据线即可扩展”，电脑端识别设备后系统弹出 USB 配件授权。
3. 点击“允许”，应用显示“正在准备画面”。
4. 电脑端选择“扩展”模式，设备端出现扩展桌面（手机端出画自动横屏）。
5. 在设备上点击、拖动、双指滚动，电脑端响应正常。
6. 打开“编码与浮窗设置”，切换 HEVC/H.264，画面可恢复。
7. 打开“关于 DeskMate 与隐私说明”，可离线查看版本信息与隐私政策。
8. 进入全屏后，左上角工具栏可退出；系统返回可回首页；拔插 USB 可恢复连接。

四、已知限制
1. 仅支持单指鼠标等价输入与双指滚动；键盘、压感未接入。
2. 退出 Host 后可能需要拔插 USB 恢复。
3. 帧率随设备、分辨率和内容变化，不以固定高刷作为承诺。

五、附件
pad.mp4：平板端完整连接、扩展、触控、退出流程演示。
phone.mp4：手机端完整连接、扩展、横屏自适应与触控流程演示。
```

## 11. 首次提审实操避坑总结（v1.6.0 经验）

1. **上传 `.app` 报错 `991`（非法软件包）**：不能上传 `-unsigned.app`，也不能用 IDE 自动签名的 Debug 证书；必须使用 AGC 申请的**发布证书（`.cer`）+ 发布 Profile（`.p7b`）+ 本地 `.p12`** 打出 `-signed.app`。
2. **配了 `signingConfigs` 仍打出未签名包**：检查 `build-profile.json5` 的 `app.products[0]` 是否绑定了 `"signingConfig": "default"`；只写 `signingConfigs` 不在 `products` 引用时，`hvigor` 会跳过签名并报 `No signingConfig found for product default`。打包完成后记得还原 `build-profile.json5` 再提交 git。
3. **一句话简介超长被拦截**：AGC 后台对「一句话简介」有 **17 字（含标点）** 严格限制，提前把简介压缩到 15-16 字以内。
4. **截图比例与清晰度**：9:16 竖图要求严格 `1080×1920`（≤ 5 MB）。修长手机原图可裁减底部纯白留白带；横屏投屏图可居中放进 `#111214` 竖屏画布避免裁切控件；务必从原始高分截图直接转换，避免二次放大糊字。
5. **开发者实名主体与隐私说明一致**：发布证书主体会显示华为账号实名名称（个人账号为个人姓名）。提审前确认 `PrivacyContent.ets` 中的 `DEVELOPER_NAME` 与 AGC 隐私政策页保持一致。

## 12. 官方链接

- 提交入口：https://developer.huawei.com/consumer/cn/app/submit/
- 创建 HarmonyOS 应用：https://developer.huawei.com/consumer/cn/doc/app/agc-help-create-app-0000002247955506
- 发布应用指引：https://developer.huawei.com/consumer/cn/doc/App/agc-help-release-app-guide-0000002287176372
- 填写审核信息：https://developer.huawei.com/consumer/cn/doc/app/agc-help-release-app-reviewinfo-0000002320780365
- 素材规范：https://developer.huawei.com/consumer/cn/doc/App/agc-help-app-visual-asset-spec-0000002277607976
- 自动签名（targetSdkVersion 26.0.0+）：https://developer.huawei.com/consumer/cn/doc/app/agc-help-autosign-0000002270851321
- 上传应用包：https://developer.huawei.com/consumer/cn/doc/app/agc-help-uploadpackage-0000001945392297
- 隐私政策配置：https://developer.huawei.com/consumer/cn/doc/app/agc-help-privacypolicy-0000001945392297
- 应用版权/著作权要求：https://developer.huawei.com/consumer/cn/doc/app/50111-02
- 应用资质审核要求：https://developer.huawei.com/consumer/cn/doc/app/80301
- 应用审核指南：https://developer.huawei.com/consumer/cn/doc/app/50104-01
- APP 备案 FAQ：https://developer.huawei.com/consumer/cn/doc/app/50130

