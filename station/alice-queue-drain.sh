#!/bin/sh
# «очисти очередь» (2026-11-19): empty Soloist's Spotify queue by skipping
# through it, one track at a time.
#
# The queue lives on Spotify's side: restarting Soloist does not clear it,
# Soloist has no clear/set-queue command, and a burst of skip_next is ignored
# (2026-11-14).  The one thing that removes a queued track is playing past it,
# so: `ctl next`, wait until the queue is one shorter, again (~1 s a track),
# while $FLAG holds the music at 0 (alice-voice-proxy) and alice-auto-advance /
# poll-status ignore the tracks that flash by.  Then the track that was playing
# goes back where it was (paused if it was).  The voice radio stays on: it
# queues its next 2 tracks again near the end of this one.
#
#   alice-queue-drain.sh count   print how many tracks are queued
#   alice-queue-drain.sh         drain; the result goes to $RESULT for
#                                alice-voice-proxy, which says it aloud:
#                                "done N" | "partial LEFT" | "empty" | "busy"
# $FLAG holds an expiry time (epoch s), so a killed drain can't mute for good.
set -u
DATA=/data/opt/spotify-connect/receiver-state/soloist/data
R=/data/opt/solo/run.sh
FLAG=/tmp/soloist-draining
RESULT=/tmp/alice-queue-clear.result
LOG=/tmp/alice-queue-drain.log
MAX_S=300        # give up after this long
STUCK=4          # skips in a row that remove nothing -> give up

port=$(cat "$DATA/ws.port" 2>/dev/null || true)
case "$port" in ''|*[!0-9]*) port=19090 ;; esac
W="-w 127.0.0.1:$port"

log() { echo "$(date +%H:%M:%S) $*" >>"$LOG"; }
arm() { echo $(( $(date +%s) + 15 )) >"$FLAG"; }
nap() { usleep "$1" 2>/dev/null || sleep 1; }
status() { $R ctl now --json $W 2>/dev/null | sed -n 's/.*"status":"\([a-z]*\)".*/\1/p' | head -1; }
# Queued tracks.  While a track is "buffering" Soloist lists only the first 10,
# so count once it plays (3 s at most).
qcount() {
    i=0
    while [ "$i" -lt 12 ]; do
        case "$(status)" in playing|paused) break ;; esac
        nap 250000
        i=$((i + 1))
    done
    n=$($R ctl queue 1000 $W 2>/dev/null | grep -c '\[queue\]')
    case "$n" in ''|*[!0-9]*) n=0 ;; esac
    printf '%s\n' "$n"
}
finish() { printf '%s\n' "$1" >"$RESULT.new" && mv "$RESULT.new" "$RESULT"; log "result: $1"; }

if [ "${1:-}" = count ]; then
    $R ctl queue 1000 $W 2>/dev/null | grep -c '\[queue\]'
    exit 0
fi

LOCK=/tmp/alice-queue-drain.lock
mkdir "$LOCK" 2>/dev/null || { finish busy; exit 0; }
trap 'rm -f "$FLAG"; rmdir "$LOCK" 2>/dev/null' EXIT
trap 'exit 1' HUP INT TERM

n=$(qcount)
[ "$n" -gt 0 ] || { finish empty; exit 0; }
total=$n

now=$($R ctl now --json $W 2>/dev/null || true)
uri=$(printf '%s' "$now" | sed -n 's/.*"item":{"uri":"\(spotify:track:[A-Za-z0-9]*\)".*/\1/p' | head -1)
pos=$(printf '%s' "$now" | sed -n 's/.*"position_ms":\([0-9]*\).*/\1/p' | head -1)
was=$(printf '%s' "$now" | sed -n 's/.*"status":"\([a-z]*\)".*/\1/p' | head -1)
case "${pos:-0}" in *[!0-9]*|'') pos=0 ;; esac

arm
nap 300000                     # let the proxy take the music down
log "drain: $n queued (now $uri at $pos ms, $was)"
start=$(date +%s); stuck=0; skips=0
while [ "$n" -gt 0 ] && [ $(( $(date +%s) - start )) -lt "$MAX_S" ]; do
    arm
    before=$n
    $R ctl next $W >/dev/null 2>&1
    skips=$((skips + 1))
    n=$(qcount)
    if [ "$n" -lt "$before" ]; then stuck=0
    else
        stuck=$((stuck + 1))
        [ "$stuck" -ge "$STUCK" ] && { log "  no progress after $stuck skips"; break; }
    fi
done
log "drain: $skips skips in $(( $(date +%s) - start )) s, $n left"

case "$uri" in
    spotify:track:[A-Za-z0-9]*)
        $R ctl play "$uri" $W >/dev/null 2>&1
        if [ "$pos" -gt 3000 ]; then
            nap 700000
            $R ctl seek "$pos" $W >/dev/null 2>&1
        fi
        [ "$was" = paused ] && $R ctl pause $W >/dev/null 2>&1
        nap 500000             # the old track is back before the music comes up
        ;;
esac
if [ "$n" -eq 0 ]; then finish "done $total"; else finish "partial $n"; fi
