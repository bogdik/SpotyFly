#!/bin/sh
# Voice radio: what plays after the track that was asked for (policy 2026-11-15).
#
#   * A request sets the SOURCE: an artist («включи группу X»; «включи песню X»
#     and a pick in the monitor -> that track's artist) or the favourites.
#   * 10 s before a track ends alice-auto-advance.sh calls `topup`: the Spotify
#     queue is filled up to AHEAD (2) tracks, never 9-10 at once.  Tracks the
#     owner queued from the phone count too, and play first.
#   * The source's tracks play in random order (random search pages, shuffled)
#     and never twice within a day (radio-played.txt, also by title, so a live
#     or remastered copy is a repeat too).  A track asked for by name always plays.
#   * Once the artist has nothing unplayed left, every next track is random: a
#     favourite or a track of an artist asked for before (coin flip each time).
#   * Spotify cannot remove a queued track.  Ours queued for an older source are
#     "stale": auto-advance skips them silently when they come up.
#
#   alice-radio.sh start artist URI NAME FIRST [--page0]   (--page0: stdin = search JSON at offset 0)
#   alice-radio.sh favorites          play a random favourite, the rest is the source
#   alice-radio.sh stop [REASON]      no source any more: nothing is queued
#   alice-radio.sh topup              queue up to AHEAD tracks (auto-advance, T-10 s)
#   alice-radio.sh refill             keep the pool stocked (network; one at a time)
#   alice-radio.sh skip               «дальше»: the queued track, else one from the pool
#   alice-radio.sh played URI TITLE ARTIST    a track started playing
#   alice-radio.sh stale URI          exit 0 (and forget it): our track of an older source
#   alice-radio.sh forget URI         it is played on purpose: not stale
#   alice-radio.sh active             exit 0 while there is a source
#   alice-radio.sh main-artist        stdin = track JSON -> "uri<TAB>name" of its first track's artist
#   alice-radio.sh next-name          "Title — Artist" queued next by us (monitor)
set -u
BASE=/data/opt/spotify-connect
STATE=${RADIO_STATE:-$BASE/receiver-state/web-api}     # override: tests
[ -r "$STATE/net.conf" ] && . "$STATE/net.conf" 2>/dev/null || true
PROXY=${PROXY:-}
CREDS=$STATE/credentials.env
CA=/data/opt/solo/ca-certificates.crt
DATA=$BASE/receiver-state/soloist/data
R=${RADIO_CTL:-/data/opt/solo/run.sh}
BL=$BASE/alice-blacklist.sh
OAUTH=$BASE/spotify-oauth.sh
JOURNAL=/tmp/alice-voice-journal.log
LOG=/tmp/alice-radio.log
MARKET=$(cat "$STATE/market" 2>/dev/null); case "$MARKET" in [A-Z][A-Z]) ;; *) MARKET=US ;; esac   # the account's country (spotify-oauth.sh market)

SRC=$STATE/radio-source         # "kind<TAB>artist uri<TAB>artist name", kind = artist|fav; absent = no radio
GEN=$STATE/radio-gen            # bumped by every start / stop
POOL=$STATE/radio-pool.txt      # "uri<TAB>key<TAB>Title — Artist", next first
PAGES=$STATE/radio-pages        # artist search offsets not fetched yet, one per line
ALBUMS=$STATE/radio-albums      # gen, then the artist's albums not fetched yet ("gen done": none left)
QUEUED=$STATE/radio-queued.txt  # "gen<TAB>uri<TAB>key" put in the Spotify queue by us
PLAYED=$STATE/radio-played.txt  # "epoch<TAB>uri<TAB>key"
ARTISTS=$STATE/radio-artists.txt  # "uri<TAB>name" asked for, latest last
DRY=$STATE/radio-dry            # gen whose artist ran dry (said once in the journal)
LOCK=/tmp/alice-radio.lock
RLOCK=/tmp/alice-radio-refill.lock
FAILF=/tmp/alice-radio-fail     # epoch of the last failed refill
AHEAD=2         # queued tracks to keep ahead
LOW=3           # refill the pool below this
DAY=86400       # no repeats within this
PAGE_MAX=90     # artist pages at offsets 0..PAGE_MAX (100 tracks)
umask 077

