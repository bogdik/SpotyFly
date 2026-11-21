#!/bin/sh
# «Алиса, ты поставила не то» / «что за дичь ты включила» (2026-11-19).
#
#   alice-wrong-pick.sh REQUEST    REQUEST = the last music request in the user's
#                                  own words, as the proxy keeps it ("@artist стая",
#                                  "@album страна льдов|стая", "группа крови")
#
# The track playing now was the wrong pick, so:
#   * it leaves the monitor's history (playback list, the search that chose it)
#     and its artist leaves the radio's past artists (else the random tracks
#     would bring it back);
#   * the request is searched again with that track, its artist and its album
#     excluded (SEARCH_NOT for alice-spotify-search.sh) -> something else.
set -u
REQ=${1:-}
BASE=/data/opt/spotify-connect
STATE=$BASE/receiver-state/web-api
MON=$BASE/monitor
R=/data/opt/solo/run.sh
JOURNAL=/tmp/alice-voice-journal.log
LOG=/tmp/alice-wrong-pick.log

log() { echo "$(date +%H:%M:%S) $*" >>"$LOG"; }
journal() { printf '%s\tplay\t%s\n' "$(date +%s)" "$*" >>"$JOURNAL" 2>/dev/null || true; }
# delete the latest of the last 3 lines of file $1 whose first field ($2 = f1)
# or whole line ($2 = line) is $3 (poll-status may have logged the next track
# after it meanwhile)
drop_recent() {
    [ -s "$1" ] && [ -n "$3" ] || return 0
    V=$3 M=$2 awk -F'\t' '{ l[NR] = $0; k[NR] = (ENVIRON["M"] == "f1") ? $1 : $0 }
        END { d = 0
              for (i = NR; i > NR - 3 && i > 0; i--) if (k[i] == ENVIRON["V"]) { d = i; break }
              for (i = 1; i <= NR; i++) if (i != d) print l[i] }' "$1" >"$1.new" && mv "$1.new" "$1"
}

[ -n "$REQ" ] || exit 2
now=$($R ctl now --json -w 127.0.0.1:19090 2>/dev/null || true)
uri=$(printf '%s' "$now" | sed -n 's/.*"item":{"uri":"\(spotify:track:[A-Za-z0-9]*\)".*/\1/p' | head -1)
au=$(printf '%s' "$now" | sed -n 's/.*"creators":\[{"entity":{"uri":"\(spotify:artist:[A-Za-z0-9]*\)".*/\1/p' | head -1)
ctx=$(printf '%s' "$now" | sed -n 's/.*"context":{"uri":"\(spotify:album:[A-Za-z0-9]*\)".*/\1/p' | head -1)
title=$(sed -n 's/.*"title":"\([^"]*\)".*/\1/p' "$MON/status.json" 2>/dev/null)
artist=$(sed -n 's/.*"artist":"\([^"]*\)".*/\1/p' "$MON/status.json" 2>/dev/null)
log "wrong: $uri ($title — $artist) artist $au ctx $ctx; again: $REQ"

SH=$STATE/radio-search-history.txt
sh_n=$(wc -l <"$SH" 2>/dev/null || echo 0); sh_last=$(tail -n 1 "$SH" 2>/dev/null)
line="$title${artist:+ — $artist}"
journal "не то: «${line:-?}» — убираю из истории, ищу заново"

# first the new pick (the wrong track stops), then the wrong one leaves the lists
SEARCH_NOT="$uri $au $ctx" "$BASE/alice-spotify-search.sh" "$REQ" >/dev/null 2>&1
rc=$?
log "search again rc=$rc"
drop_recent "$MON/history-uris.txt" f1 "$uri"
drop_recent "$STATE/radio-history.txt" f1 "$uri"
drop_recent "$MON/history-display.txt" line "$line"
# the search that chose it: the line that was last before this one
[ "$sh_n" -gt 0 ] && [ "$(sed -n "${sh_n}p" "$SH" 2>/dev/null)" = "$sh_last" ] &&
    sed "${sh_n}d" "$SH" >"$SH.new" && mv "$SH.new" "$SH"
if [ -n "$au" ] && [ -s "$STATE/radio-artists.txt" ]; then     # not for the random tracks
    awk -F'\t' -v a="$au" '$1 != a' "$STATE/radio-artists.txt" >"$STATE/radio-artists.txt.new" &&
        mv "$STATE/radio-artists.txt.new" "$STATE/radio-artists.txt"
    chmod 600 "$STATE/radio-artists.txt" 2>/dev/null
fi
[ "$rc" = 0 ] || journal "не нашла другого для «$REQ»"
exit "$rc"
