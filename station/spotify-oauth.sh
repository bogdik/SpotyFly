#!/bin/sh
# One-time Authorization Code flow for the owner of this private receiver.
# Tokens live only on the speaker; this program never prints them.
set -eu

STATE=/data/opt/spotify-connect/receiver-state/web-api
CREDS=$STATE/credentials.env
TOKENS=$STATE/oauth.env
MARKETF=$STATE/market   # the account's country: the catalog every search/lookup uses
PENDING=$STATE/oauth-pending.env
CA=/data/opt/solo/ca-certificates.crt
# Proxy + redirect come from net.conf so they match the monitor's Settings.
[ -r "$STATE/net.conf" ] && . "$STATE/net.conf" 2>/dev/null || true
PROXY=${PROXY:-}
REDIRECT_URI=${REDIRECT_URI:-http://127.0.0.1:8088/callback}
SCOPES='playlist-modify-private user-library-modify user-library-read user-modify-playback-state user-read-playback-state user-read-private'

[ -r "$CREDS" ] || exit 2
. "$CREDS"
umask 077
mkdir -p "$STATE"

urlencode() {
    # The only values sent through this are fixed strings and the client id.
    printf '%s' "$1" | sed 's/%/%25/g; s/ /%20/g; s/:/%3A/g; s/\//%2F/g'
}

begin() {
    state=$(od -An -N 24 -tx1 /dev/urandom | tr -d ' \n')
    [ "${#state}" -eq 48 ] || exit 3
    printf 'OAUTH_STATE=%s\n' "$state" >"$PENDING"
    chmod 600 "$PENDING"
    printf '%s\n' "https://accounts.spotify.com/authorize?response_type=code&client_id=$(urlencode "$CLIENT_ID")&redirect_uri=$(urlencode "$REDIRECT_URI")&scope=$(urlencode "$SCOPES")&state=$state&show_dialog=true"
}

finish() {
    code=${1:-}
    [ -r "$PENDING" ] || exit 4
    . "$PENDING"
    # Spotify authorization codes are opaque base64url strings. Reject shell
    # syntax and URLs: the callback is parsed by the caller, not executed here.
    case "$code" in ''|*[!A-Za-z0-9_-]*) exit 5 ;; esac
    oauth_token() { # exchange over the configured proxy, fall back to direct
        /opt/bin/curl --fail --silent --show-error --max-time 20 \
            -x "$PROXY" --cacert "$CA" \
            -u "$CLIENT_ID:$CLIENT_SECRET" \
            --data-urlencode 'grant_type=authorization_code' \
            --data-urlencode "code=$code" \
            --data-urlencode "redirect_uri=$REDIRECT_URI" \
            https://accounts.spotify.com/api/token 2>/dev/null \
        || /opt/bin/curl --fail --silent --show-error --max-time 20 \
            --cacert "$CA" \
            -u "$CLIENT_ID:$CLIENT_SECRET" \
            --data-urlencode 'grant_type=authorization_code' \
            --data-urlencode "code=$code" \
            --data-urlencode "redirect_uri=$REDIRECT_URI" \
            https://accounts.spotify.com/api/token 2>/dev/null
    }
    token=$(oauth_token)
    access=$(printf '%s' "$token" | sed -n 's/.*"access_token":"\([^"]*\)".*/\1/p')
    refresh=$(printf '%s' "$token" | sed -n 's/.*"refresh_token":"\([^"]*\)".*/\1/p')
    [ -n "$access" ] && [ -n "$refresh" ] || exit 6
    {
        printf 'OAUTH_ACCESS_TOKEN=%s\n' "$access"
        printf 'OAUTH_REFRESH_TOKEN=%s\n' "$refresh"
    } >"$TOKENS"
    chmod 600 "$TOKENS"
    rm -f "$PENDING"
    ( market ) >/dev/null 2>&1 || true
    printf '%s\n' OK
}

api() { # $@ passed to curl; proxy first, then direct
    /opt/bin/curl --fail --silent --show-error --max-time 15 -x "$PROXY" --cacert "$CA" "$@" 2>/dev/null \
        || /opt/bin/curl --fail --silent --show-error --max-time 15 --cacert "$CA" "$@" 2>/dev/null
}