port=$(cat "$DATA/ws.port" 2>/dev/null || true)
case "$port" in ''|*[!0-9]*) W="-D $DATA" ;; *) W="-w 127.0.0.1:$port" ;; esac

log() {
    echo "$(date +%H:%M:%S) $*" >>"$LOG"
    [ "$(wc -c <"$LOG")" -lt 65536 ] || { tail -n 200 "$LOG" >"$LOG.new"; mv "$LOG.new" "$LOG"; }
}
journal() { printf '%s\tplay\t%s\n' "$(date +%s)" "$*" >>"$JOURNAL" 2>/dev/null || true; }
rnd() { od -An -N4 -tu4 /dev/urandom | tr -d ' '; }
shuffle() { awk -v s="$(rnd)" 'BEGIN{srand(s)} {a[NR]=$0} END{for(i=NR;i>0;i--){j=int(rand()*i)+1; print a[j]; a[j]=a[i]}}'; }
gen() { g=$(cat "$GEN" 2>/dev/null); case "$g" in ''|*[!0-9]*) g=0 ;; esac; echo "$g"; }
active() { [ -s "$SRC" ]; }
pool_n() { if [ -s "$POOL" ]; then awk 'END{print NR}' "$POOL"; else echo 0; fi; }
lock() {
    i=0
    until mkdir "$LOCK" 2>/dev/null; do
        i=$((i + 1)); [ "$i" -gt 50 ] && rmdir "$LOCK" 2>/dev/null   # its holder was killed
        usleep 100000 2>/dev/null || sleep 1
    done
}
unlock() { rmdir "$LOCK" 2>/dev/null; }

# --- names --------------------------------------------------------------------
norm() { # stdin -> name for comparing: lower case, no spaces/punctuation, ё=е
    tr 'A-Z' 'a-z' | tr -d " .,!?:;'\"()&/_*+-" | sed 's/ё/е/g; s/Ё/е/g; s/А/а/g; s/Б/б/g;
        s/В/в/g; s/Г/г/g; s/Д/д/g; s/Е/е/g; s/Ж/ж/g; s/З/з/g; s/И/и/g; s/Й/й/g; s/К/к/g;
        s/Л/л/g; s/М/м/g; s/Н/н/g; s/О/о/g; s/П/п/g; s/Р/р/g; s/С/с/g; s/Т/т/g; s/У/у/g;
        s/Ф/ф/g; s/Х/х/g; s/Ц/ц/g; s/Ч/ч/g; s/Ш/ш/g; s/Щ/щ/g; s/Ы/ы/g; s/Э/э/g; s/Ю/ю/g;
        s/Я/я/g'
}
# "Song - Live at X", "Song (Remastered)" and "Song" are one song.
title_norm() { sed 's/ - .*//; s/ (.*//; s/ \[.*//' | norm; }
key_of() { # title artist -> "title|artist"
    printf '%s|%s' "$(printf '%s\n' "$1" | title_norm)" "$(printf '%s\n' "$2" | norm)"
}

