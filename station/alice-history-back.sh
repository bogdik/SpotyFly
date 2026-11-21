#!/bin/sh
# «Включи трек, который играл N песен назад»: alice-voice-proxy runs
#   alice-history-back.sh N
# history-uris.txt (written by monitor/poll-status.sh) holds one line per played
# track, "uri<TAB>title<TAB>artist", the last line being the track playing now.
# N back = the line N above it.  It is played as that exact track (no text
# search) the way a pick in the monitor is: alice-spotify-search.sh --track.
set -u
HIST=/data/opt/spotify-connect/monitor/history-uris.txt
LOG=/tmp/alice-history-back.log
N=${1:-}
case "$N" in ''|*[!0-9]*) echo "bad N: $N" >>"$LOG"; exit 2 ;; esac
[ "$N" -ge 1 ] || exit 2

total=$(wc -l <"$HIST" 2>/dev/null || echo 0)
line=$((total - N))
if [ "$line" -lt 1 ]; then
    echo "$(date +%H:%M:%S) $N back: history has only $total tracks" >>"$LOG"
    exit 1
fi
entry=$(sed -n "${line}p" "$HIST")
uri=$(printf '%s' "$entry" | cut -f1)
title=$(printf '%s' "$entry" | cut -f2)
artist=$(printf '%s' "$entry" | cut -f3)
case "$uri" in spotify:track:[A-Za-z0-9]*) ;; *) echo "bad entry: $entry" >>"$LOG"; exit 1 ;; esac
echo "$(date +%H:%M:%S) $N back -> $title — $artist ($uri)" >>"$LOG"
exec /data/opt/spotify-connect/alice-spotify-search.sh --track "$uri" "$artist" "$title"
