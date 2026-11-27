#!/bin/sh
# Pushed to the tablet and run in the background BEFORE DeskMate switches the
# port to accessory mode.
#
# Why this exists: on this tablet the ArkTS timers of a backgrounded ability are
# suspended. AccessoryLink polls getAccessoryList() from setInterval, so once the
# port flips to 18d1:2d00 the poll stops and the accessory is never opened -
# the Host then sees rx=0 tx=0 and reports hello_timeout. An `aa start` issued
# from the device shell after the flip re-creates the page and the poll resumes.
#
# Usage (from the Mac, while the tablet is still in HDC mode):
#   HDC=.../hdc
#   "$HDC" file send tools/device-foreground.sh /data/local/tmp/deskmate_fg.sh
#   "$HDC" shell "chmod 755 /data/local/tmp/deskmate_fg.sh"
#   "$HDC" shell "nohup sh /data/local/tmp/deskmate_fg.sh >/dev/null 2>&1 &"
#   # then start DeskMate.app on the Mac
#
# The script keeps re-issuing `aa start` for a while so the app survives the
# re-enumeration no matter how long the Host takes to claim the interface.

set -u

BUNDLE=${DESKMATE_BUNDLE:-com.supremacy.deskmate}
ABILITY=${DESKMATE_ABILITY:-EntryAbility}
LOG=${DESKMATE_FG_LOG:-/data/local/tmp/deskmate_fg.log}
ROUNDS=${DESKMATE_FG_ROUNDS:-12}
INTERVAL=${DESKMATE_FG_INTERVAL:-5}

: > "$LOG"

log() {
  echo "$(date +%s) $*" >> "$LOG"
}

start_ability() {
  out=$(aa start -a "$ABILITY" -b "$BUNDLE" 2>&1)
  rc=$?
  if [ "$rc" -eq 0 ] && ! echo "$out" | grep -Eiq 'error:|failed to start|Error Code'; then
    log "aa start ok rc=$rc"
    return 0
  fi
  log "aa start failed rc=$rc out=$(echo "$out" | tr '\n' ' ')"
  return 1
}

log "start bundle=$BUNDLE ability=$ABILITY rounds=$ROUNDS interval=$INTERVAL"

round=0
while [ "$round" -lt "$ROUNDS" ]; do
  start_ability
  round=$((round + 1))
  sleep "$INTERVAL"
done

log "done"
sync