# --- Web API JSON -------------------------------------------------------------
# "uri<TAB>title<TAB>main artist uri<TAB>main artist name<TAB>has" per track,
# has = 1 when $1 is among its (or its album's) artists.  In a track object the
# album (with its artists) comes first, then the track's artists, then its own
# name and uri; every artist's and album's name comes just before its uri.
tracks_of() { # $1 = artist uri to mark ("" = none); stdin = JSON
    grep -oE '"name":"([^"\\]|\\.)*"|"uri":"spotify:(artist|album|track):[A-Za-z0-9]+"' | awk -v want="$1" '
        /^"name":/ { nm = substr($0, 9, length($0) - 9); next }
        /spotify:artist:/ { u = substr($0, 8, length($0) - 8)
                            if (!alb) { if (aa == "") { aa = u; aan = nm } }
                            else if (ta == "") { ta = u; tan = nm }
                            if (u == want) has = 1; next }
        /spotify:album:/ { alb = 1; next }
        { u = substr($0, 8, length($0) - 8)
          if (ta == "") { ta = aa; tan = aan }
          if (!s[u]++) print u "\t" nm "\t" ta "\t" tan "\t" (has + 0)
          aa = ""; aan = ""; ta = ""; tan = ""; has = 0; alb = 0 }'
}
# Allowed tracks of the JSON on stdin -> "uri<TAB>key<TAB>Title — Artist";
# $1 = keep only this artist's ("" = all).  The чёрный список is applied here.
parse() {
    t=/tmp/alice-radio.parse.$$
    cat >"$t.j"
    tracks_of "$1" <"$t.j" | awk -F'\t' -v w="$1" 'w == "" || $5 == 1' >"$t.l"
    if [ -x "$BL" ]; then "$BL" filter <"$t.j" >"$t.ok"; else cut -f1 "$t.l" >"$t.ok"; fi
    cut -f2 "$t.l" | title_norm >"$t.t"
    cut -f4 "$t.l" | norm >"$t.a"
    awk -F'\t' -v f1="$t.ok" -v f2="$t.t" -v f3="$t.a" '
        BEGIN { while ((getline l < f1) > 0) ok[l] = 1
                n = 0; while ((getline l < f2) > 0) tt[++n] = l
                n = 0; while ((getline l < f3) > 0) ar[++n] = l }
        { k = tt[NR] "|" ar[NR]; if (ok[$1] && k != "|") print $1 "\t" k "\t" $2 " — " $4 }' "$t.l"
    rm -f "$t.j" "$t.l" "$t.ok" "$t.t" "$t.a"
}
# Of the pool lines on stdin: the ones not played within a day, not queued, not in the pool yet.
fresh() {
    touch "$PLAYED" "$POOL" "$QUEUED"
    awk -F'\t' -v now="$(date +%s)" -v day=$DAY -v pl="$PLAYED" -v po="$POOL" -v qu="$QUEUED" '
        BEGIN { while ((getline l < pl) > 0) { split(l, f, "\t"); if (now - f[1] < day) { u[f[2]] = 1; k[f[3]] = 1 } }
                while ((getline l < qu) > 0) { split(l, f, "\t"); u[f[2]] = 1; if (f[3] != "") k[f[3]] = 1 }
                while ((getline l < po) > 0) { split(l, f, "\t"); u[f[1]] = 1; k[f[2]] = 1 } }
        NF >= 3 && !u[$1] && !k[$2] { u[$1] = 1; k[$2] = 1; print }'
}

