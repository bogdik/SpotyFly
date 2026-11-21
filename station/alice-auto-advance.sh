#!/bin/sh
# Voice-radio keeper for Soloist:
#   * records every track that actually plays into radio-history.txt (Soloist's
#     own auto-advance otherwise leaves gaps -> "история не записывается") and
#     tells alice-radio.sh, which then never picks it again within a day;
#   * 10 s before a track ends asks alice-radio.sh to fill the Spotify queue up
#     to 2 tracks (policy 2026-11-15: never 9-10 at once);
#   * skips, silently, our tracks queued for an earlier request (Spotify cannot
#     remove them from its queue) and blacklisted ones;
#   * unsticks the player when a track ends but Soloist stalls ("buffering",
#     position frozen, or "playing" on past the end) instead of moving on ->
#     "залипает при окончании";
#   * revives a "ghost" play: Soloist says "playing" with no track and the
#     position running on (2026-11-15: a ctl play it could not load -> 3 h of
#     silence, «Продолжи» did nothing) -> that track counts as played, next one;
#     from the third try on, when the network through the proxy works, soloist
#     itself is stuck (its proxied connections dead): the keeper restarts it,
#     at most every 5 min; else (network down) keeps trying once a minute;
#   * drops the radio when the phone starts a playlist/album (a context that is
#     not a single track): from then on that context plays, nothing is added.
set -u
STATE=/data/opt/spotify-connect/receiver-state/web-api
HISTORY=$STATE/radio-history.txt
SRC=$STATE/radio-source
SOLO_DATA=/data/opt/spotify-connect/receiver-state/soloist/data
R=/data/opt/solo/run.sh
RADIO=/data/opt/spotify-connect/alice-radio.sh
BL=/data/opt/spotify-connect/alice-blacklist.sh
QUIET=/tmp/soloist-draining   # alice-voice-proxy holds the music at 0 while set
TOPUP_MS=10000    # queue the next tracks this long before the end
NEAR_MS=12000     # within this of the end counts as "the end"
STALL_TICKS=3     # frozen position for this many polls => stuck -> force next
OVERRUN_MS=4000   # "playing" this far past the track's end counts as stuck too
GHOST_TICKS=8     # "playing" with no track this many polls => it never loaded
GHOST_SLOW=25     # after three such tries: retry this often (~1 min)
KICK=/tmp/soloist-restart-request   # alice-soloist-keeper.sh restarts soloist
KICK_GAP=300      # s: at most one such restart this often
NETCONF=$STATE/net.conf  # PROXY= (monitor ⚙), soloist's way out (run.sh ALL_PROXY)
JOURNAL=/tmp/alice-voice-journal.log

record_history() { # $1=uri ; append unless it's already the last line
    case "$1" in spotify:track:[A-Za-z0-9]*) ;; *) return;; esac
    last=$(tail -n 1 "$HISTORY" 2>/dev/null)
    [ "$1" = "$last" ] && return
    printf '%s\n' "$1" >>"$HISTORY"
    tail -n 100 "$HISTORY" >"$HISTORY.new" 2>/dev/null && mv "$HISTORY.new" "$HISTORY" || true
    chmod 600 "$HISTORY" 2>/dev/null || true
}
title_of() { sed 's/"identity":{"name":"/\n"identity":{"name":"/g' | sed -n '2s/.*"name":"\([^"]*\)".*/\1/p'; }
artist_of() { sed -n 's/.*"creators":\[{"entity":{"uri":"spotify:artist:[A-Za-z0-9]*","entity_type":"artist","decorations":{"identity":{"name":"\([^"]*\)".*/\1/p'; }

