#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
PROBE_ROOT="$(cd "$SCRIPT_DIR/.." && pwd -P)"
DEVICE_ROOT="$PROBE_ROOT/device"
source "$SCRIPT_DIR/common.sh"

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
  cat <<'EOF'
Usage: build_device.sh

Builds the unsigned Stage-model HAP with DevEco hvigor.

Environment overrides:
  DEVECO_SDK_HOME   DevEco SDK root (.../sdk or .../sdk/default)
  DEVECO_STUDIO_HOME
  HVIGORW           hvigorw executable
  NODE_HOME         bundled Node root
  JAVA_HOME         JDK root

An unsigned HAP proves compilation only. It cannot prove install, accessory
authorization, or duplex I/O.
EOF
  exit 0
fi
if [[ -n "${1:-}" ]]; then
  echo "unknown argument: $1" >&2
  exit 2
fi

CONTENTS="$(gate1_deveco_contents)" || {
  echo "DevEco Studio not found. Set DEVECO_STUDIO_HOME." >&2
  exit 2
}
SDK_ROOT="$(gate1_hvigor_sdk_root)" || {
  echo "DevEco SDK not found." >&2
  echo "Set DEVECO_SDK_HOME to .../sdk or .../sdk/default." >&2
  exit 2
}

HVIGORW_BIN="${HVIGORW:-$CONTENTS/tools/hvigor/bin/hvigorw}"
NODE_ROOT="${NODE_HOME:-$CONTENTS/tools/node}"
JDK_ROOT="${JAVA_HOME:-$CONTENTS/jbr/Contents/Home}"

for required in "$HVIGORW_BIN" "$NODE_ROOT/bin/node" "$JDK_ROOT/bin/java"; do
  if [[ ! -e "$required" ]]; then
    echo "required DevEco component not found: $required" >&2
    exit 2
  fi
done

echo "DevEco contents: $CONTENTS"
echo "SDK root:        $SDK_ROOT"
echo "hvigorw:         $HVIGORW_BIN"
echo "Node:            $NODE_ROOT"
echo "Java:            $JDK_ROOT"

cd "$DEVICE_ROOT"
export DEVECO_SDK_HOME="$SDK_ROOT"
export NODE_HOME="$NODE_ROOT"
export JAVA_HOME="$JDK_ROOT"
export PATH="$JAVA_HOME/bin:$NODE_HOME/bin:$PATH"

"$HVIGORW_BIN" \
  --mode module \
  -p product=default \
  -p module=entry@default \
  assembleHap \
  --no-daemon

HAP="$DEVICE_ROOT/entry/build/default/outputs/default/entry-default-unsigned.hap"
NATIVE_LIB="$DEVICE_ROOT/entry/build/default/intermediates/libs/default/arm64-v8a/libgate1_probe.so"

for artifact in "$HAP" "$NATIVE_LIB"; do
  if [[ ! -f "$artifact" ]]; then
    echo "expected artifact not found: $artifact" >&2
    exit 3
  fi
done

echo
echo "Build artifacts:"
shasum -a 256 "$HAP" "$NATIVE_LIB"
echo
echo "Unsigned HAP: install requires matching signing material and is not proven."
