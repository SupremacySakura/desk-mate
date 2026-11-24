#!/bin/sh
set -u

BUNDLE=${GATE1_BUNDLE:-com.deskmate.gate1probe}
ABILITY=${GATE1_ABILITY:-EntryAbility}
LOG=${GATE1_AUTO_LOG:-/data/local/tmp/gate1_auto.log}
STATE=${GATE1_AUTO_LAYOUT:-/data/local/tmp/gate1_auto_layout.json}
UITEST_ERROR=${GATE1_AUTO_UITEST_ERROR:-/data/local/tmp/gate1_auto_uitest.err}
AA_OUTPUT=${GATE1_AUTO_AA_OUTPUT:-/data/local/tmp/gate1_auto_aa.log}
RUN_SECONDS=${GATE1_AUTO_RUN_SECONDS:-40}
REBOOT_AFTER=${GATE1_AUTO_REBOOT_AFTER:-0}

: > "$LOG"

log()
{
  echo "$(date +%s) $*" >> "$LOG"
}

dump_layout()
{
  if uitest dumpLayout -p "$STATE" >/dev/null 2>"$UITEST_ERROR"; then
    log "layout dump ok bytes=$(wc -c < "$STATE" | tr -d ' ')"
    return 0
  fi
  log "layout dump failed: $(cat "$UITEST_ERROR" 2>/dev/null)"
  return 1
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
  aa start -a "$ABILITY" -b "$BUNDLE" >"$AA_OUTPUT" 2>&1 </dev/null
  aa_rc=$?
  if [ "$aa_rc" -eq 0 ] && aa_output_ok "$AA_OUTPUT"; then
    log "aa start -a $ABILITY -b $BUNDLE rc=$aa_rc output=ok"
  else
    log "aa start failed rc=$aa_rc output=$(tr '\n' ' ' < "$AA_OUTPUT")"
    log "Device may be screen-locked; unlock it manually and rerun"
    return 1
  fi
  sleep 1
  return 0
}

find_bounds()
{
  sed 's/},{/}\n{/g' "$STATE" 2>/dev/null \
    | grep -F "\"text\":\"$1\"" \
    | head -n 1 \
    | sed -n 's/.*"bounds":"\[\([0-9][0-9]*\),\([0-9][0-9]*\)\]\[\([0-9][0-9]*\),\([0-9][0-9]*\)\]".*/\1 \2 \3 \4/p'
}

ensure_visible()
{
  dump_layout || return 1
  if ! layout_has "$1"; then
    log "expected text not visible: $1; bringing Ability to foreground"
    bring_to_front || return 1
    dump_layout || return 1
  fi
  if ! layout_has "$1"; then
    log "expected text still missing after aa start: $1"
    return 1
  fi
  return 0
}

click_text()
{
  click_label=$1
  click_tag=$2
  ensure_visible "$click_label" || return 1
  click_bounds=$(find_bounds "$click_label")
  if [ -z "$click_bounds" ]; then
    log "bounds not found for $click_tag text=$click_label"
    return 1
  fi
  set -- $click_bounds
  click_x=$((($1 + $3) / 2))
  click_y=$((($2 + $4) / 2))
  log "click $click_tag text=$click_label bounds=$click_bounds center=$click_x,$click_y"
  uitest uiInput click "$click_x" "$click_y" >/dev/null 2>&1
  click_rc=$?
  log "click $click_tag rc=$click_rc"
  return "$click_rc"
}

capture_hilog()
{
  hilog -x 2>/dev/null \
    | grep -E 'Gate1Device|Gate1Probe|read failed|poll failed|accessory poll error' \
    > /data/local/tmp/gate1_auto_hilog.log
  log "hilog captured /data/local/tmp/gate1_auto_hilog.log"
}

if [ "${GATE1_AUTO_LIB_ONLY:-0}" = "1" ]; then
  return 0 2>/dev/null || exit 0
fi

log "start bundle=$BUNDLE ability=$ABILITY"
bring_to_front

opened=0
attempt=0
while [ "$attempt" -lt 45 ]; do
  if dump_layout; then
    if layout_has "manufacturer="; then
      opened=1
      log "accessory already open"
      break
    fi
    if layout_has "1. 授权并打开"; then
      click_text "1. 授权并打开" authorize || true
    else
      log "authorize button not visible; retrying foreground/layout"
      bring_to_front
    fi
  fi
  attempt=$((attempt + 1))
  sleep 2
done

if [ "$opened" -eq 0 ]; then
  dump_layout || true
  if layout_has "manufacturer="; then
    opened=1
  fi
fi

if [ "$opened" -eq 1 ]; then
  click_text "启动 eventfd" start_eventfd || true
  log "eventfd start requested"

  sleep "$RUN_SECONDS"
  click_text "刷新统计" refresh_stats || true
  sleep 1
  dump_layout || true
  cp "$STATE" /data/local/tmp/gate1_auto_stats.json 2>/dev/null
  log "stats layout copied"

  click_text "停止并 join" stop_eventfd || true
  sleep 3
  click_text "关闭空闲句柄" close_idle || true
else
  log "accessory did not open"
  click_text "关闭空闲句柄" close_idle || true
fi

sleep 2
dump_layout || true
cp "$STATE" /data/local/tmp/gate1_auto_final.json 2>/dev/null
capture_hilog
log "done"
sync

if [ "$REBOOT_AFTER" = "1" ]; then
  log "reboot requested"
  sleep 1
  reboot
fi