# --- Spotify search with the app token ---------------------------------------
curl_limited() {
    /opt/bin/curl --fail --location --silent --show-error --connect-timeout 5 --max-time 12 "$@" &
    cpid=$!
    ( sleep 13; kill -TERM "$cpid" 2>/dev/null || true; sleep 1; kill -KILL "$cpid" 2>/dev/null || true ) >/dev/null 2>&1 &
    wpid=$!
    wait "$cpid"; st=$?
    kill "$wpid" 2>/dev/null || true; wait "$wpid" 2>/dev/null || true
    return "$st"
}
req() { curl_limited -x "$PROXY" --cacert "$CA" "$@" 2>/dev/null || curl_limited --cacert "$CA" "$@" 2>/dev/null; }
TOKEN=""
token() {
    [ -n "$TOKEN" ] && return 0
    [ -r "$CREDS" ] || return 1
    . "$CREDS"
    TOKEN=$(req -u "$CLIENT_ID:$CLIENT_SECRET" --data grant_type=client_credentials \
        https://accounts.spotify.com/api/token | sed -n 's/.*"access_token":"\([^"]*\)".*/\1/p')
    [ -n "$TOKEN" ]
}
search_page() { # query offset -> track search JSON (limit 10: the app token's maximum)
    if [ -n "${RADIO_SEARCH:-}" ]; then "$RADIO_SEARCH" "$1" "$2"; return; fi   # tests
    token || return 1
    req -H "Authorization: Bearer $TOKEN" --get --data-urlencode "q=$1" --data type=track \
        --data limit=10 --data "offset=$2" --data "market=$MARKET" https://api.spotify.com/v1/search
}
liked_page() { # a random page of the owner's Liked Songs
    if [ -n "${RADIO_LIKED:-}" ]; then "$RADIO_LIKED"; return; fi              # tests
    "$OAUTH" liked-page 2>/dev/null
}
resolve_au() { # $1 = name, stdin = JSON -> the artist so named, else the most frequent one
    grep -oE '"name":"([^"\\]|\\.)*"|"uri":"spotify:artist:[A-Za-z0-9]+"' | awk -v n="$(printf '%s\n' "$1" | norm)" '
        /^"name":/ { nm = substr($0, 9, length($0) - 9); next }
        { u = substr($0, 8, length($0) - 8); c[u]++; nmu[u] = nm }
        END { for (u in c) if (c[u] > b) { b = c[u]; t = u }
              print t; for (u in nmu) print "N\t" u "\t" nmu[u] }' >/tmp/alice-radio.au.$$
    want=$(printf '%s\n' "$1" | norm)
    best=$(sed -n 1p /tmp/alice-radio.au.$$)
    while IFS='	' read -r tag u nm; do
        [ "$tag" = N ] || continue
        [ "$(printf '%s\n' "$nm" | norm)" = "$want" ] && { best=$u; break; }
    done </tmp/alice-radio.au.$$
    rm -f /tmp/alice-radio.au.$$
    printf '%s\n' "$best"
}

# --- the pool -----------------------------------------------------------------
add_pool() { # $1 = gen it was fetched for; stdin = pool lines (shuffled here)
    l=$(fresh | shuffle)
    lock
    [ "$(gen)" = "$1" ] && [ -n "$l" ] && printf '%s\n' "$l" >>"$POOL"
    unlock
}
pop() { # the next pool track not played meanwhile -> "uri<TAB>key<TAB>disp", out of the pool
    lock
    touch "$PLAYED" "$POOL"
    : >"$POOL.new"
    awk -F'\t' -v now="$(date +%s)" -v day=$DAY -v pl="$PLAYED" -v out="$POOL.new" '
        BEGIN { while ((getline l < pl) > 0) { split(l, f, "\t"); if (now - f[1] < day) { u[f[2]] = 1; k[f[3]] = 1 } } }
        u[$1] || k[$2] { next }
        !got { print; got = 1; next }
        { print > out }' "$POOL" >"$POOL.pick"
    mv "$POOL.new" "$POOL"
    unlock
    cat "$POOL.pick"; rm -f "$POOL.pick"
}
forget() { # $1 uri: drop it from the queued list
    [ -s "$QUEUED" ] || return 0
    lock
    awk -F'\t' -v u="$1" '$2 != u' "$QUEUED" >"$QUEUED.new"; mv "$QUEUED.new" "$QUEUED"
    unlock
}
is_stale() { # $1 uri: ours, queued for another source (or none)
    [ -s "$QUEUED" ] || return 1
    if active; then g=$(gen); else g=none; fi
    awk -F'\t' -v u="$1" -v g="$g" '$2 == u { s = ($1 != g) } END { exit !s }' "$QUEUED"
}
upcoming() { # queued (not context) track uris, next first
    $R ctl queue 100 --json $W 2>/dev/null |
        grep -o '"source":"queue","item":{"uri":"spotify:track:[A-Za-z0-9]*"' | sed 's/.*"uri":"//; s/"$//'
}

refill_artist() { # $1 gen; one random page of the source artist
    au=$(cut -f2 "$SRC"); an=$(cut -f3 "$SRC")
    o=$(shuffle <"$PAGES" | grep . | head -1)
    [ -n "$o" ] || return 0
    raw=$(search_page "artist:\"$an\"" "$o")
    printf '%s' "$raw" | grep -q '"tracks"' || return 1
    if [ -z "$au" ]; then          # a pick in the monitor names the artist only
        au=$(printf '%s' "$raw" | resolve_au "$an")
        [ -n "$au" ] || return 1
        lock; [ "$(gen)" = "$1" ] && printf 'artist\t%s\t%s\n' "$au" "$an" >"$SRC"; unlock
    fi
    mine=$(printf '%s' "$raw" | tracks_of "$au" | awk -F'\t' '$5 == 1' | awk 'END{print NR}')
    lock
    if [ "$(gen)" = "$1" ]; then   # an empty page: the artist has no more beyond it
        if [ "$mine" = 0 ]; then awk -v o="$o" '$1 < o' "$PAGES" >"$PAGES.new"
        else awk -v o="$o" '$1 != o' "$PAGES" >"$PAGES.new"; fi
        mv "$PAGES.new" "$PAGES"
    fi
    unlock
    printf '%s' "$raw" | parse "$au" | add_pool "$1"
    log "refill $an @$o: $mine by the artist, pool $(pool_n)"
}
# Tracks from the source artist's own albums, one random album per call: when
# the search pages are used up, or never had its tracks at all (2026-11-19:
# «Стая» — `artist:"Стая"` is all namesakes, the real one has 1 album, so the
# radio went random at once).  Returns 2 when every album has been fetched.
refill_albums() { # $1 gen
    au=$(cut -f2 "$SRC"); an=$(cut -f3 "$SRC")
    [ -n "$au" ] || return 2
    token || return 1
    if [ "$(sed -n 1p "$ALBUMS" 2>/dev/null)" != "$1" ]; then
        off=0
        while [ "$off" -lt 50 ]; do         # app token: limit <= 10
            p=$(req -H "Authorization: Bearer $TOKEN" \
                "https://api.spotify.com/v1/artists/${au#spotify:artist:}/albums?include_groups=album,single,compilation&limit=10&offset=$off&market=$MARKET")
            printf '%s' "$p" | grep -q 'spotify:album:' || break
            printf '%s\n' "$p"
            off=$((off + 10))
        done | grep -oE 'spotify:album:[A-Za-z0-9]+' | awk '!s[$0]++' | shuffle >"$ALBUMS.l"
        lock; [ "$(gen)" = "$1" ] && { echo "$1"; cat "$ALBUMS.l"; } >"$ALBUMS"; unlock
        rm -f "$ALBUMS.l"
    fi
    al=$(sed -n 2p "$ALBUMS" 2>/dev/null)
    [ -n "$al" ] || return 2
    lock; [ "$(gen)" = "$1" ] && { sed -n 1p "$ALBUMS"; sed -n '3,$p' "$ALBUMS"; } >"$ALBUMS.new" && mv "$ALBUMS.new" "$ALBUMS"; unlock
    raw=$(req -H "Authorization: Bearer $TOKEN" \
        "https://api.spotify.com/v1/albums/${al#spotify:album:}/tracks?limit=50&market=$MARKET")
    printf '%s' "$raw" | grep -q 'spotify:track:' || return 1
    printf '%s' "$raw" | parse "$au" | add_pool "$1"
    log "refill $an album ${al#spotify:album:}: pool $(pool_n)"
}
refill_fav() { # $1 gen
    raw=$(liked_page) || return 1
    printf '%s' "$raw" | grep -q 'spotify:track:' || return 1
    printf '%s' "$raw" | parse "" | add_pool "$1"
    log "refill favourites: pool $(pool_n)"
}
refill_random() { # $1 gen; one random track: a favourite, or an artist asked for before
    cur=$(cut -f2 "$SRC")
    if [ $(( $(rnd) % 2 )) = 0 ]; then order="fav artist"; else order="artist fav"; fi
    for pick in $order; do
        if [ "$pick" = fav ]; then
            raw=$(liked_page) || continue
            one=$(printf '%s' "$raw" | parse "" | fresh | shuffle | head -1)
        else
            line=$(awk -F'\t' -v c="$cur" '$1 != c && $1 != ""' "$ARTISTS" 2>/dev/null | shuffle | head -1)
            [ -n "$line" ] || continue
            au=$(printf '%s' "$line" | cut -f1); an=$(printf '%s' "$line" | cut -f2)
            raw=$(search_page "artist:\"$an\"" "$(printf '0\n10\n20\n' | shuffle | head -1)") || continue
            one=$(printf '%s' "$raw" | parse "$au" | fresh | shuffle | head -1)
        fi
        [ -n "$one" ] || continue
        printf '%s\n' "$one" | add_pool "$1"
        log "random ($pick): $(printf '%s' "$one" | cut -f3)"
        return 0
    done
    return 1
}
refill() {
    active || return 0
    if ! mkdir "$RLOCK" 2>/dev/null; then       # one at a time; a killed one is no holder
        p=$(cat "$RLOCK/pid" 2>/dev/null)
        [ -n "$p" ] && kill -0 "$p" 2>/dev/null && return 0
        rm -rf "$RLOCK"; mkdir "$RLOCK" 2>/dev/null || return 0
    fi
    echo $$ >"$RLOCK/pid"
    last=$(cat "$FAILF" 2>/dev/null); case "$last" in ''|*[!0-9]*) last=0 ;; esac
    if [ $(( $(date +%s) - last )) -ge 30 ]; then
        g=$(gen); n=0
        while [ "$n" -lt 4 ] && [ "$(pool_n)" -lt "$LOW" ] && active && [ "$(gen)" = "$g" ]; do
            n=$((n + 1))
            kind=$(cut -f1 "$SRC")
            if [ "$kind" = artist ] && grep -q . "$PAGES" 2>/dev/null; then
                refill_artist "$g" || { date +%s >"$FAILF"; break; }
            elif [ "$kind" = artist ] && [ "$(sed -n 1p "$ALBUMS" 2>/dev/null)" != "$g done" ]; then
                refill_albums "$g"; rc=$?
                [ "$rc" = 1 ] && { date +%s >"$FAILF"; break; }
                [ "$rc" = 2 ] && { lock; [ "$(gen)" = "$g" ] && echo "$g done" >"$ALBUMS"; unlock; }
            elif [ "$kind" = fav ] && [ "$n" -le 2 ]; then
                refill_fav "$g" || { date +%s >"$FAILF"; break; }
            else
                if [ "$(cat "$DRY" 2>/dev/null)" != "$g" ]; then
                    echo "$g" >"$DRY"
                    if [ "$kind" = artist ]; then journal "$(cut -f3 "$SRC"): всё сыграно — дальше случайные треки"
                    else journal "избранное: всё сыграно — дальше случайные треки"; fi
                fi
                refill_random "$g" || { date +%s >"$FAILF"; break; }
            fi
        done
    fi
    rm -rf "$RLOCK"
}

