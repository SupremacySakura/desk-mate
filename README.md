<div align="center">

# DeskMate

**Turn HarmonyOS tablets and phones into low-latency, high-refresh-rate wired extended displays for macOS — with multi-device support and reverse touch input.**

[![macOS](https://img.shields.io/badge/macOS-15%2B-111111?style=flat-square&logo=apple&logoColor=white)](./apps/host)
[![HarmonyOS NEXT](https://img.shields.io/badge/HarmonyOS-NEXT%206.1%2B-0A59F7?style=flat-square)](./apps/device)
[![USB AOA](https://img.shields.io/badge/Transport-USB--C%20AOA-9D193D?style=flat-square)](#architecture)
[![Codec](https://img.shields.io/badge/Video-HEVC%20%2F%20H.264%20up%20to%20120Hz-2D7D5B?style=flat-square)](#features)
[![Stack](https://img.shields.io/badge/Stack-C%2B%2B20%20%C2%B7%20Swift%20%C2%B7%20ArkTS-5A545E?style=flat-square)](#project-layout)
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
│                              macOS Host (apps/host)                        │
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
│                        HarmonyOS Client (apps/device)                      │
│  ArkTS Adaptive UI (Tablet & Phone Auto-Orientation)                       │
│  N-API Native Engine  ·  fd_byte_io (POLLIN)  ·  AVCodec HW Decode         │
│  XComponent Multi-Touch Capture ──▶ Reverse Input Injection                │
└────────────────────────────────────────────────────────────────────────────┘
```

---

## Quick Start

> **💡 Zero-effort setup with an AI Agent**: Think the manual build & setup steps look too complicated? Just copy this repository URL (`https://github.com/SupremacySakura/desk-mate`) and hand it to an AI coding agent (such as **Antigravity**, **Claude Code**, **Cursor**, or **Codex**) — ask it to clone the repo, install dependencies, build `DeskMate.app`, and walk you through installing & trusting it on your devices!

### Prerequisites

- **Mac Host**: macOS 15.0+, Xcode Command Line Tools (`xcode-select --install`), Homebrew [`libusb`](https://formulae.brew.sh/formula/libusb)
- **HarmonyOS Device**: HarmonyOS NEXT (`compatibleSdkVersion 6.1.1(24)` / `targetSdkVersion 26.0.0`), **DevEco Studio** (for building & signing the `.hap`), USB-C data cable

### 1. Clone & Build the macOS Host

```bash
git clone https://github.com/SupremacySakura/desk-mate.git
cd desk-mate

# Install libusb if you haven't already
brew install libusb

# Build DeskMate.app
cd apps/host
make app

# Launch DeskMate
open build/DeskMate.app
```

> **Note**: If your `libusb` is installed outside `/opt/homebrew` (e.g., `/usr/local` on Intel Macs), pass `LIBUSB_PREFIX=/usr/local make app`.

### 2. Build & Install the HarmonyOS Client

1. Open `./apps/device` in **DevEco Studio**.
2. Connect your HarmonyOS tablet or phone via USB with Developer Mode / USB Debugging enabled.
3. Configure **automatic / personal signing** in DevEco Studio (**File → Project Structure → Signing Configs**).
   > **Security tip**: Never commit your personal `signingConfigs` inside `apps/device/build-profile.json5` if you fork or open a PR.
4. Click **Run (`entry`)** to install DeskMate onto each HarmonyOS device you want to use as a secondary monitor.

### 3. Grant macOS Permissions

On first launch, DeskMate guides you through two macOS permissions:

- **Screen Recording** *(Required)* — Needed by `ScreenCaptureKit` to capture your virtual or extended displays. Grant in **System Settings → Privacy & Security → Screen & System Audio Recording**, then reopen or click Retry.
- **Accessibility** *(Required for reverse touch)* — Needed to inject clicks, drags, scrolls, and multi-finger gestures back into macOS. Click **一键获取 (One-Click Grant)** in DeskMate Overview/Settings to register and open Accessibility settings (if you rebuilt the `.app` and macOS caches an old signature, click **重置旧授权 (Reset Stale Permission)**).

### 4. Connect One or Multiple Devices

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

### Host CLI (`apps/host/build/DeskMate.app/Contents/MacOS/DeskMate`)

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
make -C apps/host test
```

---

## Want Windows or Android? Build It with an AI Agent!

DeskMate currently ships first-class implementations for **macOS (Host)** and **HarmonyOS NEXT (Device)**.

If you want a **Windows Host** or an **Android Tablet/Phone Client**, you don't have to start from scratch — **let an AI coding agent (such as Antigravity, Claude Code, Cursor, or Codex) build it using this repository as the blueprint!**

Why this works especially well here:
1. **The C++20 core ([`core/`](./core)) is 100% platform-agnostic** — zero third-party dependencies, explicit little-endian 32-byte wire protocol, handshake state machine, GOP-aware scheduler, frame recovery gate, clock sync, and 100+ unit tests can be compiled directly on **Windows** (MSVC / Clang) and **Android** (NDK CMake).
2. **Clear architectural seams (`ByteIo`, `HostBoundary`, `DeviceBoundary`)** — every OS-specific responsibility (USB AOA, virtual display, screen capture, hardware encode/decode, touch injection) is cleanly isolated in [`apps/host`](./apps/host) and [`apps/device`](./apps/device).

### Porting Blueprint for AI Agents

| Layer | Existing Implementation | Windows Host Equivalent | Android Client Equivalent |
| --- | --- | --- | --- |
| **Protocol & Session Core** | [`core/`](./core) (C++20) | Compile [`core/`](./core) directly via CMake/MSVC | Compile [`core/`](./core) via Android NDK CMake |
| **USB AOA Transport** | [`Bridge.cpp`](./apps/host/Sources/Bridge.cpp) (`libusb`) · [`fd_byte_io.cpp`](./apps/device/entry/src/main/cpp/fd_byte_io.cpp) | `libusb` / WinUSB multi-link AOA switching (`PID 0x2D00–0x2D05`) | Android `UsbManager.openAccessory()` + `ParcelFileDescriptor` (`POLLIN` loop in C++ NDK) |
| **Virtual Extended Display** | [`DMVirtualDisplay.m`](./apps/host/Sources/DMVirtualDisplay.m) | IddCx Indirect Display Driver (e.g., Virtual Display Driver / Parsec-VDD pattern) | N/A (Android renders directly to fullscreen `SurfaceView` / `TextureView`) |
| **Capture & HW Encode / Decode** | `ScreenCaptureKit` + `VideoToolbox` · [`video_decoder.cpp`](./apps/device/entry/src/main/cpp/video_decoder.cpp) (`AVCodec`) | Windows.Graphics.Capture / DXGI Desktop Duplication + Media Foundation / NVENC | `MediaCodec` (`video/hevc`, `video/avc`) outputting directly to `Surface` |
| **Reverse Touch Injection / Capture** | [`InputInjector.swift`](./apps/host/Sources/InputInjector.swift) · [`napi_init.cpp`](./apps/device/entry/src/main/cpp/napi_init.cpp) | `SendInput` / `InitializeTouchInjection` (Win32) | `View.onTouchEvent` (`MotionEvent` multi-touch pointers) → C++ JNI/NDK |

<details>
<summary><b>📋 Copy-Paste Starter Prompt for Your AI Agent</b></summary>

```text
Inspect this DeskMate repository thoroughly — especially `core/`, `docs/protocol.md`, `apps/host/Sources/Bridge.cpp`, and `apps/device/entry/src/main/cpp/`.

Goal: Implement a compatible [Windows Host in apps/windows-host / Android Client in apps/android-client] that interoperates 1:1 with the existing DeskMate [HarmonyOS Client / macOS Host] over USB AOA.

Requirements:
1. Reuse `core/` directly via CMake without altering the 32-byte wire protocol, opcodes, or InputEvent layout.
2. Implement the ByteIo and Session boundary adapters for [Windows: WinUSB/libusb multi-device AOA + WGC/DXGI capture + Media Foundation HEVC/H.264 + IddCx virtual display + SendInput | Android: UsbManager openAccessory + NDK POLLIN reader + MediaCodec Surface decoder + multi-touch MotionEvent].
3. Preserve multi-device support, 12 KiB USB video fragment sizing, and non-blocking full-duplex USB I/O.
4. Build and add unit/contract tests verifying protocol compatibility with `core/`.
```

</details>

---

## Project Layout

```text
desk-mate/
├── core/               # Transport-agnostic C++20 protocol, session state machine, scheduler & tests
├── apps/
│   ├── host/           # macOS Host app (SwiftUI, ScreenCaptureKit, VideoToolbox, CGVirtualDisplay, libusb)
│   └── device/         # HarmonyOS NEXT client (ArkTS UI + C++ N-API native AOA & AVCodec decoder)
├── docs/
│   ├── assets/               # Documentation screenshots & media
│   ├── architecture.md       # System architecture & platform constraints
│   ├── protocol.md           # Detailed wire format, Session API & golden vector spec
│   ├── verification-notes.md # Contract tests, real-device fixes & probe status
│   └── adr/                  # Architecture Decision Records
├── testdata/golden/    # Independent binary protocol golden test vectors
├── GLOSSARY.md         # Domain terminology
└── Makefile            # Shared C++20 core build & test entrypoints
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
- macOS Host Engineering Notes: [apps/host/README.md](./apps/host/README.md)
- Domain Glossary: [GLOSSARY.md](./GLOSSARY.md)

Issues and Pull Requests are welcome! If you build a **Windows Host** or **Android Client** port compatible with `core/`, feel free to share it or open a PR.

---

## License

DeskMate is licensed under the **[PolyForm Noncommercial License 1.0.0](./LICENSE)** (`Copyright (c) 2026 SupremacySakura`):

- **Personal & Non-Commercial Use**: Free to use, study, modify, and share for personal, educational, or other non-commercial purposes.
- **Commercial Use**: Commercial use (including commercial redistribution, commercial bundling, SaaS, or commercial organizational use) requires prior written authorization from the author (`SupremacySakura`). Please open an issue or contact the maintainer for commercial licensing.
