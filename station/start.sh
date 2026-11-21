#!/bin/sh
# Boot of the Spotify stack (from /etc/init.d/S99spotify-connect): the voice
# proxy (Alice's events -> Spotify actions), Soloist (plays the music) and its
# keeper, the monitor.  spotiya.receiver is no longer used (2026-11-15).
set -eu

SPOTIFY_DIR=/data/opt/spotify-connect
PROXY="$SPOTIFY_DIR/alice-voice-proxy"
PIDFILE=/run/alice-voice-proxy.pid
SOLOIST_DAEMON="$SPOTIFY_DIR/soloist-daemon.sh"

# This 4.9 kernel prints a register dump for every syscall it does not know,
# and the 32-bit Spotify binaries ask for time64/statx ones (397, 403) dozens
# of times a second.  dmesg and syslog then turn over every few minutes and
# keep nothing: when soloist died on 2026-11-14 no trace of it was left.  Only
# the printing is switched off; the calls still fail with ENOSYS as before.
echo 0 >/proc/sys/debug/exception-trace 2>/dev/null || true

# Keep Master quiet while the boot is still busy: alsa starts it at 100% and loud
# audio then browns out the 5 V supply (cold reboot).  Must run before anything
# can make a sound; it exits by itself once the boot window is over.
if [ -x "$SPOTIFY_DIR/alice-volume-guard.sh" ]; then
    nohup setsid "$SPOTIFY_DIR/alice-volume-guard.sh" </dev/null >/dev/null 2>&1 &
fi

proxy_running() {
    [ -r "$PIDFILE" ] || return 1
    pid=$(cat "$PIDFILE" 2>/dev/null) || return 1
    case "$pid" in ''|*[!0-9]*) return 1 ;; esac
    kill -0 "$pid" 2>/dev/null
}

if [ -x "$PROXY" ] && ! proxy_running; then
    rm -f "$PIDFILE"
    nohup setsid "$PROXY" </dev/null >/tmp/alice-voice-proxy.out 2>&1 &
    echo "$!" >"$PIDFILE"
fi


# The official Soloist daemon owns Spotify audio.
if [ -x "$SOLOIST_DAEMON" ]; then
    "$SOLOIST_DAEMON" || true
fi
# ...and is restarted if it dies or stops answering (2026-11-14: it died
# silently after a long internet outage and stayed dead until a reboot).  The
# keeper also brings the voice proxy back if it is gone.
KEEPER="$SPOTIFY_DIR/alice-soloist-keeper.sh"
if [ -x "$KEEPER" ] && ! ps 2>/dev/null | grep -q "[a]lice-soloist-keeper"; then
    nohup setsid "$KEEPER" </dev/null >/dev/null 2>&1 &
fi

"$SPOTIFY_DIR/monitor/start-monitor.sh" || true

"$SPOTIFY_DIR/alice-boot-fix.sh" || true
exit 0
