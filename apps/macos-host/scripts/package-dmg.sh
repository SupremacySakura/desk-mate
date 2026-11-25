#!/usr/bin/env bash
set -euo pipefail

APP_PATH="${1:?usage: package-dmg.sh /path/to/DeskMate.app /path/to/output.dmg [libusb_prefix] [volume_name] [out_zip]}"
OUT_DMG="${2:?usage: package-dmg.sh /path/to/DeskMate.app /path/to/output.dmg [libusb_prefix] [volume_name] [out_zip]}"
LIBUSB_PREFIX="${3:-/opt/homebrew}"
VOL_NAME="${4:-DeskMate}"
OUT_ZIP="${5:-${OUT_ZIP:-${OUT_DMG%.dmg}.zip}}"

BIN="$APP_PATH/Contents/MacOS/DeskMate"
FRAMEWORKS_DIR="$APP_PATH/Contents/Frameworks"

if [[ ! -d "$APP_PATH" || ! -x "$BIN" ]]; then
  echo "error: DeskMate.app executable not found at $BIN" >&2
  exit 1
fi

LIBUSB_LINKED_PATH="$(otool -L "$BIN" | awk '/libusb-1\.0.*\.dylib/{print $1; exit}')"
LIBUSB_SRC=""
if [[ -n "$LIBUSB_LINKED_PATH" && -f "$LIBUSB_LINKED_PATH" ]]; then
  LIBUSB_SRC="$LIBUSB_LINKED_PATH"
elif [[ -f "$LIBUSB_PREFIX/lib/libusb-1.0.0.dylib" ]]; then
  LIBUSB_SRC="$LIBUSB_PREFIX/lib/libusb-1.0.0.dylib"
elif [[ -f "$LIBUSB_PREFIX/opt/libusb/lib/libusb-1.0.0.dylib" ]]; then
  LIBUSB_SRC="$LIBUSB_PREFIX/opt/libusb/lib/libusb-1.0.0.dylib"
fi

if [[ -z "$LIBUSB_SRC" || ! -f "$LIBUSB_SRC" ]]; then
  echo "error: could not locate libusb-1.0.0.dylib (linked='$LIBUSB_LINKED_PATH', prefix='$LIBUSB_PREFIX')" >&2
  exit 1
fi

REAL_LIBUSB="$(python3 -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' "$LIBUSB_SRC")"
BUNDLED_DYLIB="$FRAMEWORKS_DIR/libusb-1.0.0.dylib"

mkdir -p "$FRAMEWORKS_DIR"
cp -f "$REAL_LIBUSB" "$BUNDLED_DYLIB"
chmod 755 "$BUNDLED_DYLIB"

install_name_tool -id "@rpath/libusb-1.0.0.dylib" "$BUNDLED_DYLIB"

if [[ -n "$LIBUSB_LINKED_PATH" ]]; then
  install_name_tool -change "$LIBUSB_LINKED_PATH" "@executable_path/../Frameworks/libusb-1.0.0.dylib" "$BIN"
fi

if ! otool -l "$BIN" | grep -q "@executable_path/../Frameworks"; then
  install_name_tool -add_rpath "@executable_path/../Frameworks" "$BIN"
fi
install_name_tool -delete_rpath "$LIBUSB_PREFIX/lib" "$BIN" 2>/dev/null || true
install_name_tool -delete_rpath "/opt/homebrew/lib" "$BIN" 2>/dev/null || true
install_name_tool -delete_rpath "/usr/local/lib" "$BIN" 2>/dev/null || true

if otool -L "$BIN" | grep -E '/opt/homebrew|/usr/local' >/dev/null; then
  echo "error: binary still references non-system dylibs:" >&2
  otool -L "$BIN" >&2
  exit 1
fi

SIGN_ID="${CODESIGN_IDENTITY:-}"
if [[ -z "$SIGN_ID" ]]; then
  if security find-identity -v -p codesigning 2>/dev/null | grep -Fq '"DeskMate Local Signer"'; then
    SIGN_ID="DeskMate Local Signer"
  else
    SIGN_ID="-"
  fi
fi

echo "Signing DeskMate.app with identity: $SIGN_ID"
codesign --force --sign "$SIGN_ID" "$BUNDLED_DYLIB"
codesign --force --deep --sign "$SIGN_ID" "$APP_PATH"
codesign --verify --deep --strict "$APP_PATH"

STAGE_DIR="$(mktemp -d)"
cleanup() {
  rm -rf "$STAGE_DIR"
}
trap cleanup EXIT

cp -R "$APP_PATH" "$STAGE_DIR/DeskMate.app"
ln -s /Applications "$STAGE_DIR/Applications"

mkdir -p "$(dirname "$OUT_DMG")"
rm -f "$OUT_DMG"
hdiutil create -volname "$VOL_NAME" -srcfolder "$STAGE_DIR" -ov -format UDZO "$OUT_DMG" >/dev/null

if [[ -n "$OUT_ZIP" ]]; then
  mkdir -p "$(dirname "$OUT_ZIP")"
  rm -f "$OUT_ZIP"
  ditto -c -k --sequesterRsrc --keepParent "$APP_PATH" "$OUT_ZIP"
  echo "Created updater ZIP: $OUT_ZIP"
fi

echo "Created self-contained DMG: $OUT_DMG"
otool -L "$BIN" | grep -E 'libusb|Frameworks' || true