played=""; last_pos=-1; last_status=""; last_dur=0; stall=0; stall_uri=""; bl_uri=""; bl_tries=0; topped=""
ghost=0; ghost_tries=0; kicked=-100000
while :; do
    sleep 2
    now=$($R ctl now --json -D "$SOLO_DATA" 2>/dev/null || printf '{}')
    status=$(printf '%s' "$now" | sed -n 's/.*"status":"\([a-z]*\)".*/\1/p' | head -1)
    uri=$(printf '%s' "$now" | sed -n 's/.*"item":{"uri":"\(spotify:track:[A-Za-z0-9]*\)".*/\1/p' | head -1)
    pos=$(printf '%s' "$now" | sed -n 's/.*"position_ms":\([0-9]*\).*/\1/p' | head -1)
    dur=$(printf '%s' "$now" | sed -n 's/.*"duration_ms":\([0-9]*\).*/\1/p' | head -1)
    if [ -z "$uri" ]; then
        # a queue drain (alice-queue-drain.sh) is skipping: not a ghost
        [ "$(cat "$QUIET" 2>/dev/null || echo 0)" -gt "$(date +%s)" ] 2>/dev/null && { ghost=0; continue; }
        case "$status" in playing|buffering) ghost=$((ghost + 1)) ;; *) ghost=0 ;; esac
        # Three quick tries, then (network down: nothing loads, the radio cannot
        # refill) one a minute until something plays (2026-11-18: gave up after
        # three, silent for 7 h).
        need=$GHOST_TICKS; [ "$ghost_tries" -ge 3 ] && need=$GHOST_SLOW
        if [ "$ghost" -ge "$need" ]; then
            ghost=0; ghost_tries=$((ghost_tries + 1))
            dead=$(printf '%s' "$now" | sed -n 's/.*"context":{"uri":"\(spotify:track:[A-Za-z0-9]*\)".*/\1/p' | head -1)
            [ -n "$dead" ] && "$RADIO" played "$dead" "$dead" ""     # not picked again today
            msg=""; kick=0
            if [ "$ghost_tries" -le 2 ]; then
                msg="трек не загрузился — включаю следующий"
            elif [ $(( $(cut -d. -f1 /proc/uptime) - kicked )) -ge "$KICK_GAP" ] \
                 && /opt/bin/curl -s -o /dev/null -m 8 \
                    -x "$(PROXY=""; [ -r "$NETCONF" ] && . "$NETCONF" 2>/dev/null; printf '%s' "${PROXY:-}")" \
                    https://apresolve.spotify.com/; then
                kick=1; kicked=$(cut -d. -f1 /proc/uptime)
                msg="треки не загружаются, сеть есть — перезапускаю Spotify"
            elif [ "$ghost_tries" = 3 ]; then
                msg="треки не загружаются — пробую раз в минуту"
            fi
            [ -n "$msg" ] && printf '%s\tplay\t%s\n' "$(date +%s)" "$msg" >>"$JOURNAL" 2>/dev/null
            if [ "$kick" = 1 ]; then
                echo "ghost play, network ok" >"$KICK"   # resumes, then the next try skips
            else
                "$RADIO" skip >/dev/null 2>&1 || true
            fi
        fi
        continue
    fi
    ghost=0; ghost_tries=0
    case "$uri" in spotify:track:[A-Za-z0-9]*) ;; *) continue;; esac
    case "${pos:-x}${dur:-x}" in *[!0-9]*) pos=0; dur=0;; esac
    # alice-queue-drain.sh / a skip below is moving through the queue (flag =
    # expiry epoch): those tracks are not played.
    [ "$(cat "$QUIET" 2>/dev/null || echo 0)" -gt "$(date +%s)" ] 2>/dev/null && { stall=0; continue; }

    # Чёрный список: whatever starts playing (a queued track, the voice radio,
    # the phone) is skipped if it, or its artist, is blacklisted.  The skip holds
    # the music at 0 and walks past every blacklisted track queued behind it.
    # Three tries per track at most, so a stuck player cannot keep us looping.
    if [ -x "$BL" ] && why=$(printf '%s' "$now" | "$BL" check); then
        [ "$uri" = "$bl_uri" ] || { bl_uri=$uri; bl_tries=0; }
        if [ "$bl_tries" -lt 3 ]; then
            bl_tries=$((bl_tries + 1))
            "$BL" skip "$why"
        fi
        stall=0
        continue
    fi

    if [ "$uri" != "$played" ]; then
        # Ours, queued for an earlier request: never meant to play any more.
        if "$RADIO" stale "$uri"; then
            echo $(( $(date +%s) + 8 )) >"$QUIET"
            "$RADIO" skip >/dev/null 2>&1 || true
            i=0
            while [ "$i" -lt 12 ]; do
                usleep 250000 2>/dev/null || sleep 1
                [ "$($R ctl now --json -D "$SOLO_DATA" 2>/dev/null | sed -n 's/.*"item":{"uri":"\(spotify:track:[A-Za-z0-9]*\)".*/\1/p' | head -1)" != "$uri" ] && break
                i=$((i + 1))
            done
            rm -f "$QUIET"
            continue
        fi
        # (#7) record each track that starts playing.
        record_history "$uri"
        "$RADIO" played "$uri" "$(printf '%s' "$now" | title_of)" "$(printf '%s' "$now" | artist_of)"
        played="$uri"; topped=""
        if [ -s "$SRC" ]; then
            ctx=$(printf '%s' "$now" | sed -n 's/.*"context":{"uri":"[^"]*","entity_type":"\([a-z]*\)".*/\1/p' | head -1)
            case "$ctx" in
                ''|track) "$RADIO" refill </dev/null >/dev/null 2>&1 & ;;    # stock up while it plays
                *) "$RADIO" stop "телефон включил $ctx" ;;
            esac
        fi
    fi

    # (#4) stall-at-end recovery: position frozen near the end while "playing",
    # or running on past the end (2026-11-19: "7:29 / 6:22", soloist's own log
    # frozen at 6:22, the queued next track never started; «Продолжи» no help).
    if [ "$status" != paused ] && [ "$dur" -gt 0 ] && [ "$uri" = "$stall_uri" ] \
       && { { [ "$pos" -ge $((dur - NEAR_MS)) ] && [ "$pos" = "$last_pos" ]; } \
            || [ "$pos" -ge $((dur + OVERRUN_MS)) ]; }; then
        stall=$((stall + 1))
    else
        stall=0
    fi
    # A track that ended with nothing queued (a refill failed) stops "paused" at
    # its end: go on with the radio.
    ended=0
    [ -s "$SRC" ] && [ "$status" = paused ] && [ "$last_status" = playing ] && [ "$uri" = "$stall_uri" ] \
        && [ "$last_dur" -gt 0 ] && [ "$last_pos" -ge $((last_dur - 4000)) ] && ended=1
    if [ "$stall" -ge "$STALL_TICKS" ] || [ "$ended" = 1 ]; then
        [ -s "$SRC" ] && [ "$topped" != "$uri" ] && "$RADIO" topup
        "$RADIO" skip >/dev/null 2>&1 || true
        stall=0
    fi
    stall_uri="$uri"; last_pos="$pos"; last_status="$status"; last_dur="$dur"

    # 10 s before the end: the next 2 tracks into the queue (once per track).
    [ -s "$SRC" ] && [ "$topped" != "$uri" ] || continue
    case "$status" in playing|buffering) ;; *) continue;; esac
    # A search in progress sets the new source itself.
    [ -d "$STATE/search.lock" ] && continue
    [ "$dur" -gt 0 ] && [ "$pos" -ge $((dur - TOPUP_MS)) ] || continue
    topped="$uri"
    "$RADIO" topup
done
