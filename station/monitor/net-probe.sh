#!/bin/sh
# Network watch for the monitor's "сеть" dot (2026-11-19).  Spotify reaches the
# internet only through the user's SOCKS proxy (run.sh ALL_PROXY); twice on
# 2026-11-18 music stopped while that path was broken.  Every INTERVAL s (FAST
# while something is wrong) it checks, in order:
#   wifi  — the default gateway answers ping;
#   inet  — a plain HTTPS request goes out directly (either of two hosts);
#   proxy — Spotify answers through the proxy (what soloist itself needs).
# The first that fails names the state: nowifi / noinet / noproxy, else ok;
# a failure right after "ok" counts only if it is still there 3 s later.
# Writes $OUT (one JSON object, read by monitor-server) and appends each change
# of state to $EV on flash (only changes, last 40), so the monitor can show when
# the network went down and for how long, even across a reboot.
#
# Wi‑Fi diagnostics (2026-11-19): the speaker's Wi‑Fi drops for a few seconds
# every ~3 min — the firmware itself ("Reseting wifi. Reason is nointernet",
# deauth reason 3, locally generated) while the internet works.  So:
#   * every drop of wlan0 (sysfs carrier_changes) is counted, and those the
#     firmware made are told apart (its maind.log "Reseting wifi" line);
#   * a text snapshot (Wi‑Fi link, signal, counters, pings, DNS, the network
#     lines of the firmware's and the kernel's logs, Spotify, memory) goes to
#     $DIAG on flash when an outage starts, when it ends and after a boot
#     (the newest $KEEP kept; at most one per $SNAP_GAP s);
#   * $ALIVE gets the time every 5 min: after a reboot the boot event says
#     when the previous run was last alive.
set -u
OUT=/tmp/net-status.json
EV=/data/opt/spotify-connect/receiver-state/net-events.txt   # epoch<TAB>state
NETCONF=/data/opt/spotify-connect/receiver-state/web-api/net.conf   # PROXY= (monitor ⚙)
DEF_PROXY=
CURL=/opt/bin/curl
INTERVAL=30
FAST=10
DIAG=/data/opt/spotify-connect/receiver-state/netdiag
ALIVE=$DIAG/alive
MAIND=/data/quasar/daemons_logs
KEEP=12
SNAP_GAP=120

probe_ms() { # url [proxy] -> prints the time in ms; fails without an HTTP answer
    r=$($CURL -s -o /dev/null -m 8 ${2:+-x "$2"} -w '%{http_code} %{time_total}' "$1" 2>/dev/null)
    case "${r%% *}" in ''|000) return 1 ;; esac
    awk -v s="${r#* }" 'BEGIN { printf "%d", s * 1000 }'
}
gw_ms() {
    gw=$(ip route 2>/dev/null | sed -n 's/^default via \([0-9.]*\).*/\1/p' | head -1)
    [ -n "$gw" ] || return 1
    ping -c 1 -W 2 "$gw" 2>/dev/null | sed -n 's/.*time=\([0-9]*\).*/\1/p' | grep .
}

