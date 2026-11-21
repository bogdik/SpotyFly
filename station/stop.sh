#!/bin/sh
# Stops the Spotify stack (/etc/init.d/S99spotify-connect stop, installer.sh,
# uninstaller.sh): the keeper first — it would bring the rest back — then the
# voice proxy, Soloist, the monitor and every helper script of ours.
#   --list print what would be stopped (with --all: the relay too), stop nothing
#   --all  also the mediad relay (S98zz-wsfake).  Without the relay Alice stops
#          answering until quasar.cfg points mediad back at its stock server
#          (uninstaller.sh does that before rebooting).
# Only processes started from our directories are touched: never the firmware's own —
# killing `maind --platformd` resets the speaker to factory settings.
set -u
ALL=0; LIST=0
for a in "$@"; do case "$a" in --all) ALL=1 ;; --list) LIST=1 ;; esac; done
D=/data/opt/spotify-connect
me=$$; parent=$PPID

pids() { # pattern... -> pids of our processes whose command line has one of them
    ps -o pid,args 2>/dev/null | awk -v me="$me" -v pp="$parent" -v pats="$*" '
        BEGIN { n = split(pats, p, " ") }
        NR == 1 || $1 == me || $1 == pp || /maind|awk/ { next }
        { for (i = 1; i <= n; i++) if (index($0, p[i])) { print $1; break } }'
}
kill_all() { # pattern...
    if [ "$LIST" = 1 ]; then
        for p in $(pids "$@"); do ps -o pid,args | awk -v p="$p" '$1 == p' | cut -c1-80; done
        return 0
    fi
    set -- $(pids "$@")
    [ $# -gt 0 ] || return 0
    kill "$@" 2>/dev/null
    i=0
    while [ "$i" -lt 10 ] && [ -n "$(for p in "$@"; do [ -d "/proc/$p" ] && echo "$p"; done)" ]; do
        sleep 1; i=$((i + 1))
    done
    for p in "$@"; do [ -d "/proc/$p" ] && kill -9 "$p" 2>/dev/null; done
    return 0
}

kill_all alice-soloist-keeper.sh
kill_all "$D/alice-" "$D/monitor/" "$D/soloist-daemon.sh" /data/opt/solo/
if [ "$ALL" = 1 ]; then
    kill_all S98zz-wsfake "$D/quasar-ws-relay"
    [ "$LIST" = 1 ] || rm -f /run/quasar-ws-relay.pid
fi
[ "$LIST" = 1 ] && exit 0
rm -f /run/alice-voice-proxy.pid /run/soloist-manual.pid /run/spotify-monitor-tls.pid \
      /run/spotify-monitor.pid /run/spotify-auto-advance.pid /run/spotify-net-probe.pid \
      /run/alice-suppress.pid
exit 0
