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
[[ -f "$APP/Contents/Resources/AppIcon.icns" ]] || fail "missing AppIcon.icns"

/usr/libexec/PlistBuddy -c 'Print :LSUIElement' "$PLIST" | grep -qx false \
  || fail "the desktop client must launch as a regular app"
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

binary_strings="$(strings "$BIN")"
if grep -Eq 'BEGIN (RSA |EC )?PRIVATE KEY|password=' <<<"$binary_strings"; then
  fail "binary contains signing material"
fi
echo "PASS no signing material"

if grep -q 'set_configuration' <<<"$binary_strings"; then
  fail "accessory teardown must not reconfigure the device (it strands it in AOA)"
fi
grep -q 'waiting for the tablet to allow the USB accessory' <<<"$binary_strings" \
  || fail "missing the consent-dialog hint in the state log"
echo "PASS AOA teardown and diagnostics"

echo "all host contract checks passed"
