#!/bin/sh
set -u

# Drives one Gate 1 wake branch on the Device UI.
# GATE1_BRANCH: eventfd | signal | owner-close

BUNDLE=${GATE1_BUNDLE:-com.deskmate.gate1probe}
ABILITY=${GATE1_ABILITY:-EntryAbility}
BRANCH=${GATE1_BRANCH:-eventfd}
LOG=${GATE1_AUTO_LOG:-/data/local/tmp/gate1_branch.log}
STATE=${GATE1_AUTO_LAYOUT:-/data/local/tmp/gate1_branch_layout.json}
RUN_SECONDS=${GATE1_AUTO_RUN_SECONDS:-12}

: > "$LOG"

log()
{
  echo "$(date +%s) $*" >> "$LOG"
}

dump_layout()
{
  uitest dumpLayout -p "$STATE" >/dev/null 2>&1
}

layout_has()
{
  grep -Fq "$1" "$STATE" 2>/dev/null
}

aa_output_ok()
{
  if [ ! -s "$1" ]; then
    return 0
  fi
  if grep -Eiq 'error:|failed to start|Error Code' "$1"; then
    return 1
  fi
  return 0
}

bring_to_front()
{
  aa start -a "$ABILITY" -b "$BUNDLE" >/data/local/tmp/gate1_branch_aa.log 2>&1 </dev/null
  rc=$?
  if [ "$rc" -eq 0 ] && aa_output_ok /data/local/tmp/gate1_branch_aa.log; then
    log "aa start ok rc=$rc"
    return 0
  fi
  log "aa start failed rc=$rc output=$(tr '\n' ' ' < /data/local/tmp/gate1_branch_aa.log)"
  return 1
}

find_bounds()
{
  sed 's/},{/}\n{/g' "$STATE" 2>/dev/null \
    | grep -F "\"text\":\"$1\"" \
    | head -n 1 \
    | sed -n 's/.*"bounds":"\[\([0-9][0-9]*\),\([0-9][0-9]*\)\]\[\([0-9][0-9]*\),\([0-9][0-9]*\)\]".*/\1 \2 \3 \4/p'
}

click_text()
{
  label=$1
  tag=$2
  dump_layout || return 1
  if ! layout_has "$label"; then
    log "missing text $label; bringing to front"
    bring_to_front || return 1
    sleep 1
    dump_layout || return 1
  fi
  if ! layout_has "$label"; then
    log "still missing text $label"
    return 1
  fi
  bounds=$(find_bounds "$label")
  if [ -z "$bounds" ]; then
    log "bounds not found for $tag text=$label"
    return 1
  fi
  set -- $bounds
  x=$((($1 + $3) / 2))
  y=$((($2 + $4) / 2))
  log "click $tag text=$label center=$x,$y"
  uitest uiInput click "$x" "$y" >/dev/null 2>&1
  rc=$?
  log "click $tag rc=$rc"
  sleep 1
  return "$rc"
}

log "start branch=$BRANCH bundle=$BUNDLE"
bring_to_front || log "continuing despite aa start failure"

opened=0
attempt=0
while [ "$attempt" -lt 40 ]; do
  if dump_layout; then
    if layout_has "manufacturer="; then
      opened=1
      log "accessory already open"
      break
    fi
    if layout_has "1. 授权并打开"; then
      click_text "1. 授权并打开" authorize || true
    else
      log "authorize button not visible; retrying"
      bring_to_front || true
    fi
  fi
  attempt=$((attempt + 1))
  sleep 1
done

if [ "$opened" -eq 0 ]; then
  dump_layout || true
  if layout_has "manufacturer="; then
    opened=1
  fi
fi

if [ "$opened" -ne 1 ]; then
  log "accessory did not open; aborting branch"
  cp "$STATE" /data/local/tmp/gate1_branch_final.json 2>/dev/null
  hilog -x 2>/dev/null | grep -E 'Gate1Device|Gate1Probe' > /data/local/tmp/gate1_branch_hilog.log
  log done
  sync
  exit 0
fi

case "$BRANCH" in
  eventfd)
    start_label="启动 eventfd"
    stop_label="停止并 join"
    ;;
  signal)
    start_label="启动 signal"
    stop_label="信号唤醒并 join"
    ;;
  owner-close)
    start_label="启动 owner-close"
    stop_label=""
    ;;
  *)
    log "unknown branch $BRANCH"
    exit 1
    ;;
esac

click_text "$start_label" start_branch || true
log "branch started: $BRANCH"

# Wait for real traffic before stopping. A fixed sleep is unreliable: AOA
# re-enumeration can push the Ability to the background and the authorize loop
# may take 40s+, so a fixed sleep would fire the stop click before the Host
# handshake completed.
waited=0
while [ "$waited" -lt "$RUN_SECONDS" ]; do
  dump_layout || true
  frames=$(sed 's/},{/}\n{/g' "$STATE" 2>/dev/null | grep -F '{"running"' | head -n 1 \
    | sed -n 's/.*"frames_read":\([0-9][0-9]*\).*/\1/p')
  if [ -n "$frames" ] && [ "$frames" -gt 0 ]; then
    log "traffic observed frames_read=$frames after ${waited}s"
    break
  fi
  sleep 2
  waited=$((waited + 2))
done
if [ -z "${frames:-}" ] || [ "${frames:-0}" -eq 0 ]; then
  log "WARN no traffic observed within ${RUN_SECONDS}s; stopping anyway"
fi
log "pre-stop wait finished after ${waited}s frames_read=${frames:-none}"

dump_layout || true
cp "$STATE" /data/local/tmp/gate1_branch_before_stop.json 2>/dev/null

click_text "刷新统计" refresh_stats_before_stop || true
dump_layout || true
cp "$STATE" "/data/local/tmp/gate1_branch_prestop_$BRANCH.json" 2>/dev/null

if [ "$BRANCH" = "owner-close" ]; then
  click_text "A. 拒绝新工作" owner_close_a || true
  sleep 1
  click_text "B. closeAccessory" owner_close_b || true
  sleep 1
  click_text "C. join" owner_close_c || true
else
  click_text "$stop_label" stop_branch || true
fi

sleep 2
dump_layout || true
cp "$STATE" /data/local/tmp/gate1_branch_stats.json 2>/dev/null
cp "$STATE" "/data/local/tmp/gate1_branch_result_$BRANCH.json" 2>/dev/null
log "stats captured"

if layout_has "关闭空闲句柄"; then
  click_text "关闭空闲句柄" close_idle || true
fi

sleep 1
dump_layout || true
cp "$STATE" /data/local/tmp/gate1_branch_final.json 2>/dev/null
hilog -x 2>/dev/null | grep -E 'Gate1Device|Gate1Probe' > /data/local/tmp/gate1_branch_hilog.log
log done
sync
