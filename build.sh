#!/bin/sh
# Builds what the speaker needs, and with `pack` the release packages:
#   ZIG=/path/to/zig ./build.sh [ARCH…]          -> out/<arch>/, out/common/
#   ZIG=/path/to/zig ./build.sh pack [ARCH…]     -> dist/spotyfly-<version>-<arch>.tar.gz + SHA256SUMS
# ARCH: armv7 (32-bit userland — every speaker tested so far), aarch64; both by
# default.  zig 0.13+ and python3, any host OS.
#
# Our programs are static and per architecture: out/<arch>/.  Two files are
# the same for every speaker (out/common/): the time shim, which is loaded into
# Soloist — and Soloist for ARM exists only as the official arm32 build — and
# the silence the stock player "plays".  A package carries its architecture's
# binaries in bin/ (and bin/ARCH); installer.sh builds from src/ only when the
# speaker needs another architecture and zig is at hand.
#
# armv7 targets the plain ARMv7-A + VFPv3 baseline (not a newer core): it runs
# on real ARMv7 CPUs and on ARMv8 ones in 32-bit mode alike.  Binaries are stripped
# (-s): no debug info, so no paths of the build machine inside.
set -eu
cd "$(dirname "$0")"
ZIG=${ZIG:-zig}
VERSION=$(cat VERSION)
PACK=0
[ "${1:-}" = pack ] && { PACK=1; shift; }
ARCHES=${*:-armv7 aarch64}

target() {
    case "$1" in
        armv7) echo "arm-linux-musleabihf -mcpu=generic+v7a+vfp3d16" ;;
        aarch64) echo "aarch64-linux-musl -mcpu=generic" ;;
        *) echo "unknown architecture: $1 (armv7, aarch64)" >&2; exit 2 ;;
    esac
}

for a in $ARCHES; do
    set -- $(target "$a")
    T=$1; shift
    O=out/$a
    mkdir -p "$O"
    "$ZIG" cc -target "$T" "$@" -O2 -static -s -Wall -o "$O/alice-voice-proxy" src/alice-voice-proxy.c -lpthread
    "$ZIG" cc -target "$T" "$@" -O2 -static -s -Wall -o "$O/monitor-server" src/monitor-server.c
    "$ZIG" cc -target "$T" "$@" -O2 -static -s -Wall -o "$O/quasar-ws-relay" src/quasar-ws-relay.c
    echo "$a" >"$O/ARCH"
    echo "built $O: $(ls "$O" | tr '\n' ' ')"
done

# LD_PRELOAD for Soloist (arm32 glibc, not musl).  The version script is
# mandatory: without `local: *` the library would export zig's compiler-rt.
mkdir -p out/common
"$ZIG" cc -target arm-linux-gnueabihf -mcpu=generic+v7a+vfp3d16 -marm \
    -O2 -fPIC -fno-stack-protector -fomit-frame-pointer -ffunction-sections \
    -fdata-sections -shared -nostdlib -s -Wl,--gc-sections \
    -Wl,--version-script=src/time64-shim.map \
    -Wl,-soname,libspotiya-time-shim.so -o out/common/libspotiya-time-shim.so \
    src/time64-shim.c
# 60 s of silence, 8 kHz mono 16-bit: what the stock music player "plays"
# instead of music (docs/dialog-completion-filesrc.md).
python3 - out/common/silence.wav <<'EOF'
import sys, wave
with wave.open(sys.argv[1], "wb") as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(8000)
    w.writeframes(b"\0\0" * 8000 * 60)
EOF
echo "built out/common: $(ls out/common | tr '\n' ' ')"

[ "$PACK" = 1 ] || exit 0
mkdir -p dist
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
for a in $ARCHES; do
    NAME=spotyfly-$VERSION-$a
    mkdir -p "$STAGE/$NAME/bin"
    cp -R installer.sh uninstaller.sh build.sh tools station solo init.d docs src README.md \
          LICENSE THIRD_PARTY VERSION "$STAGE/$NAME/"
    [ ! -e CHANGELOG.md ] || cp CHANGELOG.md "$STAGE/$NAME/"
    cp out/"$a"/* out/common/* "$STAGE/$NAME/bin/"
    find "$STAGE/$NAME" -name '__pycache__' -prune -exec rm -rf {} +
    tar -C "$STAGE" --owner=0 --group=0 --sort=name --mtime='2026-01-01 00:00Z' \
        -czf "dist/$NAME.tar.gz" "$NAME"
done
(cd dist && sha256sum spotyfly-"$VERSION"-*.tar.gz >SHA256SUMS && cat SHA256SUMS)
