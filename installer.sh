#!/bin/sh
# installer.sh — ставит SpotyFly на колонку по adb (запускать на ПК).
#
#   ./installer.sh [-s IP[:порт]] [--dry-run] [--yes] [--force] [--update-soloist] [--no-reboot]
#
#   -s IP[:порт]      колонка (adb connect делается сам); без него — единственное
#                     устройство из `adb devices`
#   --dry-run         ничего не менять на колонке, только показать, что будет сделано
#   --yes             не спрашивать подтверждения
#   --force           ставить на модель, на которой проект не проверялся
#   --update-soloist  заново скачать Soloist и рантайм, даже если они уже стоят
#   --no-reboot       не перезагружать колонку в конце
#
# Что делает: кладёт наш стек в /data/opt/spotify-connect, Soloist (скачивается
# у Spotify) и его рантайм — в /data/opt/solo, автозапуск — в /etc/init.d, и
# правит в quasar.cfg шесть значений (родной музыкальный плеер играет тишину,
# медиасервис ходит через локальный релей). Перед правкой всё, что меняется,
# копируется на ПК в backups/. Повторный запуск = обновление: настройки, ключи,
# история и свои голосовые команды сохраняются.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
HELPER="python3 -I $HERE/tools/spotyfly-helper.py"
D=/data/opt/spotify-connect
SOLO=/data/opt/solo
CFG=/system/vendor/quasar/quasar.cfg
TESTED="plum"                     # deviceType моделей, где проект проверен (armv7, ядро 4.9)
VERSION=$(cat "$HERE/VERSION" 2>/dev/null || echo dev)

SERIAL=""; DRY=0; YES=0; FORCE=0; NEWSOLO=0; REBOOT=1
while [ $# -gt 0 ]; do
    case "$1" in
        -s) SERIAL=${2:?}; shift ;;
        --dry-run) DRY=1 ;;
        --yes) YES=1 ;;
        --force) FORCE=1 ;;
        --update-soloist) NEWSOLO=1 ;;
        --no-reboot) REBOOT=0 ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "неизвестный параметр: $1" >&2; exit 2 ;;
    esac
    shift
done

say() { printf '%s\n' "$*"; }
die() { printf 'ОШИБКА: %s\n' "$*" >&2; exit 1; }
command -v adb >/dev/null || die "нужен adb (Android platform-tools)"
command -v python3 >/dev/null || die "нужен python3"

# --- колонка -----------------------------------------------------------------
if [ -n "$SERIAL" ]; then
    case "$SERIAL" in *:*) ;; *) SERIAL="$SERIAL:5555" ;; esac
    adb connect "$SERIAL" >/dev/null 2>&1 || true
else
    SERIAL=$(adb devices | awk 'NR>1 && $2=="device" {print $1}')
    [ "$(printf '%s\n' "$SERIAL" | grep -c .)" = 1 ] || die "укажите колонку: -s IP (устройств: $(printf '%s\n' "$SERIAL" | grep -c .))"
fi
A="adb -s $SERIAL"
# dev CMD — выполнить на колонке; код возврата передаётся и со старым adbd
dev() {
    out=$($A shell "$1; echo __rc=\$?" 2>&1 | tr -d '\r') || die "нет связи с колонкой $SERIAL"
    rc=$(printf '%s\n' "$out" | sed -n 's/^__rc=//p' | tail -1)
    printf '%s\n' "$out" | sed '/^__rc=/d'
    return "${rc:-1}"
}
# act CMD — изменение на колонке (в --dry-run только печатается)
act() { if [ "$DRY" = 1 ]; then say "  [dry-run] $1"; else dev "$1" >/dev/null || die "не удалось: $1"; fi; }
push() { # local remote — через временное имя и mv: работающий бинарь не ломается
    if [ "$DRY" = 1 ]; then say "  [dry-run] push $(basename "$1") -> $2"; return 0; fi
    $A push "$1" "$2.new" >/dev/null 2>&1 || die "adb push $1"
    dev "mv -f '$2.new' '$2'" >/dev/null || die "mv $2"
}

