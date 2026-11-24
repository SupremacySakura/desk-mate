#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/gate1-auto-layout.XXXXXX")
trap 'rm -rf "$TMP_DIR"' EXIT HUP INT TERM

cat > "$TMP_DIR/layout.json" <<'EOF'
{"children":[{"attributes":{"bounds":"[34,349][310,434]","text":"1. 授权并打开"}},{"attributes":{"bounds":"[34,539][300,624]","text":"启动 eventfd"}}]}
EOF

GATE1_AUTO_LAYOUT="$TMP_DIR/layout.json" \
GATE1_AUTO_LOG="$TMP_DIR/log" \
GATE1_AUTO_LIB_ONLY=1 \
  . "$SCRIPT_DIR/auto_device.sh"

bounds=$(find_bounds "启动 eventfd")
if [ "$bounds" != "34 539 300 624" ]; then
  echo "unexpected bounds: $bounds" >&2
  exit 1
fi

if layout_has "不存在的按钮"; then
  echo "unexpected layout match" >&2
  exit 1
fi

printf '%s\n' \
  'Error Code:10106102  Error Message:The device screen is locked' \
  > "$TMP_DIR/aa-error.log"
if aa_output_ok "$TMP_DIR/aa-error.log"; then
  echo "aa failure output was not detected" >&2
  exit 1
fi

: > "$TMP_DIR/aa-ok.log"
aa_output_ok "$TMP_DIR/aa-ok.log" || {
  echo "empty aa output should be accepted" >&2
  exit 1
}

echo "auto_device layout helpers: PASS"
