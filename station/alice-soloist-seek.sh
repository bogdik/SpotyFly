#!/bin/sh
# Seek through Soloist's local WebSocket API.  The app receives only an
# operation name; this helper reads current state and clamps the target.
set -eu

DATA=/data/opt/spotify-connect/receiver-state/soloist/data
STATUS=$(/data/opt/solo/run.sh ctl now --json -D "$DATA" 2>/dev/null)
POSITION=$(printf '%s' "$STATUS" | sed -n 's/.*"position_ms":\([0-9][0-9]*\).*/\1/p')
DURATION=$(printf '%s' "$STATUS" | sed -n 's/.*"duration_ms":\([0-9][0-9]*\).*/\1/p')

case "${POSITION:-}" in ''|*[!0-9]*) exit 1 ;; esac
case "${DURATION:-}" in ''|*[!0-9]*) exit 1 ;; esac

seconds=${2:-10}
case "$seconds" in ''|*[!0-9]*) seconds=10;; esac
[ "$seconds" -le 3600 ] || seconds=3600

case "${1:-}" in
    start) target=0 ;;
    end) target=$((DURATION - 10000)); [ "$target" -ge 0 ] || target=0 ;;
    forward) target=$((POSITION + seconds * 1000)) ;;
    back) target=$((POSITION - seconds * 1000)); [ "$target" -ge 0 ] || target=0 ;;
    *) exit 2 ;;
esac
[ "$target" -le "$DURATION" ] || target=$DURATION
exec /data/opt/solo/run.sh ctl seek "$target" -D "$DATA"
