<div align="center">

# DeskMate

**Turn HarmonyOS and Android tablets/phones into low-latency, high-refresh-rate wired extended displays for macOS — with multi-device support and reverse touch input.**

[![macOS](https://img.shields.io/badge/macOS-15%2B-111111?style=flat-square&logo=apple&logoColor=white)](./apps/macos-host)
[![HarmonyOS NEXT](https://img.shields.io/badge/HarmonyOS-NEXT%206.1%2B-0A59F7?style=flat-square)](./apps/harmonyos-client)
[![Android](https://img.shields.io/badge/Android-8.0%2B-3DDC84?style=flat-square&logo=android&logoColor=white)](./apps/android-client)
[![USB AOA](https://img.shields.io/badge/Transport-USB--C%20AOA-9D193D?style=flat-square)](#architecture)
[![Codec](https://img.shields.io/badge/Video-HEVC%20%2F%20H.264%20up%20to%20120Hz-2D7D5B?style=flat-square)](#features)
[![Stack](https://img.shields.io/badge/Stack-C%2B%2B20%20%C2%B7%20Swift%20%C2%B7%20ArkTS%20%C2%B7%20Android-5A545E?style=flat-square)](#project-layout)
[![License](https://img.shields.io/badge/License-PolyForm%20Noncommercial%201.0.0-B9975B?style=flat-square)](./LICENSE)

English | [简体中文](./README.zh-CN.md)

</div>

---

<div align="center">
  <img src="./docs/assets/screenshot-multi-display.jpg" alt="DeskMate running with two HarmonyOS devices connected simultaneously as extended displays on macOS" width="880" />
  <br />
  <sub>
    <i>Dual-device live session: <b>HUAWEI MatePad Pro 12.2</b> (left, 2800×1840 @ 120 Hz) and <b>HUAWEI Mate 80 Pro Max</b> (bottom, 2848×1320 @ 120 Hz) connected simultaneously over USB-C AOA with hardware-accelerated HEVC streaming.</i>
  </sub>
</div>

---

## Why DeskMate?

DeskMate transforms your HarmonyOS tablet or smartphone into a crisp, responsive secondary monitor over a single USB-C cable — **zero Wi-Fi dependency, zero cloud relay, and native resolution HiDPI output**.

Unlike generic wireless casting tools, DeskMate uses a dedicated custom binary protocol over **USB Accessory (AOA)** bulk transfers, paired with **ScreenCaptureKit + VideoToolbox** on macOS and native **C++ / AVCodec** hardware decoding on HarmonyOS.

---

## Features

| Capability | Description |
| --- | --- |
| **Wired USB-C AOA Direct Link** | Full-duplex USB bulk transfers (`usb_byte_io` + `fd_byte_io`), zero Wi-Fi jitter, works offline anywhere. |
| **Multi-Device Simultaneous Displays** | Plug in **2 or more HarmonyOS tablets/phones** at once. Each device gets its own independent `CGVirtualDisplay`, capture/encoder pipeline, touch engine, and non-overlapping spatial placement around your Mac primary screen. |
| **Up to 120 Hz Hardware Video** | Hardware-accelerated **HEVC / H.264** via Apple `VideoToolbox` and HarmonyOS `OH_VideoDecoder` (`AVCodec`), auto-matching each device's native aspect ratio and refresh rate (`60 / 90 / 120 Hz`). |
| **4-Mode Reverse Touch Input** | Control macOS directly from your tablet or phone screen: **Compatible Gestures**, **Single-Touch Only**, **Virtual Trackpad (Experimental)**, and **Native Screen Touch (Experimental)**, with one-click Accessibility authorization. |
| **Tablet & Phone Adaptive UI** | Responsive **Alabaster Studio** interface on both macOS and HarmonyOS. On HarmonyOS phones, the app stays **portrait** before connecting and switches automatically to **auto-rotation landscape** during extended display streaming. |
| **Paired Device Management** | Automatic device tracking, allow/block policy toggles, custom device nicknames, per-device default screen placement (`Left / Right / Top / Bottom`), and instant per-link disconnect. |
| **Mac Menu Bar & Launch Controls** | Native menu bar popover for instant telemetry (FPS / Mbps / Touch events), active display switching, launch-at-login (`ServiceManagement`), and auto-start background service toggle. |

---

## Architecture

DeskMate splits transport-agnostic protocol state from platform-native I/O, encoding, and UI via a zero-dependency C++20 core (`core/`):

```text
┌────────────────────────────────────────────────────────────────────────────┐
│                           macOS Host (apps/macos-host)                     │
│  SwiftUI Studio UI  ·  CGVirtualDisplay  ·  ScreenCaptureKit + VideoToolbox│
│  InputInjector (4-Mode CGEvent / Virtual HID)  ·  libusb Multi-Link AOA Hub│
└───────────────────────────────────────┬────────────────────────────────────┘
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

> **💡 Zero-effort setup with an AI Agent**: Think the manual build & setup steps look too complicated? Just copy this repository URL (`https://github.com/SupremacySakura/desk-mate`) and hand it to an AI coding agent (such as **Antigravity**, **Claude Code**, **Cursor**, or **Codex**) — ask it to clone the repo, install dependencies, build `DeskMate.app`, and walk you through installing & trusting it on your devices!

### Option A: Download Prebuilt Releases (Fastest)

Download the latest artifacts from [**GitHub Releases**](https://github.com/SupremacySakura/desk-mate/releases/latest):

1. **macOS Host (`DeskMate-v*-macos-arm64.dmg`)**
   - Open the `.dmg` and drag **DeskMate.app** into **Applications** (`libusb` is bundled inside the app — no Homebrew or Xcode required).
   - Because the build is ad-hoc signed without Apple notarization, if macOS Gatekeeper blocks the first launch, clear the quarantine attribute once:
     ```bash
     xattr -dr com.apple.quarantine /Applications/DeskMate.app
     ```
2. **Android Client (`DeskMate-v*-android.apk`)** *(Ready to install)*
   - Download `DeskMate-v*-android.apk` directly onto your Android 8.0+ tablet or phone and install it (supports `arm64-v8a` hardware & `x86_64` emulators; no USB debugging or Android Studio required).
3. **HarmonyOS Client (`DeskMate-v*-harmonyos-unsigned.hap`)**
   - HarmonyOS NEXT does **not** allow installing unsigned `.hap` packages directly. Download the `-unsigned.hap` to skip installing DevEco Studio and compiling C++ code, then **re-sign it with your own Huawei debug certificate** bound to your device UDID (using `hap-sign-tool` or community HarmonyOS re-signing tools) and install via `hdc install`, or use Option B below in DevEco Studio.

### Option B: Build from Source

#### Prerequisites

- **Mac Host**: macOS 15.0+, Xcode Command Line Tools (`xcode-select --install`), Homebrew [`libusb`](https://formulae.brew.sh/formula/libusb)
- **Android Client**: Android SDK 35 & NDK 27+ (offline script needs no Gradle/Maven network access), USB-C data cable
- **HarmonyOS Client**: HarmonyOS NEXT (`compatibleSdkVersion 6.1.1(24)` / `targetSdkVersion 26.0.0`), **DevEco Studio** (for building & signing the `.hap`), USB-C data cable

#### 1. Clone & Build the macOS Host

```bash
git clone https://github.com/SupremacySakura/desk-mate.git
cd desk-mate

# Install libusb if you haven't already
brew install libusb

# Build DeskMate.app (or `make dmg` to create a self-contained .dmg)
cd apps/macos-host
make app

# Launch DeskMate
open build/DeskMate.app
```

> **Note**: If your `libusb` is installed outside `/opt/homebrew` (e.g., `/usr/local` on Intel Macs), pass `LIBUSB_PREFIX=/usr/local make app`.

#### 2. Build & Install the Client

- **Android (`apps/android-client`)**:
  ```bash
  # Builds & debug-signs app/build/outputs/apk/debug/app-debug.apk offline
  ANDROID_HOME=~/Library/Android/sdk NDK=~/Library/Android/sdk/ndk/27.2.12479018 \
    bash apps/android-client/package_apk.sh
  adb install -r apps/android-client/app/build/outputs/apk/debug/app-debug.apk
  ```
- **HarmonyOS NEXT (`apps/harmonyos-client`)**:
  1. Open `./apps/harmonyos-client` in **DevEco Studio**.
  2. Connect your HarmonyOS tablet or phone via USB with Developer Mode / USB Debugging enabled.
  3. Configure **automatic / personal signing** in DevEco Studio (**File → Project Structure → Signing Configs**).
     > **Security tip**: Never commit your personal `signingConfigs` inside `apps/harmonyos-client/build-profile.json5` if you fork or open a PR.
  4. Click **Run (`entry`)** to install DeskMate onto each HarmonyOS device you want to use as a secondary monitor.

#### 3. Grant macOS Permissions

On first launch, DeskMate guides you through two macOS permissions:

- **Screen Recording** *(Required)* — Needed by `ScreenCaptureKit` to capture your virtual or extended displays. Grant in **System Settings → Privacy & Security → Screen & System Audio Recording**, then reopen or click Retry.
- **Accessibility** *(Required for reverse touch)* — Needed to inject clicks, drags, scrolls, and multi-finger gestures back into macOS. Click **一键获取 (One-Click Grant)** in DeskMate Overview/Settings to register and open Accessibility settings (if you rebuilt the `.app` and macOS caches an old signature, click **重置旧授权 (Reset Stale Permission)**).

#### 4. Connect One or Multiple Devices

1. Open **DeskMate** on your HarmonyOS tablet(s) / phone(s).
2. Connect them to your Mac with USB-C cables (directly or through a powered USB hub).
3. Accept the **"Allow USB Accessory"** system prompt on each HarmonyOS device.
4. DeskMate automatically creates a matching **HiDPI virtual display** for each connected device and arranges them around your Mac primary screen without overlapping.
5. Drag windows onto your new extended displays, click the **Spatial Topology Map** (or per-device **Left / Right / Top / Bottom** buttons) to move any screen, or switch **Touch Input Modes** in **Settings**.

---

## Reverse Touch Input Modes

Configure under **Settings → 触控输入 (Touch Input)** or directly from the Menu Bar popover:

| Mode | Behavior |
| --- | --- |
| **兼容手势映射** *(Compatible Gestures, Default)* | Maps 1-finger tap/drag/pointer, 2-finger scroll/right-click/pinch zoom (`⌘+` / `⌘-`), and 3/4-finger Space switching / Mission Control / App Exposé (`⌃←/→/↑/↓`). |
| **仅单点触控** *(Single-Touch Only)* | Maps primary single-finger tap & drag cleanly while ignoring multi-finger gestures. |
| **虚拟妙控板（实验性）** *(Virtual Trackpad)* | Uses relative cursor movement + trackpad gesture mapping (long-press drag latching; uses `IOHIDUserDevice` when entitled, with automatic relative fallback). |
| **原生屏幕触控（实验性）** *(Native Screen Touch)* | Forwards up to 10 simultaneous touch contacts via `IOHIDUserDevice` when signed with virtual HID entitlements (automatically falls back to Compatible Gestures otherwise). |

---

## CLI Options & Running Tests

### Host CLI (`apps/macos-host/build/DeskMate.app/Contents/MacOS/DeskMate`)

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

### Automated Tests

```bash
# 1. Shared C++20 core protocol, session state machine, and scheduler tests
make test

# 2. macOS Host build, bundle contract checks, and multi-link USB bridge tests
make -C apps/macos-host test

# 3. Android accessory fd contract tests (host toolchain, no NDK/USB needed)
bash apps/common/tests/run_android_io_test.sh
```

---

## Want a Windows Host? Build It with an AI Agent!

DeskMate currently ships first-class implementations for **macOS Host ([`apps/macos-host`](./apps/macos-host))**, **HarmonyOS NEXT ([`apps/harmonyos-client`](./apps/harmonyos-client))**, and **Android ([`apps/android-client`](./apps/android-client))**.

If you want a **Windows Host**, you don't have to start from scratch — **let an AI coding agent (such as Antigravity, Claude Code, Cursor, or Codex) build it using this repository as the blueprint!**

Why this works especially well here:
1. **The C++20 core ([`core/`](./core)) is 100% platform-agnostic** — zero third-party dependencies, explicit little-endian 32-byte wire protocol, handshake state machine, GOP-aware scheduler, frame recovery gate, clock sync, and 100+ unit tests can be compiled directly on **Windows** (MSVC / Clang).
2. **Clear architectural seams (`ByteIo`, `HostBoundary`, `DeviceBoundary`)** — every OS-specific responsibility (USB AOA, virtual display, screen capture, hardware encode/decode, touch injection) is cleanly isolated in [`apps/macos-host`](./apps/macos-host), [`apps/harmonyos-client`](./apps/harmonyos-client), and [`apps/android-client`](./apps/android-client).

### Porting Blueprint for AI Agents

| Layer | Existing Implementation | Windows Host Equivalent | Android Client (Included) |
| --- | --- | --- | --- |
| **Protocol & Session Core** | [`core/`](./core) (C++20) | Compile [`core/`](./core) directly via CMake/MSVC | [`apps/android-client`](./apps/android-client) (NDK C++20) |
| **USB AOA Transport** | [`Bridge.cpp`](./apps/macos-host/Sources/Bridge.cpp) (`libusb`) · [`fd_byte_io.cpp`](./apps/harmonyos-client/entry/src/main/cpp/fd_byte_io.cpp) | `libusb` / WinUSB multi-link AOA switching (`PID 0x2D00–0x2D05`) | [`accessory_io.cpp`](./apps/android-client/app/src/main/cpp/accessory_io.cpp) (`POLLIN` + lock-free stats) |
| **Virtual Extended Display** | [`DMVirtualDisplay.m`](./apps/macos-host/Sources/DMVirtualDisplay.m) | IddCx Indirect Display Driver (e.g., Virtual Display Driver / Parsec-VDD pattern) | N/A (Android renders directly to fullscreen `SurfaceView` / `TextureView`) |
| **Capture & HW Encode / Decode** | `ScreenCaptureKit` + `VideoToolbox` · [`video_decoder.cpp`](./apps/harmonyos-client/entry/src/main/cpp/video_decoder.cpp) (`AVCodec`) | Windows.Graphics.Capture / DXGI Desktop Duplication + Media Foundation / NVENC | [`video_decoder.cpp`](./apps/android-client/app/src/main/cpp/video_decoder.cpp) (dual-thread `AMediaCodec`) |
| **Reverse Touch Injection / Capture** | [`InputInjector.swift`](./apps/macos-host/Sources/InputInjector.swift) · [`napi_init.cpp`](./apps/harmonyos-client/entry/src/main/cpp/napi_init.cpp) | `SendInput` / `InitializeTouchInjection` (Win32) | [`MainActivity.java`](./apps/android-client/app/src/main/java/dev/deskmate/client/MainActivity.java) (`MotionEvent` → JNI) |

<details>
<summary><b>📋 Copy-Paste Starter Prompt for Your AI Agent</b></summary>

```text
Inspect this DeskMate repository thoroughly — especially `core/`, `docs/protocol.md`, `apps/macos-host/Sources/Bridge.cpp`, and `apps/harmonyos-client/entry/src/main/cpp/`.

Goal: Implement a compatible Windows Host that interoperates 1:1 with the existing DeskMate HarmonyOS and Android clients over USB AOA.

Requirements:
1. Reuse `core/` directly via CMake without altering the 32-byte wire protocol, opcodes, or InputEvent layout.
2. Implement the ByteIo and HostBoundary adapters for Windows: WinUSB/libusb multi-device AOA + WGC/DXGI capture + Media Foundation HEVC/H.264 + IddCx virtual display + SendInput.
3. Preserve multi-device support, 12 KiB USB video fragment sizing, and non-blocking full-duplex USB I/O.
4. Build and add unit/contract tests verifying protocol compatibility with `core/`.
```

</details>

---

## Project Layout

```text
desk-mate/
├── core/                      # Transport-agnostic C++20 protocol, session state machine, scheduler & tests
├── apps/
│   ├── common/                # Shared cross-platform adapters (AOA IDs, session profiles, 12 KiB policy, Annex-B)
│   ├── macos-host/            # macOS Host app (SwiftUI, ScreenCaptureKit, VideoToolbox, CGVirtualDisplay, libusb)
│   ├── harmonyos-client/      # HarmonyOS NEXT client (ArkTS UI + C++ N-API native AOA & AVCodec decoder)
│   └── android-client/        # Android client (UsbManager accessory fd, MediaCodec Surface, MotionEvent touch)
├── docs/
│   ├── assets/               # Documentation screenshots & media
│   ├── architecture.md       # System architecture & platform constraints
│   ├── protocol.md           # Detailed wire format, Session API & golden vector spec
│   ├── verification-notes.md # Contract tests, real-device fixes & probe status
│   └── adr/                  # Architecture Decision Records
├── testdata/golden/           # Independent binary protocol golden test vectors
├── GLOSSARY.md                # Domain terminology
└── Makefile                   # Shared C++20 core build & test entrypoints
```

---

## Troubleshooting

- **Device stays in "Waiting for confirmation" (`awaiting_client_hello`)**:
  Make sure **DeskMate** is open in the foreground on your HarmonyOS device and tap **Allow** on the USB accessory permission dialog.
- **Black screen or no video frames after connecting**:
  Check **System Settings → Privacy & Security → Screen & System Audio Recording** and confirm DeskMate is enabled.
- **Touch works visually in diagnostics, but Mac cursor doesn't move**:
  Click **一键获取 (One-Click Grant)** (or **重置旧授权 (Reset Stale Permission)** after recompiling) for Accessibility permission, and make sure **平板触控回传** is toggled **On**.
- **Device stuck in USB Accessory mode after force-quitting**:
  Unplug and re-plug the USB cable once so the device returns cleanly to standard HDC/MTP mode.

---

## Contributing & Docs

- System Architecture & Platform Constraints: [docs/architecture.md](./docs/architecture.md)
- Protocol & Wire Format Spec: [docs/protocol.md](./docs/protocol.md)
- Verification & Real-Device Notes: [docs/verification-notes.md](./docs/verification-notes.md)
- macOS Host Engineering Notes: [apps/macos-host/README.md](./apps/macos-host/README.md)
- HarmonyOS Client Notes: [apps/harmonyos-client/README.md](./apps/harmonyos-client/README.md)
- Android Client Notes: [apps/android-client/README.md](./apps/android-client/README.md)
- Domain Glossary: [GLOSSARY.md](./GLOSSARY.md)

Issues and Pull Requests are welcome! If you build a **Windows Host** or **Android Client** port compatible with `core/`, feel free to share it or open a PR.

---

## License

DeskMate is licensed under the **[PolyForm Noncommercial License 1.0.0](./LICENSE)** (`Copyright (c) 2026 SupremacySakura`):

- **Personal & Non-Commercial Use**: Free to use, study, modify, and share for personal, educational, or other non-commercial purposes.
- **Commercial Use**: Commercial use (including commercial redistribution, commercial bundling, SaaS, or commercial organizational use) requires prior written authorization from the author (`SupremacySakura`). Please open an issue or contact the maintainer for commercial licensing.
