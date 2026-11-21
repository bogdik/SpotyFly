#!/bin/sh
# Voice music search for Soloist.
#  * «включи песню/трек X»  -> play the matching track; its artist goes on.
#  * «включи группу X» (proxy marks it "@artist X") -> play that ARTIST's tracks
#    (the artist is resolved first, see artist_tracks; never a title match).
#  * «включи группу X альбом Y», «альбом Y группы X», «альбом Y» (proxy marks it
#    "@album Y|X") -> play that ALBUM from its first track to its end; the voice
#    radio stops (nothing is added after it).
#  * --track URI ARTIST TITLE (a suggestion picked in the monitor) -> play exactly
#    that track at once (no text search, no token needed); its artist goes on.
# Only the first track is played here.  What follows is alice-radio.sh: it gets
# the artist as its source and queues 2 tracks at a time near each track's end.
# Searched with the owner's OAuth token (the app token is the fallback); in
# Spotify's dev mode either way artists/top-tracks & recommendations
# are 403/404 and search limit must stay <= 10.  We therefore build the artist
# sequence with `q=artist:<name>`.
set -eu
[ -r '/data/opt/spotify-connect/receiver-state/web-api/net.conf' ] && . '/data/opt/spotify-connect/receiver-state/web-api/net.conf' 2>/dev/null || true
PROXY=${PROXY:-}

STATE=/data/opt/spotify-connect/receiver-state/web-api
CREDS=$STATE/credentials.env
CA=/data/opt/solo/ca-certificates.crt
SOLO_DATA=/data/opt/spotify-connect/receiver-state/soloist/data
RADIO=/data/opt/spotify-connect/alice-radio.sh
MARKET=$(cat "$STATE/market" 2>/dev/null); case "$MARKET" in [A-Z][A-Z]) ;; *) MARKET=US ;; esac   # the account's country (spotify-oauth.sh market)
LIMIT=10
QUERY=${1:-}

# A specific track picked in the monitor: its exact URI, no text search.
TRACK_URI=""; TRACK_ARTIST=""; TRACK_TITLE=""
if [ "$QUERY" = --track ]; then
    TRACK_URI=${2:-}; TRACK_ARTIST=${3:-}; TRACK_TITLE=${4:-}
    [ -z "${SEARCH_DRY:-}" ] || { printf 'DRY track %s\n' "$TRACK_URI"; exit 0; }
    case "$TRACK_URI" in spotify:track:[A-Za-z0-9]*) ;; *) exit 2 ;; esac
    case "${TRACK_URI#spotify:track:}" in *[!A-Za-z0-9]*) exit 2 ;; esac
    QUERY="${TRACK_TITLE:-$TRACK_URI}"
fi
# An artist picked in the monitor's dropdown: --artist URI NAME -> that very
# artist's tracks (no namesake guessing), as «включи группу NAME».
PICK_AU=""
if [ "$QUERY" = --artist ]; then
    PICK_AU=${2:-}
    case "$PICK_AU" in spotify:artist:[A-Za-z0-9]*) ;; *) exit 2 ;; esac
    case "${PICK_AU#spotify:artist:}" in *[!A-Za-z0-9]*) exit 2 ;; esac
    QUERY="@artist ${3:-}"
fi

# SEARCH_NOT="uri uri …" (alice-wrong-pick.sh, «ты поставила не то»): tracks,
# artists, albums that were the wrong pick — never chosen again here.
NOT=" ${SEARCH_NOT:-} "
is_not() { case "$NOT" in *" $1 "*) return 0 ;; esac; return 1; }
not_filter() { if [ -n "${SEARCH_NOT:-}" ]; then awk -v n="$NOT" 'index(n, " " $0 " ") == 0'; else cat; fi; }
# SEARCH_DRY=1 (tests): print what would play ("DRY …") and stop; nothing
# plays, the radio and the search history stay as they are.
dry() { [ -z "${SEARCH_DRY:-}" ] || { printf 'DRY %s\n' "$*"; exit 0; }; }

