#!/bin/sh
set -u

LOG=/data/local/tmp/gate1_auto_v5.log
LAYOUT=/data/local/tmp/gate1_auto_v5_layout.json
HILOG=/data/local/tmp/gate1_hilog_v5.txt
: > "$LOG"

log() {
  echo "$(date +%s) $*" >> "$LOG"
}

dump() {
  uitest dumpLayout -p "$LAYOUT" >/dev/null 2>&1
}

capture_step() {
  dump
  cp "$LAYOUT" "/data/local/tmp/gate1_auto_v5_$1.json"
  log "layout $1"
}

click() {
  capture_step "before_$3"
  uitest uiInput click "$1" "$2" >/dev/null 2>&1
  log "click $3 at $1,$2"
  sleep 1
  capture_step "after_$3"
}

log start
sleep 5
log "aa start after AOA window"
aa start -a EntryAbility -b com.deskmate.gate1probe >/dev/null 2>&1
sleep 2

opened=0
attempt=0
while [ "$attempt" -lt 30 ]; do
  dump
  if grep -q 'manufacturer=' "$LAYOUT"; then
    opened=1
    log "accessory already open"
    break
  fi
  if grep -q 'Accessory:' "$LAYOUT"; then
    click 172 391 authorize
  else
    log "accessory UI not present; retry aa start"
    aa start -a EntryAbility -b com.deskmate.gate1probe >/dev/null 2>&1
    sleep 2
  fi
  attempt=$((attempt + 1))
  sleep 1
done

if [ "$opened" -eq 0 ]; then
  dump
  if grep -q 'manufacturer=' "$LAYOUT"; then
    opened=1
  fi
fi

if [ "$opened" -eq 1 ]; then
  click 167 581 start_eventfd
  log "eventfd start requested"

  sleep 90
  click 136 1128 refresh_stats
  sleep 1
  dump
  cp "$LAYOUT" /data/local/tmp/gate1_auto_v5_stats.json
  log "stats captured"

  click 435 581 stop_eventfd
  sleep 3
  click 463 391 close_idle
else
  log "accessory did not open"
  click 463 391 close_idle
fi

sleep 2
dump
cp "$LAYOUT" /data/local/tmp/gate1_auto_v5_final.json
hilog -x -T Gate1Device,Gate1Probe > "$HILOG" 2>&1
log "hilog captured"
log done
sync
sleep 1
log reboot
reboot
