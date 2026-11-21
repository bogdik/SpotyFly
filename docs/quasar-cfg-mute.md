# Заглушка родного музыкального плеера в `quasar.cfg`

Это **ключевой механизм** текущего рабочего состояния: родная музыка прошивки
немая, а голос Алисы и TTS — слышно. Делается не блокировкой DNS и не убийством
процессов, а на уровне **GStreamer-пайплайнов** в `/system/vendor/quasar/quasar.cfg`.

> Сам `quasar.cfg` в репозиторий не входит — в нём секреты устройства.
> Здесь только патч (номера строк ориентировочны для прошивки автора).

## Идея

В `quasar.cfg` playback-пайплайны делятся по типам источника:

- **Голос / TTS / звуки** — проигрываются типами `AUDIO_FILE`, `OPUS_STREAM`,
  `PCM_STREAM`, `PCM_STREAM_WITH_BUFFER`. Их оставляем **живыми** (реальный
  вывод в `alsasink device=default`).
- **Музыка** — проигрывается типами `RADIO`, `TRACK`, `TRACK:CACHE`,
  `TRACK:DBUF` (аудиоклиент приложения) и отдельным аудиоклиентом **mediad**.
  Их `gstPipeline` заменяем на тишину.

Пайплайн-заглушка:

```
audiotestsrc wave=silence is-live=true ! fakesink sync=true
```

Он корректно «проигрывает» (mediad думает, что всё ок, диалог не виснет), но
звука нет.

## Что именно заменить

В блоке `audioClient … "playbackParams"` (около строк 211–345 текущего cfg):

| Тип | Было (суть) | Стало |
|-----|-------------|-------|
| `RADIO` | `{SOUPHTTPSRC} … ! {SINK}` | `audiotestsrc wave=silence is-live=true ! fakesink sync=true` |
| `TRACK` | `{SOUPHTTPSRC} … ! {SINK}` | то же (тишина) |
| `TRACK:CACHE` | `{FILESRC} … ! {SINK}` | то же (тишина) |
| `TRACK:DBUF` | `{MEDIA_PIPELINE} … ! {SINK}` | то же (тишина) |

Оставить БЕЗ изменений (это голос — иначе Алиса онемеет):

| Тип | Пайплайн |
|-----|----------|
| `AUDIO_FILE` | `{FILESRC} ! {DECODEBIN} ! … ! {VOLUME_MASTER} ! {SINK}` |
| `OPUS_STREAM` | `appsrc … ! opusparse ! opusdec ! {VOLUME_MASTER} ! {OPT_SPECTRUM} ! {SINK}` |
| `PCM_STREAM` / `PCM_STREAM_WITH_BUFFER` | `{STREAM_PIPELINE}` |

Плюс отдельный аудиоклиент **mediad** (около строки 953, рядом с
`"apiUrl" : "ws://127.0.0.1:9/quasar/websocket"` и `"port" : 9900`):

```json
"playbackParams" : {
   "deviceName" : "default",
   "gstPipeline" : "audiotestsrc wave=silence is-live=true ! fakesink sync=true"
},
```

## Как редактировать (корень read-only!)

`/system/vendor/quasar/quasar.cfg` лежит на корневой ФС — только через remount:

```bash
mount -o remount,rw /
# ... правка quasar.cfg (заменить 5 пайплайнов на тишину) ...
sync
mount -o remount,ro /
```

После правки перезапустить стек Quasar (или ребут). Проверка: Алису слышно,
«включи музыку» → наш Spotify играет, родной плеер молчит.

## Примечания

- `apiUrl` mediad указывает на `ws://127.0.0.1:9/...` (порт relay). Relay в
  текущем рабочем состоянии НЕ запущен — это неважно, т.к. вывод всё равно
  заглушён на уровне пайплайна.