[ "$(dev 'id -u' | tail -1)" = 0 ] || die "нужен root-доступ по adb"
get() { rm -f "$2"; $A pull "$1" "$2" >/dev/null 2>&1 || rm -f "$2"; }   # remote local (binary-safe; the Station's adbd cannot stream raw output)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/spotyfly-install.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
get "$CFG" "$WORK/quasar.cfg"
[ -s "$WORK/quasar.cfg" ] || die "не читается $CFG — это та колонка?"
info=$($HELPER cfg-info "$WORK/quasar.cfg") || die "quasar.cfg не разобрать"
model=$(printf '%s' "$info" | sed -n 's/^deviceType=\([^ ]*\).*/\1/p')
cfgstate=$(printf '%s' "$info" | sed -n 's/.*patch=//p')
arch=$(dev 'od -An -tx1 -N20 /bin/busybox' | $HELPER elf -)
kernel=$(dev 'uname -m' | tail -1)
say "Колонка $SERIAL: модель $model, userland $arch, ядро $kernel, quasar.cfg: $cfgstate"
case "$arch" in armv7|aarch64) ;; *) die "архитектура $arch не поддерживается (нужна ARM)";; esac
case " $TESTED " in *" $model "*) ;; *)
    [ "$FORCE" = 1 ] || die "на модели «$model» проект не проверялся; если готовы рискнуть — --force"
    say "ВНИМАНИЕ: модель «$model» не проверялась (--force)" ;;
esac
[ "$cfgstate" != unknown-layout ] || die "в quasar.cfg нет нужных ключей — прошивка другая, правка небезопасна"
dev '[ -x /opt/bin/curl ]' >/dev/null || die "нет /opt/bin/curl — поставьте Entware и в нём curl (opkg install curl)"
dev '[ -x /usr/bin/socat ]' >/dev/null || say "ВНИМАНИЕ: нет /usr/bin/socat — не будет https-страницы для входа в Spotify"
dev "[ -e $D/bin/spotiya ] || [ -e $D/spotiya ]" >/dev/null && \
    die "стоит spotiya (тот же каталог и автозапуск) — сначала удалите её"

# --- что ставим: бинари под архитектуру колонки -----------------------------------
# пакет: bin/ (+ bin/ARCH); из исходников: out/<arch>/ + out/common/ (build.sh)
BIN=""; COM=""
if [ -x "$HERE/bin/alice-voice-proxy" ] && [ "$(cat "$HERE/bin/ARCH" 2>/dev/null)" = "$arch" ]; then
    BIN=$HERE/bin; COM=$HERE/bin
elif [ -x "$HERE/out/$arch/alice-voice-proxy" ] && [ -e "$HERE/out/common/silence.wav" ]; then
    BIN=$HERE/out/$arch; COM=$HERE/out/common
else
    have=$(cat "$HERE/bin/ARCH" 2>/dev/null || echo "нет готовых")
    zig=${ZIG:-$(command -v zig 2>/dev/null || true)}
    if [ -n "$zig" ] && [ -x "$zig" ] && [ -d "$HERE/src" ]; then
        say "В пакете бинари не под $arch ($have) — собираю из исходников ($zig)…"
        ZIG=$zig sh "$HERE/build.sh" "$arch" >"$WORK/build.log" 2>&1 || die "сборка не удалась: $(tail -5 "$WORK/build.log")"
        BIN=$HERE/out/$arch; COM=$HERE/out/common
    else
        die "колонке нужны бинари $arch, а в этом пакете — $have. Скачайте spotyfly-$VERSION-$arch.tar.gz или поставьте zig (https://ziglang.org) и запустите снова"
    fi
fi
for f in alice-voice-proxy monitor-server quasar-ws-relay; do [ -x "$BIN/$f" ] || die "в $BIN нет $f"; done
for f in libspotiya-time-shim.so silence.wav; do [ -e "$COM/$f" ] || die "в $COM нет $f"; done
need_solo=$NEWSOLO
dev "[ -x $SOLO/soloist ] && [ -x $SOLO/ld.so ] && [ -e $SOLO/lib/libc.so.6 ]" >/dev/null || need_solo=1
if [ "$need_solo" = 1 ]; then
    say "Скачиваю Soloist (у Spotify) и рантайм совместимости (spotiya v0.10.1)…"
    mkdir -p "$WORK/solo"
    $HELPER soloist "$WORK/solo" && $HELPER compat "$WORK/solo" || die "загрузка не удалась"
fi
free=$(dev "df -k /data | tail -1" | awk '{print $4}')
want=$(du -sk "$BIN" $([ "$COM" = "$BIN" ] || echo "$COM") "$HERE/station" | awk '{s += $1} END {print s}')
[ "$need_solo" = 1 ] && want=$((want + $(du -sk "$WORK/solo" | cut -f1)))
[ "${free:-0}" -gt $((want + 2048)) ] || die "мало места в /data: свободно ${free} КБ, нужно ~$((want + 2048)) КБ"

