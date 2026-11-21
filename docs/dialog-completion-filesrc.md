# Завершение диалога без «висит» — локальная тишина через filesrc

## Проблема

После перехвата музыкального запроса («включи X» → наш Spotify) диалог Алисы
**зависал** (`activity=3`), и **колонка переставала принимать любые команды**,
пока диалог не закроется. Раньше это «лечили» убийством `maind --platformd`, но
повторные киллы доводят колонку до **factory reset**.
Так делать НЕЛЬЗЯ.

## Почему висело (разобрано по декомпиляции maind + живому AliceState)

platformd на музыкальный запрос формирует директиву `audio_play` (client_action,
sub_name `music`) с коллбэками `on_started` / `on_stopped` / `on_finished` /
`on_failed`. platformd отдаёт её **локальному плееру** и ждёт `on_started`
(«музыка пошла») → только тогда закрывает диалог.

Старая заглушка `audiotestsrc wave=silence ! fakesink` **подменяла весь
пайплайн**: плеер «играл вечную тишину» и НЕ проходил штатный цикл «загрузил трек
→ playing», поэтому `on_started`/`on_failed` не отправлялись → platformd висел.

## Решение

Заглушка, которая проигрывает **локальный тихий файл** (не URL облака!):

```
filesrc location=/data/opt/spotify-connect/silence.wav ! decodebin ! \
  audioconvert ! audioresample ! fakesink sync=true
```

во всех 5 музыкальных пайплайнах `quasar.cfg` (TRACK-типы и аудиоклиент mediad —
см. `quasar-cfg-mute.md`). Теперь плеер штатно «загружает и играет» локальный WAV
→ шлёт `on_started` → platformd закрывает диалог → **Алиса остаётся отзывчивой**.
Звука нет (`fakesink`), родная музыка прошивки молчит, **в облако за музыкой ничего
не уходит** (источник — локальный файл).

`silence.wav`: 8 kHz, mono, 16-bit PCM, 60 секунд (≈938 KB), лежит в
`/data/opt/spotify-connect/`.

## Про EOS (проверено — не проблема)

Опасение: за 60 секунд файл доиграет (EOS) → нативный сценарий запросит next
(`@@mm_stack_engine_get_next` = server_action в облако). **Проверено замером 120 с
при играющей песне (две EOS-границы):** новых директив нет, strm/музыка-соединений
к облаку = 0. Нативный тихий плеер НЕ уходит в EOS→next (диалог уже закрыт,
сценарий не перезапрашивает). 60-сек файла достаточно; длинный файл/loop не нужен.

## Установка на чистой колонке

```bash
adb push out/silence.wav /data/opt/spotify-connect/
# пайплайн в /system/vendor/quasar/quasar.cfg (корень RO -> remount):
adb shell 'mount -o remount,rw /
  sed -i "s#audiotestsrc wave=silence is-live=true ! fakesink sync=true#filesrc location=/data/opt/spotify-connect/silence.wav ! decodebin ! audioconvert ! audioresample ! fakesink sync=true#g" /system/vendor/quasar/quasar.cfg
  sync; mount -o remount,ro /'
# применяется РЕБУТОМ (НЕ kill platformd!)
```

Перед правкой сохраните копию исходного `quasar.cfg` у себя на компьютере.
