#!/bin/sh
# Download the currently-playing cover ON the speaker (through the SOCKS proxy)
# and emit it as a base64 data: URI, so the monitor page shows art without the
# browser needing internet/proxy access.  Cached by URL for instant repeats.
set -u
STATE=/data/opt/spotify-connect/receiver-state/web-api
[ -r "$STATE/net.conf" ] && . "$STATE/net.conf" 2>/dev/null || true
PROXY=${PROXY:-}
CA=/data/opt/solo/ca-certificates.crt
SJ=/data/opt/spotify-connect/monitor/status.json
CACHE=/tmp/alice-cover.cache

url=$(sed -n 's/.*"cover":"\([^"]*\)".*/\1/p' "$SJ" 2>/dev/null | head -1)
case "$url" in https://i.scdn.co/image/*) ;; *) exit 0 ;; esac

# Serve from cache when the cover has not changed.
if [ -r "$CACHE" ] && [ "$(sed -n 1p "$CACHE" 2>/dev/null)" = "$url" ]; then
    sed -n 2p "$CACHE" 2>/dev/null
    exit 0
fi

b=$(/opt/bin/curl -fsS --max-time 10 -x "$PROXY" --cacert "$CA" "$url" 2>/dev/null | base64 2>/dev/null | tr -d '\n')
[ -n "$b" ] || b=$(/opt/bin/curl -fsS --max-time 10 --cacert "$CA" "$url" 2>/dev/null | base64 2>/dev/null | tr -d '\n')
[ -n "$b" ] || exit 0

data="data:image/jpeg;base64,$b"
{ printf '%s\n' "$url"; printf '%s\n' "$data"; } >"$CACHE.tmp" 2>/dev/null && mv "$CACHE.tmp" "$CACHE" 2>/dev/null || true
printf '%s' "$data"
