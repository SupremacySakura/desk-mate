#!/bin/sh
set -u

# Runs the three Gate 1 wake branches inside ONE AOA session. The device stays
# in AOA accessory mode after closeAccessory (only VBUS removal leaves it), so
# one session can host every branch; the Host must be restarted per branch.
#
# Must be started BEFORE the Host issues AOA START: HDC goes Offline on the
# re-enumeration.
#
# Per branch it captures: before-start, post-transfer stats, post-stop result,
# and final. Native counters reset on every startProbe(), so each branch's
# wake counter belongs to that branch alone.

BUNDLE=${GATE1_BUNDLE:-com.deskmate.gate1probe}
ABILITY=${GATE1_ABILITY:-EntryAbility}
LOG=${GATE1_AUTO_LOG:-/data/local/tmp/gate1_wake_matrix.log}
STATE=${GATE1_AUTO_LAYOUT:-/data/local/tmp/gate1_wake_matrix_layout.json}
OUT=${GATE1_AUTO_OUT:-/data/local/tmp}
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

bring_to_front()
{
  aa start -a "$ABILITY" -b "$BUNDLE" >/dev/null 2>&1 </dev/null
  sleep 1
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
  dump_layout
  if ! layout_has "$label"; then
    log "missing text $label; bringing to front"
    bring_to_front
    dump_layout
  fi
  if ! layout_has "$label"; then
    log "FAIL still missing text $label"
    return 1
  fi
  bounds=$(find_bounds "$label")
  if [ -z "$bounds" ]; then
    log "FAIL bounds not found for $tag text=$label"
    return 1
  fi
  set -- $bounds
  x=$((($1 + $3) / 2))
  y=$((($2 + $4) / 2))
  uitest uiInput click "$x" "$y" >/dev/null 2>&1
  rc=$?
  log "click $tag center=$x,$y rc=$rc"
  sleep 1
  return "$rc"
}

snapshot()
{
  tag=$1
  dump_layout
  cp "$STATE" "$OUT/gate1_wake_${tag}.json" 2>/dev/null
  log "snapshot $tag"
}

# Native stats live in one Text node; extract the JSON object for the log.
stats_line()
{
  sed 's/},{/}\n{/g' "$STATE" 2>/dev/null \
    | grep -F '{"running"' \
    | head -n 1
}

open_accessory()
{
  attempt=0
  while [ "$attempt" -lt 60 ]; do
    dump_layout
    if layout_has "manufacturer="; then
      log "accessory open"
      return 0
    fi
    if layout_has "1. 授权并打开"; then
      click_text "1. 授权并打开" authorize >/dev/null 2>&1 || true
    else
      bring_to_front
    fi
    attempt=$((attempt + 1))
    sleep 1
  done
  dump_layout
  layout_has "manufacturer="
}

run_branch()
{
  branch=$1
  start_label=$2
  stop_label=$3

  log "=== branch $branch start ==="
  if ! open_accessory; then
    log "FAIL branch $branch: accessory did not open"
    snapshot "${branch}_open_failed"
    return 1
  fi
  click_text "刷新统计" "${branch}_stats_before" >/dev/null 2>&1 || true
  snapshot "${branch}_before_start"
  log "pre-start stats: $(stats_line)"

  if ! click_text "$start_label" "${branch}_start"; then
    log "FAIL branch $branch: could not click start"
    return 1
  fi
  sleep "$RUN_SECONDS"

  click_text "刷新统计" "${branch}_stats_transfer" >/dev/null 2>&1 || true
  snapshot "${branch}_transfer"
  log "post-transfer stats: $(stats_line)"

  if [ "$branch" = "owner-close" ]; then
    click_text "A. 拒绝新工作" owner_close_a || true
    sleep 1
    click_text "B. closeAccessory" owner_close_b || true
    sleep 1
    click_text "C. join" owner_close_c || true
  else
    click_text "$stop_label" "${branch}_stop" || true
  fi

  sleep 3
  click_text "刷新统计" "${branch}_stats_after_stop" >/dev/null 2>&1 || true
  snapshot "${branch}_result"
  log "post-stop stats: $(stats_line)"

  if layout_has "关闭空闲句柄"; then
    click_text "关闭空闲句柄" "${branch}_close_idle" || true
  fi
  sleep 1
  snapshot "${branch}_final"
  log "=== branch $branch done ==="
}

log "start wake matrix bundle=$BUNDLE"
bring_to_front

run_branch eventfd "启动 eventfd" "停止并 join"
run_branch signal "启动 signal" "信号唤醒并 join"
run_branch owner-close "启动 owner-close" ""

hilog -x 2>/dev/null | grep -E 'Gate1Device|Gate1Probe' > "$OUT/gate1_wake_matrix_hilog.log"
log done
sync
