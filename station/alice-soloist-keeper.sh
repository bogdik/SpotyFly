#!/bin/sh
# Soloist watchdog.  On 2026-11-14 soloist died silently after ~30 min without
# internet and nothing restarted it (soloist-daemon.sh runs once, from
# start.sh): no music until the next reboot.  This loop restarts it when
#   * its process is gone, or
#   * its websocket API has not answered for DEAD_AFTER consecutive checks
#     (before dying it stopped answering for ~2 min while still running).
# A network outage alone is not a reason: soloist reconnects by itself, and its
# API keeps answering meanwhile.  After a restart the speaker is made the active
# device again and playback resumed, but only when it was active/playing before,
# so a phone that was playing is never hijacked.  Launched from start.sh.
# It also restarts the voice proxy if that one is gone (keep_proxy).
# A failed login is a reason too: on 2026-11-15 soloist started 13 s after boot,
# before the network was ready, logged "login failed: make sure --api-key is
# valid" and never tried again — it kept answering, so nothing played for 14 h.
# Its log is rewritten at each start, so a "login failed" there is this run's.
# On request too (REQ file, written by alice-auto-advance.sh): on 2026-11-18,
# after the SOCKS proxy dropped its connections, soloist answered and was
# logged in but loaded no track at all ("playing" with no item) until restarted.
set -u
BASE=/data/opt/spotify-connect
DAEMON=$BASE/soloist-daemon.sh
PID=/run/soloist-manual.pid
R=/data/opt/solo/run.sh
W="-w 127.0.0.1:19090"
LOG=/tmp/alice-soloist-keeper.log
INTERVAL=10          # seconds between checks
DEAD_AFTER=6         # unanswered checks (60 s) before a running soloist is restarted
MIN_RESTART_GAP=60   # never restart more often than this (loop guard)
REQ=/tmp/soloist-restart-request   # its text = the reason

up() { cut -d. -f1 /proc/uptime; }
log() { echo "$(date +%H:%M:%S) [$(up)s] $*" >>"$LOG"; }

# `ctl` with a hard time limit (busybox here has no `timeout`): a wedged soloist
# can hold the connection open instead of refusing it.
ctl() {
    $R ctl "$@" $W >/tmp/.keeper.out 2>/dev/null &
    cpid=$!
    ( sleep 5; kill -TERM "$cpid" 2>/dev/null; sleep 1; kill -KILL "$cpid" 2>/dev/null ) >/dev/null 2>&1 &
    wpid=$!
    wait "$cpid"; rc=$?
    kill "$wpid" 2>/dev/null; wait "$wpid" 2>/dev/null
    cat /tmp/.keeper.out
    return "$rc"
}
alive() {
    p=$(cat "$PID" 2>/dev/null) || return 1
    case "$p" in ''|*[!0-9]*) return 1 ;; esac
    kill -0 "$p" 2>/dev/null
}
SLOG=/tmp/soloist-manual.log
answers() { st=$(ctl status 2>/dev/null); printf '%s\n' "$st" | grep -q '^soloist: reachable'; }
# After answers(): not logged in, and this run's log says the login failed.
login_failed() {
    printf '%s\n' "$st" | grep -q 'logged in: yes' && return 1
    grep -q 'login failed' "$SLOG" 2>/dev/null
}

# A soloist that died abruptly (SIGKILL, crash) leaves its ALSA dmix shared
# state behind: SysV shm with ipc_key 1024 (+1 for the mix buffer) and the sem,
# see /etc/asound.conf.  The next instance attaches to that leftover and never
# gets the device out of PREPARED: it reports "buffering" forever and nothing
# else can play through dmix either (seen 2026-11-14 after a kill -9 test; a
# graceful exit cleans the IPC up by itself).  Drop it while nobody is attached.
DMIX_KEY=1024
clean_dmix() {
    for key in $DMIX_KEY $((DMIX_KEY + 1)); do
        n=$(awk -v k="$key" 'NR>1 && $1==k {print $7}' /proc/sysvipc/shm)
        [ "$n" = 0 ] && ipcrm -M "$key" 2>/dev/null && log "removed stale dmix shm $key"
    done
    if ! awk -v k="$DMIX_KEY" 'NR>1 && ($1==k || $1==k+1) && $7>0' /proc/sysvipc/shm | grep -q . \
       && awk -v k="$DMIX_KEY" 'NR>1 && $1==k' /proc/sysvipc/sem | grep -q .; then
        ipcrm -S "$DMIX_KEY" 2>/dev/null && log "removed stale dmix sem $DMIX_KEY"
    fi
}

