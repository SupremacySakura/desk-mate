# DeskMate Device (HarmonyOS)

产品端设备应用：以 USB Accessory（AOA）接收 Mac 发来的视频分片，解码到
`XComponent` Surface 显示，并把触控事件回传给 Host。

- Bundle：`com.supremacy.deskmate`
- 目标设备：`tablet`、`phone`（首发支持平板与手机；平板实测 MatePad Pro `MRDI-W00`，OpenHarmony 7.0.0.111 / API 26）

## 构建

需要 DevEco Studio 的 SDK、hvigor 与自带 JBR。命令行方式：

```sh
cd apps/harmonyos-client

export DEVECO_SDK_HOME=/Applications/DevEco-Studio.app/Contents/sdk
export JAVA_HOME=/Applications/DevEco-Studio.app/Contents/jbr/Contents/Home
export PATH="$JAVA_HOME/bin:$PATH"

/Applications/DevEco-Studio.app/Contents/tools/hvigor/bin/hvigorw \
  assembleHap --mode module -p product=default -p buildMode=debug --no-daemon
```

产物：`entry/build/default/outputs/default/entry-default-unsigned.hap`。

仓库内的 `build-profile.json5` 保持 `"signingConfigs": []`，**签名材料与口令不入库**。
装真机时请在 DevEco Studio 里用自己的签名配置构建，或用本地临时配置签名后再安装。

## 安装与运行

```sh
HDC=/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc

$HDC install entry/build/default/outputs/default/entry-default-signed.hap
```

从平板桌面打开 DeskMate，再打开 Mac 应用并接上 USB 数据线。在系统提示中允许 USB 配件连接。
如果切换到 AOA 模式时平板应用退到后台，请从平板桌面重新打开。当前版本尚未真机验收无终端流程。
如果重新打开仍无法恢复轮询，可使用既有的开发辅助脚本；需要在平板仍处于 HDC 模式时启动：

```sh
cd ../macos-host
make device-foreground HDC=$HDC BUNDLE=com.supremacy.deskmate
```

平板以原生解码器成功输出到 Surface 作为进入全屏的触发信号。全屏左上角可展开工具栏并返回首页；
也可使用系统返回。同一次连接中主动退出全屏后，可点「返回画面」。设置中的帧率来自平板解码输出计数，
不是协商刷新率，也不是系统合成器的实测呈现帧率。

## 结构

```text
entry/src/main/ets/deskmate/AccessoryLink.ets   ArkTS 侧 USB 授权、handle 所有权
entry/src/main/ets/pages/Index.ets              首页、设置、诊断与常驻的 XComponent Surface
entry/src/main/cpp/device_engine.cpp            会话、调度、输入采集
entry/src/main/cpp/video_decoder.cpp            OH_VideoDecoder → Surface
entry/src/main/cpp/fd_byte_io.hpp               以 POLLIN 为许可的字节 I/O
```

## 已知限制

- 只处理单指鼠标等价输入与双指滚动；键盘与压感未接入。
- 退出 Host 不会把平板送回 HDC 模式，恢复方式是拔插 USB。
- 帧率偏低（实测约 9 fps）尚未优化，不构成性能结论。

## 发布到华为应用市场

发布流程、AGC 填写项、素材规格、隐私政策与自测说明模板见
[docs/harmonyos-appgallery-release.md](../../docs/harmonyos-appgallery-release.md)。
