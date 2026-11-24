#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
PROBE_ROOT="$(cd "$SCRIPT_DIR/.." && pwd -P)"
CPP_ROOT="$PROBE_ROOT/device/entry/src/main/cpp"
ARKTS_ROOT="$PROBE_ROOT/device/entry/src/main/ets"

status=0

scan_forbidden() {
  local description="$1"
  local pattern="$2"
  local matches
  matches="$(
    grep -RInE "$pattern" "$CPP_ROOT" --include='*.cpp' --include='*.hpp' || true
  )"
  if [[ -n "$matches" ]]; then
    echo "FORBIDDEN: $description" >&2
    printf '%s\n' "$matches" >&2
    status=1
  fi
}

scan_required() {
  local description="$1"
  local pattern="$2"
  local root="$3"
  if ! grep -RInE "$pattern" "$root" --include='*.cpp' --include='*.hpp' \
      --include='*.ets' >/dev/null; then
    echo "MISSING: $description" >&2
    status=1
  fi
}

scan_forbidden \
  "native close(accessoryFd/accessory_fd)" \
  'close[[:space:]]*\([[:space:]]*accessory_fd_?[[:space:]]*\)'
scan_forbidden \
  "native shutdown(accessoryFd/accessory_fd)" \
  'shutdown[[:space:]]*\([[:space:]]*accessory_fd_?[[:space:]]*[,)]'

scan_required "ArkTS accessory authorization" 'requestAccessoryRight' "$ARKTS_ROOT"
scan_required "ArkTS official openAccessory" '\bopenAccessory[[:space:]]*\(' "$ARKTS_ROOT"
scan_required "ArkTS official closeAccessory" '\bcloseAccessory[[:space:]]*\(' "$ARKTS_ROOT"
scan_required "poll-based worker" '\bpoll[[:space:]]*\(' "$CPP_ROOT"
scan_required "eventfd stop path" '\beventfd[[:space:]]*\(' "$CPP_ROOT"
scan_required "targeted pthread_kill" '\bpthread_kill[[:space:]]*\(' "$CPP_ROOT"

if ! grep -RInE 'action\.sa_flags[[:space:]]*=[[:space:]]*0[[:space:]]*;' \
    "$CPP_ROOT" --include='*.cpp' >/dev/null; then
  echo "MISSING: explicit sigaction flags without SA_RESTART" >&2
  status=1
fi

owner_sequence="$(
  grep -RInE 'beginOwnerCloseStop|closeAccessory|finishOwnerCloseJoin' \
    "$ARKTS_ROOT" --include='*.ets' | wc -l | tr -d ' '
)"
if [[ "$owner_sequence" -lt 3 ]]; then
  echo "MISSING: owner-close A/B/C sequence in ArkTS" >&2
  status=1
fi

if [[ "$status" -eq 0 ]]; then
  echo "PASS: Device ownership/wake static boundary checks"
fi
exit "$status"