restart() { # $1 = reason
    log "restart: $1 (was active=$was_active status=$was_status)"
    p=$(cat "$PID" 2>/dev/null)
    case "$p" in
        ''|*[!0-9]*) ;;
        *) if kill -0 "$p" 2>/dev/null; then
               kill -TERM "$p" 2>/dev/null; i=0
               while kill -0 "$p" 2>/dev/null && [ "$i" -lt 30 ]; do usleep 200000; i=$((i + 1)); done
               kill -0 "$p" 2>/dev/null && kill -KILL "$p" 2>/dev/null && usleep 300000
           fi ;;
    esac
    rm -f "$PID"
    clean_dmix
    "$DAEMON" || log "soloist-daemon.sh failed"
    i=0
    while [ "$i" -lt 40 ] && ! answers; do usleep 500000; i=$((i + 1)); done
    answers || { log "soloist did not come up"; return 1; }
    logged=0
    i=0
    while [ "$i" -lt 30 ]; do ctl status | grep -q 'logged in: yes' && { logged=1; break; }; sleep 1; i=$((i + 1)); done
    [ "$logged" = 1 ] || log "not logged in after 30 s"
    if [ "$was_active" = true ]; then
        ctl activate >/dev/null; log "activated again"
        if [ "$was_status" = playing ] || [ "$was_status" = buffering ]; then
            sleep 2; ctl play >/dev/null; log "playback resumed"
        fi
    fi
    last_restart=$(up)
}

# The voice proxy is the only reader of Alice's events now (spotiya.receiver,
# which reconnected by itself, is gone): if it dies, no voice command works
# until a reboot.  Start it again, at most once per PROXY_GAP seconds.
PROXY=$BASE/alice-voice-proxy
PROXY_PID=/run/alice-voice-proxy.pid
PROXY_GAP=30
proxy_alive() {
    p=$(cat "$PROXY_PID" 2>/dev/null) || return 1
    case "$p" in ''|*[!0-9]*) return 1 ;; esac
    grep -q '^alice-voice-pro' "/proc/$p/comm" 2>/dev/null
}
keep_proxy() {
    [ -x "$PROXY" ] && ! proxy_alive || return 0
    [ $(( $(up) - last_proxy )) -ge "$PROXY_GAP" ] || return 0
    setsid "$PROXY" </dev/null >/tmp/alice-voice-proxy.out 2>&1 &
    echo "$!" >"$PROXY_PID"
    last_proxy=$(up)
    log "voice proxy was not running: started (pid $!)"
}

log "keeper started"
was_active=false; was_status=""; silent=0; last_restart=0; last_proxy=0
st=""; login_gap=$MIN_RESTART_GAP
while :; do
    sleep "$INTERVAL"
    keep_proxy
    if [ -f "$REQ" ]; then
        reason="$(cat "$REQ" 2>/dev/null)"; rm -f "$REQ"
        reason="requested: ${reason:-?}"
    elif ! alive; then
        reason="process gone"
    elif answers && login_failed; then
        # back off while the network stays down: 1, 2, 4, … up to 10 min
        if [ $(( $(up) - last_restart )) -lt "$login_gap" ]; then continue; fi
        reason="$(grep -m1 'login failed' "$SLOG" | cut -c26-)"
        [ "$login_gap" -lt 600 ] && login_gap=$((login_gap * 2))
        [ "$login_gap" -gt 600 ] && login_gap=600
    elif [ -n "$st" ] && printf '%s\n' "$st" | grep -q '^soloist: reachable'; then
        silent=0
        printf '%s\n' "$st" | grep -q 'logged in: yes' && login_gap=$MIN_RESTART_GAP
        now=$(ctl now --json)
        a=$(printf '%s' "$now" | grep -o '"is_active":[a-z]*' | head -1 | cut -d: -f2)
        s=$(printf '%s' "$now" | sed -n 's/.*"status":"\([a-z]*\)".*/\1/p' | head -1)
        [ -n "$a" ] && was_active=$a
        [ -n "$s" ] && was_status=$s
        continue
    else
        silent=$((silent + 1))
        [ "$silent" -ge "$DEAD_AFTER" ] || continue
        reason="api silent for $((silent * INTERVAL)) s"
    fi
    if [ $(( $(up) - last_restart )) -lt "$MIN_RESTART_GAP" ]; then
        log "skip restart ($reason): last one $(( $(up) - last_restart )) s ago"
        continue
    fi
    silent=0
    restart "$reason"
done