# The proxy as set in the monitor, read afresh each time (login:pass@ included).
proxy_url() { (PROXY=""; [ -r "$NETCONF" ] && . "$NETCONF" 2>/dev/null; printf '%s' "${PROXY:-$DEF_PROXY}"); }
check() { # -> sets now, gw, inet, px
    gw=""; inet=""; px=""
    if ! gw=$(gw_ms); then
        now=nowifi
    elif ! inet=$(probe_ms https://1.1.1.1/cdn-cgi/trace) && ! inet=$(probe_ms http://connectivitycheck.gstatic.com/generate_204); then
        now=noinet
    elif ! px=$(probe_ms https://apresolve.spotify.com/ "$(proxy_url)"); then
        now=noproxy
    else
        now=ok
    fi
}

# The firmware's Wi‑Fi resets: "<epoch> <reason>" of its latest one, from its
# own log (its time is local, the same clock as ours).
fw_reset() {
    l=$(cat "$MAIND/maind.log.1" "$MAIND/maind.log" 2>/dev/null | grep 'Reseting wifi\. Reason is' | tail -n 1)
    [ -n "$l" ] || return 1
    ts=$(printf '%s' "$l" | sed -n 's/.*\[\([0-9-]* [0-9:]*\)\.[0-9]*\].*/\1/p')
    why=$(printf '%s' "$l" | sed -n 's/.*Reason is \([a-z_]*\).*/\1/p')
    e=$(date -d "$ts" +%s 2>/dev/null) || return 1
    printf '%s %s\n' "$e" "${why:-?}"
}
snapshot() { # kind -> a text file in $DIAG
    t0=$(date +%s)
    [ "$1" = boot ] || [ $((t0 - last_snap)) -ge "$SNAP_GAP" ] || return 0
    last_snap=$t0
    mkdir -p "$DIAG"
    f=$DIAG/$(date +%Y%m%d-%H%M%S)-$1.txt
    {
        echo "== $1  $(date '+%Y-%m-%d %H:%M:%S')  uptime $(cut -d' ' -f1 /proc/uptime) s  (сеть: $state с $since)"
        [ -n "${2:-}" ] && echo "$2"
        echo "-- Wi‑Fi"; wpa_cli -i wlan0 status 2>&1 | grep -viE 'psk|passphrase|password|key_mgmt=none'
        wpa_cli -i wlan0 signal_poll 2>&1; sed -n 3p /proc/net/wireless
        echo "carrier_changes $(cat /sys/class/net/wlan0/carrier_changes 2>/dev/null)"
        for x in rx_errors tx_errors rx_dropped tx_dropped; do echo "$x $(cat /sys/class/net/wlan0/statistics/$x 2>/dev/null)"; done
        echo "-- адрес и маршрут"; ip addr show wlan0 2>&1 | grep -E 'inet |state'; ip route 2>&1
        echo "-- ping"; gw=$(ip route 2>/dev/null | sed -n 's/^default via \([0-9.]*\).*/\1/p' | head -1)
        for h in $gw 8.8.8.8; do ping -c 2 -W 2 "$h" 2>&1 | tail -n 2; done
        echo "-- DNS"; nslookup apresolve.spotify.com 2>&1 | tail -n 2
        echo "-- прошивка (сеть)"
        cat "$MAIND/maind.log.1" "$MAIND/maind.log" 2>/dev/null \
            | grep -E 'Reseting wifi|Do not reset|no Internet|CTRL-EVENT|wifi_|network_adapter|Network state|Wifi status|onConnectionFailure' \
            | grep -viE 'token|session=|sign=|password=[^*]' | tail -n 40 | cut -c1-240
        echo "-- ядро (Wi‑Fi)"; dmesg 2>/dev/null | grep -iE 'deauth|disconnect|reason_code|cfg80211_connect|link (up|down)' | tail -n 20 | cut -c1-200
        echo "-- Spotify"; /data/opt/solo/run.sh ctl status -w 127.0.0.1:19090 2>&1 | sed -n '1,4p'
        tail -n 3 /tmp/alice-soloist-keeper.log 2>/dev/null
        echo "-- память / нагрузка"; free 2>&1 | sed -n 2p; cat /proc/loadavg
    } 2>&1 | head -c 20000 >"$f"
    ls -1 "$DIAG"/*.txt 2>/dev/null | sort -r | tail -n +$((KEEP + 1)) | while read -r o; do rm -f "$o"; done
}

state=""; since=$(date +%s); last_snap=0; alive_at=0
cc_last=$(cat /sys/class/net/wlan0/carrier_changes 2>/dev/null || echo 0); started=$(date +%s)   # drops: since boot (sysfs)
fw_n=0; fw_last=0; drop_last=0; drop_why=""
mkdir -p "$DIAG"
last=$(tail -n 1 "$EV" 2>/dev/null)
[ -n "$last" ] && { state=$(printf '%s' "$last" | cut -f2); since=$(printf '%s' "$last" | cut -f1); }
# Started with the system (start-monitor.sh at boot): note the reboot.
if [ "$(cut -d. -f1 /proc/uptime)" -lt 600 ]; then
    prev=$(cat "$ALIVE" 2>/dev/null || echo 0)
    t=$(date +%s)
    { tail -n 39 "$EV" 2>/dev/null; printf '%s\tboot\t%s\n' "$t" "$prev"; } >"$EV.new" && mv "$EV.new" "$EV"
    snapshot boot "до перезагрузки колонка была жива последний раз: $prev (epoch), сеть тогда: $state"
    state=""
fi
while :; do
    check
    # one lost ping / one slow answer is not an outage: confirm it 3 s later
    if [ "$now" != ok ] && [ "$state" = ok ]; then sleep 3; check; fi
    t=$(date +%s)
    if [ "$now" != "$state" ]; then
        was=$state; state=$now; since=$t
        { tail -n 39 "$EV" 2>/dev/null; printf '%s\t%s\n' "$t" "$now"; } >"$EV.new" && mv "$EV.new" "$EV"
        if [ "$now" != ok ]; then snapshot "down-$now"; elif [ -n "$was" ]; then snapshot "up-after-$was"; fi
    fi
    # Wi‑Fi drops since boot; the firmware's own resets told apart
    cc=$(cat /sys/class/net/wlan0/carrier_changes 2>/dev/null || echo "$cc_last")
    if [ "$cc" -gt "$cc_last" ]; then
        drop_last=$t; drop_why=""
        if fr=$(fw_reset) && [ "${fr%% *}" -gt "$fw_last" ]; then
            fw_last=${fr%% *}; fw_n=$((fw_n + 1)); drop_why=${fr#* }
            # the first one since the start: what the link looks like right after it
            [ "$fw_n" = 1 ] && snapshot fw-reset "прошивка сбросила Wi‑Fi: $drop_why"
        fi
    fi
    cc_last=$cc
    if [ $((t - alive_at)) -ge 300 ]; then echo "$t" >"$ALIVE.new" && mv "$ALIVE.new" "$ALIVE"; alive_at=$t; fi
    ev=$(tail -n 12 "$EV" 2>/dev/null | awk -F'\t' '{ printf "%s[%s,\"%s\"]", (n++ ? "," : ""), $1, $2 }')
    printf '{"state":"%s","since":%s,"checked":%s,"gw_ms":%s,"inet_ms":%s,"proxy_ms":%s,"drops":%s,"fw_resets":%s,"fw_since":%s,"drop_last":%s,"drop_why":"%s","events":[%s]}\n' \
        "$state" "$since" "$t" "${gw:-null}" "${inet:-null}" "${px:-null}" "$(( cc / 2 ))" "$fw_n" "$started" "$drop_last" "$drop_why" "$ev" >"$OUT.new" && mv "$OUT.new" "$OUT"
    if [ "$state" = ok ]; then sleep "$INTERVAL"; else sleep "$FAST"; fi
done