# --- бэкап на ПК ----------------------------------------------------------------
BK="$HERE/backups/$(printf '%s' "$SERIAL" | tr -c 'A-Za-z0-9.-' _)-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$BK"; chmod 700 "$BK"
cp "$WORK/quasar.cfg" "$BK/quasar.cfg"
for f in /etc/init.d/S99spotify-connect /etc/init.d/S98zz-wsfake "$D/voice-commands.json" "$D/quasar-orig.json" "$D/.installed"; do
    get "$f" "$BK/$(basename "$f")"
done
cp "$BK/quasar-orig.json" "$WORK/orig.json" 2>/dev/null || echo '{}' >"$WORK/orig.json"
$HELPER cfg-patch "$WORK/quasar.cfg" "$WORK/quasar.new" "$WORK/orig.json" >"$WORK/patch.log" || die "правка quasar.cfg: $(cat "$WORK/patch.log")"
known=$(sed -n 's/.*(\([0-9]*\) originals known).*/\1/p' "$WORK/patch.log")

say ""
say "Будет сделано (версия $VERSION):"
say "  • остановлен наш стек (если стоит), файлы — в $D и $SOLO"
[ "$need_solo" = 1 ] && say "  • Soloist и рантайм — свежие, с серверов Spotify / релиза spotiya"
[ "$cfgstate" = patched ] || say "  • quasar.cfg: 6 значений (родной плеер → тишина, медиасервис → релей)"
say "  • автозапуск: /etc/init.d/S99spotify-connect, S98zz-wsfake"
[ "$REBOOT" = 1 ] && say "  • перезагрузка колонки"
say "  Бэкап того, что меняется: $BK (в quasar.cfg секреты устройства — не выкладывайте)"
[ "${known:-0}" = 6 ] || say "  ВНИМАНИЕ: исходные значения quasar.cfg известны не все ($known из 6) — удалятору понадобится бэкап"
if [ "$DRY" = 0 ] && [ "$YES" = 0 ]; then
    printf 'Продолжить? [y/N] '; read -r ok
    case "$ok" in y|Y|д|Д|yes|да) ;; *) say "Отменено."; exit 1 ;; esac
fi

# --- остановка и файлы --------------------------------------------------------------
say "Останавливаю стек…"
if [ "$DRY" = 0 ]; then
    $A push "$HERE/station/stop.sh" /data/local/tmp/spotyfly-stop.sh >/dev/null
    dev "sh /data/local/tmp/spotyfly-stop.sh; rm -f /data/local/tmp/spotyfly-stop.sh" >/dev/null || true
else say "  [dry-run] stop.sh"; fi

say "Копирую файлы…"
act "mkdir -p $D/monitor $D/receiver-state/web-api $D/receiver-state/soloist/data $SOLO/lib && chmod 700 $D/receiver-state"
# the Spotify Connect name: keep the one a previous install used
if ! dev "[ -s $D/receiver-state/device-name ]" >/dev/null; then
    name=$(dev "sed -n 's/.*--device-name \\([^ \"\$]*\\) .*/\\1/p' $D/soloist-daemon.sh 2>/dev/null" | head -1)
    act "echo '${name:-SpotyFly}' >$D/receiver-state/device-name"
