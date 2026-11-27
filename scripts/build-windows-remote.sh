#!/usr/bin/env bash
# Builds dist/windows/DeskMateHost.exe on the LAN Windows machine over SSH.
#
# The Windows host is a dedicated build box that only needs SSH + the portable
# llvm-mingw toolchain; see docs/windows-remote-build.md for the environment
# contract and troubleshooting.
#
# Usage:
#   bash scripts/build-windows-remote.sh            # build + fetch + package
#   bash scripts/build-windows-remote.sh --verify   # also run the smoke check
#
# Overridable via environment:
#   DM_WIN_HOST       Administrator@192.168.43.4
#   DM_WIN_TOOLCHAIN  C:\dm\llvm-mingw-20261117-ucrt-x86_64
#   DM_WIN_LIBUSB     C:\dm\libusb-1.0.30
#   DM_WIN_WORKDIR    C:\dm\buildwork
#   DM_WIN_SRC        C:\dm\src
#   DM_WIN_UPLOAD     C:\Users\Administrator\dm-src.zip
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST="$REPO/dist/windows"

HOST="${DM_WIN_HOST:-Administrator@192.168.43.4}"
TOOLCHAIN="${DM_WIN_TOOLCHAIN:-C:\\dm\\llvm-mingw-20261117-ucrt-x86_64}"
LIBUSB="${DM_WIN_LIBUSB:-C:\\dm\\libusb-1.0.30}"
WORKDIR="${DM_WIN_WORKDIR:-C:\\dm\\buildwork}"
SRC="${DM_WIN_SRC:-C:\\dm\\src}"
UPLOAD="${DM_WIN_UPLOAD:-C:\\Users\\Administrator\\dm-src.zip}"

SSH_OPTS=(-o StrictHostKeyChecking=no -o BatchMode=yes)
ARCHIVE=/tmp/dm-src.zip

VERIFY=0
for arg in "$@"; do
  case "$arg" in
    --verify) VERIFY=1 ;;
    -h|--help)
      sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "unknown argument: $arg" >&2
      exit 2
      ;;
  esac
done

VERSION="$(sed -n 's/.*kWindowsHostVersion = "\([^"]*\)".*/\1/p' \
  "$REPO/apps/windows-host/include/deskmate/windows/updater.hpp" | head -1)"
if [[ -z "$VERSION" ]]; then
  echo "cannot read kWindowsHostVersion from updater.hpp" >&2
  exit 1
fi
echo "==> Windows host version: $VERSION"

echo "==> Packaging sources"
rm -f "$ARCHIVE"
( cd "$REPO" && zip -q -r "$ARCHIVE" core apps/common apps/windows-host CMakeLists.txt \
    -x '*/build/*' -x '*.o' )

echo "==> Uploading to $HOST"
scp "${SSH_OPTS[@]}" "$ARCHIVE" "$HOST:C:/Users/Administrator/dm-src.zip"

echo "==> Rebuilding on Windows (full clean, ~30-60s)"
ssh "${SSH_OPTS[@]}" "$HOST" \
  "cmd /c \"rmdir /s /q $SRC 2>nul & mkdir $SRC & tar -xf $UPLOAD -C $SRC\" && powershell -NoProfile -ExecutionPolicy Bypass -File $SRC\\apps\\windows-host\\build-portable.ps1 -ToolchainDir $TOOLCHAIN -LibusbSource $LIBUSB -WorkDir $WORKDIR 2>&1 | Select-Object -Last 14"

echo "==> Fetching DeskMateHost.exe"
mkdir -p "$DIST"
scp "${SSH_OPTS[@]}" "$HOST:C:/dm/src/dist/windows/DeskMateHost.exe" "$DIST/DeskMateHost.exe"
chmod 755 "$DIST/DeskMateHost.exe"

echo "==> Staging bundled drivers (Parsec VDD + Zadig)"
bash "$REPO/scripts/fetch-windows-drivers.sh" "$DIST/drivers"

cat > "$DIST/README.txt" <<EOF
DeskMate Windows Host ${VERSION}
===========================

单文件 GUI 程序，静态链接，无需安装运行库或 VC++ 可再发行组件。
压缩包已内置 drivers\\ 驱动工具目录，首次使用无需再去外部单独下载。

快速开始
--------
1. 双击 DeskMateHost.exe 打开工作台窗口
2. 首次使用建议按需安装内置驱动（也可直接点击工作台内的按钮一键启动）：
   - drivers\\parsec-vdd-0.45.0.0-setup.exe
     安装 Parsec VDD 虚拟扩展屏驱动（微软 WHQL 签名，可自动创建独立副屏）
   - drivers\\zadig-2.9.exe
     连接手机并允许 USB 配件授权后，勾选 Options -> List All Devices，
     为 Android Accessory (18D1:2D00 / 18D1:2D01, Interface 0) 安装 WinUSB 驱动
3. 手机/平板端打开 DeskMate，工作台自动握手并开启扩展屏与反向触控

常用参数
--------
  --width N --height N --fps N   期望分辨率与刷新率
  --codec hevc|h264              首选编码格式（默认 h264）
  --no-auto-match                按请求尺寸采集，而非显示器原生模式
  --list-monitors                列出可采集显示器后退出
  --headless                     不创建窗口
  --help                         打印用法

说明
----
- 程序会自动把处于 MTP/ADB/HDC 模式的兼容 Android / HarmonyOS 设备切换到
  AOA 配件模式（18d1:2d00..2d05）并持续扫描连接。
- 多台设备可同时连接，每台设备有独立的 Session、视频与输入队列。
- 视频分片固定 12 KiB 载荷，与 macOS Host / 移动端客户端线缆协议一致。
- 第三方驱动工具许可证与源码说明见 drivers\\THIRD_PARTY_LICENSES.txt。
EOF

echo "==> Repackaging zip + checksums"
( cd "$DIST" && rm -f "DeskMate-v${VERSION}-windows-x64.zip" \
  && zip -q -r -9 "DeskMate-v${VERSION}-windows-x64.zip" DeskMateHost.exe README.txt drivers \
  && shasum -a 256 DeskMateHost.exe > DeskMateHost.exe.sha256 \
  && shasum -a 256 "DeskMate-v${VERSION}-windows-x64.zip" > "DeskMate-v${VERSION}-windows-x64.zip.sha256" )

cat "$DIST/DeskMateHost.exe.sha256" "$DIST/DeskMate-v${VERSION}-windows-x64.zip.sha256"

if [[ "$VERIFY" == "1" ]]; then
  echo "==> Verifying on Windows"
  ssh "${SSH_OPTS[@]}" "$HOST" \
    'powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\Administrator\verify-win.ps1'
fi

echo "==> Done: $DIST/DeskMateHost.exe"
echo "    Remember to refresh dist/release/SHA256SUMS if you are preparing a release."