refresh() { # mint a fresh access token from the stored refresh token
    [ -r "$TOKENS" ] || return 1
    . "$TOKENS"
    [ -n "${OAUTH_REFRESH_TOKEN:-}" ] || return 1
    out=$(api -u "$CLIENT_ID:$CLIENT_SECRET" \
        --data-urlencode 'grant_type=refresh_token' \
        --data-urlencode "refresh_token=$OAUTH_REFRESH_TOKEN" \
        https://accounts.spotify.com/api/token)
    acc=$(printf '%s' "$out" | sed -n 's/.*"access_token":"\([^"]*\)".*/\1/p')
    [ -n "$acc" ] || return 1
    # keep the refresh token (Spotify may or may not rotate it)
    newref=$(printf '%s' "$out" | sed -n 's/.*"refresh_token":"\([^"]*\)".*/\1/p')
    [ -n "$newref" ] || newref=$OAUTH_REFRESH_TOKEN
    { printf 'OAUTH_ACCESS_TOKEN=%s\n' "$acc"; printf 'OAUTH_REFRESH_TOKEN=%s\n' "$newref"; } >"$TOKENS"
    chmod 600 "$TOKENS"
    OAUTH_ACCESS_TOKEN=$acc
    return 0
}

SOLO_R=/data/opt/solo/run.sh
SOLO_D=/data/opt/spotify-connect/receiver-state/soloist/data
LIKEPL=$STATE/like-playlist.id   # id of the "SpotyFly Likes" private playlist

# Like a write to Spotify: no --fail (a 4xx is a real answer, not a curl error),
# proxy first then direct only if the proxy connection itself fails.
apiw() {
    /opt/bin/curl --silent --max-time 15 -x "$PROXY" --cacert "$CA" "$@" 2>/dev/null \
        || /opt/bin/curl --silent --max-time 15 --cacert "$CA" "$@" 2>/dev/null
}

# Echo the id of the private "SpotyFly Likes" playlist, creating it once if needed.
ensure_playlist() {
    pid=$(cat "$LIKEPL" 2>/dev/null)
    if [ -n "$pid" ]; then
        got=$(api -H "Authorization: Bearer $OAUTH_ACCESS_TOKEN" \
            "https://api.spotify.com/v1/playlists/$pid?fields=id" \
            | sed -n 's/.*"id":"\([^"]*\)".*/\1/p' | head -1)
        [ -n "$got" ] && { printf '%s' "$got"; return 0; }
    fi
    # Non-deprecated create endpoint (the /users/{id}/playlists form is deprecated
    # and 403s for this app).  The playlist id is the first "id" in the response.
    pid=$(apiw -X POST -H "Authorization: Bearer $OAUTH_ACCESS_TOKEN" -H 'Content-Type: application/json' \
        --data '{"name":"SpotyFly Likes","public":false,"description":"Лайки с колонки"}' \
        "https://api.spotify.com/v1/me/playlists" \
        | grep -o '"id":"[^"]*"' | head -1 | cut -d'"' -f4)
    [ -n "$pid" ] || return 1
    printf '%s\n' "$pid" >"$LIKEPL"; chmod 600 "$LIKEPL" 2>/dev/null || true
    printf '%s' "$pid"
}

# 0 if track $2 is already in playlist $1 (checks the first 100 items).
playlist_has() {
    api -H "Authorization: Bearer $OAUTH_ACCESS_TOKEN" \
        "https://api.spotify.com/v1/playlists/$1/items?fields=items(item(id))&limit=100" \
        | grep -q "\"id\":\"$2\""
}

cur_id() { # id of the now-playing track, "" if none
    "$SOLO_R" ctl now --json -D "$SOLO_D" 2>/dev/null \
        | sed -n 's/.*"item":{"uri":"spotify:track:\([A-Za-z0-9]*\)".*/\1/p' | head -1
}

ensure_access() { # make sure OAUTH_ACCESS_TOKEN is set (refresh if needed)
    [ -r "$TOKENS" ] || return 1
    . "$TOKENS"
    [ -n "${OAUTH_ACCESS_TOKEN:-}" ] || refresh || return 1
    return 0
}

liked() { # $1 = id/uri/"current" ; prints 1 if track is in the SpotyFly Likes playlist
    id=${1#spotify:track:}
    [ -z "$id" ] || [ "$1" = current ] && id=$(cur_id)
    case "$id" in ''|*[!A-Za-z0-9]*) printf '0\n'; return ;; esac
    ensure_access || { printf '0\n'; return; }
    pid=$(cat "$LIKEPL" 2>/dev/null)
    [ -n "$pid" ] || { printf '0\n'; return; }   # no playlist yet -> nothing liked
    if playlist_has "$pid" "$id"; then printf '1\n'; else printf '0\n'; fi
}

# A random page (50) of the owner's Liked Songs, as the Web API JSON: the voice
# radio (alice-radio.sh) takes favourites from it, at random over the library.
LIKED_TOTAL=$STATE/liked-total
liked_page() {
    ensure_access || exit 6
    total=$(cat "$LIKED_TOTAL" 2>/dev/null || true)
    case "$total" in ''|*[!0-9]*) total=0 ;; esac
    off=0
    [ "$total" -gt 50 ] && off=$(od -An -N4 -tu4 /dev/urandom | awk -v t="$total" '{ print $1 % (t - 49) }')
    get_page() { api -H "Authorization: Bearer $OAUTH_ACCESS_TOKEN" \
        "https://api.spotify.com/v1/me/tracks?limit=50&offset=$off&market=$(market_of)"; }
    out=$(get_page || true)
    case "$out" in *spotify:track:*) ;; *) refresh && out=$(get_page || true) ;; esac
    case "$out" in *spotify:track:*) ;; *) exit 7 ;; esac
    t=$(printf '%s' "$out" | grep -o '"total":[0-9]*' | tail -1 | cut -d: -f2)
    [ -n "$t" ] && printf '%s\n' "$t" >"$LIKED_TOTAL"
    printf '%s' "$out"
}

