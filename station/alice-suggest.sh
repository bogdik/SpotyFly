#!/bin/sh
# alice-suggest.sh QUERY [N]
# The monitor's live search: up to N (1-30, default 10; ⚙ in the monitor) rows
# as a JSON array, like the Spotify app's own dropdown —
#   tracks  {"k":"t","n":title,"a":"artist, artist","q":"title artist",
#            "u":"spotify:track:ID","i":cover 64px,"e":1 if explicit}
#   artist  {"k":"a","n":name,"a":"","q":name,"u":"spotify:artist:ID","i":photo}
# (the top artist goes third, after two tracks, as in the app).
# Searched with the owner's OAuth token: Spotify ranks it like the app and
# forgives typos («coal chabmer» -> Coal Chamber). The app token (client
# credentials) is only the fallback — its search misses even exact names.
set -u
STATE=/data/opt/spotify-connect/receiver-state/web-api
[ -r "$STATE/net.conf" ] && . "$STATE/net.conf" 2>/dev/null || true
PROXY=${PROXY:-}
. "$STATE/credentials.env"
TOKENS=$STATE/oauth.env
MARKET=$(cat "$STATE/market" 2>/dev/null); case "$MARKET" in [A-Z][A-Z]) ;; *) MARKET=US ;; esac   # the account's country (spotify-oauth.sh market)
OAUTH=/data/opt/spotify-connect/spotify-oauth.sh
CA=/data/opt/solo/ca-certificates.crt
Q=${1:-}
N=${2:-10}
case "$N" in ''|*[!0-9]*) N=10 ;; esac
[ "$N" -ge 1 ] && [ "$N" -le 30 ] || N=10
[ -n "$Q" ] || { printf '[]'; exit 0; }

req() { /opt/bin/curl -fsS --connect-timeout 4 --max-time 8 -x "$PROXY" --cacert "$CA" "$@"; }
app_token() {
    req -u "$CLIENT_ID:$CLIENT_SECRET" --data grant_type=client_credentials \
        https://accounts.spotify.com/api/token 2>/dev/null | sed -n 's/.*"access_token":"\([^"]*\)".*/\1/p'
}
user_token() { OAUTH_ACCESS_TOKEN=""; [ -r "$TOKENS" ] && . "$TOKENS" 2>/dev/null; printf '%s' "${OAUTH_ACCESS_TOKEN:-}"; }
# $1 = types, $2 = limit, $3 = offset
search() {
    req -H "Authorization: Bearer $acc" --get --data-urlencode "q=$Q" \
        --data "type=$1" --data "limit=$2" --data "offset=$3" --data "market=$MARKET" \
        https://api.spotify.com/v1/search 2>/dev/null
}

lim=$N; [ "$lim" -gt 10 ] && lim=10        # at most 10 a request
acc=$(user_token); json=""
[ -n "$acc" ] && json=$(search artist,track "$lim" 0)
if [ -z "$json" ] && [ -n "$acc" ]; then     # the hour-long token ran out
    "$OAUTH" refresh >/dev/null 2>&1 && acc=$(user_token) && json=$(search artist,track "$lim" 0)
fi
if [ -z "$json" ]; then
    acc=$(app_token); [ -n "$acc" ] || { printf '[]'; exit 0; }
    json=$(search artist,track "$lim" 0)
fi
[ -n "$json" ] || { printf '[]'; exit 0; }
# more tracks than one page: the next pages of tracks only
off=10
while [ "$off" -lt "$N" ]; do
    case "$json" in *'"next":null'*) break ;; esac
    l=$((N - off)); [ "$l" -gt 10 ] && l=10
    page=$(search track "$l" "$off") || break
    json="$json$page"; off=$((off + 10))
done

# One token per line: section starts (#ST tracks, #SA artists), then the
# values that matter in order — image url + height, name, explicit, uri.
printf '%s' "$json" \
  | sed 's/"tracks":{"href"/\n#ST\n/g; s/"artists":{"href"/\n#SA\n/g
         s/"name":"/\n#N#/g; s/"uri":"/\n#U#/g; s/"url":"/\n#I#/g; s/"explicit":/\n#E#/g
         s/"height":/\n#H#/g' \
  | awk -v max="$N" '
      # a JSON string value up to its closing quote, escapes kept as they are
      function val(s,   i, c, o) {
          o = ""
          for (i = 1; i <= length(s); i++) {
              c = substr(s, i, 1)
              if (c == "\\") { o = o c substr(s, i + 1, 1); i++; continue }
              if (c == "\"") break
              o = o c
          }
          return o
      }
      /^#ST/ { sec = "t"; next }
      /^#SA/ { sec = "a"; next }
      # images come big to small, url and height in either order (albums:
      # height first, artists: url first): the smallest that is still >= 60 px
      # (sharp at 40 px on a 2x screen), else whatever there is
      function pic(u, h) { if (h == 0 || h >= 60 || img == "") img = u; cu = ""; ch = "" }
      /^#I#/ { cu = val(substr($0, 4))
               if (cu !~ /^https:\/\/i\.scdn\.co\/image\//) { cu = ""; ch = ""; next }
               if (ch != "") pic(cu, ch + 0)
               next }
      /^#H#/ { ch = substr($0, 4) + 0 ""
               if (cu != "") pic(cu, ch + 0)
               next }
      /^#N#/ { nm = val(substr($0, 4)); next }
      /^#E#/ { ex = (substr($0, 4) ~ /^true/) ? 1 : 0; next }
      /^#U#/ {
          u = val(substr($0, 4))
          if (sec == "a" && u ~ /^spotify:artist:/) {      # images (big..small), name, uri
              if (!na) { na = 1; an = nm; au = u; ai = img }
              img = ""; next
          }
          if (sec != "t") next
          # a track: album {artists, images, name, uri}, artists [{name, uri}],
          # explicit, name, uri
          if (u ~ /^spotify:album:/)  { alimg = img; inart = 1; ta = ""; next }
          if (u ~ /^spotify:artist:/) { if (inart) { sep = (ta == "") ? "" : ", "; ta = ta sep nm }; next }
          if (u ~ /^spotify:track:/) {
              if (!seen[u]++ && nm != "" && ta != "") {
                  nt++; tn[nt] = nm; tA[nt] = ta; tu[nt] = u; ti[nt] = alimg; te[nt] = ex
              }
              inart = 0; ta = ""; ex = 0; img = ""
          }
          next
      }
      function row(k, n, a, q, u, i, e) {
          if (rows++) printf ","
          printf "{\"k\":\"%s\",\"n\":\"%s\",\"a\":\"%s\",\"q\":\"%s\",\"u\":\"%s\",\"i\":\"%s\",\"e\":%d}", k, n, a, q, u, i, e
      }
      END {
          printf "["
          apos = (nt < 2) ? nt : 2                  # the artist after two tracks
          for (j = 1; j <= nt || (na && !done); j++) {
              if (rows >= max) break
              if (na && !done && j - 1 == apos) { row("a", an, "", an, au, ai, 0); done = 1; j--; continue }
              if (j > nt) break
              row("t", tn[j], tA[j], tn[j] " " tA[j], tu[j], ti[j], te[j])
          }
          printf "]"
      }'
