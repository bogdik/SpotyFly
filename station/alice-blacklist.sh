#!/bin/sh
# Чёрный список: artists and tracks that must never play.
#
#   alice-blacklist.sh add-artist current|NAME   now-playing artist, or found by name
#   alice-blacklist.sh add-track current         now-playing track
#   alice-blacklist.sh remove URI                (the monitor's ✕)
#   alice-blacklist.sh check  < ctl-now-json     exit 0 + reason if it is blacklisted
#   alice-blacklist.sh filter < web-api-json     allowed track URIs, in order, deduped
#   alice-blacklist.sh has-track URI | has-artist-name NAME
#   alice-blacklist.sh skip [REASON]             skip off a blacklisted track, silently
#
# $LIST holds "kind<TAB>uri<TAB>name<TAB>epoch", kind = artist|track (track name
# is "Title — Artist").  Callers: alice-voice-proxy (voice add), auto-advance
# (skips what plays), alice-spotify-search.sh / spotify-oauth.sh (never start or
# queue a blacklisted track), monitor-server (list / remove).
set -u
BASE=/data/opt/spotify-connect
STATE=$BASE/receiver-state/web-api
MARKET=$(cat "$STATE/market" 2>/dev/null); case "$MARKET" in [A-Z][A-Z]) ;; *) MARKET=US ;; esac   # the account's country (spotify-oauth.sh market)
LIST=${BLACKLIST_FILE:-$BASE/receiver-state/blacklist.txt}   # override: tests
DATA=$BASE/receiver-state/soloist/data
R=/data/opt/solo/run.sh
TRANSPORT=$BASE/alice-spotify-transport.sh
RADIO=$BASE/alice-radio.sh
JOURNAL=/tmp/alice-voice-journal.log
LOG=/tmp/alice-blacklist.log
QUIET=/tmp/soloist-draining     # alice-voice-proxy holds the music at 0 while set
SKIPLOCK=/tmp/alice-blacklist-skip.lock

port=$(cat "$DATA/ws.port" 2>/dev/null || true)
case "$port" in ''|*[!0-9]*) port=19090 ;; esac
W="-w 127.0.0.1:$port"

log() { echo "$(date +%H:%M:%S) $*" >>"$LOG"; }
journal() { printf '%s\tbl\t%s\n' "$(date +%s)" "$*" >>"$JOURNAL" 2>/dev/null || true; }
lower() { printf '%s' "$1" | tr 'A-Z' 'a-z'; }

# --- the now-playing track, from `ctl now --json` ------------------------------
now_json() { $R ctl now --json $W 2>/dev/null || true; }
track_uri() { sed -n 's/.*"item":{"uri":"\(spotify:track:[A-Za-z0-9]*\)".*/\1/p' | head -1; }
track_title() { sed 's/"identity":{"name":"/\n"identity":{"name":"/g' | sed -n '2s/.*"name":"\([^"]*\)".*/\1/p'; }
# One "artist-uri<TAB>name" line per creator, first = main artist.
creators() {
    sed -n 's/.*"creators":\[\(.*\)\],"playback".*/\1/p' | sed 's/{"entity":/\n/g' |
        sed -n 's/.*"uri":"\(spotify:artist:[A-Za-z0-9]*\)".*"identity":{"name":"\([^"]*\)".*/\1\t\2/p'
}

listed() { [ -r "$LIST" ] && grep -q "^[a-z]*	$1	" "$LIST"; }   # $1 = uri

add() { # kind uri name
    case "$2" in spotify:artist:[A-Za-z0-9]*|spotify:track:[A-Za-z0-9]*) ;; *) return 2 ;; esac
    if listed "$2"; then log "already listed: $1 $3"; return 0; fi
    umask 077
    printf '%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$(date +%s)" >>"$LIST"
    sync
    log "added $1 $3 ($2)"
}

# Why the now-playing JSON (stdin) is blacklisted; nothing and exit 1 if it isn't.
check() {
    [ -s "$LIST" ] || return 1
    j=$(cat)
    u=$(printf '%s' "$j" | track_uri)
    [ -n "$u" ] || return 1
    if listed "$u"; then
        printf 'трек %s\n' "$(printf '%s' "$j" | track_title)"; return 0
    fi
    printf '%s\n' "$j" | creators | while IFS='	' read -r au an; do
        if listed "$au" || has_artist_name "$an"; then printf '%s\n' "$an"; exit 0; fi
    done | head -1 | grep . && return 0
    return 1
}

