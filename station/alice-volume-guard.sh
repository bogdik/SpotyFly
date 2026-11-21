#!/bin/sh
# Boot volume guard.  The ALSA softvol "Master" (every sound goes through it) is
# created by alsa-lib at 100% and stays there until quasar applies the saved
# volume (~18 s).  Loud audio during the busy first minute browns out the 5 V
# supply and the speaker cold-reboots.  So the alsa default is cut to START_CAP
# at once.  A level that quasar or the user sets is a real choice: it is
# applied as is (music stays in step with Alice, "громче/тише" work at once);
# only a level above HARD_CAP is held down until GUARD_UNTIL seconds of uptime
# and restored when the window ends.  Launched first thing from start.sh.
set -u
START_CAP=${START_CAP:-40}      # percent, for alsa's 100% default
HARD_CAP=${HARD_CAP:-90}        # percent, for real settings during the window
GUARD_UNTIL=${GUARD_UNTIL:-90}  # seconds of uptime
LOG=/tmp/alice-volume-guard.log

up() { cut -d. -f1 /proc/uptime; }
log() { echo "$(up)s $*" >>"$LOG"; }
get() { amixer get Master 2>/dev/null | sed -n 's/.*Playback [0-9]* \[\([0-9]*\)%\].*/\1/p' | head -1; }
set_pct() { amixer -q set Master "$1%" 2>/dev/null; }
tick() { usleep 250000 2>/dev/null || sleep 1; }

[ "$(up)" -lt "$GUARD_UNTIL" ] || exit 0

# Master appears once alsa-lib first opens "default" (S91alsa); wait for it.
v=""
while [ -z "$v" ] && [ "$(up)" -lt "$GUARD_UNTIL" ]; do v=$(get); [ -n "$v" ] || tick; done
[ -n "$v" ] || { log "no Master control, giving up"; exit 0; }

desired=""
capped=0
if [ "$v" -gt "$START_CAP" ]; then set_pct "$START_CAP"; log "start: $v% (alsa default) -> $START_CAP%"; last=$START_CAP; capped=1
else log "start: $v%"; last=$v; fi

# Spotify plays through its own softvol "Spotify" (asound-spotify.conf), which
# alsa-lib creates at 100% on first open.  Create it now, before Soloist starts,
# at the boot cap; alice-voice-proxy then keeps it at Alice's (Master) level.
SPOTIFY_CONF=/data/opt/solo/asound-spotify.conf
if [ -r "$SPOTIFY_CONF" ]; then
    ALSA_CONFIG_PATH="/usr/share/alsa/alsa.conf:$SPOTIFY_CONF" \
        aplay -q -D plug:spotify -t raw -f S16_LE -r 48000 -c 2 -d 1 /dev/zero 2>/dev/null
    amixer -q sset Spotify "$START_CAP%" 2>/dev/null && log "Spotify control created at $START_CAP%"
fi

# The receiver starts (~15 s) while Master still holds our boot cap, takes it as
# its Spotify volume and writes it back when Spotify connects (~19 s) -- right
# over the level quasar has just applied.  A drop to exactly the cap soon after
# a real level is our own cap echoed, not a choice: put the real level back.
real=""; real_at=0
ECHO_WINDOW=15

while [ "$(up)" -lt "$GUARD_UNTIL" ]; do
    tick
    v=$(get)
    [ -n "$v" ] && [ "$v" != "$last" ] || continue
    if [ "$capped" = 1 ] && [ "$v" = "$START_CAP" ] && [ -n "$real" ] && [ $(( $(up) - real_at )) -le "$ECHO_WINDOW" ]; then
        set_pct "$real"; last=$real; log "echo of boot cap $START_CAP% -> back to $last%"
        continue
    fi
    # Someone else changed the volume: that is a real choice.
    if [ "$v" -gt "$HARD_CAP" ]; then
        desired=$v; set_pct "$HARD_CAP"; last=$HARD_CAP; log "clamp $v% -> $HARD_CAP% (will restore)"
    else
        desired=""; last=$v; log "set to $v% (accepted)"
    fi
    real=$last; real_at=$(up)
done

v=$(get)
if [ -n "$desired" ] && [ "$v" = "$HARD_CAP" ]; then
    set_pct "$desired"; log "window over: restored $desired%"
else
    log "window over: left at ${v:-?}%"
fi
