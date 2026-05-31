#!/usr/bin/env bash
# claude-fire.sh <state>
# Fire-and-forget notifier for the M5Stack Fire "Claude companion" display.
# Never blocks the caller: backgrounds the curl with a tight timeout and always exits 0.
#
# State is one of: idle | thinking | working | waiting | done
# Device host/IP is read from ~/.claude/claude-fire.host (falls back to mDNS name).

STATE="${1:-idle}"
HOSTFILE="$HOME/.claude/claude-fire.host"
HOST="claude-fire.local"
[ -r "$HOSTFILE" ] && HOST="$(tr -d '[:space:]' < "$HOSTFILE")"

curl -s --max-time 0.4 "http://${HOST}/state?s=${STATE}" >/dev/null 2>&1 &
exit 0
