# DeskMate Device (HarmonyOS)

产品端设备应用：以 USB Accessory（AOA）接收 Mac 发来的视频分片，解码到
`XComponent` Surface 显示，并把触控事件回传给 Host。

- Bundle：`com.deskmate.gate3`
- 目标设备：`tablet`（实测 MatePad Pro `MRDI-W00`，OpenHarmony 7.0.0.111 / API 26）

## 构建

需要 DevEco Studio 的 SDK、hvigor 与自带 JBR。命令行方式：

```sh
cd apps/device

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

先启动 Host 侧的 AOA 前台保活（平板切到 accessory 模式后 ArkTS 定时器会被挂起，
必须靠本地 `aa start` 把应用拉回前台）：

```sh
cd ../host
make device-foreground HDC=$HDC BUNDLE=com.deskmate.gate3
```

然后再启动 Host 应用。切换 AOA 时平板会弹一次「允许访问 USB 配件」，需手动允许。

## 结构

```text
entry/src/main/ets/deskmate/AccessoryLink.ets   ArkTS 侧 USB 授权、handle 所有权
entry/src/main/ets/pages/Index.ets              XComponent 与状态栏
entry/src/main/cpp/device_engine.cpp            会话、调度、输入采集
entry/src/main/cpp/video_decoder.cpp            OH_VideoDecoder → Surface
entry/src/main/cpp/fd_byte_io.hpp               以 POLLIN 为许可的字节 I/O
```

## 已知限制

- 只处理单指鼠标等价输入与双指滚动；键盘与压感未接入。
- 退出 Host 不会把平板送回 HDC 模式，恢复方式是拔插 USB。
- 帧率偏低（实测约 9 fps）尚未优化，不构成性能结论。