has_artist_name() { # exact name, latin case-insensitive
    [ -n "$1" ] && [ -r "$LIST" ] || return 1
    n=$(lower "$1")
    awk -F'\t' -v n="$n" '$1=="artist" && tolower($3)==n { f=1 } END { exit !f }' "$LIST"
}

# Track URIs from a Spotify Web API answer (search, /me/tracks) on stdin,
# minus the blacklisted ones.  In each track object the album's and the
# track's artists come before the track's own uri, so artists seen since the
# last track uri belong to the next one.
filter() {
    grep -oE 'spotify:(artist|track):[A-Za-z0-9]+' |
        awk -F'\t' -v list="$LIST" '
            BEGIN { while ((getline l < list) > 0) { split(l, f, "\t"); bad[f[2]] = 1 } }
            /^spotify:artist:/ { if ($0 in bad) hit = 1; next }
            { if (!hit && !($0 in bad) && !seen[$0]++) print; hit = 0 }'
}

# --- skipping --------------------------------------------------------------------
quiet_on() { echo $(( $(date +%s) + 12 )) >"$QUIET"; }
# How many of the next queued tracks are blacklisted ("N. [queue] Title - A1, A2").
queue_bad_run() {
    [ -s "$LIST" ] || { echo 0; return; }
    $R ctl queue 25 $W 2>/dev/null | sed -n 's/^ *[0-9]*\. \[[a-z]*\] //p' |
        awk -F'\t' -v list="$LIST" '
            BEGIN { while ((getline l < list) > 0) { split(l, f, "\t")
                        if (f[1] == "artist") a[f[3]] = 1; else t[f[3]] = 1 } }
            { rest = $0; last = 0; off = 0          # the LAST " - " splits title / artists
              while ((p = index(rest, " - ")) > 0) { last = off + p; off += p + 2; rest = substr(rest, p + 3) }
              if (!last) exit
              title = substr($0, 1, last - 1); n = split(substr($0, last + 3), ar, ", "); bad = 0
              for (k = 1; k <= n; k++) if (ar[k] in a) bad = 1
              if ((title " — " ar[1]) in t) bad = 1
              if (!bad) exit
              run++ }
            END { print run + 0 }'
}

skip() { # reason
    mkdir "$SKIPLOCK" 2>/dev/null || return 0
    trap 'rmdir "$SKIPLOCK" 2>/dev/null; rm -f "$QUIET"' EXIT
    quiet_on
    journal "пропускаю (чёрный список): ${1:-?}"
    log "skip: ${1:-?}"
    rounds=0
    while [ "$rounds" -lt 8 ]; do
        j=$(now_json); u=$(printf '%s' "$j" | track_uri)
        why=$(printf '%s' "$j" | check) || break       # landed on an allowed track
        rounds=$((rounds + 1)); quiet_on
        moved=0
        for how in first fallback; do
            if [ "$how" = fallback ]; then
                $R ctl next $W >/dev/null 2>&1 || true      # e.g. the voice radio ran out
            elif "$RADIO" active; then
                "$TRANSPORT" next >/dev/null 2>&1 || true   # same path as «следующий»
            else
                k=$(( $(queue_bad_run) + 1 )); i=0          # past every bad one queued
                while [ "$i" -lt "$k" ] && [ "$i" -lt 25 ]; do
                    $R ctl next $W >/dev/null 2>&1 || true; i=$((i + 1))
                done
            fi
            i=0                                             # wait for the switch to land
            while [ "$i" -lt 12 ]; do
                usleep 250000 2>/dev/null || sleep 1
                [ "$(now_json | track_uri)" != "$u" ] && { moved=1; break; }
                i=$((i + 1))
            done
            [ "$moved" = 1 ] && break
        done
        if [ "$moved" = 0 ]; then                           # nothing left to skip to
            $R ctl pause $W >/dev/null 2>&1 || true
            journal "дальше только чёрный список — пауза"
            log "skip: no other track, paused"
            break
        fi
    done
    usleep 300000 2>/dev/null || true
}

