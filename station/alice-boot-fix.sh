#!/bin/sh
# Runs once per boot from start.sh.
# Clear stale search.lock (an interrupted search leaves it; then voice no-ops).
#
# This used to also kill maind --platformd 45 s after boot so the launcher
# respawned it "clean".  That was a workaround for the hanging music dialog,
# which the filesrc silence pipeline in quasar.cfg fixed without any kill, and
# killing platformd is risky (repeated kills end in a factory reset).  The
# reboots in the first ~45 s turned out to be a 5 V brown-out from loud audio
# during the busy boot, handled by alice-volume-guard.sh.  So no kill anymore.
set -u
SPOTIFY_DIR=/data/opt/spotify-connect
rm -rf "$SPOTIFY_DIR/receiver-state/web-api/search.lock" 2>/dev/null || true
exit 0