fi
: >"$WORK/manifest"
put() { # local remote mode
    push "$1" "$2"; act "chmod $3 '$2'"
    case "$2" in "$D/"*) printf '%s\n' "${2#$D/}" >>"$WORK/manifest" ;; *) printf '%s\n' "$2" >>"$WORK/manifest" ;; esac
}
for f in "$HERE"/station/*.sh "$HERE"/station/monitor/*.sh; do
    rel=${f#$HERE/station/}; put "$f" "$D/$rel" 755
done
put "$HERE/station/voice-commands.default.json" "$D/voice-commands.default.json" 644
for f in alice-voice-proxy quasar-ws-relay; do put "$BIN/$f" "$D/$f" 755; done
put "$BIN/monitor-server" "$D/monitor/monitor-server" 755
put "$COM/silence.wav" "$D/silence.wav" 644
dev "[ -e $D/voice-commands.json ]" >/dev/null || act "cp $D/voice-commands.default.json $D/voice-commands.json"
for f in run.sh set-key.sh; do put "$HERE/solo/$f" "$SOLO/$f" 755; done
put "$HERE/solo/asound-spotify.conf" "$SOLO/asound-spotify.conf" 644
put "$COM/libspotiya-time-shim.so" "$SOLO/lib/libspotiya-time-shim.so" 755
if [ "$need_solo" = 1 ]; then
    (cd "$WORK/solo" && find . -type f | sed 's#^\./##') | while read -r f; do
        case "$f" in */*) act "mkdir -p '$SOLO/${f%/*}'" ;; esac
        mode=644; case "$f" in soloist|ld.so|lib/*) mode=755 ;; esac
        put "$WORK/solo/$f" "$SOLO/$f" "$mode"
    done
fi
# https для входа в Spotify: самоподписанный сертификат, делается один раз
if ! dev "[ -s $D/monitor/tls.pem ]" >/dev/null; then
    if command -v openssl >/dev/null; then
        openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -subj /CN=spotyfly \
            -keyout "$WORK/k" -out "$WORK/c" >/dev/null 2>&1 && cat "$WORK/k" "$WORK/c" >"$WORK/tls.pem"
        push "$WORK/tls.pem" "$D/monitor/tls.pem"; act "chmod 600 $D/monitor/tls.pem"
    else
        say "ВНИМАНИЕ: нет openssl — https-страница входа в Spotify не заработает (см. README)"
    fi
fi
# файлы прошлой версии, которых больше нет в пакете
if [ -s "$BK/.installed" ] && [ "$DRY" = 0 ]; then
    sed -n 's/^file //p' "$BK/.installed" | while read -r old; do
        grep -qxF "$old" "$WORK/manifest" && continue
        case "$old" in /*) p=$old ;; *) p=$D/$old ;; esac
        case "$p" in "$D"/*|"$SOLO"/*) dev "rm -f '$p'" >/dev/null || true ;; esac
    done
fi
dev "$D/alice-voice-proxy --check $D/voice-commands.default.json" >/dev/null 2>&1 || \
    [ "$DRY" = 1 ] || die "alice-voice-proxy не запускается на этой колонке"

# --- система: init.d и quasar.cfg (корень read-only) ---------------------------------
sys_changes=0
for f in S99spotify-connect S98zz-wsfake; do
    cmp -s "$HERE/init.d/$f" "$BK/$f" 2>/dev/null || sys_changes=1
done
cmp -s "$WORK/quasar.cfg" "$WORK/quasar.new" || [ "$cfgstate" = patched ] || sys_changes=1
if [ "$sys_changes" = 1 ]; then
    say "Правлю системные файлы (remount rw → ro)…"
    for f in S99spotify-connect S98zz-wsfake; do push "$HERE/init.d/$f" "/data/local/tmp/spotyfly-$f"; done
    [ "$cfgstate" = patched ] || push "$WORK/quasar.new" /data/local/tmp/spotyfly-quasar.cfg
    # every step is checked; / goes back to read-only whatever happens
    act "mount -o remount,rw / || exit 1; ok=1; \
for f in S99spotify-connect S98zz-wsfake; do cp /data/local/tmp/spotyfly-\$f /etc/init.d/\$f && chmod 755 /etc/init.d/\$f || ok=0; done; \
if [ -s /data/local/tmp/spotyfly-quasar.cfg ]; then cat /data/local/tmp/spotyfly-quasar.cfg >$CFG || ok=0; fi; \
sync; mount -o remount,ro /; rm -f /data/local/tmp/spotyfly-S9* /data/local/tmp/spotyfly-quasar.cfg; [ \$ok = 1 ]"
fi
push "$WORK/orig.json" "$D/quasar-orig.json"; act "chmod 600 $D/quasar-orig.json"
if [ "$DRY" = 0 ]; then
    { echo "version $VERSION"; echo "date $(date +%Y-%m-%dT%H:%M:%S)"; [ "$need_solo" = 1 ] && echo "solo ours"
      [ -s "$BK/.installed" ] && grep -qx 'solo ours' "$BK/.installed" && [ "$need_solo" = 0 ] && echo "solo ours"
      sed 's/^/file /' "$WORK/manifest"; } | awk '!s[$0]++' >"$WORK/.installed"
    push "$WORK/.installed" "$D/.installed"
    dev sync >/dev/null
fi

say ""
if dev "[ -s $D/receiver-state/web-api/credentials.env ] && [ -s $D/receiver-state/soloist/api-key ]" >/dev/null; then
    say "Готово. Ключи и вход в Spotify сохранены — после загрузки всё заиграет как раньше."
else
    say "Готово. Дальше:"
    say "  1. откройте http://<IP колонки>:28088 → ⚙: Client ID/Secret приложения Spotify, ключ Soloist"
    say "  2. «Авторизоваться» в Spotify (Redirect URI в приложении: https://<IP колонки>:8443/callback)"
    say "  3. «включи группу Кино»"
fi
if [ "$REBOOT" = 1 ]; then
    if [ "$DRY" = 1 ]; then say "  [dry-run] reboot"; exit 0; fi
    say "Перезагружаю колонку (около минуты)…"
    $A shell 'sync && reboot' >/dev/null 2>&1 || true
fi
