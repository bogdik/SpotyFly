#!/bin/sh
# Resolve the last played track URIs into "Track — Artist" lines for the monitor.
set -u
STATE=/data/opt/spotify-connect/receiver-state/web-api
MARKET=$(cat "$STATE/market" 2>/dev/null); case "$MARKET" in [A-Z][A-Z]) ;; *) MARKET=US ;; esac   # the account's country (spotify-oauth.sh market)
. "$STATE/credentials.env"
PROXY=""; [ -r "$STATE/net.conf" ] && . "$STATE/net.conf" 2>/dev/null   # PROXY= (monitor ⚙)
PROXY=${PROXY:-}
CA=/data/opt/solo/ca-certificates.crt
HIST=$STATE/radio-history.txt
OUT=/data/opt/spotify-connect/monitor/history-display.txt

req() { /opt/bin/curl -fsS --connect-timeout 5 --max-time 12 -x "$PROXY" --cacert "$CA" "$@"; }

access=""
i=0
while [ "$i" -lt 3 ]; do
  access=$(req -u "$CLIENT_ID:$CLIENT_SECRET" --data grant_type=client_credentials \
    https://accounts.spotify.com/api/token 2>/dev/null | sed -n 's/.*"access_token":"\([^"]*\)".*/\1/p')
  [ -n "$access" ] && break
  i=$((i + 1))
done
[ -n "$access" ] || exit 1

: >"$OUT.new"
# newest first
tail -16 "$HIST" 2>/dev/null | awk '$0!=p{print} {p=$0}' | tail -12 | sed '1!G;h;$!d' | while IFS= read -r uri; do
  id=${uri#spotify:track:}
  case "$id" in *[!A-Za-z0-9]*|'') continue;; esac
  j=""; k=0
  while [ "$k" -lt 2 ]; do
    j=$(req -H "Authorization: Bearer $access" "https://api.spotify.com/v1/tracks/$id?market=$MARKET" 2>/dev/null)
    [ -n "$j" ] && break; k=$((k + 1))
  done
  name=$(printf '%s' "$j" | sed -n 's/.*"name":"\([^"]*\)".*/\1/p' | head -1)
  artist=$(printf '%s' "$j" | sed 's/"name":"/\n"name":"/g' | sed -n '2s/.*"name":"\([^"]*\)".*/\1/p')
  [ -n "$name" ] && printf '%s — %s\n' "$name" "$artist" >>"$OUT.new"
done
# Only replace the display if we actually resolved something; otherwise keep the
# last good history (a transient 429/quota or proxy miss must not blank it out).
if [ -s "$OUT.new" ]; then
  mv "$OUT.new" "$OUT" 2>/dev/null || true
else
  rm -f "$OUT.new" 2>/dev/null || true
fi
