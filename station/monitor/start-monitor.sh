#!/bin/sh
set -u

BASE=/data/opt/spotify-connect/monitor
PID=/run/spotify-monitor.pid
LOG=/tmp/spotify-monitor.log

if [ ! -r /run/spotify-monitor-poller.pid ] || ! [ -d "/proc/$(cat /run/spotify-monitor-poller.pid 2>/dev/null)" ]; then
  nohup setsid "$BASE/poll-run.sh" </dev/null >/tmp/spotify-monitor-poller.log 2>&1 &
  echo "$!" >/run/spotify-monitor-poller.pid
fi

# Auto-advance: records what plays, queues the voice radio's next 2 tracks 10 s
# before each end (alice-radio.sh), skips stale/blacklisted ones, unsticks a
# track that stalls at its end.
AA_PID=/run/spotify-auto-advance.pid
if [ ! -r "$AA_PID" ] || ! [ -d "/proc/$(cat "$AA_PID" 2>/dev/null)" ]; then
  nohup setsid /data/opt/spotify-connect/alice-auto-advance.sh </dev/null >/tmp/spotify-auto-advance.log 2>&1 &
  echo "$!" >"$AA_PID"
fi

# Network watch: wifi / internet / Spotify through the proxy -> the "сеть" dot.
NP_PID=/run/spotify-net-probe.pid
if [ ! -r "$NP_PID" ] || ! [ -d "/proc/$(cat "$NP_PID" 2>/dev/null)" ]; then
  nohup setsid "$BASE/net-probe.sh" </dev/null >/dev/null 2>&1 &
  echo "$!" >"$NP_PID"
fi

# HTTPS terminator for the OAuth redirect: Spotify rejects http:// on a LAN IP
# ("redirect_uri Insecure"), so socat wraps the monitor in TLS on :8443 with a
# self-signed cert.  https://<ip>:8443/callback then reaches the device directly.
# The monitor's port is set in its ⚙ Настройки (receiver-state/monitor.conf).
PORT=$(sed -n 's/^PORT=\([0-9][0-9]*\)$/\1/p' /data/opt/spotify-connect/receiver-state/monitor.conf 2>/dev/null)
PORT=${PORT:-28088}
TLS_PID=/run/spotify-monitor-tls.pid
if [ -r "$BASE/tls.pem" ] && { [ ! -r "$TLS_PID" ] || ! [ -d "/proc/$(cat "$TLS_PID" 2>/dev/null)" ]; }; then
  nohup setsid /usr/bin/socat OPENSSL-LISTEN:8443,reuseaddr,fork,cert="$BASE/tls.pem",verify=0 \
    TCP4:127.0.0.1:"$PORT" </dev/null >/tmp/spotify-monitor-tls.log 2>&1 &
  echo "$!" >"$TLS_PID"
fi

if [ -r "$PID" ]; then
    old=$(cat "$PID" 2>/dev/null || true)
    case "$old" in ''|*[!0-9]*) ;; *) [ -d "/proc/$old" ] && exit 0 ;; esac
fi

rm -f "$PID"
nohup "$BASE/monitor-server" >"$LOG" 2>&1 &
echo "$!" >"$PID"
