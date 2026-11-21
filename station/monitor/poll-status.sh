#!/bin/sh
set -u
BASE=/data/opt/spotify-connect/monitor
DATA=/data/opt/spotify-connect/receiver-state/soloist/data
OUT=$BASE/status.json
HIST=/data/opt/spotify-connect/receiver-state/web-api/radio-history.txt
while :; do
  port=$(cat "$DATA/ws.port" 2>/dev/null || true)
  case "$port" in ''|*[!0-9]*) sleep 3; continue;; esac
  now=$(/data/opt/solo/run.sh ctl now --json -w "127.0.0.1:$port" 2>/dev/null || printf '{}')
  queue=$(/data/opt/solo/run.sh ctl queue 1 -w "127.0.0.1:$port" 2>/dev/null || true)
  status=$(printf '%s' "$now" | sed -n 's/.*"status":"\([^"]*\)".*/\1/p' | head -1)
  # soloist health for the monitor: ok (logged in) / nologin (up, not logged in to
  # Spotify — 2026-11-15 it stayed so for 14 h after a login at boot failed) / down.
  case "$now" in
    *'"is_active"'*) solo=ok ;;
    *) case "$(/data/opt/solo/run.sh ctl status -w "127.0.0.1:$port" 2>/dev/null)" in
         *'logged in: yes'*) solo=ok ;;
         *'soloist: reachable'*) solo=nologin ;;
         *) solo=down ;;
       esac ;;
  esac
  title=$(printf '%s' "$now" | sed 's/"identity":{"name":"/\n"identity":{"name":"/g' | sed -n '2s/.*"name":"\([^"]*\)".*/\1/p')
  artist=$(printf '%s' "$now" | sed -n 's/.*"creators":\[.*"identity":{"name":"\([^"]*\)".*/\1/p' | head -1)
  album=$(printf '%s' "$now" | sed -n 's/.*"entity_type":"album","decorations":{"identity":{"name":"\([^"]*\)".*/\1/p' | head -1)
  cover=$(printf '%s' "$now" | grep -oE 'https://i\.scdn\.co/image/[A-Za-z0-9]+' | sed -n '3p')
  dur=$(printf '%s' "$now" | sed -n 's/.*"duration_ms":\([0-9]*\).*/\1/p' | head -1)
  pos=$(printf '%s' "$now" | sed -n 's/.*"position_ms":\([0-9]*\).*/\1/p' | head -1)
  # What you actually hear is the ALSA "Master" softvol (Alice's volume); Soloist's
  # own "volume" is just the Spotify Connect level and differs from it.
  vol=$(amixer get Master 2>/dev/null | sed -n 's/.*Playback [0-9]* \[\([0-9]*\)%\].*/\1/p' | head -1)
  [ -n "$vol" ] || vol=$(printf '%s' "$now" | sed -n 's/.*"volume":\([0-9]*\).*/\1/p' | head -1)
  next=$(printf '%s\n' "$queue" | sed -n '1s/^[0-9. ]*\[queue\] //p')
  # nothing queued yet (the radio queues 10 s before the end): its next pick
  [ -n "$next" ] || next=$(/data/opt/spotify-connect/alice-radio.sh next-name 2>/dev/null || true)
  uri=$(printf '%s' "$now" | grep -oE 'spotify:track:[A-Za-z0-9]+' | head -1)

  # Record playback history LOCALLY the moment a track plays, from Soloist's own
  # title/artist (no Spotify API call -> works even when the app token is rate-limited).
  HISTDISP=$BASE/history-display.txt
  LASTF=$BASE/.lasttrack   # persistent (next to scripts), survives reboots
  # Same history with the exact track, for «N песен назад» (alice-history-back.sh):
  # "uri<TAB>title<TAB>artist", the last line is always the track playing now.
  HISTURI=$BASE/history-uris.txt
  # While alice-queue-drain.sh skips through the queue (flag = expiry epoch),
  # the tracks flashing by were never heard: keep them out of the history.
  [ "$(cat /tmp/soloist-draining 2>/dev/null || echo 0)" -gt "$(date +%s)" ] 2>/dev/null && uri=""
  if [ -n "$uri" ] && [ -n "$title" ] && [ "$uri" != "$(tail -n 1 "$HISTURI" 2>/dev/null | cut -f1)" ]; then
    printf '%s\t%s\t%s\n' "$uri" "$title" "$artist" >>"$HISTURI"
    tail -n 100 "$HISTURI" >"$HISTURI.tmp" 2>/dev/null && mv "$HISTURI.tmp" "$HISTURI"
  fi
  if [ -n "$uri" ] && [ -n "$title" ]; then
    if [ "$uri" != "$(cat "$LASTF" 2>/dev/null)" ]; then
      printf '%s\n' "$uri" >"$LASTF"
      if [ -n "$artist" ]; then line="$title — $artist"; else line="$title"; fi
      if [ "$line" != "$(tail -n 1 "$HISTDISP" 2>/dev/null)" ]; then
        printf '%s\n' "$line" >>"$HISTDISP"
        tail -n 100 "$HISTDISP" >"$HISTDISP.tmp" 2>/dev/null && mv "$HISTDISP.tmp" "$HISTDISP"
        sync   # flush to ubifs so an abrupt reboot can't lose the history
      fi
    fi
  fi

  # Fallback: if the local log is still nearly empty (fresh flash, nothing played
  # yet, or names missing), backfill from Spotify via resolve-history — but at most
  # once every ~15 min, so the app-token quota is not burned.  Local capture above
  # stays the primary source; this only kicks in when there is little to show.
  if [ "$(wc -l < "$HISTDISP" 2>/dev/null || echo 0)" -lt 3 ]; then
    FB=$BASE/.last-fallback
    now_s=$(date +%s 2>/dev/null || echo 0)
    last_s=$(cat "$FB" 2>/dev/null || echo 0)
    case "$last_s" in ''|*[!0-9]*) last_s=0;; esac
    if [ $((now_s - last_s)) -gt 900 ]; then
      printf '%s\n' "$now_s" >"$FB"
      nohup "$BASE/resolve-history.sh" >/tmp/spotify-history-resolve.log 2>&1 &
    fi
  fi

  hist=$(tail -30 "$HISTDISP" 2>/dev/null | awk 'NF && $0!=p {print} {p=$0}' | tail -10 | tr '\n' '|' )
  shist=$(tail -14 /data/opt/spotify-connect/receiver-state/web-api/radio-search-history.txt 2>/dev/null \
    | awk -F'\t' 'NF>=2{print $2" · "$1} NF<2{print $0}' | tr '\n' '|')
  esc(){ printf '%s' "$1" | sed 's/\\/\\\\/g;s/"/\\"/g'; }
  printf '{"status":"%s","title":"%s","artist":"%s","album":"%s","cover":"%s","duration_ms":%s,"position_ms":%s,"volume":%s,"next":"%s","history":"%s","search_history":"%s","soloist":"%s"}\n' \
    "$(esc "$status")" "$(esc "$title")" "$(esc "$artist")" "$(esc "$album")" "$(esc "$cover")" \
    "${dur:-0}" "${pos:-0}" "${vol:-0}" "$(esc "$next")" "$(esc "$hist")" "$(esc "$shist")" "$solo" >"$OUT.new"
  mv "$OUT.new" "$OUT"
  sleep 3
done
