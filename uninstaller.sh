#!/bin/sh
# uninstaller.sh — убирает spotyfly с колонки и возвращает её как было (запускать на ПК).
#
#   ./uninstaller.sh [-s IP[:порт]] [--dry-run] [--yes] [--keep-data] [--cfg-backup ФАЙЛ] [--no-reboot]
#
#   --keep-data        оставить receiver-state (ключи, вход в Spotify, история, чёрный
#                      список) — для переустановки
#   --cfg-backup ФАЙЛ  вернуть quasar.cfg целиком из бэкапа установщика
#                      (backups/…/quasar.cfg); без него возвращаются только шесть
#                      значений, которые менял установщик
#
# Порядок: стек и релей останавливаются, quasar.cfg и /etc/init.d возвращаются,
# каталоги проекта удаляются, колонка перезагружается. Удаление необратимо:
# без --keep-data ключи и вход в Spotify придётся вводить заново.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
HELPER="python3 -I $HERE/tools/spotyfly-helper.py"
D=/data/opt/spotify-connect
SOLO=/data/opt/solo
CFG=/system/vendor/quasar/quasar.cfg

SERIAL=""; DRY=0; YES=0; KEEP=0; CFGBK=""; REBOOT=1
while [ $# -gt 0 ]; do
    case "$1" in
        -s) SERIAL=${2:?}; shift ;;
        --dry-run) DRY=1 ;;
        --yes) YES=1 ;;
        --keep-data) KEEP=1 ;;
        --cfg-backup) CFGBK=${2:?}; shift ;;
        --no-reboot) REBOOT=0 ;;
        -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
        *) echo "неизвестный параметр: $1" >&2; exit 2 ;;
    esac
    shift
done

say() { printf '%s\n' "$*"; }
die() { printf 'ОШИБКА: %s\n' "$*" >&2; exit 1; }
command -v adb >/dev/null || die "нужен adb"
command -v python3 >/dev/null || die "нужен python3"
if [ -n "$SERIAL" ]; then
    case "$SERIAL" in *:*) ;; *) SERIAL="$SERIAL:5555" ;; esac
    adb connect "$SERIAL" >/dev/null 2>&1 || true
else
    SERIAL=$(adb devices | awk 'NR>1 && $2=="device" {print $1}')
    [ "$(printf '%s\n' "$SERIAL" | grep -c .)" = 1 ] || die "укажите колонку: -s IP"
fi
A="adb -s $SERIAL"
dev() {
    out=$($A shell "$1; echo __rc=\$?" 2>&1 | tr -d '\r') || die "нет связи с колонкой $SERIAL"
    rc=$(printf '%s\n' "$out" | sed -n 's/^__rc=//p' | tail -1)
    printf '%s\n' "$out" | sed '/^__rc=/d'
    return "${rc:-1}"
}
act() { if [ "$DRY" = 1 ]; then say "  [dry-run] $1"; else dev "$1" >/dev/null || die "не удалось: $1"; fi; }

[ "$(dev 'id -u' | tail -1)" = 0 ] || die "нужен root-доступ по adb"
get() { rm -f "$2"; $A pull "$1" "$2" >/dev/null 2>&1 || rm -f "$2"; }   # remote local (binary-safe; the Station's adbd cannot stream raw output)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/spotyfly-uninstall.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
get "$CFG" "$WORK/quasar.cfg"
[ -s "$WORK/quasar.cfg" ] || die "не читается $CFG"
get "$D/quasar-orig.json" "$WORK/orig.json"
get "$D/.installed" "$WORK/installed"
[ -s "$WORK/installed" ] || say "ВНИМАНИЕ: нет $D/.installed — ставили не этим установщиком; убираю то, что знаю"

# quasar.cfg: из бэкапа целиком или шесть значений обратно
if [ -n "$CFGBK" ]; then
    [ -s "$CFGBK" ] || die "нет файла $CFGBK"
    $HELPER cfg-info "$CFGBK" | grep -q 'patch=stock' || die "$CFGBK — не исходный quasar.cfg (в нём наша правка)"
    cp "$CFGBK" "$WORK/quasar.new"
