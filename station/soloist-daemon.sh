#!/bin/sh
# Keep the official Soloist daemon independent from the legacy receiver.
set -eu
BASE=/data/opt/spotify-connect
DATA=$BASE/receiver-state/soloist/data
# Audio is disposable. Keep it in RAM so it never consumes the small flash
# partition and it is naturally cleared after a reboot.
CACHE=/tmp/soloist-cache
KEY=$BASE/receiver-state/soloist/api-key
PID=/run/soloist-manual.pid

mkdir -p "$CACHE"

running() {
    [ -r "$PID" ] || return 1
    p=$(cat "$PID" 2>/dev/null) || return 1
    case "$p" in ''|*[!0-9]*) return 1;; esac
    kill -0 "$p" 2>/dev/null
}

if ! running && [ -r "$KEY" ]; then
    rm -f "$PID"
    nohup setsid /data/opt/solo/run.sh \
        --device-name "$(cat "$BASE/receiver-state/device-name" 2>/dev/null || echo "SpotyFly")" \
        --api-key "$(cat "$KEY")" \
        --cache-size 100 --ws 127.0.0.1:19090 \
        --data-dir "$DATA" --cache-dir "$CACHE" \
        </dev/null >/tmp/soloist-manual.log 2>&1 &
    echo "$!" >"$PID"
fi
