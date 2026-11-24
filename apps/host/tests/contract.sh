#!/usr/bin/env bash
set -euo pipefail

APP="${1:?usage: contract.sh /path/to/DeskMate.app}"
BIN="$APP/Contents/MacOS/DeskMate"
PLIST="$APP/Contents/Info.plist"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

[[ -x "$BIN" ]] || fail "missing executable: $BIN"
[[ -f "$PLIST" ]] || fail "missing Info.plist"

/usr/libexec/PlistBuddy -c 'Print :LSUIElement' "$PLIST" | grep -qx true \
  || fail "LSUIElement must be true"
/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$PLIST" | grep -qx 'dev.deskmate.host' \
  || fail "unexpected bundle identifier"
/usr/libexec/PlistBuddy -c 'Print :NSScreenCaptureUsageDescription' "$PLIST" >/dev/null \
  || fail "missing screen capture usage description"
echo "PASS bundle metadata"

help="$("$BIN" --help 2>&1)"
grep -q -- '--display-id' <<<"$help" || fail "--help must document --display-id"
grep -q -- '--codec' <<<"$help" || fail "--help must document --codec"
echo "PASS help output"

"$BIN" --codec vp9 >/dev/null 2>&1 && fail "invalid codec was accepted"
"$BIN" --unknown-flag >/dev/null 2>&1 && fail "unknown flag was accepted"
echo "PASS argument validation"

if grep -q 'CGVirtualDisplaySession\|Gate0VirtualDisplay' "$BIN" 2>/dev/null; then
  fail "probe-era symbol leaked into the product binary"
fi
echo "PASS no probe-era symbols"

otool -L "$BIN" | grep -q 'libusb-1.0' || fail "libusb is not linked"
otool -L "$BIN" | grep -q 'ScreenCaptureKit' || fail "ScreenCaptureKit is not linked"
otool -L "$BIN" | grep -q 'VideoToolbox' || fail "VideoToolbox is not linked"
echo "PASS required frameworks"

if strings "$BIN" | grep -Eq 'BEGIN (RSA |EC )?PRIVATE KEY|password='; then
  fail "binary contains signing material"
fi
echo "PASS no signing material"

echo "all host contract checks passed"