# --- commands -------------------------------------------------------------------
start() { # kind artist-uri artist-name first [--page0]
    kind=$1; au=$2; an=$3; first=$4
    page0=""; [ "${5:-}" = --page0 ] && page0=$(cat)
    lock
    g=$(( $(gen) + 1 )); echo "$g" >"$GEN"
    printf '%s\t%s\t%s\n' "$kind" "$au" "$an" >"$SRC"
    : >"$POOL"
    if [ "$kind" = artist ]; then
        o=0; : >"$PAGES"
        while [ "$o" -le "$PAGE_MAX" ]; do
            [ -n "$page0" ] && [ "$o" = 0 ] || echo "$o" >>"$PAGES"
            o=$((o + 10))
        done
        if [ -n "$au" ]; then   # an artist asked for: a candidate for the random tracks
            touch "$ARTISTS"
            { awk -F'\t' -v u="$au" '$1 != u' "$ARTISTS"; printf '%s\t%s\n' "$au" "$an"; } | tail -n 40 >"$ARTISTS.new"
            mv "$ARTISTS.new" "$ARTISTS"
        fi
    else
        rm -f "$PAGES"
    fi
    unlock
    [ -n "$first" ] && forget "$first"
    if [ -n "$page0" ]; then
        if [ "$kind" = artist ]; then printf '%s' "$page0" | parse "$au"; else printf '%s' "$page0" | parse ""; fi |
            awk -F'\t' -v f="$first" '$1 != f' | add_pool "$g"
    fi
    chmod 600 "$SRC" "$GEN" "$POOL" "$ARTISTS" 2>/dev/null
    log "start $kind ${an:-} (gen $g): pool $(pool_n)"
}

