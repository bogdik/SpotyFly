# Как не дать mediad упасть (чтобы Алиса отвечала на обычные вопросы)

## Проблема

Когда `maind --mediad` падает, Алиса перестаёт отвечать на **обычные** (не
музыкальные) вопросы — голос/диалог завязаны на живой mediad.

Почему mediad падает: его `apiUrl` в `quasar.cfg` уведён с облака на
локальный порт `:9`, чтобы он НЕ ходил в облачный музыкальный сервис:

```
"apiUrl" : "ws://127.0.0.1:9/quasar/websocket"
```

(Стоковое значение — облачный адрес `wss://…/quasar/websocket`.)

Если на `:9` никто не слушает → mediad ловит ECONNREFUSED → крэш-луп →
`quasar_launcher2` бросает его (`allowedRestartIntervalSeconds: 10`) → mediad
мёртв → Алиса молчит.

## Что НЕ сработало: чистый фейк

Пробовали «настоящий фейк» `quasar-ws-fake` (в `research/`): слушает `:9`, сам
отвечает на WS-handshake, никуда не форвардит. **mediad это не держит** — он
подключается, проходит handshake, но без реальной Quasar-сессии через пару минут
всё равно умирает, и launcher2 его бросает. Проверено ребутом: mediad падал.

> Вывод: mediad нужна настоящая сессия, а не тупик.

## Что работает: форвард-релей `:9 → 9875`

`quasar-ws-relay` (исходник `src/quasar-ws-relay.c`) — прозрачный TCP-форвард:
слушает `127.0.0.1:9` и пробрасывает байты на **локальный** Quasar `127.0.0.1:9875`
(platformd). mediad получает реальную WS-сессию с локальным Quasar:

- mediad жив и стабилен (реальная сессия, `ESTABLISHED` к 9875);
- если mediad всё же разово перезапустится — launcher2 сразу его поднимает, и он
  восстанавливается (т.к. `:9` не отказывает и сессия настоящая);
- **родная музыка при этом не играет**: пайплайн воспроизведения mediad в
  `quasar.cfg` заглушён тишиной (см. `quasar-cfg-mute.md`). Релей даёт только
  управляющий канал, не звук.

## Критично: порядок загрузки

`S99quasar` (и mediad) стартует **раньше** нашего `S99spotify-connect`. Если
поднимать релей из `start.sh`, он опоздает: mediad успеет словить ECONNREFUSED и
быть брошенным. Поэтому релей поднимается **ранним** init-скриптом:

- `system/etc/init.d/S98zz-wsfake` — сортируется до `S99quasar`, поднимает
  `quasar-ws-relay` с respawn-циклом (упал → перезапуск за 5с).

(Имя `S98zz-wsfake` историческое — внутри запускается форвард-релей.)

## Установка на чистой колонке

```bash
adb push out/quasar-ws-relay /data/opt/spotify-connect/
adb shell 'chmod 755 /data/opt/spotify-connect/quasar-ws-relay'

adb push init.d/S98zz-wsfake /data/local/tmp/S98
adb shell 'mount -o remount,rw / && cp /data/local/tmp/S98 /etc/init.d/S98zz-wsfake \
  && chmod 755 /etc/init.d/S98zz-wsfake && sync && mount -o remount,ro / \
  && rm /data/local/tmp/S98'
```

apiUrl mediad в `quasar.cfg` должен быть `ws://127.0.0.1:9/quasar/websocket`
(см. `quasar-cfg-mute.md`).

## Сборка (если менять исходник)

```bash
zig cc -target arm-linux-musleabihf -mcpu=cortex_a53 \
  -O2 -static -o quasar-ws-relay src/quasar-ws-relay.c
```

## Проверка

```bash
adb shell 'netstat -ln | grep "127.0.0.1:9 "'                 # релей слушает
adb shell 'netstat -tn | grep 127.0.0.1:9875 | grep -c ESTAB' # mediad имеет сессию (>0)
adb shell 'ps | grep "[m]aind --mediad"'                      # mediad жив
# голосом: задать Алисе НЕмузыкальный вопрос — должна ответить
```

Проверено ребутом 2026-11-09: релей встаёт до quasar, mediad стабильно жив
(est к 9875), разовый перезапуск само-восстанавливается, родная музыка немая,
Spotify играет, Алиса отвечает на обычные вопросы.

## Альтернатива в `research/`

- `quasar-ws-fake` + `src/quasar-ws-fake.c` — «чистый фейк», НЕ держит mediad
  (оставлен как отрицательный результат / основа, если кто-то реализует
  протокол-осознанный стаб).