# Artist intent from the proxy: «включи группу/исполнителя X» -> "@artist X".
ARTIST_MODE=0
case "$QUERY" in
    '@artist '*) ARTIST_MODE=1; QUERY=${QUERY#@artist } ;;
esac
# Album intent: "@album Y|X" (X, the artist, may be missing).
ALBUM_MODE=0; ALBUM_ARTIST=""
case "$QUERY" in
    '@album '*) ALBUM_MODE=1; QUERY=${QUERY#@album }
        case "$QUERY" in *'|'*) ALBUM_ARTIST=${QUERY#*|}; QUERY=${QUERY%%|*} ;; esac ;;
esac

[ -n "$QUERY" ] && [ "${#QUERY}" -le 400 ] && [ -r "$CREDS" ] || exit 2
. "$CREDS"
umask 077

# Exactly one short lookup in flight; never hang behind an unavailable SOCKS.
LOCK="$STATE/search.lock"
mkdir "$LOCK" 2>/dev/null || exit 0
trap 'rmdir "$LOCK" 2>/dev/null || true' EXIT HUP INT TERM

# The bundled curl can ignore its own timeout while SOCKS is half-open; a
# watchdog gives every HTTP attempt a hard limit.  Its stdout/stderr must go to
# /dev/null, or it holds the command-substitution pipe open for the whole wait.
curl_limited() {
    /opt/bin/curl --fail --location --silent --show-error --connect-timeout 5 --max-time 12 "$@" &
    cpid=$!
    ( sleep 13; kill -TERM "$cpid" 2>/dev/null || true; sleep 1; kill -KILL "$cpid" 2>/dev/null || true ) >/dev/null 2>&1 &
    wpid=$!
    wait "$cpid"
    status=$?
    kill "$wpid" 2>/dev/null || true
    wait "$wpid" 2>/dev/null || true
    return "$status"
}

spotify_request() {
    curl_limited -x "$PROXY" --cacert "$CA" "$@" || \
        curl_limited --cacert "$CA" "$@"
}

# All track URIs in a JSON blob, de-duplicated, order preserved, minus the
# чёрный список (blacklisted tracks and every track of a blacklisted artist).
BL=/data/opt/spotify-connect/alice-blacklist.sh
uris_of() {
    if [ -x "$BL" ]; then "$BL" filter
    else grep -o 'spotify:track:[A-Za-z0-9]*' | awk '!seen[$0]++'; fi
}
# Asked for something blacklisted: say why nothing starts (monitor's voice log).
blocked() { # what
    printf '%s\tbl\t%s\n' "$(date +%s)" "не включаю — в чёрном списке: $1" \
        >>/tmp/alice-voice-journal.log 2>/dev/null || true
    exit 0
}
shuffle() { awk 'BEGIN{srand()} {a[NR]=$0} END{for(i=NR;i>0;i--){j=int(rand()*i)+1; print a[j]; a[j]=a[i]}}'; }

# Token / primary search go over a flaky SOCKS gateway: a single miss used to
# leave the request silent (native music already pre-empted).  Retry a few times.
# The owner's OAuth token first: Spotify ranks its search like the app and
# forgives typos («coal chabmer» -> Coal Chamber); the app token (client
# credentials) misses even exact names and is only the fallback.  The OAuth
# token lives an hour and oauth.env is rewritten on each refresh, so one older
# than 50 min is refreshed before use.
TOKENS=$STATE/oauth.env
OAUTH=/data/opt/spotify-connect/spotify-oauth.sh
access=""; access_kind=""
user_token() {
    [ -r "$TOKENS" ] || return 1
    age=$(( $(date +%s) - $(date -r "$TOKENS" +%s 2>/dev/null || echo 0) ))
    if [ "$age" -gt 3000 ] || [ "$age" -lt 0 ]; then
        "$OAUTH" refresh >/dev/null 2>&1 || return 1
    fi
    OAUTH_ACCESS_TOKEN=""; . "$TOKENS" 2>/dev/null || return 1
    [ -n "${OAUTH_ACCESS_TOKEN:-}" ] || return 1
    access=$OAUTH_ACCESS_TOKEN; access_kind=user
}
fetch_token() {
    if [ "$access_kind" != app ] && user_token; then return 0; fi
    app_token
}
# The owner's token was refused (revoked, clock off): the app token from here
# on.  0 if switched, so the caller retries its lookup once.
token_fallback() {
    [ "$access_kind" = user ] || return 1
    app_token
}
app_token() {
    access_kind=app
    i=0
    while [ "$i" -lt 3 ]; do
        token=$(spotify_request -u "$CLIENT_ID:$CLIENT_SECRET" --data 'grant_type=client_credentials' \
            https://accounts.spotify.com/api/token 2>/dev/null)
        access=$(printf '%s' "$token" | sed -n 's/.*"access_token":"\([^"]*\)".*/\1/p')
        [ -n "$access" ] && return 0
        i=$((i + 1))
    done
    return 1
}
# A picked track needs no search at all.
[ -n "$TRACK_URI" ] || fetch_token || exit 3

search_raw() { # $1=query [$2=type] -> the search JSON, "" if lost; retries over flaky SOCKS
    i=0
    while [ "$i" -lt 3 ]; do
        sraw=$(spotify_request -H "Authorization: Bearer $access" --get \
            --data-urlencode "q=$1" --data "type=${2:-track}" --data "limit=$LIMIT" \
            --data "market=$MARKET" https://api.spotify.com/v1/search 2>/dev/null)
        if printf '%s' "$sraw" | grep -q "spotify:${2:-track}:"; then
            printf '%s' "$sraw"
            return 0
        fi
        i=$((i + 1))
        # three misses on the owner's token: once more on the app token
        [ "$i" -lt 3 ] || ! token_fallback || i=2
    done
    return 0
}
# $FOUNDF says the search itself found tracks (all of them may be blacklisted).
FOUNDF=$STATE/.search-found

# «включи группу X» by the artist, never by a title.  `artist:X` alone is a loose
# filter: for spoken spellings ("ska p") it returns other artists.  So take the
# artist on most of its tracks ("дельфин" -> Dolphin), else Spotify's artist
# search ("ska p" -> Ska-P, "5 diez" -> 5diez; an exact name first), and keep
# only that artist's tracks.
norm() { # stdin -> name for comparing: lower case, no spaces/punctuation, ё=е
    tr 'A-Z' 'a-z' | tr -d " .,!?:;'\"()&/_*+-" | sed 's/ё/е/g; s/Ё/е/g; s/А/а/g; s/Б/б/g;
        s/В/в/g; s/Г/г/g; s/Д/д/g; s/Е/е/g; s/Ж/ж/g; s/З/з/g; s/И/и/g; s/Й/й/g; s/К/к/g;
        s/Л/л/g; s/М/м/g; s/Н/н/g; s/О/о/g; s/П/п/g; s/Р/р/g; s/С/с/g; s/Т/т/g; s/У/у/g;
        s/Ф/ф/g; s/Х/х/g; s/Ц/ц/g; s/Ч/ч/g; s/Ш/ш/g; s/Щ/щ/g; s/Ы/ы/g; s/Э/э/g; s/Ю/ю/g;
        s/Я/я/g'
}
top_artist() { # stdin=track search JSON -> the artist URI on more than half its tracks
    # (a track's artists, its album's too, come before its own uri)
    grep -oE 'spotify:(artist|track):[A-Za-z0-9]+' | awk '
        /^spotify:artist:/ { if (!($0 in cur)) { cur[$0] = 1; ord[++k] = $0 } next }
        { n++; for (i = 1; i <= k; i++) c[ord[i]]++; for (a in cur) delete cur[a]; k = 0 }
        END { for (a in c) if (c[a] > best) { best = c[a]; top = a }
              if (n && best * 2 > n) print top }'
}
tracks_by() { # $1=artist URI, stdin=track JSON -> that artist's track URIs
    grep -oE 'spotify:(artist|track):[A-Za-z0-9]+' | awk -v a="$1" '
        $0 == a { has = 1; next }
        /^spotify:artist:/ { next }
        { if (has && !s[$0]++) print; has = 0 }'
}
find_artist() { # $1=name -> "URI<TAB>Name" from the artist search, "" if none fits
    # Only a name that matches what was said, or is part of it («группа X <song>»):
    # Spotify's top hit for an unknown name is some other artist.
    want=$(printf '%s' "$1" | norm)
    search_raw "$1" artist | sed 's/.*"items":\[//' \
        | grep -oE '"name":"[^"]*"|spotify:artist:[A-Za-z0-9]+' \
        | awk '/^"name"/ { sub(/^"name":"/, ""); sub(/"$/, ""); nm = $0; next }
               nm != "" { print $0 "\t" nm; nm = "" }' >"$STATE/.artists"
    best=""
    while IFS='	' read -r au an; do
        have=$(printf '%s' "$an" | norm)
        [ -n "$have" ] && ! is_not "$au" || continue
        [ "$have" = "$want" ] && { best="$au	$an"; break; }
        case "$want" in "$have"*) [ -n "$best" ] || best="$au	$an" ;; esac
    done <"$STATE/.artists"
    rm -f "$STATE/.artists"
    [ -z "$best" ] || printf '%s\n' "$best"
}
lc() { # stdin -> lower case, ё=е, single spaces; punctuation kept («с.т.а.я.» ≠ «стая»)
    tr 'A-Z' 'a-z' | sed 's/ё/е/g; s/Ё/е/g; s/А/а/g; s/Б/б/g; s/В/в/g; s/Г/г/g; s/Д/д/g;
        s/Е/е/g; s/Ж/ж/g; s/З/з/g; s/И/и/g; s/Й/й/g; s/К/к/g; s/Л/л/g; s/М/м/g; s/Н/н/g;
        s/О/о/g; s/П/п/g; s/Р/р/g; s/С/с/g; s/Т/т/g; s/У/у/g; s/Ф/ф/g; s/Х/х/g; s/Ц/ц/g;
        s/Ч/ч/g; s/Ш/ш/g; s/Щ/щ/g; s/Ы/ы/g; s/Э/э/g; s/Ю/ю/g; s/Я/я/g; s/  */ /g; s/^ //; s/ $//'
}
# An artist whose name is exactly what was said (case aside), or its first
# words («стая война» -> Стая): it wins over one that only looks the same once
# punctuation is gone (2026-11-19: «стая» played С.Т.А.Я., which has more
# tracks).  $1=spoken -> "URI<TAB>Name", "" if none.
exact_artist() {
    want=$(printf '%s' "$1" | lc)
    for q in "$1" "${1%% *}"; do
        search_raw "$q" artist | sed 's/.*"items":\[//' \
            | grep -oE '"name":"[^"]*"|spotify:artist:[A-Za-z0-9]+' \
            | awk '/^"name"/ { sub(/^"name":"/, ""); sub(/"$/, ""); nm = $0; next }
                   nm != "" { print $0 "\t" nm; nm = "" }' >"$STATE/.artists"
        best=""; bl=0
        while IFS='	' read -r au_ an_; do
            have=$(printf '%s' "$an_" | lc)
            [ -n "$have" ] && ! is_not "$au_" || continue
            case "$want" in "$have"|"$have "*) [ "${#have}" -gt "$bl" ] && { best="$au_	$an_"; bl=${#have}; } ;; esac
        done <"$STATE/.artists"
        rm -f "$STATE/.artists"
        [ -n "$best" ] && { printf '%s\n' "$best"; return 0; }
        [ "$q" = "${1%% *}" ] && break           # one word: nothing more to try
    done
    return 0
}
# An artist asked for or played before whose name is exactly what was said (or
# its first words): the user's own «стая» among Spotify's namesakes (Стая,
# СтаЯ, С.Т.А.Я., Стая FAM …).  -> "URI<TAB>Name", the latest first.
known_artist() {
    want=$(printf '%s' "$1" | lc)
    { [ "$(cut -f1 "$STATE/radio-source" 2>/dev/null)" = artist ] && cut -f2,3 "$STATE/radio-source"
      awk '{ l[NR] = $0 } END { for (i = NR; i > 0; i--) print l[i] }' "$STATE/radio-artists.txt" 2>/dev/null
    } | awk -F'\t' '$1 ~ /^spotify:artist:/' >"$STATE/.known"
    hit=""
    for exact in 1 0; do
        while IFS='	' read -r ku kn; do
            have=$(printf '%s' "$kn" | lc)
            [ -n "$have" ] && ! is_not "$ku" || continue
            if [ "$exact" = 1 ]; then [ "$want" = "$have" ] && { hit="$ku	$kn"; break; }
            else case "$want" in "$have "*) hit="$ku	$kn"; break ;; esac; fi
        done <"$STATE/.known"
        [ -n "$hit" ] && break
    done
    rm -f "$STATE/.known"
    [ -z "$hit" ] || printf '%s\n' "$hit"
}
album_tracks_raw() { # $1=artist URI -> the tracks JSON of one of its albums, at random
    al=$(artist_albums "$1" | albums_of | cut -f1 | shuffle | head -1)
    [ -n "$al" ] || return 0
    spotify_request -H "Authorization: Bearer $access" \
        "https://api.spotify.com/v1/albums/${al#spotify:album:}/tracks?limit=50&market=$MARKET" 2>/dev/null || true
}
name_in() { # $1=artist URI, stdin=track JSON -> that artist's name
    grep -o "\"id\":\"${1#spotify:artist:}\",\"name\":\"[^\"]*\"" | head -1 | sed 's/.*"name":"//; s/"$//'
}
artist_tracks() { # $1=spoken name -> shuffled allowed track URIs; sets ANAME
    rm -f "$FOUNDF"
    ANAME=""; au=""
    # the one asked for before, if its name is what was said; its tracks from
    # the search, else (all namesakes there) from one of its albums
    hist=$(known_artist "$1")
    [ -z "$PICK_AU" ] || hist=$(printf '%s\t%s' "$PICK_AU" "$1")
    if [ -n "$hist" ]; then
        au=$(printf '%s' "$hist" | cut -f1); ANAME=$(printf '%s' "$hist" | cut -f2)
        araw=$(search_raw "artist:\"$ANAME\"")
        [ -n "$(printf '%s' "$araw" | tracks_by "$au")" ] || araw=$(album_tracks_raw "$au")
        [ -n "$(printf '%s' "$araw" | tracks_by "$au")" ] || { au=""; ANAME=""; }
    fi
    if [ -n "$au" ]; then
        :
    elif araw=$(search_raw "artist:$1"); au=$(printf '%s' "$araw" | top_artist | not_filter); [ -n "$au" ]; then
        ANAME=$(printf '%s' "$araw" | name_in "$au")
        # It only LOOKS like what was said (С.Т.А.Я. for «стая»)?  An artist
        # named exactly so wins («дельфин» -> Dolphin is not such a case).
        w=$(printf '%s' "$1" | norm); h=$(printf '%s' "$ANAME" | norm)
        case "$w" in
            "$h"|"$h"*)
                pick=$(exact_artist "$1")
                if [ -n "$pick" ] && [ "$(printf '%s' "$pick" | cut -f1)" != "$au" ]; then
                    xr=$(search_raw "artist:\"$(printf '%s' "$pick" | cut -f2)\"")
                    if [ -n "$(printf '%s' "$xr" | tracks_by "$(printf '%s' "$pick" | cut -f1)")" ]; then
                        au=$(printf '%s' "$pick" | cut -f1); ANAME=$(printf '%s' "$pick" | cut -f2); araw=$xr
                    fi
                fi ;;
        esac
    else
        pick=$(find_artist "$1")
        au=$(printf '%s' "$pick" | cut -f1)
        ANAME=$(printf '%s' "$pick" | cut -f2)
        [ -n "$au" ] || return 0
        araw=$(search_raw "artist:\"$ANAME\"")
        # Spotify may file the tracks under another artist ("5diez" -> "#####")
        if [ -z "$(printf '%s' "$araw" | tracks_by "$au")" ]; then
            au=$(printf '%s' "$araw" | top_artist | not_filter)
            [ -n "$au" ] || return 0
            ANAME=$(printf '%s' "$araw" | name_in "$au")
        fi
    fi
    [ -n "$ANAME" ] || ANAME=$1
    mine=$(printf '%s' "$araw" | tracks_by "$au")
    [ -n "$mine" ] || return 0
    : >"$FOUNDF"
    ok=$(printf '%s' "$araw" | uris_of)
    # «группа X <song>»: more words than the artist's name -> that song of THIS
    # artist first (a title of someone else never plays).
    pref=""
    if [ "$(printf '%s' "$1" | norm)" != "$(printf '%s' "$ANAME" | norm)" ]; then
        traw=$(search_raw "$1")
        pref=$(printf '%s' "$traw" | tracks_by "$au" | head -1)
        [ -n "$pref" ] && ok=$(printf '%s\n%s' "$ok" "$(printf '%s' "$traw" | uris_of)")
    fi
    { [ -z "$pref" ] || printf '%s\n' "$pref"
      printf '%s\n' "$mine" | shuffle; } | awk -v ok="$ok" '
        BEGIN { n = split(ok, a, "\n"); for (i = 1; i <= n; i++) k[a[i]] = 1 }
        k[$0] && !s[$0]++' | not_filter
}

# --- albums -------------------------------------------------------------------
albums_of() { # stdin=album JSON -> "uri<TAB>name<TAB> artist uris" per album
    # an album's name comes right before its uri, its artists right after it
    grep -oE '"name":"([^"\\]|\\.)*"|spotify:(album|artist):[A-Za-z0-9]+' | awk '
        function out() { if (cur != "" && !seen[cur]++) print cur "\t" cn "\t" arts }
        /^"name":/ { nm = substr($0, 9, length($0) - 9); next }
        /^spotify:artist:/ { arts = arts " " $0; nm = ""; next }
        { out(); cur = $0; cn = nm; arts = ""; nm = "" }
        END { out() }'
}
artist_albums() { # $1=artist URI -> its albums (album JSON pages; app token: limit 10)
    off=0
    while [ "$off" -lt 50 ]; do
        page=$(spotify_request -H "Authorization: Bearer $access" \
            "https://api.spotify.com/v1/artists/${1#spotify:artist:}/albums?include_groups=album,single,compilation&limit=10&offset=$off&market=$MARKET" 2>/dev/null) || break
        printf '%s' "$page" | grep -q 'spotify:album:' || break
        printf '%s\n' "$page"
        off=$((off + 10))
    done
}
albums_not() { # albums_of lines minus the excluded albums / albums of excluded artists
    if [ -z "${SEARCH_NOT:-}" ]; then cat; return; fi
    awk -F'\t' -v n="$NOT" 'index(n, " " $1 " ") { next }
        { k = split($3, a, " "); for (i = 1; i <= k; i++) if (index(n, " " a[i] " ")) next; print }'
}
best_album() { # $1=spoken album name, $2=artist URI or "", stdin=albums_of -> "uri<TAB>name"
    # name: exact > starts with what was said (a "(Remastered)" tail) > contains it
    # > is part of it; of the artist asked for, if any; ties -> the first listed
    want=$(printf '%s' "$1" | norm)
    cat >"$STATE/.albums"
    cut -f2 "$STATE/.albums" | norm | paste "$STATE/.albums" - | awk -F'\t' -v w="$want" -v a="$2" '
        { s = 0; h = $4
          if (h == w) s = 4
          else if (index(h, w) == 1) s = 3
          else if (index(h, w) > 0) s = 2
          else if (length(h) >= 4 && index(w, h) > 0) s = 1
          if (s && a != "" && index($3, a) == 0) s = 0
          if (s > best) { best = s; top = $1 "\t" $2 } }
        END { if (top != "") print top }'
    rm -f "$STATE/.albums"
}
pick_album() { # $1=album name, $2=spoken artist or "" -> "uri<TAB>name"; sets ANAME, au
    au=""; ANAME=""
    if [ -n "$2" ]; then
        artist_tracks "$2" >/dev/null           # resolves au / ANAME («кино» -> Kino)
        if [ -n "$au" ]; then
            hit=$(artist_albums "$au" | albums_of | albums_not | best_album "$1" "$au")
            [ -n "$hit" ] && { printf '%s\n' "$hit"; return 0; }
        fi
    fi
    hit=$(search_raw "$1${ANAME:+ $ANAME}" album | albums_of | albums_not | best_album "$1" "$au")
    [ -n "$hit" ] && { printf '%s\n' "$hit"; return 0; }
    # not that artist's: the album so named by whoever has it, the words said
    # about its artist helping the search (Spotify may know a namesake better)
    [ -n "$au" ] || return 0
    hit=$(search_raw "$1${2:+ $2}" album | albums_of | albums_not | best_album "$1" "")
    [ -n "$hit" ] && { ANAME=""; au=""; printf '%s\n' "$hit"; }
    return 0
}

artist_of_result() { # stdin=track JSON -> first artist name
    sed 's/"artists":\[/\n#A#/; s/"name":"/\n#N#/g' \
        | sed -n '/#A#/,$p' | grep '#N#' | head -1 \
        | sed 's/.*#N#\([^"]*\)".*/\1/'
}

# Stock the radio's pool in the background: the search is done, music plays.
refill_bg() { "$RADIO" refill </dev/null >/dev/null 2>&1 & }

if [ -z "$TRACK_URI" ] && [ "$ARTIST_MODE" = 1 ]; then
    artist_tracks "$QUERY" >"$STATE/.artist-seq"   # not $(…): it sets ANAME
    SEQ=$(cat "$STATE/.artist-seq"); rm -f "$STATE/.artist-seq"
    # No such artist on Spotify (misheard, or not there): a plain search beats silence.
    [ -n "$SEQ" ] || [ -e "$FOUNDF" ] || ARTIST_MODE=0
fi

if [ "$ALBUM_MODE" = 1 ] && [ -z "$TRACK_URI" ]; then
    pick_album "$QUERY" "$ALBUM_ARTIST" >"$STATE/.album-hit" || true   # not $(…): it sets ANAME
    hit=$(cat "$STATE/.album-hit"); rm -f "$STATE/.album-hit"
    if [ -z "$hit" ]; then
        # no such album: the artist's music (or the words) is still better than silence
        if [ -n "$ALBUM_ARTIST" ]; then
            dry "no album -> @artist $ALBUM_ARTIST"
            rmdir "$LOCK" 2>/dev/null || true; exec "$0" "@artist $ALBUM_ARTIST"
        fi
        dry "no album -> plain search $QUERY"
        ALBUM_MODE=0
    fi
fi

if [ "$ALBUM_MODE" = 1 ] && [ -z "$TRACK_URI" ]; then
    album=$(printf '%s' "$hit" | cut -f1); aname=$(printf '%s' "$hit" | cut -f2)
    # SEARCH_DRY=1 (tests): say what would play, play nothing
    dry "album $album	$aname	${ANAME:-}"
    [ -x "$BL" ] && [ -n "$ANAME" ] && "$BL" has-artist-name "$ANAME" && blocked "$ANAME ($aname)"
    "$RADIO" stop "альбом $aname"           # the album plays to its end, nothing after it
    /data/opt/solo/run.sh ctl activate -D "$SOLO_DATA" >/dev/null 2>&1 || true
    /data/opt/solo/run.sh ctl shuffle off -D "$SOLO_DATA" >/dev/null 2>&1 || true   # in its order
    /data/opt/solo/run.sh ctl play "$album" -D "$SOLO_DATA" >/dev/null
    QUERY="$aname${ANAME:+ — $ANAME}"
elif [ -n "$TRACK_URI" ]; then
    # Exactly the picked track, right away; then its artist.
    first=$TRACK_URI
    if [ -x "$BL" ]; then
        "$BL" has-track "$first" && blocked "${TRACK_TITLE:-$first}"
        "$BL" has-artist-name "$TRACK_ARTIST" && blocked "$TRACK_ARTIST"
    fi
    /data/opt/solo/run.sh ctl activate -D "$SOLO_DATA" >/dev/null 2>&1 || true
    /data/opt/solo/run.sh ctl play "$first" -D "$SOLO_DATA" >/dev/null
    if [ -n "$TRACK_ARTIST" ]; then
        # the artist's exact URI from the track: by its name alone the radio
        # took a namesake (2026-11-19: Стая -> the rap «Стая FAM»)
        main=""
        if fetch_token; then
            tj=$(spotify_request -H "Authorization: Bearer $access" \
                "https://api.spotify.com/v1/tracks/${first#spotify:track:}?market=$MARKET" 2>/dev/null) || tj=""
            if [ -z "$tj" ] && token_fallback; then
                tj=$(spotify_request -H "Authorization: Bearer $access" \
                    "https://api.spotify.com/v1/tracks/${first#spotify:track:}?market=$MARKET" 2>/dev/null) || tj=""
            fi
            main=$(printf '%s' "$tj" | "$RADIO" main-artist)
        fi
        "$RADIO" start artist "$(printf '%s' "$main" | cut -f1)" "$TRACK_ARTIST" "$first"; refill_bg
    else
        "$RADIO" stop "трек без исполнителя"
    fi
elif [ "$ARTIST_MODE" = 1 ]; then
    # «включи группу X»: play that artist's tracks (SEQ from artist_tracks above).
    first=$(printf '%s\n' "$SEQ" | sed -n '1p')
    [ -z "$first" ] && [ -e "$FOUNDF" ] && blocked "${ANAME:-$QUERY}"
    case "$first" in spotify:track:[A-Za-z0-9]*) ;; *) exit 4;; esac
    dry "artist $au	${ANAME:-$QUERY}	first $first"
    QUERY=${ANAME:-$QUERY}
    /data/opt/solo/run.sh ctl activate -D "$SOLO_DATA" >/dev/null 2>&1 || true
    /data/opt/solo/run.sh ctl play "$first" -D "$SOLO_DATA" >/dev/null
    # the page artist_tracks fetched is the radio's first stock
    printf '%s' "$araw" | "$RADIO" start artist "$au" "$QUERY" "$first" --page0
    refill_bg
