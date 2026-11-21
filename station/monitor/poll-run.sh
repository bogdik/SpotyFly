#!/bin/sh
# Keep the status poller alive across any crash so the monitor page never
# freezes on a stale track.
while :; do
  /data/opt/spotify-connect/monitor/poll-status.sh
  sleep 2
done
