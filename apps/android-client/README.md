# DeskMate Android Client

Android 副屏客户端：通过 `UsbManager.openAccessory()` 接入 AOA，NDK 侧复用
`core/` 的 `DeviceSession`，MediaCodec 直接解码到 `SurfaceView`，并把
`MotionEvent` 多点触控归一化为 28 字节 Basic Input。

## 构建

要求：

- Android Studio（AGP 8.7 / Gradle 8.9）
- Android SDK 35
- NDK 27+、CMake 3.22.1（SDK Manager 安装）

```sh
cd apps/android-client
./gradlew :app:assembleDebug
```

`local.properties` 指向本机 SDK；签名配置按 Android Studio 默认调试签名。
首次需在 Android Studio 中生成 Gradle wrapper，或执行
`gradle wrapper --gradle-version 8.9`。

### 离线打包（无需 Gradle / Maven）

没有 Gradle 或网络时，用 SDK build-tools + NDK 直接产出可安装的签名 APK：

```sh
ANDROID_HOME=~/Library/Android/sdk NDK=~/Library/Android/sdk/ndk/27.2.12479018 \
  bash apps/android-client/package_apk.sh
# -> apps/android-client/app/build/outputs/apk/debug/app-debug.apk
```

脚本依次执行 `aapt2 compile/link` → `javac` → `d8` → NDK clang++ → `zip`
→ `zipalign -P 16` → `apksigner`。可用 `ABIS="arm64-v8a"` 只打单一 ABI。

注意：`core/lifecycle.cpp` 使用 `__int128`，32 位 ARM 无法编译，因此默认
只打 `arm64-v8a` 与 `x86_64`（真机用前者，模拟器用后者）。

调试密钥保存在 `app/build/debug.keystore` 并跨构建复用；删除它会导致新包
无法覆盖安装已安装的旧版本，需先卸载。

## 宿主侧契约测试

```sh
bash apps/common/tests/run_android_io_test.sh
```

其中 `run_android_io_test.sh` 用宿主编译器验证配件 fd 语义：`POLLIN` 门控、
非阻塞部分写返回精确前缀、取消唤醒。NDK 编译仍需在装有 NDK 的机器上执行。

## 结构

```text
app/src/main/cpp/accessory_io.cpp          fd 借用 + POLLIN 门控 + 无锁原子计数
app/src/main/cpp/client_engine.cpp         DeviceSession、输入队列、Surface 生命周期
app/src/main/cpp/video_decoder.cpp         AMediaCodec 双线程低延迟解码 → Surface
app/src/main/java/.../AccessoryLink.java   USB 权限与 handle 所有权
app/src/main/java/.../DeviceLayout.java    平板/手机形态判断、竖屏/自动横屏与安全区
app/src/main/java/.../MainActivity.java    Alabaster Studio 四页响应式 UI、SurfaceView 与 MotionEvent
```

## 约束

- Java 独占 `ParcelFileDescriptor`；停止顺序是 `requestStop()` → `close()` →
  `stop()`，用 `POLLHUP` 唤醒阻塞读。
- 视频按协议最多 64 片、每片 12 KiB；解码器只在 `Streaming` 且收到首帧 IDR 后工作。
- `ANativeWindow_fromSurface` 的引用由 `ClientEngine` 在 worker 线程接管；Surface
  重建时先关闭解码器再释放旧窗口，USB 会话保持不断开。
- 触控按整个 `MotionEvent` 快照转发（`NativeBridge.touchFrame`），
  `TouchTracker` 通过快照差分生成 down/move/up，避免逐指转发把其他触点误判为抬起。

## 已知边界

- 已在 Android 真机上跑通 USB-C AOA 授权、分辨率协商、MediaCodec 硬解画面输出与多点触控。
- 离线脚本已用 NDK r27c 实际编译、签名并校验（`aapt2 dump badging`、`apksigner verify`、`zipalign -c -P 16`、`ELF max-page-size=16384`）；Gradle 路径需本机配置 AGP 依赖后执行。
- 触控未实现压感与手写笔协议；多点触控按触点快照透传，最多 10 个触点。
