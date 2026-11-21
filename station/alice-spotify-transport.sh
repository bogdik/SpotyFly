#!/bin/sh
# Voice transport for Soloist: «дальше», «назад», «очисти очередь».
# While the voice radio runs (alice-radio.sh has a source) «дальше» takes the
# queued track, or one from the radio at once, and «назад» walks back through
# what actually played (radio-history.txt, last line = playing now).  Without
# the radio (the phone chose a playlist) Spotify's own next/prev are used.
set -eu

STATE=/data/opt/spotify-connect/receiver-state/web-api
HISTORY=$STATE/radio-history.txt
SOLO_DATA=/data/opt/spotify-connect/receiver-state/soloist/data
RADIO=/data/opt/spotify-connect/alice-radio.sh
OP=${1:-}
umask 077

case "$OP" in
    reset) exit 0 ;;   # «пауза» keeps the radio: «продолжи» goes on with it
    clear) exec "$RADIO" stop "очисти очередь" ;;   # ours still queued are skipped
    next) exec "$RADIO" skip ;;
    previous) ;;
    *) exit 64 ;;
esac

"$RADIO" active || exec /data/opt/solo/run.sh ctl prev -D "$SOLO_DATA"
n=$(wc -l <"$HISTORY" 2>/dev/null || echo 0)
[ "$n" -ge 2 ] || exit 0         # nothing before this one; Spotify `prev` would rewind it
uri=$(sed -n "$((n - 1))p" "$HISTORY")
case "$uri" in spotify:track:[A-Za-z0-9]*) ;; *) exit 2 ;; esac
# drop the playing one: the earlier track is the last line again (and is not re-added)
sed '$d' "$HISTORY" >"$HISTORY.new" && mv "$HISTORY.new" "$HISTORY"
"$RADIO" forget "$uri"
exec /data/opt/solo/run.sh ctl play "$uri" -D "$SOLO_DATA"
