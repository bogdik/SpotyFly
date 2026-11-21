#!/bin/sh
set -eu
NET='/data/opt/spotify-connect/receiver-state/web-api/net.conf'; [ -r "$NET" ] && . "$NET" 2>/dev/null || true
base=/data/opt/solo
# ARM64 compat emulation clears the exclusive monitor inside Soloist atomics.
# Use the kernel-supported hardware barriers instead of modifying Spotify code.
# Touch /proc/sys/abi ONCE per boot: kernel 4.9's handler swaps table->data for
# the duration of every read/write, so concurrent run.sh calls (pollers run it
# every few seconds) eventually corrupt it -- the mode then reads 0, writing 2
# fails with EINVAL and nothing plays until a reboot.
CP15_OK=/run/soloist-cp15.ok
if [ ! -e "$CP15_OK" ] && [ -f /proc/sys/abi/cp15_barrier ]; then
    if mkdir /run/soloist-cp15.lock 2>/dev/null; then
        if [ "$(cat /proc/sys/abi/cp15_barrier)" != 2 ] && ! echo 2 > /proc/sys/abi/cp15_barrier; then
            rmdir /run/soloist-cp15.lock
            echo 'Soloist needs hardware CP15 barriers on this ARM64 kernel' >&2
            exit 1
        fi
        : > "$CP15_OK"
        rmdir /run/soloist-cp15.lock
    else
        # Another run.sh is setting it right now: wait for it, don't touch /proc.
        i=0
        while [ ! -e "$CP15_OK" ] && [ "$i" -lt 50 ]; do usleep 100000 2>/dev/null || sleep 1; i=$((i + 1)); done
    fi
fi
export LD_LIBRARY_PATH="$base/lib:/system/vendor/quasar/libs:/usr/lib:/lib"
# Kernel 4.9 lacks the time64 syscalls: glibc 2.38 would try clock_gettime64
# (403) and fail on every clock read.  The time shim goes straight to the
# 32-bit syscall (source: src/time64-shim.c).
pre="$base/lib/libspotiya-time-shim.so"; [ -r "$base/lib/libspotiya-math-shim.so" ] && pre="$base/lib/libspotiya-math-shim.so:$pre"
export LD_PRELOAD="$pre${LD_PRELOAD:+:$LD_PRELOAD}"
# rseq (398) is also missing on 4.9; stop glibc registering it for every thread.
export GLIBC_TUNABLES="glibc.pthread.rseq=0${GLIBC_TUNABLES:+:$GLIBC_TUNABLES}"
# Spotify gets its own softvol ("Spotify"), not the shared "Master" that carries
# Alice's voice: alice-voice-proxy keeps it at Alice's volume, ducks it during
# her dialogs and can silence only her.  Without the config: the old path.
if [ -r "$base/asound-spotify.conf" ]; then
    export ALSA_CONFIG_PATH="/usr/share/alsa/alsa.conf:$base/asound-spotify.conf"
    export APULSE_PLAYBACK_DEVICE="${APULSE_PLAYBACK_DEVICE:-plug:spotify}"
else
    export APULSE_PLAYBACK_DEVICE="${APULSE_PLAYBACK_DEVICE:-plug:default}"
fi
export SPOTIYA_SOLOIST_HARDWARE_VOLUME=1
export MALLOC_ARENA_MAX=1
# The compatibility runtime needs the Station's system trust store explicitly.
export CURL_CA_BUNDLE="$base/ca-certificates.crt"
export SSL_CERT_FILE="$base/ca-certificates.crt"
# Route Spotify HTTPS through the user-provided SOCKS5 proxy. Local IPC remains direct.
export ALL_PROXY="${PROXY:-}"
export NO_PROXY="127.0.0.1,localhost"
exec "$base/soloist" "$@"