else
    [ -s "$WORK/orig.json" ] || echo '{}' >"$WORK/orig.json"
    $HELPER cfg-unpatch "$WORK/quasar.cfg" "$WORK/quasar.new" "$WORK/orig.json" >"$WORK/unpatch.log"
    if grep -q 'still ours' "$WORK/unpatch.log"; then
        cat "$WORK/unpatch.log"
        die "исходные значения quasar.cfg неизвестны — укажите --cfg-backup backups/…/quasar.cfg"
    fi
fi
before=$($HELPER cfg-info "$WORK/quasar.cfg" | sed 's/.*patch=//')

solo_ours=0
grep -qx 'solo ours' "$WORK/installed" 2>/dev/null && solo_ours=1
say "Будет сделано на $SERIAL:"
say "  • остановлены наш стек и релей медиасервиса"
[ "$before" = stock ] || say "  • quasar.cfg возвращается к исходному (родной плеер и медиасервис — как было)"
say "  • удаляются /etc/init.d/S99spotify-connect и S98zz-wsfake"
if [ "$KEEP" = 1 ]; then say "  • удаляется $D, кроме receiver-state (ключи и вход сохраняются)"
else say "  • удаляется $D целиком — вместе с ключами и входом в Spotify"; fi
[ "$solo_ours" = 1 ] && say "  • удаляется $SOLO (Soloist ставил установщик)"
[ "$solo_ours" = 1 ] || say "  • $SOLO не трогаю (не нашим установщиком поставлен)"
[ "$REBOOT" = 1 ] && say "  • перезагрузка"
if [ "$DRY" = 0 ] && [ "$YES" = 0 ]; then
    printf 'Удалить? Это необратимо. [y/N] '; read -r ok
    case "$ok" in y|Y|д|Д|yes|да) ;; *) say "Отменено."; exit 1 ;; esac
fi

say "Останавливаю…"
if [ "$DRY" = 0 ]; then
    $A push "$HERE/station/stop.sh" /data/local/tmp/spotyfly-stop.sh >/dev/null
    dev "sh /data/local/tmp/spotyfly-stop.sh --all; rm -f /data/local/tmp/spotyfly-stop.sh" >/dev/null || true
else say "  [dry-run] stop.sh --all"; fi

say "Возвращаю системные файлы…"
if [ "$before" != stock ]; then
    if [ "$DRY" = 1 ]; then say "  [dry-run] push quasar.cfg"
    else $A push "$WORK/quasar.new" /data/local/tmp/spotyfly-quasar.cfg >/dev/null || die "adb push"; fi
fi
# our init scripts only (they name our directory); / back to read-only whatever happens
act "mount -o remount,rw / || exit 1; ok=1; \
for f in /etc/init.d/S99spotify-connect /etc/init.d/S98zz-wsfake; do \
  if [ -e \$f ] && grep -q /data/opt/spotify-connect \$f; then rm -f \$f || ok=0; fi; done; \
if [ -s /data/local/tmp/spotyfly-quasar.cfg ]; then cat /data/local/tmp/spotyfly-quasar.cfg >$CFG || ok=0; fi; \
sync; mount -o remount,ro /; rm -f /data/local/tmp/spotyfly-quasar.cfg; [ \$ok = 1 ]"

say "Удаляю файлы…"
if [ "$KEEP" = 1 ]; then
    act "cd $D && for f in * .installed quasar-orig.json; do [ \"\$f\" = receiver-state ] || rm -rf \"\$f\"; done"
else
    act "rm -rf $D"
fi
[ "$solo_ours" = 1 ] && act "rm -rf $SOLO"
act sync

if [ "$REBOOT" = 1 ]; then
    if [ "$DRY" = 1 ]; then say "  [dry-run] reboot"; exit 0; fi
    say "Перезагружаю колонку…"
    $A shell 'sync && reboot' >/dev/null 2>&1 || true
fi
say "Готово."