# --- resolving an artist said by name ----------------------------------------------
artist_by_name() { # name -> "uri<TAB>name" via the Spotify search API (app token)
    [ -r "$STATE/net.conf" ] && . "$STATE/net.conf" 2>/dev/null
    PROXY=${PROXY:-}
    [ -r "$STATE/credentials.env" ] || return 1
    . "$STATE/credentials.env"
    CA=/data/opt/solo/ca-certificates.crt
    req() { /opt/bin/curl --fail --silent --connect-timeout 5 --max-time 12 -x "$PROXY" --cacert "$CA" "$@" ||
            /opt/bin/curl --fail --silent --connect-timeout 5 --max-time 12 --cacert "$CA" "$@"; }
    tok=$(req -u "$CLIENT_ID:$CLIENT_SECRET" --data grant_type=client_credentials \
        https://accounts.spotify.com/api/token | sed -n 's/.*"access_token":"\([^"]*\)".*/\1/p')
    [ -n "$tok" ] || return 1
    res=$(req -H "Authorization: Bearer $tok" --get --data-urlencode "q=$1" \
        --data type=artist --data limit=1 --data "market=$MARKET" https://api.spotify.com/v1/search)
    au=$(printf '%s' "$res" | grep -oE 'spotify:artist:[A-Za-z0-9]+' | head -1)
    # first "name" after "items" is the artist's (external_urls/followers/genres/
    # images that come before it carry none)
    an=$(printf '%s' "$res" | sed 's/.*"items":\[//' | grep -o '"name":"[^"]*"' | head -1 |
        sed 's/^"name":"//; s/"$//')
    [ -n "$au" ] && [ -n "$an" ] || return 1
    printf '%s\t%s\n' "$au" "$an"
}

# A blacklisted artist that is also the current voice radio: drop the radio,
# or every "next" would land on that artist again.
drop_radio_of() { # artist name
    [ "$(cut -f3 "$STATE/radio-source" 2>/dev/null)" = "$1" ] || return 0
    "$RADIO" stop "в чёрном списке: $1"
}

case "${1:-}" in
    add-artist)
        if [ "${2:-current}" = current ]; then
            line=$(now_json | creators | head -1)
        else
            line=$(artist_by_name "$2") || { journal "не нашла исполнителя «$2» для чёрного списка"; log "not found: $2"; exit 1; }
        fi
        au=$(printf '%s' "$line" | cut -f1); an=$(printf '%s' "$line" | cut -f2)
        [ -n "$au" ] || { log "add-artist: nothing playing"; exit 1; }
        add artist "$au" "$an" || exit 1
        journal "в чёрный список: исполнитель $an"
        drop_radio_of "$an"
        why=$(now_json | check) && skip "$why"
        exit 0 ;;
    add-track)
        j=$(now_json)
        u=$(printf '%s' "$j" | track_uri); t=$(printf '%s' "$j" | track_title)
        an=$(printf '%s\n' "$j" | creators | head -1 | cut -f2)
        [ -n "$u" ] || { log "add-track: nothing playing"; exit 1; }
        add track "$u" "$t${an:+ — $an}" || exit 1
        journal "в чёрный список: трек $t${an:+ — $an}"
        skip "трек $t"
        exit 0 ;;
    remove)
        case "${2:-}" in spotify:artist:[A-Za-z0-9]*|spotify:track:[A-Za-z0-9]*) ;; *) exit 2 ;; esac
        [ -r "$LIST" ] || exit 0
        name=$(awk -F'\t' -v u="$2" '$2==u {print $3; exit}' "$LIST")
        awk -F'\t' -v u="$2" '$2!=u' "$LIST" >"$LIST.new" && mv "$LIST.new" "$LIST" && sync
        [ -n "$name" ] && { journal "убрано из чёрного списка: $name"; log "removed $name ($2)"; }
        exit 0 ;;
    check) check ;;
    filter)
        if [ -s "$LIST" ]; then filter; else grep -oE 'spotify:track:[A-Za-z0-9]+' | awk '!s[$0]++'; fi ;;
    has-track) listed "${2:-x}" ;;
    has-artist-name) listed "${2:-x}" || has_artist_name "${2:-}" ;;
    skip) skip "${2:-}" ;;
    list) [ -r "$LIST" ] && cat "$LIST" ;;
    *) echo "usage: $0 add-artist current|NAME | add-track current | remove URI | check | filter | skip" >&2; exit 2 ;;
esac