like() { # $1 = track id/uri (empty/"current" -> now-playing); add to SpotyFly Likes
    # Spotify forbids PUT /me/tracks for this app (403), so likes go to a private
    # playlist instead (playlist-modify-private).
    id=${1#spotify:track:}
    [ -z "$id" ] || [ "$1" = current ] && id=$(cur_id)
    case "$id" in ''|*[!A-Za-z0-9]*) exit 5 ;; esac
    ensure_access || exit 4
    pid=$(ensure_playlist) || { refresh && pid=$(ensure_playlist); } || exit 6
    [ -n "$pid" ] || exit 6
    playlist_has "$pid" "$id" && { printf '%s\n' OK; return; }   # already liked
    add_item() { apiw -X POST -H "Authorization: Bearer $OAUTH_ACCESS_TOKEN" -H 'Content-Type: application/json' \
        --data "{\"uris\":[\"spotify:track:$id\"]}" \
        "https://api.spotify.com/v1/playlists/$pid/items"; }
    resp=$(add_item)
    case "$resp" in *snapshot_id*) printf '%s\n' OK; return ;; esac
    refresh || exit 6
    resp=$(add_item)
    case "$resp" in *snapshot_id*) printf '%s\n' OK ;; *) exit 7 ;; esac
}

# The account's country -> $MARKETF. Searches with another market miss what the
# account can play (market=RU: no Coal Chamber at all, the account is SG).
market() {
    ensure_access || exit 1
    me() { api -H "Authorization: Bearer $OAUTH_ACCESS_TOKEN" https://api.spotify.com/v1/me |
        sed -n 's/.*"country":"\([A-Z][A-Z]\)".*/\1/p'; }
    c=$(me || true)
    [ -n "$c" ] || { refresh && c=$(me || true); }
    [ -n "$c" ] || exit 7
    printf '%s\n' "$c" >"$MARKETF"
    printf '%s\n' "$c"
}
market_of() { m=$(cat "$MARKETF" 2>/dev/null || true); case "$m" in [A-Z][A-Z]) printf '%s' "$m" ;; *) printf US ;; esac; }

case "${1:-}" in
    begin) begin ;;
    market) market ;;
    finish) finish "${2:-}" ;;
    refresh) refresh && printf '%s\n' OK || exit 1 ;;
    like) like "${2:-}" ;;
    liked) liked "${2:-current}" ;;
    favorites) exec /data/opt/spotify-connect/alice-radio.sh favorites ;;   # plays one, the rest follows
    liked-page) liked_page ;;
    status) [ -r "$TOKENS" ] && printf 'set\n' || printf 'unset\n' ;;
    *) exit 64 ;;
esac