stop() {
    active || return 0
    lock
    echo $(( $(gen) + 1 )) >"$GEN"
    rm -f "$SRC" "$POOL" "$PAGES"
    unlock
    log "stop: ${1:-}"
}

topup() {
    active || return 0
    touch "$QUEUED"
    # queued ones that will play: the phone's, and ours for this source
    valid=$(upcoming | awk -F'\t' -v g="$(gen)" -v qf="$QUEUED" '
        BEGIN { while ((getline l < qf) > 0) { split(l, f, "\t"); last[f[2]] = f[1] } }
        !($0 in last) || last[$0] == g { n++ } END { print n + 0 }')
    need=$((AHEAD - valid))
    [ "$need" -gt 0 ] || return 0
    if [ "$(pool_n)" -lt "$need" ]; then
        i=0       # a refill started with the track may still be running: wait a little
        while [ -d "$RLOCK" ] && [ "$i" -lt 16 ]; do usleep 500000 2>/dev/null || sleep 1; i=$((i + 1)); done
        [ "$(pool_n)" -ge "$need" ] || refill
    fi
    g=$(gen)
    while [ "$need" -gt 0 ]; do
        l=$(pop); [ -n "$l" ] || break
        u=$(printf '%s' "$l" | cut -f1)
        $R ctl add-to-queue "$u" $W >/dev/null 2>&1 || { log "add-to-queue failed: $u"; break; }
        lock; printf '%s\t%s\t%s\n' "$g" "$u" "$(printf '%s' "$l" | cut -f2)" >>"$QUEUED"
        tail -n 200 "$QUEUED" >"$QUEUED.new"; mv "$QUEUED.new" "$QUEUED"; unlock
        log "queued: $(printf '%s' "$l" | cut -f3)"
        need=$((need - 1))
    done
    [ "$(pool_n)" -lt "$LOW" ] && refill
}

skip() {
    if active; then
        head=$(upcoming | head -1)
        if [ -n "$head" ] && ! is_stale "$head"; then exec $R ctl next $W; fi
        l=$(pop)
        [ -n "$l" ] || { refill; l=$(pop); }
        if [ -n "$l" ]; then
            u=$(printf '%s' "$l" | cut -f1)
            forget "$u"
            log "play: $(printf '%s' "$l" | cut -f3)"
            exec $R ctl play "$u" $W
        fi
        log "skip: nothing to play"
    fi
    exec $R ctl next $W
}

played() { # uri title artist
    case "$1" in spotify:track:[A-Za-z0-9]*) ;; *) return 0 ;; esac
    k=$(key_of "$2" "$3")
    lock
    touch "$PLAYED" "$QUEUED" "$POOL"
    { awk -F'\t' -v now="$(date +%s)" -v day=$DAY 'now - $1 < day' "$PLAYED"
      printf '%s\t%s\t%s\n' "$(date +%s)" "$1" "$k"; } | tail -n 400 >"$PLAYED.new"
    mv "$PLAYED.new" "$PLAYED"
    # consumed; a stale copy of it further down the queue stays stale
    awk -F'\t' -v u="$1" -v g="$(gen)" '!($2 == u && $1 == g)' "$QUEUED" >"$QUEUED.new"; mv "$QUEUED.new" "$QUEUED"
    awk -F'\t' -v u="$1" -v k="$k" '$1 != u && $2 != k' "$POOL" >"$POOL.new"; mv "$POOL.new" "$POOL"
    chmod 600 "$PLAYED" "$QUEUED" 2>/dev/null
    unlock
}

