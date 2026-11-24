#!/usr/bin/env bash
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
PROBE_ROOT="$(cd "$SCRIPT_DIR/.." && pwd -P)"
source "$SCRIPT_DIR/common.sh"

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
  cat <<'EOF'
Usage: capture_environment.sh [OUTPUT_DIR]

Captures Host identity, USB topology, system_profiler output, optional hdc
target/UDID/build parameters, and SHA-256 for the evidence files.

Environment:
  GATE1_DEVICE_SERIAL  USB serial used to print the exact ancestor path
  GATE1_HDC_TARGET     hdc target when more than one is connected
  HDC                  hdc executable

The script records empty/error results verbatim. It never invents Reference
Environment data.
EOF
  exit 0
fi

OUT_DIR="${1:-$PROBE_ROOT/evidence/local/env-$(date -u +%Y%m%dT%H%M%SZ)}"
mkdir -p "$OUT_DIR"
cd "$OUT_DIR"

{
  echo "generated_at_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "generated_at_local=$(date +%Y-%m-%dT%H:%M:%S%z)"
  echo
  sw_vers
  echo
  uname -a
  echo "hw.model=$(sysctl -n hw.model 2>&1)"
  echo "hw.memsize=$(sysctl -n hw.memsize 2>&1)"
  echo
  system_profiler SPHardwareDataType -json 2>&1
} > host.txt

system_profiler SPUSBDataType -json > system-profiler-usb.json 2> system-profiler-usb.json.err
json_usb_status=$?
printf '%s\n' "$json_usb_status" > system-profiler-usb.json.status
system_profiler SPUSBDataType > system-profiler-usb.txt 2> system-profiler-usb.txt.err
text_usb_status=$?
printf '%s\n' "$text_usb_status" > system-profiler-usb.txt.status

ioreg -p IOUSB -w0 -l > ioreg-usb.txt 2> ioreg-usb.txt.err
ioreg_status=$?
printf '%s\n' "$ioreg_status" > ioreg-usb.txt.status

awk '/^[ |]*\+-o /' ioreg-usb.txt > usb-tree.txt 2>/dev/null || true

SERIAL="${GATE1_DEVICE_SERIAL:-}"
if [[ -n "$SERIAL" ]]; then
  awk -v needle="$SERIAL" '
    function indent_width(value, matched) {
      matched = match(value, /^[ |]*/)
      return matched ? RLENGTH : 0
    }
    /^[ |]*\+-o / {
      width = indent_width($0)
      while (depth > 0 && stack_width[depth] >= width) {
        depth--
      }
      depth++
      stack[depth] = $0
      stack_width[depth] = width
    }
    index($0, needle) {
      if (found) {
        next
      }
      found = 1
      print "serial=" needle
      print "matched_line=" $0
      print "ancestor_path:"
      for (i = 1; i <= depth; i++) {
        printf "  %s\n", stack[i]
      }
    }
    END {
      if (!found) {
        print "serial=" needle
        print "not_found_in_ioreg"
      }
    }
  ' ioreg-usb.txt > device-topology.txt
else
  cat > device-topology.txt <<'EOF'
GATE1_DEVICE_SERIAL was not set; no device-specific ancestor path was derived.
Inspect usb-tree.txt and ioreg-usb.txt, then rerun with the observed serial.
EOF
fi

HDC_BIN="$(gate1_hdc_path 2>/dev/null || true)"
if [[ -n "$HDC_BIN" && -x "$HDC_BIN" ]]; then
  {
    echo "hdc=$HDC_BIN"
    echo "command=list targets -v"
    "$HDC_BIN" list targets -v 2>&1
  } > hdc.txt
  targets="$("$HDC_BIN" list targets -v 2>/dev/null | sed '/^\[Empty\]/d;/^$/d')"
  target_count="$(printf '%s\n' "$targets" | sed '/^$/d' | wc -l | tr -d ' ')"
  target="${GATE1_HDC_TARGET:-}"
  if [[ -z "$target" && "$target_count" == "1" ]]; then
    target="$(printf '%s\n' "$targets" | sed -n '1p' | awk '{print $1}')"
  fi
  if [[ -n "$target" ]]; then
    {
      echo "target=$target"
      echo "command=shell bm get -u"
      "$HDC_BIN" -t "$target" shell bm get -u 2>&1
      for parameter in \
        const.ohos.apiversion \
        const.product.software.version \
        const.product.model \
        const.product.name \
        const.product.devicetype; do
        echo "command=shell param get $parameter"
        "$HDC_BIN" -t "$target" shell param get "$parameter" 2>&1
      done
    } > hdc-device.txt
  else
    {
      echo "No single hdc target selected; device parameters not captured."
      echo "target_count=$target_count"
      if [[ "$target_count" -gt 1 ]]; then
        echo "Set GATE1_HDC_TARGET to a serial from hdc.txt."
      fi
    } > hdc-device.txt
  fi
else
  {
    echo "hdc not found."
    echo "Set HDC or DEVECO_SDK_HOME."
  } > hdc.txt
  printf '%s\n' "hdc unavailable" > hdc-device.txt
fi

{
  echo "capture_dir=$OUT_DIR"
  echo "system_profiler_usb_json_status=$json_usb_status"
  echo "system_profiler_usb_json_bytes=$(wc -c < system-profiler-usb.json | tr -d ' ')"
  echo "system_profiler_usb_text_status=$text_usb_status"
  echo "system_profiler_usb_text_bytes=$(wc -c < system-profiler-usb.txt | tr -d ' ')"
  echo "ioreg_status=$ioreg_status"
  echo "ioreg_bytes=$(wc -c < ioreg-usb.txt | tr -d ' ')"
  echo
  if [[ "$(wc -c < system-profiler-usb.json | tr -d ' ')" -lt 5 ]]; then
    echo "WARNING: system_profiler USB JSON is empty; ioreg is the fallback source."
  fi
  if [[ -n "$SERIAL" ]]; then
    cat device-topology.txt
  fi
} > capture-summary.txt

find . -type f ! -name SHA256SUMS -print | LC_ALL=C sort | while IFS= read -r file; do
  shasum -a 256 "$file"
done > SHA256SUMS

echo "Captured environment in: $OUT_DIR"
cat capture-summary.txt
echo
echo "Evidence hashes:"
cat SHA256SUMS