else
    # «включи песню X»: match the track and PLAY IT IMMEDIATELY; its artist goes on.
    # Retry the primary lookup so a single SOCKS miss doesn't drop the request.
    result=""; first=""; i=0
    while [ "$i" -lt 3 ]; do
        result=$(spotify_request -H "Authorization: Bearer $access" --get \
            --data-urlencode "q=$QUERY" --data 'type=track' --data "limit=$([ -n "${SEARCH_NOT:-}" ] && echo 10 || echo 1)" \
            --data "market=$MARKET" https://api.spotify.com/v1/search 2>/dev/null)
        printf '%s' "$result" | grep -q 'spotify:track:' && break
        i=$((i + 1))
        [ "$i" -lt 3 ] || ! token_fallback || i=2
    done
    first=$(printf '%s' "$result" | uris_of | head -1)
    if [ -n "${SEARCH_NOT:-}" ]; then      # «не то»: the first not by an excluded artist
        okl=$(printf '%s' "$result" | uris_of)
        first=$(printf '%s' "$result" | grep -oE 'spotify:(artist|track):[A-Za-z0-9]+' | awk -v n="$NOT" -v ok="$okl" '
            BEGIN { m = split(ok, o, "\n"); for (i = 1; i <= m; i++) good[o[i]] = 1 }
            /^spotify:artist:/ { if (index(n, " " $0 " ")) bad = 1; next }
            { if (!bad && good[$0] && !index(n, " " $0 " ")) { print; exit } bad = 0 }')
    fi
    if [ -z "$first" ] && [ -z "${SEARCH_NOT:-}" ] && printf '%s' "$result" | grep -q 'spotify:track:'; then
        blocked "$(printf '%s' "$result" | artist_of_result) ($QUERY)"
    fi
    case "$first" in spotify:track:[A-Za-z0-9]*) ;; *) exit 4;; esac
    dry "track $first"
    /data/opt/solo/run.sh ctl activate -D "$SOLO_DATA" >/dev/null 2>&1 || true
    /data/opt/solo/run.sh ctl play "$first" -D "$SOLO_DATA" >/dev/null
    if [ -n "${SEARCH_NOT:-}" ]; then       # the artist of THAT track, not the first result's
        main=$(spotify_request -H "Authorization: Bearer $access" \
            "https://api.spotify.com/v1/tracks/${first#spotify:track:}?market=$MARKET" 2>/dev/null | "$RADIO" main-artist)
    else
        main=$(printf '%s' "$result" | "$RADIO" main-artist)
    fi
    "$RADIO" start artist "$(printf '%s' "$main" | cut -f1)" "$(printf '%s' "$main" | cut -f2)" "$first"
    refill_bg
fi

# Search history for the monitor page (only reached on a successful search).
SEARCHH=$STATE/radio-search-history.txt
if [ -n "$TRACK_URI" ]; then disp="${TRACK_TITLE:-$TRACK_URI}${TRACK_ARTIST:+ — $TRACK_ARTIST}"
elif [ "$ALBUM_MODE" = 1 ]; then disp="альбом $QUERY"
elif [ "$ARTIST_MODE" = 1 ]; then disp="группа $QUERY"; else disp="$QUERY"; fi
printf '%s\t%s\n' "$(date +%H:%M)" "$disp" >>"$SEARCHH" 2>/dev/null || true
tail -n 40 "$SEARCHH" >"$SEARCHH.new" 2>/dev/null && mv "$SEARCHH.new" "$SEARCHH" 2>/dev/null || true
chmod 600 "$SEARCHH" 2>/dev/null || true

# NOTE: no platformd kill here (see git history / memory: repeated kills
# factory-reset the speaker).  Dialog completion is handled by the muted
# filesrc pipeline in quasar.cfg.
printf '%s\n' OK
