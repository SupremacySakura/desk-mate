#!/bin/sh
set -u

LOG=/data/local/tmp/gate1_auto.log
STATE=/data/local/tmp/gate1_auto_layout.json
: > "$LOG"

log() {
  echo "$(date +%s) $*" >> "$LOG"
}

dump() {
  uitest dumpLayout -p "$STATE" >/dev/null 2>&1
}

click() {
  uitest uiInput click "$1" "$2" >/dev/null 2>&1
  log "click $3 at $1,$2"
}

log start

opened=0
attempt=0
while [ "$attempt" -lt 45 ]; do
  dump
  if grep -q 'Accessory:' "$STATE" && ! grep -q 'manufacturer=' "$STATE"; then
    click 172 391 authorize
  elif grep -q 'manufacturer=' "$STATE"; then
    opened=1
    log "opened"
    break
  fi
  attempt=$((attempt + 1))
  sleep 2
done

if [ "$opened" -eq 0 ]; then
  dump
  if grep -q 'manufacturer=' "$STATE"; then
    opened=1
  fi
fi

if [ "$opened" -eq 1 ]; then
  click 167 581 start_eventfd
  started=1
  log "eventfd start requested"

  sleep 40
  click 136 1128 refresh_stats
  sleep 1
  dump
  cp "$STATE" /data/local/tmp/gate1_auto_stats.json
  log "stats captured started=$started"

  click 435 581 stop_eventfd
  sleep 3
  click 463 391 close_idle
else
  log "accessory did not open"
  click 463 391 close_idle
fi

sleep 2
dump
cp "$STATE" /data/local/tmp/gate1_auto_final.json
log done
sync
sleep 1
log reboot
reboot
