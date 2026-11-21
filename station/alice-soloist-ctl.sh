#!/bin/sh
# Pause / resume Soloist for the voice proxy («пауза», «продолжи», reboot).
# This was the --pause/--resume half of the old `spotiya` wrapper; the rest of
# it ran spotiya.receiver, which is gone.
#   --pause   stop playback (the voice radio stays: «продолжи» goes on with it)
#   --resume  continue playback
set -u
STATE=/data/opt/spotify-connect/receiver-state
DATA=$STATE/soloist/data
CTL=/data/opt/solo/run.sh

where() { # ws endpoint if Soloist published one, else its data dir
    port=$(cat "$DATA/ws.port" 2>/dev/null || true)
    case "$port" in
        ''|*[!0-9]*) echo "-D $DATA" ;;
        *) echo "-w 127.0.0.1:$port" ;;
    esac
}

case "${1:-}" in
    --pause)
        # shellcheck disable=SC2046
        exec "$CTL" ctl pause $(where) ;;
    --resume)
        # shellcheck disable=SC2046
        exec "$CTL" ctl play $(where) ;;
    *)
        echo "usage: $0 --pause|--resume" >&2
        exit 64 ;;
esac