case "${1:-}" in
    start) shift; start "${1:-artist}" "${2:-}" "${3:-}" "${4:-}" "${5:-}" ;;
    favorites)
        raw=$(liked_page) || exit 7
        lines=$(printf '%s' "$raw" | parse "")
        [ -n "$lines" ] || exit 7
        pick=$(printf '%s\n' "$lines" | fresh | shuffle | head -1)          # unplayed today if any
        [ -n "$pick" ] || pick=$(printf '%s\n' "$lines" | shuffle | head -1)
        first=$(printf '%s' "$pick" | cut -f1)
        printf '%s' "$raw" | start fav "" "" "$first" --page0
        $R ctl activate $W >/dev/null 2>&1 || true
        $R ctl play "$first" $W >/dev/null 2>&1 || exit 8
        log "favourites: $(printf '%s' "$pick" | cut -f3)"
        printf '%s\n' OK ;;
    stop) stop "${2:-}" ;;
    topup) topup ;;
    refill) refill ;;
    skip) skip ;;
    played) played "${2:-}" "${3:-}" "${4:-}" ;;
    stale) is_stale "${2:-x}" || exit 1; forget "$2" ;;
    forget) forget "${2:-x}" ;;
    active) active ;;
    main-artist) tracks_of "" | head -1 | cut -f3,4 ;;
    next-name)
        active || exit 0
        l=$(sed -n 1p "$POOL" 2>/dev/null); [ -n "$l" ] && printf '%s\n' "$l" | cut -f3 ;;
    *) echo "usage: $0 start|favorites|stop|topup|refill|skip|played|stale|forget|active|main-artist|next-name" >&2; exit 2 ;;
esac
