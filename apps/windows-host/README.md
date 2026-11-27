# DeskMate Windows Host

Windows 主控端：提供 **Alabaster Studio** 自绘工作台界面（Direct2D / DirectWrite + 系统托盘），持续发现 USB AOA 设备、管理 **Parsec VDD / MttVDD** 独立虚拟扩展屏、以 DXGI Desktop Duplication / Media Foundation 硬编推流，并将 Android / HarmonyOS 回传触控通过 `SendInput` / `InitializeTouchInjection` 注入系统。

## 前置驱动配置（Windows 真机必读）

### 1. 使用 Zadig 安装 USB AOA（WinUSB）驱动

Windows 默认不会为切换到 AOA 配件模式的手机/平板绑定 WinUSB 驱动，导致 `libusb` 无法打开 Bulk 通道：

1. 运行发行压缩包内置的 `drivers\zadig-2.9.exe`（或点击工作台「启动 Zadig」，也可从 [**Zadig**](https://zadig.akeo.ie/) 下载），以管理员身份运行。
2. 用 USB-C 数据线连接手机/平板，打开两端 DeskMate 触发一次 AOA 切换（设备重枚举为 `18D1:2D00` 或 `18D1:2D01`）。
3. 在 Zadig 菜单勾选 **Options → List All Devices**。
4. 选择设备对应的 **Android Accessory / USB AOA 接口（`18D1:2D00` 或 `18D1:2D01`，Interface 0）**。
5. 将右侧目标驱动选为 **WinUSB**，点击 **Install Driver / Replace Driver**。
6. （可选替代）也可直接安装 [**UsbDk**](https://github.com/daynix/UsbDk/releases) 驱动；连接时请确认没有 `hdc` / `adb` / 厂商手机助手抢占接口。

### 2. 安装 Parsec VDD 创建独立虚拟扩展屏

1. 运行发行压缩包内置的 `drivers\parsec-vdd-0.45.0.0-setup.exe`（或点击工作台「安装驱动」，也可从 [**Parsec VDD Releases**](https://github.com/nomi-san/parsec-vdd/releases) 下载）安装 **Parsec Virtual Display Driver**（拥有微软 WHQL 正式签名，无需开启 Windows 测试模式；同时兼容 **MttVDD / Virtual Display Driver**）。
2. 安装后**不要长期开启手动的 `ParsecVDisplay` 界面**——`DeskMateHost.exe` 会直接通过 `VirtualDisplayManager`（Parsec VDD IOCTL + 100ms 保活 / MttVDD 命名管道）按需自动创建与销毁独立虚拟扩展屏，并自动错开排布在现有物理显示器旁（自动模式下绝不会抢占电脑已有的物理副屏）。
3. 启动 `DeskMateHost.exe` 后进入 **「设置 → 系统环境与驱动」**，即可查看 USB / WinUSB 与 Parsec VDD 驱动就绪状态。

## 运行方式

不带参数直接双击 `DeskMateHost.exe` 会打开 **Alabaster Studio** 工作台（含拓扑布局图、在线副屏管理、系统托盘与诊断日志）；同时保留命令行开关：

- `--headless`：无界面运行，仅链路与采集
- `--list-monitors`：打印当前可采集显示器（标注虚拟/主屏信息）后退出
- `--width N --height N --fps N`：期望分辨率与刷新率
- `--codec hevc|h264`：首选编码格式
- `--no-auto-match`：关闭按设备协商尺寸自动匹配
- `--help`：打印命令行用法

## 构建

### 方式 A：Visual Studio 2022 + CMake + vcpkg

依赖：
- Visual Studio 2022（Desktop C++ / Windows SDK 10.0.22621+）
- CMake 3.20+
- libusb 1.0（`vcpkg install libusb:x64-windows`）

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake `
  -DDESKMATE_BUILD_WINDOWS_HOST=ON
cmake --build build --config Release
```

未找到 libusb 时 CMake 会退化为 stub 构建：链路/Session 逻辑仍可编译和测试，但 `LibusbAoaDiscovery::initialized()` 返回 false。

### 方式 B：无 Visual Studio / CMake 的便携构建

可用便携 `llvm-mingw` 工具链（clang + lld + Windows SDK 头文件 + UCRT）配合 `libusb` 源码直接静态编译出单文件 `DeskMateHost.exe`（无 DLL 依赖）：

```powershell
powershell -ExecutionPolicy Bypass -File apps/windows-host/build-portable.ps1 `
  -ToolchainDir C:\dm\llvm-mingw-20261117-ucrt-x86_64 `
  -LibusbSource C:\dm\libusb-1.0.30 `
  -OutputDir    C:\dm\build
```

### 方式 C：从 macOS 驱动局域网 Windows 构建机（本仓库默认流程）

Windows 产物无法在 macOS 上交叉编译。本仓库固定使用一台局域网 Windows 构建机（`Administrator@192.168.43.4`，已配置公钥免密）远程编译，一条命令完成上传、全量编译、取回与打包：

```bash
bash scripts/build-windows-remote.sh --verify
```

地址、工具链路径、SSH 调用规范与故障恢复见 [docs/windows-remote-build.md](../../docs/windows-remote-build.md)。

## 契约测试（任意宿主机）

```sh
bash apps/common/tests/run_platform_tests.sh
```

其中 `aoa-session-tests` 验证多设备隔离、每链路视频路由、12 KiB 分片与 AOA UTF-8 编码，`windows-contract-tests` 验证 `SendInput` 状态机（主键归属、拖拽、取消），`windows-host-smoke` 确保非 Windows 宿主下共享链路与状态代码也可编译。

## 传输与媒体架构

- **USB AOA 发现与传输**：非阻塞后台发现与热插拔重连，持续扫描 `18d1:2d00..2d05` 并自动将 `12d1:5000`（HDC）或复合接口切换为 AOA；视频分片固定 **12 KiB 载荷**（32 B 头 + 12 KiB < 16 KiB 读缓冲）；Bulk IN 独立读线程，退出仅 `release_interface` + `close`。
- **虚拟扩展屏（`VirtualDisplayManager`）**：集成 Parsec VDD IOCTL（含 100ms keep-alive 与零闪烁状态保持）及 MttVDD 命名管道；自动启用非活跃扩展输出并通过 `SetDisplayConfig` 无重叠排布拓扑（支持左/右/上/下拓扑切换）。
- **画面采集与编码**：DXGI Desktop Duplication（`IDXGIOutputDuplication`）+ D3D11 VideoProcessor（BGRA → NV12 并缩放）+ GDI 静态回退（含光标绘制）；Media Foundation 硬件 HEVC / H.264 编码，统一输出 Annex-B（IDR 前附带 VPS/SPS/PPS 或 SPS/PPS），支持按设备 `REQUEST_IDR`。
- **四模式反向触控注入**：支持 **兼容手势映射**、**仅单点触控**、**虚拟触控板** 及 **原生多点触控**（基于 Win32 `SendInput` 与 `InitializeTouchInjection`），自动按独占显示器的工作区绝对坐标精确映射。
- **应用内自动更新（`WinHTTP` + Ed25519）**：从 GitHub Releases 拉取带签名的 `update-manifest.json`，校验 SHA-256 与 Ed25519 数字签名后原地重命名替换 `DeskMateHost.exe` 并重启（不会影响已安装的 WinUSB 与 Parsec VDD 驱动），支持自定义 GitHub 加速前缀。
