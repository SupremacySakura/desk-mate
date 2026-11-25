<div align="center">

# DeskMate

**Turn HarmonyOS and Android tablets/phones into low-latency, high-refresh-rate wired extended displays for macOS and Windows — with multi-device support and reverse touch input.**

[![macOS](https://img.shields.io/badge/macOS-15%2B-111111?style=flat-square&logo=apple&logoColor=white)](./apps/macos-host)
[![Windows](https://img.shields.io/badge/Windows-10%2F11%20x64-0078D6?style=flat-square&logo=windows&logoColor=white)](./apps/windows-host)
[![HarmonyOS NEXT](https://img.shields.io/badge/HarmonyOS-NEXT%206.1%2B-0A59F7?style=flat-square)](./apps/harmonyos-client)
[![Android](https://img.shields.io/badge/Android-8.0%2B-3DDC84?style=flat-square&logo=android&logoColor=white)](./apps/android-client)
[![USB AOA](https://img.shields.io/badge/Transport-USB--C%20AOA-9D193D?style=flat-square)](#architecture)
[![Codec](https://img.shields.io/badge/Video-HEVC%20%2F%20H.264%20up%20to%20120Hz-2D7D5B?style=flat-square)](#features)
[![Stack](https://img.shields.io/badge/Stack-C%2B%2B20%20%C2%B7%20Swift%20%C2%B7%20Win32%2FD2D%20%C2%B7%20ArkTS%20%C2%B7%20Android-5A545E?style=flat-square)](#project-layout)
[![License](https://img.shields.io/badge/License-PolyForm%20Noncommercial%201.0.0-B9975B?style=flat-square)](./LICENSE)

English | [简体中文](./README.zh-CN.md)

</div>

---

<div align="center">
  <img src="./docs/assets/screenshot-multi-display.jpg" alt="DeskMate running with two HarmonyOS devices connected simultaneously as extended displays" width="880" />
  <br />
  <sub>
    <i>Dual-device live session: <b>HUAWEI MatePad Pro 12.2</b> (left, 2800×1840 @ 120 Hz) and <b>HUAWEI Mate 80 Pro Max</b> (bottom, 2848×1320 @ 120 Hz) connected simultaneously over USB-C AOA with hardware-accelerated HEVC streaming.</i>
  </sub>
</div>

---

## Why DeskMate?

DeskMate transforms your HarmonyOS or Android tablet/smartphone into a crisp, responsive secondary monitor for **macOS and Windows** over a single USB-C cable — **zero Wi-Fi dependency, zero cloud relay, and native resolution output**.

Unlike generic wireless casting tools, DeskMate uses a dedicated custom binary protocol over **USB Accessory (AOA)** bulk transfers, paired with **ScreenCaptureKit + VideoToolbox + `CGVirtualDisplay`** on macOS, **DXGI Desktop Duplication + Media Foundation + Parsec VDD** on Windows, and native **C++ / AVCodec / MediaCodec** hardware decoding on mobile devices.

---

## Features

| Capability | Description |
| --- | --- |
| **Wired USB-C AOA Direct Link** | Full-duplex USB bulk transfers (`usb_byte_io` + `fd_byte_io`), zero Wi-Fi jitter, works offline anywhere. |
| **macOS & Windows Hosts** | Native SwiftUI Studio on macOS (`apps/macos-host`) and single-binary Direct2D Studio on Windows (`apps/windows-host`), sharing the exact same **Alabaster Studio** UX and protocol core. |
| **Dedicated Virtual Extended Displays** | Plug in **2 or more tablets/phones** at once: macOS creates per-device `CGVirtualDisplay`s, while Windows integrates **Parsec VDD** to create & tear down independent virtual extended displays automatically (never hijacking physical secondary monitors) with non-overlapping spatial placement. |
| **Up to 120 Hz Hardware Video** | Hardware-accelerated **HEVC / H.264** via `VideoToolbox` / `Media Foundation` and `OH_VideoDecoder` / `AMediaCodec`, auto-matching each device's aspect ratio and refresh rate (`60 / 90 / 120 Hz`). |
| **4-Mode Reverse Touch Input** | Control macOS or Windows directly from your tablet or phone: **Compatible Gestures**, **Single-Touch Only**, **Virtual Trackpad**, and **Native Screen Touch** (`IOHIDUserDevice` on macOS, `SendInput` / `InitializeTouchInjection` on Windows). |
| **Tablet & Phone Adaptive UI** | Responsive **Alabaster Studio** UI across desktop and mobile. On phones, the client stays **portrait** before connecting and switches automatically to **auto-rotation landscape** during extended display streaming. |
| **Device Policy & Spatial Topology** | Automatic device tracking, allow/block toggles, custom nicknames, per-device default edge placement (`Left / Right / Top / Bottom`), live FPS/bitrate telemetry, and instant per-link disconnect. |
| **Signed In-App Auto-Updates** | Both macOS and Windows Hosts fetch updates directly from GitHub Releases (`update-manifest.json`) verified with **SHA-256 + Ed25519** signatures and optional mirror prefix support — avoiding re-installing Windows drivers or macOS Gatekeeper quarantine prompts. |

---

## Architecture

DeskMate splits transport-agnostic protocol state from platform-native I/O, encoding, and UI via a zero-dependency C++20 core (`core/`):

```text
┌────────────────────────────────────────────────────────────────────────────┐
│   macOS Host (apps/macos-host)     │   Windows Host (apps/windows-host)    │
│  SwiftUI · CGVirtualDisplay        │ Direct2D UI · Parsec VDD / MttVDD     │
│  ScreenCaptureKit + VideoToolbox   │ DXGI Duplication + Media Foundation   │
│  CGEvent / Virtual HID             │ WinUSB AOA + SendInput / TouchInject  │
└────────────────────────────────────┴───────────────────────────────────────┘
                                        │  Shared C++20 Core (core/)
                                        │  32B Header · HostSession · Scheduler
                                        │  Recovery Gate · Clock Sync
                                        ▼
                            USB-C AOA (Bulk IN / OUT)
                                        │
                                        ▼
┌────────────────────────────────────────────────────────────────────────────┐
│    HarmonyOS (apps/harmonyos-client)  &  Android (apps/android-client)     │
│  Adaptive Studio UI (Tablet 2-Col / Phone Portrait-to-Landscape)           │
│  Native Engine (N-API / JNI)  ·  POLLIN ByteIo  ·  AVCodec / MediaCodec HW │
│  Multi-Touch Capture ──────────────────────────────▶ Reverse Input Inject  │
└────────────────────────────────────────────────────────────────────────────┘
```

---

## Quick Start

> **💡 Prefer ready-to-run binaries?**
> Download prebuilt packages directly from GitHub Releases below. If you'd like to build or customize from source with an AI coding agent (**Antigravity**, **Claude Code**, **Cursor**, **Codex**), simply point it to `https://github.com/SupremacySakura/desk-mate`.

### Option A: Download Prebuilt Releases (Fastest)

Download the latest artifacts from [**GitHub Releases**](https://github.com/SupremacySakura/desk-mate/releases/latest):

1. **macOS Host (`DeskMate-v*-macos-arm64.dmg`)**
   - Open the `.dmg` and drag **DeskMate.app** into **Applications** (`libusb` is bundled inside the app — no Homebrew or Xcode required).
   - Because the build is ad-hoc signed without Apple notarization, if macOS Gatekeeper blocks first launch, clear quarantine once:
     ```bash
     xattr -dr com.apple.quarantine /Applications/DeskMate.app
     ```
2. **Windows Host (`DeskMate-v*-windows-x64.zip`)**
   - Extract the archive and double-click the statically linked single-file **`DeskMateHost.exe`** (no VC++ redistributable or external DLLs needed).
   - ⚠️ **Required First-Time Setup on Windows**: Complete both **[Step 1: Zadig WinUSB Driver](#step-1-install-usb-aoa-winusb-driver-via-zadig)** and **[Step 2: Parsec VDD Virtual Display Driver](#step-2-install-parsec-vdd-for-standalone-virtual-extended-displays)** below before connecting your device.
3. **Android Client (`DeskMate-v*-android.apk`)** *(Ready to install)*
   - Download `DeskMate-v*-android.apk` directly onto your Android 8.0+ tablet or phone and install it (supports `arm64-v8a` hardware & `x86_64` emulators; no USB debugging or Android Studio required).
4. **HarmonyOS Client (`DeskMate-v*-harmonyos-unsigned.hap`)**
   - HarmonyOS NEXT does **not** allow installing unsigned `.hap` packages directly. Download `-unsigned.hap` to skip DevEco Studio compilation, **re-sign it with your Huawei debug certificate** bound to your device UDID (`hap-sign-tool` or community re-signing tools), and install via `hdc install`, or build directly in DevEco Studio (Option B).

---

## Windows Host First-Time Setup (Required)

Unlike macOS, Windows requires two user-installed drivers so DeskMate can talk to USB AOA interfaces and spawn standalone virtual extended displays (one-time setup):

### Step 1: Install USB AOA (WinUSB) Driver via Zadig

Windows does not automatically attach a `WinUSB` driver when a phone switches to Android Open Accessory (AOA) mode, which prevents `libusb` from claiming the bulk endpoints (symptom: after tapping "Allow" on the phone, Windows stays stuck waiting for USB connection or negotiation).

1. Download and run [**Zadig**](https://zadig.akeo.ie/) **as Administrator**.
2. Connect your phone/tablet over USB-C, launch DeskMate on both sides to trigger an AOA mode flip (tap **Allow** on the device's USB accessory prompt).
3. In Zadig's menu bar, enable **`Options` → `List All Devices`**.
4. From the device dropdown, select the device's **USB AOA / Android Accessory interface** (`USB ID`: **`18D1:2D00`** or **`18D1:2D01`**, typically **`Interface 0`**).
   > ⚠️ **Important**: Do **not** overwrite the normal `12D1:5000` (HDC) or MTP interfaces; verify the USB ID starts with `18D1` before proceeding.
5. Set the target driver to **`WinUSB`** and click **`Install Driver`** (or **`Replace Driver`**).
6. **Tips**:
   - *(Optional alternative)* You can install [**UsbDk**](https://github.com/daynix/UsbDk/releases) instead of binding WinUSB per device with Zadig.
   - Make sure background **`hdc.exe`**, **`adb.exe`**, or vendor phone-suite tools are closed so they don't seize the USB interface.
7. Open DeskMate **Settings → System & Drivers** and verify USB / WinUSB reports green/ready.

### Step 2: Install Parsec VDD for Standalone Virtual Extended Displays

Windows has no built-in user-space API equivalent to macOS `CGVirtualDisplay`. Without a virtual display driver installed, Windows cannot create a brand-new extended monitor out of thin air (on multi-monitor PCs it would only mirror or borrow an existing physical screen).

1. Download and install [**Parsec Virtual Display Driver (Parsec VDD)**](https://github.com/nomi-san/parsec-vdd/releases) (Microsoft WHQL-signed — **no Windows Test Mode required**; **MttVDD / Virtual Display Driver** is also supported).
2. After installing the driver, **do not keep the standalone `ParsecVDisplay` GUI tray app open** — let DeskMate talk to the driver directly so there is no contention over display indices.
3. **Fully Automatic Display Lifecycle**:
   - When a phone/tablet connects and commits its stream, `DeskMateHost.exe` invokes Parsec VDD IOCTLs to **automatically create** a dedicated virtual extended display matching your device's resolution and refresh rate;
   - Automatically arranges the virtual display alongside your existing physical monitors without overlapping (without touching or hijacking your real secondary screens);
   - Automatically destroys the virtual display when the device disconnects or DeskMate closes.
4. Open DeskMate **Settings → System & Drivers** and confirm **Virtual Display Driver** shows **Parsec VDD ready** (green).

---

### Option B: Build from Source

#### Prerequisites

- **Mac Host**: macOS 15.0+, Xcode Command Line Tools (`xcode-select --install`), Homebrew [`libusb`](https://formulae.brew.sh/formula/libusb)
- **Windows Host**: Windows 10/11 (x64), Visual Studio 2022 (Desktop C++ & Windows SDK 10.0.22621+) + CMake 3.20+ + `vcpkg install libusb:x64-windows` (or `apps/windows-host/build-portable.ps1` with portable `llvm-mingw` + `libusb-1.0.30`)
- **Android Client**: Android SDK 35 & NDK 27+ (offline script needs no Gradle/Maven network access), USB-C data cable
- **HarmonyOS Client**: HarmonyOS NEXT (`compatibleSdkVersion 6.1.1(24)` / `targetSdkVersion 26.0.0`), **DevEco Studio** (for building & signing the `.hap`), USB-C data cable

#### 1. Clone & Build Desktop Host

- **macOS (`apps/macos-host`)**:
  ```bash
  git clone https://github.com/SupremacySakura/desk-mate.git
  cd desk-mate

  brew install libusb
  cd apps/macos-host
  make app        # or `make dmg` for a self-contained .dmg
  open build/DeskMate.app
  ```
  > Pass `LIBUSB_PREFIX=/usr/local make app` if `libusb` is installed outside `/opt/homebrew`. To sign local builds with a consistent self-signed certificate (`DeskMate Local Signer`, helping avoid rotating ad-hoc CDHashes across updates), run `bash apps/macos-host/scripts/create-self-signed-cert.sh` once and trust it in Keychain Access.

- **Windows (`apps/windows-host`)**:
  ```powershell
  # Option 1: Visual Studio 2022 + CMake + vcpkg
  cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
    -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake `
    -DDESKMATE_BUILD_WINDOWS_HOST=ON
  cmake --build build --config Release

  # Option 2: Portable llvm-mingw + libusb source (produces static single-file exe)
  powershell -ExecutionPolicy Bypass -File apps/windows-host/build-portable.ps1 `
    -ToolchainDir C:\dm\llvm-mingw-20261117-ucrt-x86_64 `
    -LibusbSource C:\dm\libusb-1.0.30 `
    -OutputDir    C:\dm\build
  ```

#### 2. Build & Install the Mobile Client

- **Android (`apps/android-client`)**:
  ```bash
  ANDROID_HOME=~/Library/Android/sdk NDK=~/Library/Android/sdk/ndk/27.2.12479018 \
    bash apps/android-client/package_apk.sh
  adb install -r apps/android-client/app/build/outputs/apk/debug/app-debug.apk
  ```
- **HarmonyOS NEXT (`apps/harmonyos-client`)**:
  1. Open `./apps/harmonyos-client` in **DevEco Studio**.
  2. Connect your HarmonyOS tablet or phone via USB with Developer Mode / USB Debugging enabled.
  3. Configure **automatic / personal signing** in DevEco Studio (**File → Project Structure → Signing Configs**).
     > **Security tip**: Never commit your personal `signingConfigs` inside `apps/harmonyos-client/build-profile.json5`.
  4. Click **Run (`entry`)** to install DeskMate onto each HarmonyOS device.

#### 3. Grant OS Permissions / Drivers

- **macOS**: Grant **Screen Recording** and **Accessibility** (one-click inside DeskMate Overview/Settings; click **重置旧授权** if rebuilding invalidates cached permissions).
- **Windows**: Follow **[Windows Host First-Time Setup](#windows-host-first-time-setup-required)** to bind **WinUSB via Zadig** and install **Parsec VDD**, then verify green checks in **Settings → System & Drivers**.

#### 4. Connect One or Multiple Devices

1. Open **DeskMate** on your HarmonyOS / Android tablet(s) or phone(s).
2. Connect them to your computer with USB-C cables.
3. Tap **Allow** on the **"Allow USB Accessory"** prompt on each device.
4. DeskMate automatically creates a matching **virtual extended display** for each connected device and arranges them around your primary screen without overlapping.
5. Drag windows onto your new extended displays, click the **Spatial Topology Map** (or per-device **Left / Right / Top / Bottom** buttons) to reposition screens, or switch **Touch Input Modes** in **Settings**.

---

## Reverse Touch Input Modes

Configure under **Settings → 触控输入 (Touch Input)** or the tray/menu bar controls:

| Mode | Behavior |
| --- | --- |
| **兼容手势映射** *(Compatible Gestures, Default)* | Maps 1-finger tap/drag/pointer, 2-finger scroll/right-click/pinch zoom, and multi-finger desktop gestures (`⌃←/→/↑/↓` Space switching & Mission Control on macOS; desktop shortcuts & scroll via `SendInput` on Windows). |
| **仅单点触控** *(Single-Touch Only)* | Maps primary single-finger tap & drag cleanly while ignoring multi-finger gestures. |
| **虚拟妙控板 / 触控板（实验性）** *(Virtual Trackpad)* | Relative cursor movement + gesture mapping with drag latching (`IOHIDUserDevice` when entitled on macOS, or SendInput fallback). |
| **原生屏幕触控（实验性）** *(Native Screen Touch)* | Direct multi-touch forwarding (`IOHIDUserDevice` on macOS with automatic fallback; Win32 `InitializeTouchInjection` on Windows). |

---

## CLI Options & Automated Tests

### macOS Host CLI (`apps/macos-host/build/DeskMate.app/Contents/MacOS/DeskMate`)

```bash
./build/DeskMate.app/Contents/MacOS/DeskMate --list-displays
./build/DeskMate.app/Contents/MacOS/DeskMate --codec h264 --fps 60
./build/DeskMate.app/Contents/MacOS/DeskMate --display-id <ID>
./build/DeskMate.app/Contents/MacOS/DeskMate --dry-run
```

| Flag | Default | Description |
| --- | --- | --- |
| `--display-id N` | Auto virtual display | Borrow an existing extended display instead of creating a virtual one |
| `--create` | Off | Force creating a `CGVirtualDisplay` |
| `--width` / `--height` | `2800` / `1840` | Default capture dimensions when auto-matching is disabled |
| `--fps N` | `60` | Default target frame rate (`60` / `90` / `120`) |
| `--codec NAME` | `hevc` | `hevc` or `h264` |
| `--list-displays` | — | Print online CoreGraphics displays and exit |
| `--dry-run` | Off | Capture & encode locally without starting USB AOA |

### Windows Host CLI (`DeskMateHost.exe`)

```powershell
.\DeskMateHost.exe --list-monitors
.\DeskMateHost.exe --codec hevc --fps 60
.\DeskMateHost.exe --headless
```

### Automated Tests

```bash
# 1. Shared C++20 core protocol, session state machine, and scheduler tests
make test

# 2. Cross-platform common layer, Windows contract tests, and Android fd I/O tests
bash apps/common/tests/run_platform_tests.sh

# 3. macOS Host build, bundle contract checks, and multi-link USB bridge tests
make -C apps/macos-host test
```

---

## Project Layout

```text
desk-mate/
├── core/                      # Transport-agnostic C++20 protocol, session state machine, scheduler & tests
├── apps/
│   ├── common/                # Shared cross-platform adapters (AOA IDs, session profiles, 12 KiB policy, Annex-B)
│   ├── macos-host/            # macOS Host app (SwiftUI, ScreenCaptureKit, VideoToolbox, CGVirtualDisplay, libusb)
│   ├── windows-host/          # Windows Host app (Direct2D UI, Parsec VDD, DXGI, Media Foundation, WinUSB, SendInput)
│   ├── harmonyos-client/      # HarmonyOS NEXT client (ArkTS UI + C++ N-API native AOA & AVCodec decoder)
│   └── android-client/        # Android client (UsbManager accessory fd, MediaCodec Surface, MotionEvent touch)
├── docs/
│   ├── assets/                # Documentation screenshots & media
│   ├── architecture.md        # System architecture & platform constraints
│   ├── protocol.md            # Detailed wire format, Session API & golden vector spec
│   ├── verification-notes.md  # Contract tests, real-device fixes & probe status
│   └── adr/                   # Architecture Decision Records
├── testdata/golden/           # Independent binary protocol golden test vectors
├── GLOSSARY.md                # Domain terminology
├── CMakeLists.txt             # Cross-platform CMake build entrypoint (Windows Host / common tests)
└── Makefile                   # Shared C++20 core build & test entrypoints
```

---

## Troubleshooting

- **Windows: Stays stuck on "Waiting for USB connection" or negotiation after tapping Allow**:
  Bind **WinUSB** in **[Zadig](#step-1-install-usb-aoa-winusb-driver-via-zadig)** to the device's `18D1:2D00` / `18D1:2D01` (`Interface 0`) endpoint (or install **UsbDk**), and close any running `hdc`, `adb`, or vendor phone-assistant process.
- **Windows: Phone mirrors/shows an existing physical monitor or black screen instead of creating a new extended display**:
  Install **[Parsec VDD](#step-2-install-parsec-vdd-for-standalone-virtual-extended-displays)** and verify **Virtual Display Driver** is green in DeskMate **Settings**. Also close any manually opened `ParsecVDisplay` tray window and reconnect the cable.
- **Device stays in "Waiting for confirmation" (`awaiting_client_hello`)**:
  Make sure **DeskMate** is open in the foreground on your phone/tablet and tap **Allow** on the USB accessory permission dialog.
- **macOS: Black screen or no video frames after connecting**:
  Check **System Settings → Privacy & Security → Screen & System Audio Recording** and confirm DeskMate is enabled.
- **macOS: Touch works visually in diagnostics, but Mac cursor doesn't move**:
  Click **一键获取 (One-Click Grant)** (or **重置旧授权 (Reset Stale Permission)** after recompiling) for Accessibility permission, and make sure **平板触控回传** is toggled **On**.
- **Device stuck in USB Accessory mode after force-quitting**:
  Unplug and re-plug the USB cable once so the device returns cleanly to standard HDC/MTP mode.

---

## Contributing & Docs

- System Architecture & Platform Constraints: [docs/architecture.md](./docs/architecture.md)
- Protocol & Wire Format Spec: [docs/protocol.md](./docs/protocol.md)
- Verification & Real-Device Notes: [docs/verification-notes.md](./docs/verification-notes.md)
- macOS Host Engineering Notes: [apps/macos-host/README.md](./apps/macos-host/README.md)
- Windows Host Driver, Build & Design Notes: [apps/windows-host/README.md](./apps/windows-host/README.md)
- HarmonyOS Client Notes: [apps/harmonyos-client/README.md](./apps/harmonyos-client/README.md)
- Android Client Notes: [apps/android-client/README.md](./apps/android-client/README.md)
- Domain Glossary: [GLOSSARY.md](./GLOSSARY.md)

Issues and Pull Requests are welcome!

---

## License

DeskMate is licensed under the **[PolyForm Noncommercial License 1.0.0](./LICENSE)** (`Copyright (c) 2026 SupremacySakura`):

- **Personal & Non-Commercial Use**: Free to use, study, modify, and share for personal, educational, or other non-commercial purposes.
- **Commercial Use**: Commercial use (including commercial redistribution, commercial bundling, SaaS, or commercial organizational use) requires prior written authorization from the author (`SupremacySakura`). Please open an issue or contact the maintainer for commercial licensing.
