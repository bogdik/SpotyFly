/* Alice voice bridge for Lite 2 / Plum: reads aliced's event stream (:9875) and
 * turns what the user and Alice say into Spotify actions for Soloist (search,
 * transport, like, blacklist, …; commands from voice-commands.json).  It used
 * to sit between aliced and spotiya.receiver (:9876); it is the client now.
 * Recognition is intentionally loose (accepts finals while Alice is SPEAKING).
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <regex.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sound/asound.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ALICE_PORT 9875
#ifndef PROXY_LOCK
#define PROXY_LOCK "/run/alice-voice-proxy.lock"
#endif
#define MAX_FRAME (256 * 1024)
/* The native service can commit a music start several seconds after ASR final. */

static volatile int running = 1;

static int read_full(int fd, void *buf, size_t n) {
  uint8_t *p = buf;
  while (n) {
    ssize_t r = read(fd, p, n);
    if (r == 0) return -1;
    if (r < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    p += r;
    n -= (size_t)r;
  }
  return 0;
}

static int write_full(int fd, const void *buf, size_t n) {
  const uint8_t *p = buf;
  while (n) {
    ssize_t w = write(fd, p, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    p += w;
    n -= (size_t)w;
  }
  return 0;
}

static int varint(const uint8_t *data, size_t len, size_t *off, uint64_t *out) {
  uint64_t value = 0;
  for (int shift = 0; shift < 70; shift += 7) {
    if (*off >= len) return -1;
    uint8_t byte = data[(*off)++];
    if (shift == 63 && byte > 1) return -1;
    value |= (uint64_t)(byte & 127) << shift;
    if (byte < 128) {
      *out = value;
      return 0;
    }
  }
  return -1;
}

typedef struct {
  int is_bytes;
  uint64_t number;
  const uint8_t *bytes;
  size_t bytes_len;
} Value;

static int field(const uint8_t *data, size_t len, uint64_t wanted, Value *out) {
  size_t offset = 0;
  while (offset < len) {
    uint64_t key;
    if (varint(data, len, &offset, &key)) return -1;
    if ((key >> 3) == 0) return -1;
    Value v = {0};
    switch (key & 7) {
      case 0:
        if (varint(data, len, &offset, &v.number)) return -1;
        v.is_bytes = 0;
        break;
      case 2: {
        uint64_t blen;
        if (varint(data, len, &offset, &blen)) return -1;
        if (offset + blen > len) return -1;
        v.is_bytes = 1;
        v.bytes = data + offset;
        v.bytes_len = (size_t)blen;
        offset += (size_t)blen;
        break;
      }
      case 1:
        offset += 8;
        if (offset > len) return -1;
        continue;
      case 5:
        offset += 4;
        if (offset > len) return -1;
        continue;
      default:
        return -1;
    }
    if ((key >> 3) == wanted) {
      *out = v;
      return 0;
    }
  }
  return -1;
}

static int get_number(const uint8_t *data, size_t len, uint64_t n, uint64_t *out) {
  Value v;
  if (field(data, len, n, &v) || v.is_bytes) return -1;
  *out = v.number;
  return 0;
}

static int get_bytes(const uint8_t *data, size_t len, uint64_t n, const uint8_t **b, size_t *bl) {
  Value v;
  if (field(data, len, n, &v) || !v.is_bytes) return -1;
  *b = v.bytes;
  *bl = v.bytes_len;
  return 0;
}

static void put_var(uint8_t **p, uint64_t v) {
  while (v >= 128) {
    *(*p)++ = (uint8_t)((v & 127) | 128);
    v >>= 7;
  }
  *(*p)++ = (uint8_t)v;
}

static void put_number(uint8_t **p, uint64_t field_no, uint64_t value) {
  put_var(p, field_no << 3);
  put_var(p, value);
}

static void put_bytes(uint8_t **p, uint64_t field_no, const void *data, size_t len) {
  put_var(p, (field_no << 3) | 2);
  put_var(p, len);
  memcpy(*p, data, len);
  *p += len;
}

static void to_lower_ru(char *s) {
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c >= 'A' && c <= 'Z') *s = (char)(c - 'A' + 'a');
  }
  /* ё -> е (UTF-8 D0 B1 -> D0 B5 is wrong; ё is D1 91, Е is D0 81 / е D0 B5) */
  for (char *p = s; *p; ) { /* reset - operate on original via separate pass */
    (void)p;
    break;
  }
}

static int starts_with(const char *s, const char *p) {
  return strncmp(s, p, strlen(p)) == 0;
}

/* «группу X», «исполнителя X», «песни группы X»: X names an ARTIST, not a title.
 * Returns how many bytes of s the artist noun takes (with a leading «песни »,
 * «треки », «музыку »), or 0. */
static size_t artist_noun(const char *s) {
  static const char *const lead[] = {"песни ", "треки ", "музыку ", "", NULL};
  static const char *const nouns[] = {
      "группу ", "группа ", "группы ", "исполнителя ", "исполнитель ", "артиста ",
      "артист ", "певца ", "певец ", "певицу ", "певица ", "певицы ", "рэпера ", "рэпер ",
      "музыканта ", NULL};
  for (int i = 0; lead[i]; i++) {
    if (!starts_with(s, lead[i])) continue;
    const char *n = s + strlen(lead[i]);
    for (int j = 0; nouns[j]; j++)
      if (starts_with(n, nouns[j])) return (size_t)(n - s) + strlen(nouns[j]);
  }
  return 0;
}

static int is_home_word(const char *word) {
  static const char *home[] = {
      "свет", "освещ", "ламп", "люстр", "торшер", "ночник", "розетк", "телевизор", "тв",
      "кондицион", "вентилятор", "обогревател", "пылесос", "робот", "чайник", "будильник",
      "таймер", "блютуз", "bluetooth", "режим", "экран", "микрофон", "громк", "звук",
      "отоплен", "увлажнител", "очистител", "зарядк", NULL};
  for (int i = 0; home[i]; i++) {
    if (starts_with(word, home[i])) return 1;
  }
  return 0;
}

/* Returns malloc'd search query or NULL. Hardcoded fallback used only when
 * voice-commands.json fails to load (keeps the proxy working no matter what). */
static char *music_query_builtin(const char *text_in) {
  char text[512];
  size_t n = strnlen(text_in, sizeof text - 1);
  memcpy(text, text_in, n);
  text[n] = 0;
  /* strip trailing punctuation */
  while (n && (text[n - 1] == '.' || text[n - 1] == '!' || text[n - 1] == '?' || text[n - 1] == ' '))
    text[--n] = 0;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)text[i];
    if (c >= 'A' && c <= 'Z') text[i] = (char)(c - 'A' + 'a');
  }
  /* UTF-8 Cyrillic А-Я / Ё → а-я / ё */
  for (size_t i = 0; i + 1 < n; i++) {
    unsigned char c0 = (unsigned char)text[i];
    unsigned char c1 = (unsigned char)text[i + 1];
    if (c0 == 0xD0 && c1 == 0x81) { /* Ё */
      text[i] = (char)0xD1;
      text[i + 1] = (char)0x91;
      i++;
      continue;
    }
    if (c0 == 0xD0 && c1 >= 0x90 && c1 <= 0x9F) { /* А-П */
      text[i + 1] = (char)(c1 + 0x20);
      i++;
      continue;
    }
    if (c0 == 0xD0 && c1 >= 0xA0 && c1 <= 0xAF) { /* Р-Я */
      text[i] = (char)0xD1;
      text[i + 1] = (char)(c1 - 0x20);
      i++;
      continue;
    }
  }
  /* strip activation */
  const char *prefixes[] = {"алиса ", "яндекс ", NULL};
  char *body = text;
  for (int i = 0; prefixes[i]; i++) {
    size_t pl = strlen(prefixes[i]);
    if (strncmp(body, prefixes[i], pl) == 0) body += pl;
  }

  if (!strcmp(body, "стоп") || !strcmp(body, "хватит") || !strcmp(body, "пауза") ||
      !strcmp(body, "замолчи") || !strcmp(body, "заткнись") || !strcmp(body, "выруби") ||
      !strcmp(body, "выключи") || !strcmp(body, "останови") ||
      !strcmp(body, "останови музыку") || !strcmp(body, "выключи музыку") ||
      !strcmp(body, "поставь на паузу") || !strcmp(body, "останови воспроизведение")) {
    return strdup("__PAUSE__");
  }
  if (!strcmp(body, "очисти очередь") || !strcmp(body, "очистить очередь") ||
      !strcmp(body, "сбрось очередь") || !strcmp(body, "сбросить очередь")) {
    return strdup("__CLEAR_QUEUE__");
  }
  if (!strcmp(body, "дальше") || !strcmp(body, "далее") || !strcmp(body, "следующий") || !strcmp(body, "следующая") ||
      !strcmp(body, "следующий трек") || !strcmp(body, "следующая песня") ||
      !strcmp(body, "переключи") || !strcmp(body, "переключи трек") || !strcmp(body, "скип")) {
    return strdup("__NEXT__");
  }
  if (!strcmp(body, "назад") || !strcmp(body, "предыдущий") || !strcmp(body, "предыдущая") ||
      !strcmp(body, "предыдущий трек") || !strcmp(body, "предыдущая песня")) {
    return strdup("__PREVIOUS__");
  }
  if (!strcmp(body, "играй") || !strcmp(body, "продолжи") || !strcmp(body, "продолжай") ||
      !strcmp(body, "включи музыку") || !strcmp(body, "продолжи музыку") ||
      !strcmp(body, "продолжай музыку") || !strcmp(body, "запусти музыку")) {
    return strdup("__RESUME__");
  }
  if (!strcmp(body, "перемотай на начало") || !strcmp(body, "перемотай в начало") ||
      !strcmp(body, "промотай на начало") || !strcmp(body, "промотай в начало")) {
    return strdup("__SEEK_START__");
  }
  if (!strcmp(body, "перемотай на конец") || !strcmp(body, "перемотай в конец") ||
      !strcmp(body, "промотай на конец") || !strcmp(body, "промотай в конец")) {
    return strdup("__SEEK_END__");
  }
  if (!strcmp(body, "перемотай вперед") || !strcmp(body, "перемотай вперёд") ||
      !strcmp(body, "промотай вперед") || !strcmp(body, "промотай вперёд")) {
    return strdup("__SEEK_FORWARD__");
  }
  if (!strcmp(body, "перемотай назад") || !strcmp(body, "промотай назад")) {
    return strdup("__SEEK_BACK__");
  }
  if (starts_with(body, "перемотай ") || starts_with(body, "промотай ")) {
    const char *digits = strpbrk(body, "0123456789");
    if (digits) {
      unsigned seconds = (unsigned)strtoul(digits, NULL, 10);
      if (seconds > 0 && seconds <= 3600) {
        char command[48];
        const char *kind = (strstr(body, "назад") ? "BACK" : "FORWARD");
        snprintf(command, sizeof command, "__SEEK_%s_%u", kind, seconds);
        return strdup(command);
      }
    }
  }

  const char *verbs[] = {"включи ", "поставь ", "запусти ", "сыграй ", "послушай ", "давай ", NULL};
  char *rest = NULL;
  for (int i = 0; verbs[i]; i++) {
    size_t vl = strlen(verbs[i]);
    if (strncmp(body, verbs[i], vl) == 0) {
      rest = body + vl;
      break;
    }
  }
  int had_verb = rest != NULL;
  /* Exact verb alone → crumb (handled later), not a search. */
  if (!rest) {
    static const char *verb_only[] = {"включи", "поставь", "запусти", "сыграй", "послушай",
                                      "давай", NULL};
    for (int i = 0; verb_only[i]; i++) {
      if (!strcmp(body, verb_only[i])) return strdup(body);
    }
  }
  /* Bare artist/track: «Алиса, Кино» / «Радиохед» */
  if (!rest) rest = body;
  while (*rest == ' ') rest++;

  const char *music_prefix[] = {"песню ", "трек ", "музыку ", "группу ", "исполнителя ",
                                "альбом ", "плейлист ", "что нибудь ", "что-нибудь ", NULL};
  int explicit = 0;
  for (int i = 0; music_prefix[i]; i++) {
    size_t ml = strlen(music_prefix[i]);
    if (strncmp(rest, music_prefix[i], ml) == 0) {
      rest += ml;
      explicit = 1;
      break;
    }
  }
  while (*rest == ' ' || *rest == '"') rest++;
  /* Do not treat ordinary Alice conversation as a Spotify search. A bare
   * phrase is music only when it starts with an explicit music noun. */
  if (!had_verb && !explicit) return NULL;
  /* Verb with empty/short tail: incomplete ASR — preempt the stock player, no Spotify search. */
  if (rest != body && (!*rest || strlen(rest) < 2)) return strdup("__PARTIAL__");
  if (!*rest) return NULL;
  if (strlen(rest) < 2) return NULL;

  /* Skip questions / non-music chatter */
  if (strncmp(rest, "что ", 4) == 0 || strncmp(rest, "как ", 4) == 0 ||
      strncmp(rest, "какая ", 6) == 0 || strncmp(rest, "какой ", 6) == 0 ||
      strncmp(rest, "сколько ", 8) == 0 || strncmp(rest, "почему ", 7) == 0 ||
      strncmp(rest, "зачем ", 6) == 0 || strncmp(rest, "где ", 4) == 0 ||
      strncmp(rest, "когда ", 6) == 0 || strncmp(rest, "кто ", 4) == 0 ||
      strncmp(rest, "расскажи", 8) == 0 || strncmp(rest, "погода", 6) == 0) {
    return NULL;
  }

  if (!explicit) {
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", rest);
    for (char *save = NULL, *word = strtok_r(tmp, " ", &save); word; word = strtok_r(NULL, " ", &save)) {
      if (is_home_word(word)) return NULL;
    }
  }
  return strdup(rest);
}

/* ====================================================================== *
 *  JSON-driven command table (voice-commands.json).                      *
 *  Lets commands be added/edited without recompiling the proxy.          *
 *  A minimal JSON parser is included below so there are no extra deps.    *
 * ====================================================================== */

typedef enum { JNULL, JBOOL, JNUM, JSTR, JARR, JOBJ } jtype;
typedef struct jval {
  jtype t;
  double num;
  char *str;           /* JSTR value */
  struct jval **items; /* JARR items / JOBJ values */
  char **keys;         /* JOBJ keys (parallel to items) */
  int n;
} jval;

static void jfree(jval *v) {
  if (!v) return;
  if (v->t == JSTR) free(v->str);
  if (v->t == JARR || v->t == JOBJ) {
    for (int i = 0; i < v->n; i++) {
      jfree(v->items[i]);
      if (v->keys) free(v->keys[i]);
    }
    free(v->items);
    free(v->keys);
  }
  free(v);
}

static const char *jskip(const char *p) {
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
  return p;
}

static char *jparse_string(const char **pp) {
  const char *p = *pp;
  if (*p != '"') return NULL;
  p++;
  size_t cap = 16, len = 0;
  char *out = malloc(cap);
  if (!out) return NULL;
  while (*p && *p != '"') {
    if ((unsigned char)*p == '\\') {
      p++;
      char e = *p;
      if (!e) { free(out); return NULL; }
      char add[4];
      int addn = 1;
      switch (e) {
        case 'n': add[0] = '\n'; break;
        case 't': add[0] = '\t'; break;
        case 'r': add[0] = '\r'; break;
        case 'b': add[0] = '\b'; break;
        case 'f': add[0] = '\f'; break;
        case '/': add[0] = '/'; break;
        case '\\': add[0] = '\\'; break;
        case '"': add[0] = '"'; break;
        case 'u': {
          unsigned u = 0;
          int ok = 1;
          for (int k = 0; k < 4; k++) {
            char h = p[1 + k];
            u <<= 4;
            if (h >= '0' && h <= '9') u |= (unsigned)(h - '0');
            else if (h >= 'a' && h <= 'f') u |= (unsigned)(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') u |= (unsigned)(h - 'A' + 10);
            else { ok = 0; break; }
          }
          if (!ok) { free(out); return NULL; }
          p += 4;
          if (u < 0x80) { add[0] = (char)u; addn = 1; }
          else if (u < 0x800) {
            add[0] = (char)(0xC0 | (u >> 6));
            add[1] = (char)(0x80 | (u & 0x3F));
            addn = 2;
          } else {
            add[0] = (char)(0xE0 | (u >> 12));
            add[1] = (char)(0x80 | ((u >> 6) & 0x3F));
            add[2] = (char)(0x80 | (u & 0x3F));
            addn = 3;
          }
          break;
        }
        default: add[0] = e; break;
      }
      for (int k = 0; k < addn; k++) {
        if (len + 1 >= cap) {
          cap *= 2;
          char *t = realloc(out, cap);
          if (!t) { free(out); return NULL; }
          out = t;
        }
        out[len++] = add[k];
      }
      p++;
    } else {
      if (len + 1 >= cap) {
        cap *= 2;
        char *t = realloc(out, cap);
        if (!t) { free(out); return NULL; }
        out = t;
      }
      out[len++] = *p++;
    }
  }
  if (*p != '"') { free(out); return NULL; }
  p++;
  out[len] = 0;
  *pp = p;
  return out;
}

static jval *jnew(jtype t) {
  jval *v = calloc(1, sizeof *v);
  if (v) v->t = t;
  return v;
}

static jval *jparse(const char **pp) {
  const char *p = jskip(*pp);
  jval *v = NULL;
  if (*p == '"') {
    char *s = jparse_string(&p);
    if (!s) return NULL;
    v = jnew(JSTR);
    if (!v) { free(s); return NULL; }
    v->str = s;
  } else if (*p == '{') {
    p++;
    v = jnew(JOBJ);
    if (!v) return NULL;
    p = jskip(p);
    if (*p == '}') {
      p++;
    } else for (;;) {
      p = jskip(p);
      char *key = jparse_string(&p);
      if (!key) { jfree(v); return NULL; }
      p = jskip(p);
      if (*p != ':') { free(key); jfree(v); return NULL; }
      p++;
      jval *child = jparse(&p);
      if (!child) { free(key); jfree(v); return NULL; }
      jval **ni = realloc(v->items, (size_t)(v->n + 1) * sizeof *ni);
      char **nk = realloc(v->keys, (size_t)(v->n + 1) * sizeof *nk);
      if (!ni || !nk) {
        if (ni) v->items = ni;
        if (nk) v->keys = nk;
        free(key);
        jfree(child);
        jfree(v);
        return NULL;
      }
      v->items = ni;
      v->keys = nk;
      v->items[v->n] = child;
      v->keys[v->n] = key;
      v->n++;
      p = jskip(p);
      if (*p == ',') { p++; continue; }
      if (*p == '}') { p++; break; }
      jfree(v);
      return NULL;
    }
  } else if (*p == '[') {
    p++;
    v = jnew(JARR);
    if (!v) return NULL;
    p = jskip(p);
    if (*p == ']') {
      p++;
    } else for (;;) {
      jval *child = jparse(&p);
      if (!child) { jfree(v); return NULL; }
      jval **ni = realloc(v->items, (size_t)(v->n + 1) * sizeof *ni);
      if (!ni) { jfree(child); jfree(v); return NULL; }
      v->items = ni;
      v->items[v->n++] = child;
      p = jskip(p);
      if (*p == ',') { p++; continue; }
      if (*p == ']') { p++; break; }
      jfree(v);
      return NULL;
    }
  } else if (!strncmp(p, "true", 4)) {
    v = jnew(JBOOL);
    if (v) v->num = 1;
    p += 4;
  } else if (!strncmp(p, "false", 5)) {
    v = jnew(JBOOL);
    p += 5;
  } else if (!strncmp(p, "null", 4)) {
    v = jnew(JNULL);
    p += 4;
  } else {
    char *end = NULL;
    double d = strtod(p, &end);
    if (end == p) return NULL;
    v = jnew(JNUM);
    if (!v) return NULL;
    v->num = d;
    p = end;
  }
  *pp = p;
  return v;
}

static jval *jget(jval *o, const char *key) {
  if (!o || o->t != JOBJ) return NULL;
  for (int i = 0; i < o->n; i++)
    if (!strcmp(o->keys[i], key)) return o->items[i];
  return NULL;
}

/* ---- Loaded config ---- */
/* What Alice says for us (via «повтори за мной …»), voice-commands.json
 * "replies": each a list of variants, one picked at random (never the same
 * twice running).  {track} = "artist, title" (or the title), {artist},
 * {title}, {count} (queue clearing).  Built-in defaults when the file has none. */
enum { R_NOW_PLAYING, R_NOW_PAUSED, R_NOTHING, R_BL_ARTIST, R_BL_TRACK,
       R_REBOOT, R_REBOOT_REFUSED, R_CLEAR_START, R_CLEAR_DONE, R_CLEAR_EMPTY,
       R_CLEAR_PARTIAL, R_WRONG_RETRY, R_WRONG_UNKNOWN, NREPLY };
static const char *const reply_key[NREPLY] = {
    "now_playing", "now_paused", "nothing_playing", "blacklist_artist", "blacklist_track",
    "reboot", "reboot_refused", "clear_start", "clear_done", "clear_empty", "clear_partial",
    "wrong_retry", "wrong_unknown"};
static const char *const reply_def[NREPLY] = {
    "это {track}", "на паузе {track}", "ничего не играет", "{artist} теперь в чёрном списке",
    "{track} теперь в чёрном списке", "перезагружаюсь", "я только что перезагрузилась",
    "начинаю чистить очередь, подождите окончания", "я всё очистила", "очередь и так пустая",
    "очистила не всё, осталось {count}", "сейчас поищу другое",
    "не помню, что ты просил, скажи ещё раз"};

typedef struct {
  char *action;
  char **exact;
  int nexact;
  regex_t *re;
  int nre;
} cmd_t;

static struct {
  int loaded;
  char **act_prefix;
  int n_act;
  cmd_t *cmds;
  int ncmds;
  char **verbs;
  int nverbs;
  char **verb_only;
  int nvonly;
  char **music_prefix;
  int nmusic;
  char **questions;
  int nq;
  char **home;
  int nhome;
  int use_announcement;       /* recover the query from Alice's "Включаю …" TTS */
  int announcement_priority;  /* prefer that announcement over the raw request text */
  int repeat_window_sec;      /* same request again within this -> search the user's words */
  int announcement_wait_ms;   /* grace for Alice's announcement once she commits */
  int answer_wait_ms;         /* Alice silent this long after the request (no internet) */
  int duck_db;                /* music this much quieter while Alice is in a dialog */
  char **reply[NREPLY];       /* "replies": variants per situation */
  int nreply[NREPLY];
  char **say_prefix;          /* "say_prefixes": how we ask her to repeat our text */
  int nsay;
  char **reset_phrase;        /* "reset_phrases": the silent request that cuts her off */
  int nreset;
} g;

/* Copy a JSON array-of-strings into a malloc'd char* array; returns count. */
static int jstr_array(jval *arr, char ***out) {
  *out = NULL;
  if (!arr || arr->t != JARR || arr->n == 0) return 0;
  char **v = calloc((size_t)arr->n, sizeof *v);
  if (!v) return 0;
  int k = 0;
  for (int i = 0; i < arr->n; i++)
    if (arr->items[i]->t == JSTR) v[k++] = strdup(arr->items[i]->str);
  *out = v;
  return k;
}

/* Why the last load_commands() failed or what it skipped (for --check). */
static char load_err[400];
static int load_bad_regex;

static int load_commands(const char *path) {
  load_err[0] = 0;
  load_bad_regex = 0;
  FILE *f = fopen(path, "rb");
  if (!f) { snprintf(load_err, sizeof load_err, "файл не открывается"); return 0; }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  if (sz <= 0 || sz > (1 << 20)) { fclose(f); snprintf(load_err, sizeof load_err, "пустой или больше 1 МБ"); return 0; }
  fseek(f, 0, SEEK_SET);
  char *buf = malloc((size_t)sz + 1);
  if (!buf) { fclose(f); return 0; }
  size_t rd = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  buf[rd] = 0;
  const char *p = buf;
  jval *root = jparse(&p);
  free(buf);
  if (!root || root->t != JOBJ) {
    jfree(root);
    snprintf(load_err, sizeof load_err, "это не JSON-объект (ошибка синтаксиса)");
    return 0;
  }

  memset(&g, 0, sizeof g);
  g.n_act = jstr_array(jget(root, "activation_prefixes"), &g.act_prefix);

  jval *search = jget(root, "search");
  g.nverbs = jstr_array(jget(search, "verbs"), &g.verbs);
  g.nvonly = jstr_array(jget(search, "verb_only"), &g.verb_only);
  g.nmusic = jstr_array(jget(search, "music_prefixes"), &g.music_prefix);
  g.nq = jstr_array(jget(search, "question_words"), &g.questions);
  g.nhome = jstr_array(jget(search, "home_words"), &g.home);

  /* settings: { use_announcement, announcement_priority } — both default ON;
   * repeat_window_sec — default 300, 0 disables repeat detection;
   * announcement_wait_ms — default 2500, 0 = search the request text at once;
   * answer_wait_ms — default 5000, 0 disables the "Alice never answered" fallback. */
  jval *settings = jget(root, "settings");
  jval *ua = jget(settings, "use_announcement");
  g.use_announcement = (!ua) ? 1 : ((ua->t == JBOOL || ua->t == JNUM) && ua->num != 0);
  jval *ap = jget(settings, "announcement_priority");
  g.announcement_priority = (!ap) ? 1 : ((ap->t == JBOOL || ap->t == JNUM) && ap->num != 0);
  jval *rw = jget(settings, "repeat_window_sec");
  g.repeat_window_sec = (rw && rw->t == JNUM) ? (int)rw->num : 300;
  jval *aw = jget(settings, "announcement_wait_ms");
  g.announcement_wait_ms = (aw && aw->t == JNUM) ? (int)aw->num : 2500;
  jval *nw = jget(settings, "answer_wait_ms");
  g.answer_wait_ms = (nw && nw->t == JNUM) ? (int)nw->num : 5000;
  jval *dd = jget(settings, "duck_db");
  g.duck_db = (dd && dd->t == JNUM) ? (int)dd->num : 8;

  jval *replies = jget(root, "replies");
  for (int i = 0; i < NREPLY; i++) g.nreply[i] = jstr_array(jget(replies, reply_key[i]), &g.reply[i]);
  g.nsay = jstr_array(jget(root, "say_prefixes"), &g.say_prefix);
  g.nreset = jstr_array(jget(root, "reset_phrases"), &g.reset_phrase);

  jval *cmds = jget(root, "commands");
  if (cmds && cmds->t == JARR && cmds->n > 0) {
    g.cmds = calloc((size_t)cmds->n, sizeof *g.cmds);
    if (g.cmds) {
      for (int i = 0; i < cmds->n; i++) {
        jval *c = cmds->items[i];
        if (c->t != JOBJ) continue;
        jval *act = jget(c, "action");
        if (!act || act->t != JSTR) continue;
        cmd_t *cc = &g.cmds[g.ncmds];
        cc->action = strdup(act->str);
        cc->nexact = jstr_array(jget(c, "exact"), &cc->exact);
        char **pats = NULL;
        int np = jstr_array(jget(c, "regex"), &pats);
        if (np > 0) {
          cc->re = calloc((size_t)np, sizeof(regex_t));
          if (cc->re) {
            for (int j = 0; j < np; j++) {
              int rc = regcomp(&cc->re[cc->nre], pats[j], REG_EXTENDED);
              if (rc == 0) { cc->nre++; continue; }
              if (!load_bad_regex++) {   /* a bad pattern is skipped; --check reports it */
                char why[120];
                regerror(rc, &cc->re[cc->nre], why, sizeof why);
                snprintf(load_err, sizeof load_err, "%s: шаблон «%s» — %s", act->str, pats[j], why);
              }
            }
          }
          for (int j = 0; j < np; j++) free(pats[j]);
          free(pats);
        }
        g.ncmds++;
      }
    }
  }
  jfree(root);
  g.loaded = (g.ncmds > 0 || g.nverbs > 0);
  if (!g.loaded && !load_err[0]) snprintf(load_err, sizeof load_err, "нет ни команд, ни глаголов поиска");
  return g.loaded;
}

/* Normalize in-place: strip trailing punctuation, ASCII + Cyrillic to lower.
 * Mirrors music_query_builtin's inline normalization. Returns new length. */
static size_t normalize_text(char *text, size_t n) {
  while (n && (text[n - 1] == '.' || text[n - 1] == '!' || text[n - 1] == '?' || text[n - 1] == ' '))
    text[--n] = 0;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)text[i];
    if (c >= 'A' && c <= 'Z') text[i] = (char)(c - 'A' + 'a');
  }
  for (size_t i = 0; i + 1 < n; i++) {
    unsigned char c0 = (unsigned char)text[i];
    unsigned char c1 = (unsigned char)text[i + 1];
    if (c0 == 0xD0 && c1 == 0x81) { text[i] = (char)0xD1; text[i + 1] = (char)0x91; i++; continue; }
    if (c0 == 0xD0 && c1 >= 0x90 && c1 <= 0x9F) { text[i + 1] = (char)(c1 + 0x20); i++; continue; }
    if (c0 == 0xD0 && c1 >= 0xA0 && c1 <= 0xAF) { text[i] = (char)0xD1; text[i + 1] = (char)(c1 - 0x20); i++; continue; }
  }
  return n;
}

/* JSON-table matcher. Same decision order as the builtin:
 * exact commands, then regex commands, then the search path. */
/* A command's action: "__X__" as is; anything else is music to play, read like
 * «включи <action>» — «группу кино» searches the artist (monitor's editor). */
static char *action_query(const char *a) {
  if (!strncmp(a, "__", 2)) return strdup(a);
  size_t an = artist_noun(a);
  if (!an || !a[an]) return strdup(a);
  char marked[560];
  snprintf(marked, sizeof marked, "@artist %s", a + an);
  return strdup(marked);
}

/* An ALBUM was asked for: «группу X альбом Y» (artist_intent, rest "X альбом Y"),
 * «альбом Y группы X» / «альбом Y» (album_prefix, rest "Y…").  Writes the
 * search marker "@album Y|X" ("@album Y" without an artist) and returns 1. */
static int album_marker(const char *rest, int artist_intent, int album_prefix, char *out, size_t cap) {
  if (artist_intent) {
    static const char word[] = " альбом ";
    const char *a = strstr(rest, word);
    if (!a || a == rest || !a[sizeof word - 1]) return 0;
    snprintf(out, cap, "@album %s|%.*s", a + sizeof word - 1, (int)(a - rest), rest);
    return 1;
  }
  if (!album_prefix) return 0;
  static const char *const by[] = {" группы ", " исполнителя ", " певца ", " певицы ",
                                   " рэпера ", " артиста ", NULL};
  for (int i = 0; by[i]; i++) {
    const char *b = strstr(rest, by[i]);
    if (b && b > rest && b[strlen(by[i])]) {
      snprintf(out, cap, "@album %.*s|%s", (int)(b - rest), rest, b + strlen(by[i]));
      return 1;
    }
  }
  snprintf(out, cap, "@album %s", rest);
  return 1;
}

static char *music_query_json(const char *text_in) {
  char text[512];
  size_t n = strnlen(text_in, sizeof text - 1);
  memcpy(text, text_in, n);
  text[n] = 0;
  n = normalize_text(text, n);
  /* "алиса, включи …": a comma would hide both the prefix and the verb. */
  char *cm = strchr(text, ',');
  if (cm && cm - text < 16 && cm[1] == ' ') memmove(cm, cm + 1, strlen(cm));

  char *body = text;
  for (int i = 0; i < g.n_act; i++) {
    size_t pl = strlen(g.act_prefix[i]);
    if (strncmp(body, g.act_prefix[i], pl) == 0) body += pl;
  }

  /* Pass 1: exact phrases. */
  for (int i = 0; i < g.ncmds; i++) {
    cmd_t *c = &g.cmds[i];
    for (int j = 0; j < c->nexact; j++)
      if (!strcmp(body, c->exact[j])) return action_query(c->action);
  }
  /* Pass 2: regex patterns. A {1} in the action is filled with the first
   * numeric token (1..3600) found in the phrase (e.g. seek N seconds). */
  for (int i = 0; i < g.ncmds; i++) {
    cmd_t *c = &g.cmds[i];
    for (int j = 0; j < c->nre; j++) {
      if (regexec(&c->re[j], body, 0, NULL, 0) != 0) continue;
      char *ph = strstr(c->action, "{1}");
      if (!ph) return action_query(c->action);
      const char *digits = strpbrk(body, "0123456789");
      if (!digits) continue;
      unsigned sec = (unsigned)strtoul(digits, NULL, 10);
      if (sec == 0 || sec > 3600) continue;
      char out[80];
      int pre = (int)(ph - c->action);
      snprintf(out, sizeof out, "%.*s%u%s", pre, c->action, sec, ph + 3);
      return strdup(out);
    }
  }

  /* Search path (verb / explicit music noun → Spotify query). */
  char *rest = NULL;
  int had_verb = 0;
  for (int i = 0; i < g.nverbs; i++) {
    size_t vl = strlen(g.verbs[i]);
    if (strncmp(body, g.verbs[i], vl) == 0) { rest = body + vl; had_verb = 1; break; }
  }
  /* "включи алиса включи X": the wake word and verb said again inside. */
  for (int again = had_verb; again;) {
    again = 0;
    while (*rest == ' ') rest++;
    for (int i = 0; i < g.n_act; i++)
      if (!strncmp(rest, g.act_prefix[i], strlen(g.act_prefix[i]))) { rest += strlen(g.act_prefix[i]); again = 1; }
    for (int i = 0; i < g.nverbs; i++)
      if (!strncmp(rest, g.verbs[i], strlen(g.verbs[i]))) { rest += strlen(g.verbs[i]); again = 1; }
  }
  if (!rest) {
    for (int i = 0; i < g.nvonly; i++)
      if (!strcmp(body, g.verb_only[i])) return strdup(body);
  }
  if (!rest) rest = body;
  while (*rest == ' ') rest++;

  int explicit = 0;
  int artist_intent = 0;
  int album_prefix = 0;
  /* «включи группу/исполнителя X» -> search the ARTIST, not a track. */
  size_t an = artist_noun(rest);
  if (an) {
    rest += an; explicit = 1; artist_intent = 1;
  } else {
    for (int i = 0; i < g.nmusic; i++) {
      size_t ml = strlen(g.music_prefix[i]);
      if (strncmp(rest, g.music_prefix[i], ml) == 0) {
        rest += ml; explicit = 1;
        album_prefix = !strcmp(g.music_prefix[i], "альбом ");
        break;
      }
    }
  }
  while (*rest == ' ' || *rest == '"') rest++;
  if (!had_verb && !explicit) return NULL;
  if (rest != body && (!*rest || strlen(rest) < 2)) return strdup("__PARTIAL__");
  if (!*rest) return NULL;
  if (strlen(rest) < 2) return NULL;

  for (int i = 0; i < g.nq; i++)
    if (strncmp(rest, g.questions[i], strlen(g.questions[i])) == 0) return NULL;

  if (!explicit) {
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", rest);
    for (char *save = NULL, *word = strtok_r(tmp, " ", &save); word; word = strtok_r(NULL, " ", &save))
      for (int i = 0; i < g.nhome; i++)
        if (starts_with(word, g.home[i])) return NULL;
  }
  char marked[560];
  if (album_marker(rest, artist_intent, album_prefix, marked, sizeof marked)) return strdup(marked);
  if (artist_intent) {
    snprintf(marked, sizeof marked, "@artist %s", rest);
    return strdup(marked);
  }
  return strdup(rest);
}

/* Dispatch: JSON table when loaded, hardcoded fallback otherwise. */
static char *music_query(const char *text_in) {
  if (g.loaded) return music_query_json(text_in);
  return music_query_builtin(text_in);
}

/* «пауза» / «продолжи»: alice-soloist-ctl.sh --pause|--resume (waits for it). */
static void soloist_cmd(const char *cmd) {
  pid_t pid = fork();
  if (pid == 0) {
    int null = open("/dev/null", O_RDWR);
    if (null >= 0) {
      dup2(null, 0);
      dup2(null, 1);
      dup2(null, 2);
      if (null > 2) close(null);
    }
    execl("/data/opt/spotify-connect/alice-soloist-ctl.sh", "alice-soloist-ctl", cmd, (char *)NULL);
    _exit(127);
  }
  if (pid > 0) {
    int st;
    waitpid(pid, &st, 0);
  }
}

static void soloist_seek(const char *operation) {
  pid_t pid = fork();
  if (pid == 0) {
    int null = open("/dev/null", O_RDWR);
    if (null >= 0) {
      dup2(null, 0);
      dup2(null, 1);
      dup2(null, 2);
      if (null > 2) close(null);
    }
    execl("/data/opt/spotify-connect/alice-soloist-seek.sh", "alice-soloist-seek", operation,
          (char *)NULL);
    _exit(127);
  }
  if (pid > 0) {
    int st;
    waitpid(pid, &st, 0);
  }
}

static void soloist_seek_seconds(const char *operation, const char *seconds) {
  pid_t pid = fork();
  if (pid == 0) {
    int null = open("/dev/null", O_RDWR);
    if (null >= 0) { dup2(null, 0); dup2(null, 1); dup2(null, 2); if (null > 2) close(null); }
    execl("/data/opt/spotify-connect/alice-soloist-seek.sh", "alice-soloist-seek", operation,
          seconds, (char *)NULL);
    _exit(127);
  }
  if (pid > 0) { int st; waitpid(pid, &st, 0); }
}

static void spotify_sequence_transport(const char *operation) {
  pid_t pid = fork();
  if (pid == 0) {
    int null = open("/dev/null", O_RDWR);
    if (null >= 0) {
      dup2(null, 0);
      dup2(null, 1);
      dup2(null, 2);
      if (null > 2) close(null);
    }
    execl("/data/opt/spotify-connect/alice-spotify-transport.sh", "alice-spotify-transport",
          operation, (char *)NULL);
    _exit(127);
  }
  if (pid > 0) {
    int st;
    waitpid(pid, &st, 0);
  }
}

static void spotify_search_play(const char *query) {
  pid_t pid = fork();
  if (pid == 0) {
    int null = open("/dev/null", O_RDWR);
    if (null >= 0) {
      dup2(null, 0);
      dup2(null, 1);
      dup2(null, 2);
      if (null > 2) close(null);
    }
    execl("/data/opt/spotify-connect/alice-spotify-search.sh", "alice-spotify-search", query,
          (char *)NULL);
    _exit(127);
  }
}

/* «лайк» / «добавь в избранное» -> save the now-playing track to Liked Songs
 * via the owner's OAuth token (user-library-modify). Fire-and-forget. */
static void spotify_like(void) {
  pid_t pid = fork();
  if (pid == 0) {
    int null = open("/dev/null", O_RDWR);
    if (null >= 0) { dup2(null, 0); dup2(null, 1); dup2(null, 2); if (null > 2) close(null); }
    execl("/data/opt/spotify-connect/spotify-oauth.sh", "spotify-oauth", "like", "current",
          (char *)NULL);
    _exit(127);
  }
}

/* «поставь избранное / любимые треки» -> play the owner's Liked Songs. */
static void log_msg(const char *fmt, ...);

/* «N песен назад»: alice-history-back.sh plays the track N back in the
 * monitor's playback history (exact URI, not a text search). */
static void history_back(int n) {
  char arg[12];
  snprintf(arg, sizeof arg, "%d", n);
  log_msg("history back: %d", n);
  pid_t pid = fork();
  if (pid == 0) {
    int null = open("/dev/null", O_RDWR);
    if (null >= 0) { dup2(null, 0); dup2(null, 1); dup2(null, 2); if (null > 2) close(null); }
    execl("/data/opt/spotify-connect/alice-history-back.sh", "alice-history-back", arg, (char *)NULL);
    _exit(127);
  }
}

/* «Очисти очередь»: transport.sh clear skips through the whole Spotify queue
 * (several seconds), so it runs detached instead of holding this session. */
/* «ты поставила не то» / «что за дичь ты включила»: the track playing was the
 * wrong pick for the last request.  alice-wrong-pick.sh drops it from the
 * history and searches that request again (the user's words, not Alice's)
 * with the wrong track, artist and album excluded (caller holds music_mu). */
static char last_req[256];
static unsigned last_req_at;
static unsigned now_sec(void);
static void say_instead(const char *text);
static void reply_text_n(int r, char *out, size_t cap, const char *artist, const char *title,
                         const char *count);
static void wrong_pick(void) {
  char say[300];
  if (!last_req[0] || now_sec() - last_req_at > 3 * 3600) {
    log_msg("wrong pick: no recent request");
    reply_text_n(R_WRONG_UNKNOWN, say, sizeof say, NULL, NULL, NULL);
    say_instead(say);
    return;
  }
  log_msg("wrong pick: again %s", last_req);
  reply_text_n(R_WRONG_RETRY, say, sizeof say, NULL, NULL, NULL);
  say_instead(say);
  pid_t pid = fork();
  if (pid == 0) {
    setsid();
    int null = open("/dev/null", O_RDWR);
    if (null >= 0) { dup2(null, 0); dup2(null, 1); dup2(null, 2); if (null > 2) close(null); }
    execl("/data/opt/spotify-connect/alice-wrong-pick.sh", "alice-wrong-pick", last_req, (char *)NULL);
    _exit(127);
  }
}

/* «очисти очередь»: alice-queue-drain.sh skips through the Spotify queue
 * one track at a time (a minute or two for ~80) with the music held at 0.
 * Alice says so first, instead of her own answer, and says the outcome when
 * it is done (caller holds music_mu). */
#define QUEUE_DRAIN "/data/opt/spotify-connect/alice-queue-drain.sh"
#define QUEUE_RESULT "/tmp/alice-queue-clear.result"
static void say_instead(const char *text);
static void alice_say(const char *text);
static void reply_text_n(int r, char *out, size_t cap, const char *artist, const char *title,
                         const char *count);
static void *queue_clear_watch(void *arg) {
  (void)arg;
  char res[64] = "";
  for (int i = 0; i < 700 && !res[0]; i++) {          /* drain gives up after 300 s */
    usleep(500000);
    FILE *f = fopen(QUEUE_RESULT, "r");
    if (!f) continue;
    if (!fgets(res, sizeof res, f)) res[0] = 0;
    fclose(f);
  }
  res[strcspn(res, "\n")] = 0;
  log_msg("queue clear: %s", res[0] ? res : "no result");
  char say[300];
  if (!strncmp(res, "done", 4)) reply_text_n(R_CLEAR_DONE, say, sizeof say, NULL, NULL, res + 4 + (res[4] == ' '));
  else if (!strncmp(res, "partial", 7)) reply_text_n(R_CLEAR_PARTIAL, say, sizeof say, NULL, NULL, res + 7 + (res[7] == ' '));
  else return NULL;                                    /* busy: the running one will say it */
  alice_say(say);
  return NULL;
}
static void queue_clear(void) {
  char line[32] = "";
  FILE *p = popen(QUEUE_DRAIN " count 2>/dev/null", "r");
  if (p) {
    if (!fgets(line, sizeof line, p)) line[0] = 0;
    pclose(p);
  }
  int n = atoi(line);
  log_msg("queue clear: %d queued", n);
  char say[300], cnt[16];
  snprintf(cnt, sizeof cnt, "%d", n);
  if (n <= 0) {
    reply_text_n(R_CLEAR_EMPTY, say, sizeof say, NULL, NULL, cnt);
    say_instead(say);
    return;
  }
  reply_text_n(R_CLEAR_START, say, sizeof say, NULL, NULL, cnt);
  say_instead(say);
  unlink(QUEUE_RESULT);
  pid_t pid = fork();
  if (pid == 0) {
    setsid();
    int null = open("/dev/null", O_RDWR);
    if (null >= 0) { dup2(null, 0); dup2(null, 1); dup2(null, 2); if (null > 2) close(null); }
    execl(QUEUE_DRAIN, "alice-queue-drain", (char *)NULL);
    _exit(127);
  }
  if (pid < 0) return;
  pthread_t th;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&th, &attr, queue_clear_watch, NULL) != 0) log_msg("queue clear: no watcher");
  pthread_attr_destroy(&attr);
}

static void spotify_favorites(void) {
  pid_t pid = fork();
  if (pid == 0) {
    int null = open("/dev/null", O_RDWR);
    if (null >= 0) { dup2(null, 0); dup2(null, 1); dup2(null, 2); if (null > 2) close(null); }
    execl("/data/opt/spotify-connect/spotify-oauth.sh", "spotify-oauth", "favorites",
          (char *)NULL);
    _exit(127);
  }
}

static void log_msg(const char *fmt, ...);
static unsigned long now_ms(void);
static long now_s(void);
static void vol_lower_copy(char *dst, size_t dn, const char *src);

/* The count in "3 песни назад" / "две песни назад" / "пару треков назад";
 * 0 if none (1..99). */
static int spoken_count(const char *text) {
  char low[256];
  vol_lower_copy(low, sizeof low, text);
  const char *d = strpbrk(low, "0123456789");
  if (d) {
    int n = atoi(d);
    return n >= 1 && n <= 99 ? n : 0;
  }
  static const char *const words[][2] = {
      {"одну", "1"}, {"один", "1"}, {"две", "2"}, {"два", "2"}, {"пару", "2"}, {"три", "3"},
      {"четыре", "4"}, {"пять", "5"}, {"шесть", "6"}, {"семь", "7"}, {"восемь", "8"},
      {"девять", "9"}, {"десять", "10"}, {NULL, NULL}};
  for (char *save = NULL, *w = strtok_r(low, " ", &save); w; w = strtok_r(NULL, " ", &save))
    for (int i = 0; words[i][0]; i++)
      if (!strcmp(w, words[i][0])) return atoi(words[i][1]);
  return 0;
}
static volatile int alice_raw_act;   /* last AliceState activity as received */

/* Send aliced a QuasarMessage (field 211: Directive) as a framed IPC message. */
static int send_quasar_directive(const char *name, const char *json_payload) {
  uint8_t dir[1024];
  uint8_t *dp = dir;
  put_bytes(&dp, 1, name, strlen(name));
  put_bytes(&dp, 6, json_payload, strlen(json_payload));
  put_number(&dp, 12, 1);                       /* is_route_locally */
  uint8_t message[1100];
  uint8_t *mp = message;
  put_bytes(&mp, 211, dir, (size_t)(dp - dir));
  size_t msg_len = (size_t)(mp - message);
  uint8_t frame[1110];
  frame[0] = (uint8_t)(msg_len & 0xff);
  frame[1] = (uint8_t)((msg_len >> 8) & 0xff);
  frame[2] = (uint8_t)((msg_len >> 16) & 0xff);
  frame[3] = (uint8_t)((msg_len >> 24) & 0xff);
  memcpy(frame + 4, message, msg_len);
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(ALICE_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  struct timeval tv = {.tv_sec = 0, .tv_usec = 500000};
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
    close(fd);
    return -1;
  }
  int ok = write_full(fd, frame, msg_len + 4);
  /* Closing right after the write sometimes loses the message on aliced's side:
   * let it greet the client (it sends its state frames) before hanging up. */
  shutdown(fd, SHUT_WR);
  struct timeval rt = {.tv_sec = 0, .tv_usec = 300000};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rt, sizeof rt);
  char drain[4096];
  unsigned long until = now_ms() + 800;
  while (now_ms() < until && read(fd, drain, sizeof drain) > 0) {}
  close(fd);
  if (ok != 0) log_msg("aliced directive %s: send failed", name);
  return ok;
}

/* How we ask Alice to read our text out, and the neutral request that cuts
 * her own answer off first.  Both vary every time (voice-commands.json
 * "say_prefixes" / "reset_phrases"), so no one fixed phrase goes out day
 * after day.  Tested 2026-11-20 on "это Кино, Звезда по имени Солнце": only
 * "повтори за мной" and "повторяй за мной" read it back word for word.
 * "алиса, повтори …", "давай / слушай / теперь / ну-ка повтори …", "повтори
 * вслед за мной" turn a track name into a music request ("Включаю: КИНО …");
 * "скажи …", "произнеси …", "повтори фразу …", "… за мною" reach her
 * generative answers, which paraphrase; "повтори за мной пожалуйста" reads
 * "пожалуйста" out too.  Any thanks goes the same way as "спасибо";
 * "хорошо"/"ладно" could answer "yes" to her "Добавить в любимые песни?". */
static const char *const say_prefix_def[] = {"повтори за мной", "повторяй за мной", NULL};
static const char *const reset_def[] = {
    "спасибо", "благодарю", "спасибо большое", "благодарю тебя", "спасибо тебе", NULL};

/* A random entry of the loaded list (or of `def` when it has none), never the
 * same twice running. */
static const char *pick_variant(char **list, int n, const char *const *def, int *last) {
  int total = n;
  if (total <= 0) while (def[total]) total++;
  int k = rand() % total;
  if (total > 1 && k == *last) k = (k + 1 + rand() % (total - 1)) % total;
  *last = k;
  return n > 0 && list[k] && list[k][0] ? list[k] : def[0];
}

/* A typed request (the "type" directive, as the app's text input): `head`,
 * then " " and `text` if any.  A new request also cuts off her own answer
 * still in flight. */
static void send_typed(const char *directive, const char *head, const char *text) {
  char payload[900];
  size_t n = (size_t)snprintf(payload, sizeof payload, "{\"text\":\"");
  for (int part = 0; part < 2; part++) {
    const char *c = part ? text : head;
    if (part && (!text || !*text)) break;
    if (part && n + 8 < sizeof payload) payload[n++] = ' ';
    for (; *c && n + 8 < sizeof payload; c++) {
      if (*c == '"' || *c == '\\') payload[n++] = '\\';
      if ((unsigned char)*c >= 0x20) payload[n++] = *c;
    }
  }
  snprintf(payload + n, sizeof payload - n, "\"}");
  send_quasar_directive(directive, payload);
}

/* Make Alice say `text`: "повтори за мной …" or another form of it. */
static void alice_say(const char *text) {
  static int last = -1;
  send_typed("type", pick_variant(g.say_prefix, g.nsay, say_prefix_def, &last), text);
}

/* Cut her own answer off and close its dialog, silently ("спасибо" and the like). */
static void alice_reset(void) {
  static int last = -1;
  send_typed("type_silent", pick_variant(g.reset_phrase, g.nreset, reset_def, &last), NULL);
}

/* "title"/"artist" string from the monitor's status.json (escaped \" and \\). */
static void status_field(const char *json, const char *key, char *out, size_t cap) {
  char pat[32];
  snprintf(pat, sizeof pat, "\"%s\":\"", key);
  out[0] = 0;
  const char *p = strstr(json, pat);
  if (!p) return;
  p += strlen(pat);
  size_t n = 0;
  for (; *p && *p != '"' && n + 1 < cap; p++) {
    if (*p == '\\' && p[1]) p++;
    out[n++] = *p;
  }
  out[n] = 0;
}

/* Words Alice won't repeat or mangles, spelled so she says them anyway
 * (latin, matched case-insensitively). */
static void speakable_words(char *t, size_t cap) {
  static const char *const words[][2] = {{"fucking", "фа кинг"}, {NULL, NULL}};
  for (int i = 0; words[i][0]; i++) {
    size_t wl = strlen(words[i][0]), rl = strlen(words[i][1]);
    for (char *p = t; *p;) {
      if (strncasecmp(p, words[i][0], wl) != 0) { p++; continue; }
      size_t tail = strlen(p + wl);
      if ((size_t)(p - t) + rl + tail + 1 > cap) break;
      memmove(p + rl, p + wl, tail + 1);
      memcpy(p, words[i][1], rl);
      p += rl;
    }
  }
}

/* Track names carry noise Alice would read out: "(Remastered 2011)", "- Live". */
static void speakable_title(char *t) {
  char *cut = strstr(t, " (");
  if (!cut) cut = strstr(t, " [");
  if (!cut) cut = strstr(t, " - ");
  if (cut && cut > t) *cut = 0;
}

/* Our phrase waits for Alice's own answer to the voice request: sent while
 * she is still listening, it is overtaken by that answer ("Это The Rasmus…").
 * Sent the moment her answer starts, the new request cuts it off. */
static char say_pending[600];
static unsigned long say_deadline_ms;
static int say_thread_running;
static unsigned long now_ms(void);
static pthread_mutex_t music_mu;
static void mute_alice_start(void);
static void mute_alice_end(void);
static void mute_alice_push(void);

/* Her own answer ends with a follow-up ("Добавить в любимые песни?"), and a
 * phrase typed right after is read inside that music dialog ("Я не нашла
 * музыки…").  A silent neutral request first cuts her answer off and closes
 * that dialog; then "повтори за мной …" (or its like) is repeated as is. */
static void *say_after_reset_thread(void *arg) {
  char *text = arg;
  alice_reset();
  /* Master (her voice) stays at 0 until she has really stopped: idle or
   * listening, stable for 150 ms.  The cut-off does not always take: then she
   * would finish her answer audibly, so repeat it once and keep waiting. */
  unsigned long start = now_ms(), until = start + 12000;
  int retried = 0, stopped = 0;
  usleep(300000);
  while (now_ms() < until) {
    int a = alice_raw_act;
    if (a == 0 || a == 1) {
      usleep(150000);
      a = alice_raw_act;
      if (a == 0 || a == 1) { stopped = 1; break; }
    }
    if (!retried && a == 2 && now_ms() - start > 1200) {
      log_msg("now playing: alice still speaking, cutting off again");
      alice_reset();
      retried = 1;
    }
    usleep(50000);
  }
  if (!stopped) log_msg("now playing: alice still busy after 12 s (activity %d)", alice_raw_act);
  mute_alice_end();
  alice_say(text);
  free(text);
  return NULL;
}

static void say_with_reset(const char *text) {
  char *copy = strdup(text);
  pthread_t th;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if (!copy || pthread_create(&th, &attr, say_after_reset_thread, copy) != 0) {
    free(copy);
    mute_alice_end();
    alice_say(text);
  }
  pthread_attr_destroy(&attr);
}

/* Alice began answering (caller holds music_mu): say ours instead. */
static void say_on_alice_answer(void) {
  if (!say_pending[0]) return;
  mute_alice_push();      /* quasar re-applies her volume as she starts: silence now */
  log_msg("now playing: alice answered, interrupting with ours");
  say_with_reset(say_pending);
  say_pending[0] = 0;
}

static void *say_fallback_thread(void *arg) {
  (void)arg;
  for (;;) {
    usleep(200000);
    pthread_mutex_lock(&music_mu);
    if (!say_pending[0]) break;
    if (now_ms() >= say_deadline_ms) {   /* she never answered: say it anyway */
      log_msg("now playing: no answer from alice, saying it");
      say_with_reset(say_pending);
      say_pending[0] = 0;
      break;
    }
    pthread_mutex_unlock(&music_mu);
  }
  say_thread_running = 0;
  pthread_mutex_unlock(&music_mu);
  return NULL;
}

static void say_instead(const char *text);

/* One of the variants for reply `r`, placeholders filled in. */
static void reply_text_n(int r, char *out, size_t cap, const char *artist, const char *title,
                         const char *count) {
  static int last[NREPLY], init;
  if (!init) { for (int i = 0; i < NREPLY; i++) last[i] = -1; init = 1; }
  const char *tpl = reply_def[r];
  int n = g.nreply[r];
  if (n > 0) {
    int k = rand() % n;
    if (n > 1 && k == last[r]) k = (k + 1 + rand() % (n - 1)) % n;
    last[r] = k;
    if (g.reply[r][k] && g.reply[r][k][0]) tpl = g.reply[r][k];
  }
  char track[520];
  if (artist && artist[0] && title && title[0]) snprintf(track, sizeof track, "%s, %s", artist, title);
  else snprintf(track, sizeof track, "%s", title && title[0] ? title : artist ? artist : "");
  size_t o = 0;
  for (const char *t = tpl; *t && o + 1 < cap;) {
    const char *v = NULL;
    size_t skip = 0;
    if (!strncmp(t, "{track}", 7)) { v = track; skip = 7; }
    else if (!strncmp(t, "{artist}", 8)) { v = artist && artist[0] ? artist : title ? title : ""; skip = 8; }
    else if (!strncmp(t, "{title}", 7)) { v = title && title[0] ? title : artist ? artist : ""; skip = 7; }
    else if (!strncmp(t, "{count}", 7)) { v = count ? count : ""; skip = 7; }
    if (v) {
      o += (size_t)snprintf(out + o, cap - o, "%s", v);
      if (o >= cap) o = cap - 1;
      t += skip;
    } else {
      out[o++] = *t++;
    }
  }
  out[o] = 0;
}
static void reply_text(int r, char *out, size_t cap, const char *artist, const char *title) {
  reply_text_n(r, out, cap, artist, title, NULL);
}

/* «Что сейчас играет?» — Alice reads out the Spotify track (caller holds music_mu). */
static void say_now_playing(void) {
  char json[4096] = "";
  FILE *f = fopen("/data/opt/spotify-connect/monitor/status.json", "r");
  if (f) {
    size_t n = fread(json, 1, sizeof json - 1, f);
    json[n] = 0;
    fclose(f);
  }
  char status[24], title[256], artist[256], text[600];
  status_field(json, "status", status, sizeof status);
  status_field(json, "title", title, sizeof title);
  status_field(json, "artist", artist, sizeof artist);
  speakable_title(title);
  speakable_words(title, sizeof title);
  speakable_words(artist, sizeof artist);
  /* Plain "Это X, Y": words like "играет"/"песня" make Alice take the phrase
   * for a music request ("Включаю X…") instead of repeating it. */
  if (!title[0])
    reply_text(R_NOTHING, text, sizeof text, NULL, NULL);
  else
    reply_text(!strcmp(status, "playing") ? R_NOW_PLAYING : R_NOW_PAUSED, text, sizeof text, artist, title);
  log_msg("now playing -> alice: %s", text);
  say_instead(text);
}

/* «Алиса, перезагрузись»: say so, pause, reboot the speaker a little later
 * (caller holds music_mu).  Refused in the first minutes after boot, so a
 * phrase heard again after start can never loop the speaker through reboots. */
static void journal(const char *kind, const char *fmt, ...);
#define REBOOT_MIN_UPTIME 120
#define REBOOT_DELAY "12"   /* s: Alice's muted answer, then ours, then reboot */
static void reboot_voice(void) {
  double up = 0;
  FILE *f = fopen("/proc/uptime", "r");
  if (f) {
    if (fscanf(f, "%lf", &up) != 1) up = 0;
    fclose(f);
  }
  if (up < REBOOT_MIN_UPTIME) {
    log_msg("reboot: refused, uptime %.0f s", up);
    journal("cmd", "перезагрузка не нужна: колонка включилась %.0f с назад", up);
    char say[200];
    reply_text(R_REBOOT_REFUSED, say, sizeof say, NULL, NULL);
    say_instead(say);
    return;
  }
  log_msg("reboot: in " REBOOT_DELAY " s (uptime %.0f s)", up);
  soloist_cmd("--pause");
  char say[200];
  reply_text(R_REBOOT, say, sizeof say, NULL, NULL);
  say_instead(say);
  pid_t pid = fork();
  if (pid == 0) {
    setsid();
    int null = open("/dev/null", O_RDWR);
    if (null >= 0) {
      dup2(null, 0);
      dup2(null, 1);
      dup2(null, 2);
      if (null > 2) close(null);
    }
    execl("/bin/sh", "sh", "-c", "sleep " REBOOT_DELAY "; sync; /sbin/reboot", (char *)NULL);
    _exit(127);
  }
}

/* Alice answers our phrase instead of her own to the request just heard
 * (caller holds music_mu): her answer is muted, ours follows it. */
static void say_instead(const char *text) {
  mute_alice_start();     /* her own (wrong) answer plays into silence */
  snprintf(say_pending, sizeof say_pending, "%s", text);
  say_deadline_ms = now_ms() + 6000;
  if (say_thread_running) return;
  say_thread_running = 1;
  pthread_t th;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&th, &attr, say_fallback_thread, NULL) != 0) {
    say_thread_running = 0;
    mute_alice_end();
    alice_say(say_pending);
    say_pending[0] = 0;
  }
  pthread_attr_destroy(&attr);
}

/* «Добавь группу Кино в чёрный список»: the artist named in the phrase, or ""
 * for the one playing («эту группу …», «группу в чёрный список»). */
static void blacklist_named(const char *text_in, char *out, size_t cap) {
  char t[256];
  size_t n = strnlen(text_in, sizeof t - 1);
  memcpy(t, text_in, n);
  t[n] = 0;
  normalize_text(t, n);
  out[0] = 0;
  static const char *const nouns[] = {"группу ", "группа ", "исполнителя ", "артиста ",
                                      "певца ", "певицу ", "рэпера ", NULL};
  char *name = NULL;
  for (int i = 0; nouns[i] && !name; i++) {
    char *p = strstr(t, nouns[i]);
    if (p) name = p + strlen(nouns[i]);
  }
  if (!name) return;
  static const char *const tails[] = {" в черный список", " в чёрный список", NULL};
  for (int i = 0; tails[i]; i++) {
    char *e = strstr(name, tails[i]);
    if (e) *e = 0;
  }
  static const char *const fillers[] = {"эту ", "этого ", "этот ", "эта ", "в черный список",
                                        "в чёрный список", NULL};
  for (int again = 1; again;) {
    again = 0;
    for (int i = 0; fillers[i]; i++) {
      size_t fl = strlen(fillers[i]);
      if (!strncmp(name, fillers[i], fl)) { name += fl; again = 1; }
    }
  }
  while (*name == ' ') name++;
  n = strlen(name);
  while (n && name[n - 1] == ' ') name[--n] = 0;
  snprintf(out, cap, "%s", name);
}

/* «… в чёрный список»: alice-blacklist.sh adds the artist / track (and skips
 * it right away); Alice says what was added instead of her own answer
 * (caller holds music_mu). */
static void blacklist_voice(int artist_kind, const char *text) {
  char named[200] = "";
  if (artist_kind) blacklist_named(text, named, sizeof named);

  char json[4096] = "";
  FILE *f = fopen("/data/opt/spotify-connect/monitor/status.json", "r");
  if (f) {
    size_t n = fread(json, 1, sizeof json - 1, f);
    json[n] = 0;
    fclose(f);
  }
  char title[256], artist[256], say[600];
  status_field(json, "title", title, sizeof title);
  status_field(json, "artist", artist, sizeof artist);
  speakable_title(title);
  speakable_words(title, sizeof title);
  speakable_words(artist, sizeof artist);

  const char *what = artist_kind ? (named[0] ? named : "current") : "current";
  int playing = title[0] != 0;
  if (artist_kind && named[0])
    reply_text(R_BL_ARTIST, say, sizeof say, named, NULL);
  else if (!playing)
    reply_text(R_NOTHING, say, sizeof say, NULL, NULL);
  else if (artist_kind)
    reply_text(R_BL_ARTIST, say, sizeof say, artist[0] ? artist : title, NULL);
  else
    reply_text(R_BL_TRACK, say, sizeof say, artist, title);
  log_msg("blacklist %s: %s -> alice: %s", artist_kind ? "artist" : "track", what, say);

  if (playing || named[0]) {
    pid_t pid = fork();
    if (pid == 0) {
      int null = open("/dev/null", O_RDWR);
      if (null >= 0) { dup2(null, 0); dup2(null, 1); dup2(null, 2); if (null > 2) close(null); }
      execl("/data/opt/spotify-connect/alice-blacklist.sh", "alice-blacklist",
            artist_kind ? "add-artist" : "add-track", what, (char *)NULL);
      _exit(127);
    }
  }
  say_instead(say);
}

static char seen[64][80];
static unsigned seen_at[64];
static int seen_n;

static unsigned now_sec(void) {
  return (unsigned)now_s();
}

static int already_seen_recent(const char *req, unsigned ttl_sec) {
  unsigned t = now_sec();
  for (int i = 0; i < 64; i++) {
    if (seen[i][0] && !strcmp(seen[i], req) && (t - seen_at[i]) < ttl_sec) return 1;
  }
  return 0;
}

static void remember(const char *req) {
  int i = seen_n % 64;
  snprintf(seen[i], sizeof seen[i], "%s", req);
  seen_at[i] = now_sec();
  seen_n++;
}

static void log_msg(const char *fmt, ...) {
  FILE *log = fopen("/tmp/alice-voice-proxy.log", "a");
  if (!log) return;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(log, fmt, ap);
  va_end(ap);
  fputc('\n', log);
  fclose(log);
}

/* Voice journal for the monitor page: one line per event, "epoch\tkind\ttext".
 * kind: say (the user's phrase), alice (her reply), play (what we searched and
 * why), cmd (a command and what it did).  The monitor reads and clears it. */
#define JOURNAL "/tmp/alice-voice-journal.log"
#define JOURNAL_MAX (96 * 1024)

/* Keep the newest half once the journal outgrows JOURNAL_MAX (RAM-backed /tmp). */
static void journal_trim(void) {
  FILE *f = fopen(JOURNAL, "rb");
  if (!f) return;
  static char buf[JOURNAL_MAX / 2];
  fseek(f, -(long)sizeof buf, SEEK_END);
  size_t n = fread(buf, 1, sizeof buf, f);
  fclose(f);
  char *start = memchr(buf, '\n', n);       /* drop the cut-off first line */
  if (!start) return;
  start++;
  f = fopen(JOURNAL, "wb");
  if (!f) return;
  fwrite(start, 1, n - (size_t)(start - buf), f);
  fclose(f);
}

static void journal(const char *kind, const char *fmt, ...) {
  char text[600];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(text, sizeof text, fmt, ap);
  va_end(ap);
  for (char *c = text; *c; c++)
    if (*c == '\t' || *c == '\n' || *c == '\r') *c = ' ';
  FILE *f = fopen(JOURNAL, "a");
  if (!f) return;
  fprintf(f, "%lu\t%s\t%s\n", (unsigned long)now_s(), kind, text);
  long size = ftell(f);
  fclose(f);
  if (size > JOURNAL_MAX) journal_trim();
}

/* The user's phrase, once (frames repeat it through every dialog stage). */
static char journal_said[512];
static time_t journal_said_at;
static void journal_say(const char *text) {
  time_t now = now_s();
  if (!strcmp(text, journal_said) && now - journal_said_at < 6) return;
  snprintf(journal_said, sizeof journal_said, "%s", text);
  journal_said_at = now;
  journal("say", "%s", text);
}

/* What we searched, from which source and (optionally) why. */
static void journal_play(const char *q, const char *src, const char *why) {
  int artist = !strncmp(q, "@artist ", 8);
  char shown[600];
  if (!strncmp(q, "@album ", 7)) {   /* "@album Y|X" -> "альбом Y, X" */
    const char *bar = strchr(q + 7, '|');
    if (bar) snprintf(shown, sizeof shown, "альбом %.*s, %s", (int)(bar - q - 7), q + 7, bar + 1);
    else snprintf(shown, sizeof shown, "альбом %s", q + 7);
    q = shown;
  }
  journal("play", "%s%s — %s%s%s%s", artist ? q + 8 : q, artist ? " (исполнитель)" : "", src,
          why ? " (" : "", why ? why : "", why ? ")" : "");
}

/* The user's phrase as last heard while Alice was listening (journal only). */
static char listen_text[512];

/* Alice's reply without speech markup ("<[/domain]>", "<speaker …>"), once. */
static void journal_alice(const char *tts) {
  static char last[440];
  static time_t last_at;
  char t[440];
  size_t n = 0;
  for (const char *r = tts; *r && n + 1 < sizeof t; r++) {
    if (*r == '<') {
      const char *e = strchr(r, '>');
      if (e) { r = e; continue; }
    }
    t[n++] = *r;
  }
  t[n] = 0;
  while (n && (t[n - 1] == ' ')) t[--n] = 0;
  time_t now = now_s();
  if (!n || (!strcmp(t, last) && now - last_at < 15)) return;
  snprintf(last, sizeof last, "%s", t);
  last_at = now;
  journal("alice", "%s", t);
}

/* A command action as the journal shows it. */
static const char *cmd_label(const char *action) {
  static char buf[64];
  static const char *const names[][2] = {
      {"__PAUSE__", "пауза"}, {"__RESUME__", "продолжить"}, {"__NEXT__", "следующий трек"},
      {"__PREVIOUS__", "предыдущий трек"}, {"__PREV__", "предыдущий трек"},
      {"__CLEAR_QUEUE__", "очистить очередь"}, {"__WRONG__", "включила не то — ищу заново"}, {"__SEEK_START__", "в начало трека"},
      {"__SEEK_END__", "в конец трека"}, {"__SEEK_FORWARD__", "перемотка вперёд"},
      {"__SEEK_BACK__", "перемотка назад"}, {"__LIKE__", "лайк"},
      {"__FAVORITES__", "избранное"}, {"__NOW_PLAYING__", "что играет"},
      {"__HISTORY_BACK__", "трек из истории"}, {"__BLACKLIST_ARTIST__", "исполнителя в чёрный список"},
      {"__BLACKLIST_TRACK__", "трек в чёрный список"}, {"__REBOOT__", "перезагрузка колонки"},
      {NULL, NULL}};
  for (int i = 0; names[i][0]; i++)
    if (!strcmp(action, names[i][0])) return names[i][1];
  if (!strncmp(action, "__SEEK_FORWARD_", 15)) {
    snprintf(buf, sizeof buf, "вперёд на %.*s с", (int)strcspn(action + 15, "_"), action + 15);
    return buf;
  }
  if (!strncmp(action, "__SEEK_BACK_", 12)) {
    snprintf(buf, sizeof buf, "назад на %.*s с", (int)strcspn(action + 12, "_"), action + 12);
    return buf;
  }
  return action;
}

static int is_transport(const char *q) {
  if (!strncmp(q, "__SEEK_FORWARD_", 15) || !strncmp(q, "__SEEK_BACK_", 12)) return 1;
  return !strcmp(q, "__LIKE__") || !strcmp(q, "__FAVORITES__") || !strcmp(q, "__NOW_PLAYING__") ||
         !strcmp(q, "__HISTORY_BACK__") || !strcmp(q, "__BLACKLIST_ARTIST__") ||
         !strcmp(q, "__BLACKLIST_TRACK__") || !strcmp(q, "__REBOOT__") ||
         !strcmp(q, "__PAUSE__") || !strcmp(q, "__NEXT__") || !strcmp(q, "__PREVIOUS__") ||
         !strcmp(q, "__PREV__") || !strcmp(q, "__RESUME__") ||
         !strcmp(q, "__CLEAR_QUEUE__") || !strcmp(q, "__SEEK_START__") || !strcmp(q, "__SEEK_END__") ||
         !strcmp(q, "__SEEK_FORWARD__") || !strcmp(q, "__SEEK_BACK__") || !strcmp(q, "__WRONG__");
}

/* Incomplete ASR crumbs — cancel the stock path but don't search Spotify yet. */
static int is_play_crumb(const char *q) {
  static const char *crumbs[] = {
      "__PARTIAL__", "включи", "поставь", "запусти", "сыграй", "послушай", "давай", "музыку",
      "песню", "трек", "группу", "группа", "исполнителя", "альбом", "плейлист", NULL};
  for (int i = 0; crumbs[i]; i++)
    if (!strcmp(q, crumbs[i])) return 1;
  return 0;
}

/* A completed Spotify handoff must end the receiver's current Alice IPC
 * session.  Mutating the ASR text and keeping that session alive leaves
 * aliced waiting for a response forever. */

/* --- Music volume ---------------------------------------------------------
 * Soloist plays through its own softvol "Spotify" (run.sh, asound-spotify.conf),
 * not the shared "Master" that carries Alice's voice.  Master stays Alice's
 * volume (her "громче/тише", the buttons, the boot guard); the music follows it
 * here, is ducked by settings.duck_db while Alice is in a dialog, and is NOT
 * touched when Master is held at 0 to silence only her (now-playing). */
#define SPOTIFY_CTL "Spotify"
#define DUCK_TAIL_MS 700           /* stay ducked this long after she goes idle */
#define DIALOG_MAX_MS 60000        /* a dialog state stuck longer than this is stale */

static volatile int alice_act;               /* in a dialog: 1 = yes, 0 = idle */
static volatile unsigned long alice_act_ms;  /* when that was last seen */

/* AliceState activity -> dialog flag.  Listening (1) or speaking (2) is a
 * dialog; "busy" (3) alone is also her background requests (the muted native
 * player fetching its next track), so it only continues a dialog. */
static void note_alice_activity(uint64_t activity) {
  static int last = -1;
  if ((int)activity != last) {
    log_msg("alice activity [%lus]: %d -> %llu", now_ms() / 1000, last, (unsigned long long)activity);
    last = (int)activity;
  }
  if (activity == 1 || activity == 2) {
    alice_act = 1;
    alice_act_ms = now_ms();
  } else if (activity == 0) {
    /* Idle frames also come with no dialog at all (the background radio's
     * track switches): only the end of a dialog starts the duck tail. */
    if (alice_act) {
      alice_act = 0;
      alice_act_ms = now_ms();
    }
  } else if (alice_act) {
    alice_act_ms = now_ms();              /* busy (3) continues a dialog */
  }
}
static volatile int mute_level = -1;         /* Master level to restore; -1 = not muted */

/* Mixer access straight through the ALSA control device: no amixer process
 * per read/write (the follower polls several times a second, the mute every
 * 50 ms, and fork+exec bursts competed with the wake-word spotter).
 * `ctl` is the simple name ("Master", "Spotify"); values are 0..100 % of the
 * control's raw range, like amixer shows them. */
#ifndef ALICE_TEST_MIXER
static pthread_mutex_t mixer_mu = PTHREAD_MUTEX_INITIALIZER;
static int mixer_fd = -1;

static int mixer_info(const char *ctl, struct snd_ctl_elem_info *info) {
  if (mixer_fd < 0) mixer_fd = open("/dev/snd/controlC0", O_RDWR | O_CLOEXEC);
  if (mixer_fd < 0) return -1;
  memset(info, 0, sizeof *info);
  info->id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
  snprintf((char *)info->id.name, sizeof info->id.name, "%s Playback Volume", ctl);
  if (ioctl(mixer_fd, SNDRV_CTL_IOCTL_ELEM_INFO, info) < 0) return -1;
  if (info->type != SNDRV_CTL_ELEM_TYPE_INTEGER ||
      info->value.integer.max <= info->value.integer.min) return -1;
  return 0;
}

static int read_mixer_pct(const char *ctl) {
  pthread_mutex_lock(&mixer_mu);
  struct snd_ctl_elem_info info;
  struct snd_ctl_elem_value val;
  int pct = -1;
  if (mixer_info(ctl, &info) == 0) {
    memset(&val, 0, sizeof val);
    val.id = info.id;
    if (ioctl(mixer_fd, SNDRV_CTL_IOCTL_ELEM_READ, &val) == 0) {
      long min = info.value.integer.min, range = info.value.integer.max - min;
      pct = (int)(((val.value.integer.value[0] - min) * 100 + range / 2) / range);
    }
  }
  pthread_mutex_unlock(&mixer_mu);
  return (pct >= 0 && pct <= 100) ? pct : -1;
}

/* 0 on success (non-zero: e.g. the control does not exist yet). */
static int write_mixer_pct(const char *ctl, int pct) {
  pthread_mutex_lock(&mixer_mu);
  struct snd_ctl_elem_info info;
  struct snd_ctl_elem_value val;
  int rc = -1;
  if (mixer_info(ctl, &info) == 0) {
    long min = info.value.integer.min, range = info.value.integer.max - min;
    long raw = min + (range * pct + 50) / 100;
    memset(&val, 0, sizeof val);
    val.id = info.id;
    for (unsigned i = 0; i < info.count && i < 128; i++) val.value.integer.value[i] = raw;
    rc = ioctl(mixer_fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &val) < 0 ? -1 : 0;
  }
  pthread_mutex_unlock(&mixer_mu);
  return rc;
}
#endif

static int read_master_pct(void) { return read_mixer_pct("Master"); }
static void write_master_pct(int pct) { write_mixer_pct("Master", pct); }

static int alice_in_dialog(void) {
  unsigned long age = now_ms() - alice_act_ms;
  return alice_act ? age < DIALOG_MAX_MS : age < DUCK_TAIL_MS;
}

static int ducked(int level) {
  int d = g.loaded ? g.duck_db : 8;
  /* softvol: 255 steps over 51 dB -> 1% = 0.51 dB */
  int pct = level - (d * 100 + 25) / 51;
  return pct < 0 ? 0 : pct;
}

/* alice-queue-drain.sh empties the Spotify queue by skipping through it: the
 * skipped tracks must not be heard.  The flag holds its expiry (epoch s), so a
 * killed script can't leave the music muted. */
#define QUEUE_DRAIN_FLAG "/tmp/soloist-draining"

static int queue_draining(void) {
  FILE *f = fopen(QUEUE_DRAIN_FLAG, "re");
  if (!f) return 0;
  long until = 0;
  if (fscanf(f, "%ld", &until) != 1) until = 0;
  fclose(f);
  return until > now_s();
}

static void *music_volume_thread(void *arg) {
  (void)arg;
  int level = -1, written = -1, was_dialog = 0, was_quiet = 0;
  unsigned long refreshed = 0;
  while (running) {
    int dialog = alice_in_dialog();
    int quiet = queue_draining();
    if (quiet != was_quiet)
      log_msg("music volume [%lus]: %s", now_ms() / 1000, quiet ? "muted (queue drain)" : "queue drain over");
    int m = mute_level >= 0 ? mute_level : read_master_pct();
    if (m >= 0) {
      if (level >= 0 && m != level)
        log_msg("music volume [%lus]: Alice %d%% -> %d%%%s", now_ms() / 1000, level, m, dialog ? " (during dialog)" : "");
      level = m;
    }
    if (level >= 0) {
      int target = quiet ? 0 : dialog ? ducked(level) : level;
      if (target != written || dialog != was_dialog || quiet != was_quiet || now_ms() - refreshed > 10000) {
        if (write_mixer_pct(SPOTIFY_CTL, target) == 0) {
          if (target != written)
            log_msg("music volume [%lus]: %d%%%s", now_ms() / 1000, target, dialog ? " (ducked)" : "");
          written = target;
          refreshed = now_ms();
        } else {
          written = -1;                  /* control not created yet: retry */
        }
      }
    }
    was_dialog = dialog;
    was_quiet = quiet;
    if (dialog) {
      usleep(150000);
    } else {
      /* Idle: re-read Master every 0.6 s, but duck as soon as Alice wakes
       * and mute as soon as a queue drain starts. */
      for (int i = 0; i < 6 && !alice_in_dialog() && queue_draining() == quiet; i++) usleep(100000);
    }
  }
  return NULL;
}

static void start_music_volume(void) {
  pthread_t th;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&th, &attr, music_volume_thread, NULL) != 0)
    log_msg("music volume: thread failed");
  pthread_attr_destroy(&attr);
}

/* «Что сейчас играет»: Alice's own answer must not be heard.  Master carries
 * only her voice now, so hold it at 0 until our phrase is due (quasar may
 * re-apply her level, so keep pushing it down); the music keeps its own level.
 * Safety cap: MUTE_MAX_MS. */
#define MUTE_MAX_MS 15000
static volatile unsigned long mute_until_ms;
static pthread_mutex_t mute_mu = PTHREAD_MUTEX_INITIALIZER;  /* no 0 after the restore */

static void *mute_thread(void *arg) {
  (void)arg;
  while (mute_level >= 0 && now_ms() < mute_until_ms) {
    pthread_mutex_lock(&mute_mu);
    if (mute_level >= 0) {
      int m = read_master_pct();
      if (m > 0) log_msg("now playing [%lums]: quasar raised Master to %d%%, back to 0", now_ms(), m);
      if (m != 0) write_master_pct(0);
    }
    pthread_mutex_unlock(&mute_mu);
    usleep(20000);
  }
  mute_alice_end();       /* the safety cap ran out */
  return NULL;
}

static void mute_alice_start(void) {
  if (mute_level >= 0) return;
  int level = read_master_pct();
  if (level <= 0) return;
  mute_until_ms = now_ms() + MUTE_MAX_MS;
  mute_level = level;
  write_master_pct(0);
  log_msg("now playing: muted alice (Master %d%% -> 0)", level);
  pthread_t th;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&th, &attr, mute_thread, NULL) != 0) mute_until_ms = 0;
  pthread_attr_destroy(&attr);
}

static void mute_alice_push(void) {
  pthread_mutex_lock(&mute_mu);
  if (mute_level >= 0) write_master_pct(0);
  pthread_mutex_unlock(&mute_mu);
}

static void mute_alice_end(void) {
  pthread_mutex_lock(&mute_mu);
  int level = mute_level;
  if (level < 0) {
    pthread_mutex_unlock(&mute_mu);
    return;
  }
  write_master_pct(level);
  mute_level = -1;
  pthread_mutex_unlock(&mute_mu);
  log_msg("now playing: unmuted (Master -> %d%%)", level);
}

/* Lowercase a copy (ASCII + UTF-8 Cyrillic А-Я) so stem matching is case-safe. */
static void vol_lower_copy(char *dst, size_t dn, const char *src) {
  size_t n = strnlen(src, dn - 1);
  memcpy(dst, src, n);
  dst[n] = 0;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)dst[i];
    if (c >= 'A' && c <= 'Z') dst[i] = (char)(c - 'A' + 'a');
  }
  for (size_t i = 0; i + 1 < n; i++) {
    unsigned char c0 = (unsigned char)dst[i], c1 = (unsigned char)dst[i + 1];
    if (c0 == 0xD0 && c1 >= 0x90 && c1 <= 0x9F) {
      dst[i + 1] = (char)(c1 + 0x20);
      i++;
    } else if (c0 == 0xD0 && c1 >= 0xA0 && c1 <= 0xAF) {
      dst[i] = (char)0xD1;
      dst[i + 1] = (char)(c1 - 0x20);
      i++;
    }
  }
}

static int is_volume_phrase(const char *low) {
  return strstr(low, "громч") || strstr(low, "тише") || strstr(low, "громкост") ||
         strstr(low, "убавь") || strstr(low, "прибавь");
}

/* Current dialog id (AliceState field 7) and the dialog we last launched a
 * search for, so the request-text path and the TTS-announcement fallback never
 * double-fire for the same request. */
static char cur_dialog[80];
static char last_search_dialog[80];

/* The user's own music request (recognised words reduced to a query) and the
 * dialog it was heard in. */
static char pending[256];
static char pending_dialog[80];
static unsigned pending_at;
static int pending_sent;
static int pending_repeat;   /* same words as the previous request: Alice's pick was wrong */
static int pending_soft;     /* taken from a phrase without "включи" that Alice silently
                              * acts on: a non-music reply from her cancels it */

/* The last music request we acted on, in the user's words (not Alice's):
 * last_req / last_req_at, declared with wrong_pick(). */

/* The frame threads and the announcement-grace timer share the state above. */
static pthread_mutex_t music_mu = PTHREAD_MUTEX_INITIALIZER;

/* A request waiting a moment for Alice's answer: her "Включаю …" announcement
 * once she commits, or any answer at all (none comes without internet). */
static int grace_armed;
static int grace_thread_running;
static unsigned long grace_deadline_ms;
static char grace_dialog[80];
static const char *grace_why;
static const char WAIT_ANNOUNCE[] = "no announcement";  /* committed, "Включаю …" lags */
static const char WAIT_ANSWER[] = "no answer";          /* no reply at all (no internet) */

static int repeat_window(void) {
  return g.loaded ? g.repeat_window_sec : 300;
}

static int announcement_wait(void) {
  return g.loaded ? g.announcement_wait_ms : 2500;
}

static int answer_wait(void) {
  return g.loaded ? g.answer_wait_ms : 5000;
}

/* Time straight through the 32-bit clock_gettime syscall.  musl (the zig
 * toolchain) tries clock_gettime64 first on EVERY call; this 4.9 kernel has no
 * such syscall and logs each attempt with a register dump
 * ("alice-voice-pro[..]: syscall 403"), dozens a second from the volume threads
 * alone.  That flood turned dmesg and syslog over every few minutes, so when
 * soloist died nothing about it was left in either. */
#ifndef SYS_clock_gettime
#  ifdef SYS_clock_gettime32          /* musl >= 1.2 names the 32-bit one so */
#    define SYS_clock_gettime SYS_clock_gettime32
#  elif defined(__arm__)
#    define SYS_clock_gettime 263
#  endif
#endif
static void clock_now(int clk, long out[2]) {
  if (syscall(SYS_clock_gettime, clk, out) != 0) out[0] = out[1] = 0;
}

static unsigned long now_ms(void) {
  long t[2];
  clock_now(CLOCK_MONOTONIC, t);
  return (unsigned long)t[0] * 1000UL + (unsigned long)t[1] / 1000000UL;
}

/* Wall clock in seconds (what time(NULL) gave; same syscall reason). */
static long now_s(void) {
  long t[2];
  clock_now(CLOCK_REALTIME, t);
  return t[0];
}

/* Dialog the current request belongs to: frames do not always carry the id. */
static const char *dialog_key(void) {
  return cur_dialog[0] ? cur_dialog : pending_dialog;
}

static void note_request(void) {
  snprintf(last_req, sizeof last_req, "%s", pending);
  last_req_at = now_sec();
}

/* ASR masks profanity ("н****"), Alice masks names ("#####"); such a word only
 * derails the search. */
static void strip_masked(char *out, size_t cap, const char *in) {
  size_t n = 0;
  const char *p = in;
  while (*p) {
    while (*p == ' ') p++;
    const char *w = p;
    while (*p && *p != ' ') p++;
    size_t wl = (size_t)(p - w);
    if (!wl || memchr(w, '*', wl) || memchr(w, '#', wl)) continue;
    if (n && n + 1 < cap) out[n++] = ' ';
    for (size_t i = 0; i < wl && n + 1 < cap; i++) out[n++] = w[i];
  }
  out[n] = 0;
  if (!out[0]) snprintf(out, cap, "%s", in);  /* never send an empty query */
}

/* Every word of `small` occurs as a whole word in `big`. */
static int words_within(const char *small, const char *big) {
  const char *p = small;
  int words = 0;
  while (*p) {
    while (*p == ' ') p++;
    const char *w = p;
    while (*p && *p != ' ') p++;
    size_t wl = (size_t)(p - w);
    if (!wl || wl >= 128) continue;
    char word[128];
    memcpy(word, w, wl);
    word[wl] = 0;
    words++;
    const char *b = big;
    int found = 0;
    while (!found && (b = strstr(b, word)) != NULL) {
      if ((b == big || b[-1] == ' ') && (b[wl] == 0 || b[wl] == ' ')) found = 1;
      b++;
    }
    if (!found) return 0;
  }
  return words;
}

/* The user asked for the same thing again: the same words once masked ones are
 * dropped, or a request of 2+ words repeated inside a longer one. */
static int same_request(const char *a, const char *b) {
  char sa[256], sb[256];
  strip_masked(sa, sizeof sa, a);
  strip_masked(sb, sizeof sb, b);
  if (!strcmp(sa, sb)) return 1;
  int short_a = strlen(sa) <= strlen(sb);
  return words_within(short_a ? sa : sb, short_a ? sb : sa) >= 2;
}

/* Search for exactly what the user said in this request (caller holds music_mu). */
static void search_by_request(const char *why, const char *dlg) {
  if (!pending[0] || pending_sent) return;
  pending_sent = 1;
  grace_armed = 0;
  if (already_seen_recent(pending, 6)) return;
  remember(pending);
  snprintf(last_search_dialog, sizeof last_search_dialog, "%s", dlg);
  note_request();
  char q[256];
  strip_masked(q, sizeof q, pending);
  log_msg("search by request (%s) dialog=%s q=%s", why, dlg, q);
  if (listen_text[0]) { journal_say(listen_text); listen_text[0] = 0; }
  const char *ru = !strcmp(why, "repeat") ? "повтор"
                 : !strcmp(why, "alice did not play it") ? "Алиса не включила"
                 : !strcmp(why, "no announcement") ? "Алиса не назвала трек"
                 : !strcmp(why, "no answer") ? "Алиса не ответила"
                 : !strcmp(why, "empty announcement") ? "пустой анонс"
                 : !strcmp(why, "masked announcement") ? "Алиса скрыла название" : NULL;
  journal_play(q, "по твоим словам", ru);
  spotify_search_play(q);
}

static void *grace_thread(void *arg) {
  (void)arg;
  for (;;) {
    pthread_mutex_lock(&music_mu);
    if (!grace_armed) {
      grace_thread_running = 0;
      pthread_mutex_unlock(&music_mu);
      return NULL;
    }
    if (now_ms() >= grace_deadline_ms) {
      grace_armed = 0;
      grace_thread_running = 0;
      if (!pending_sent)                 /* Alice never announced / answered it */
        search_by_request(grace_why, grace_dialog);
      pthread_mutex_unlock(&music_mu);
      return NULL;
    }
    pthread_mutex_unlock(&music_mu);
    usleep(100000);
  }
}

/* Wait `wait` ms for Alice, then fall back to the user's words (caller holds
 * music_mu).  An existing wait for the same dialog and reason is kept unless
 * `restart` (the request text changed: count from the user's last words). */
static void arm_request_wait(const char *dlg, int wait, const char *why, int restart) {
  int same = grace_armed && grace_why == why && !strcmp(grace_dialog, dlg);
  if (same && !restart) return;
  grace_armed = 1;
  grace_why = why;
  snprintf(grace_dialog, sizeof grace_dialog, "%s", dlg);
  grace_deadline_ms = now_ms() + (unsigned long)wait;
  if (!same)
    log_msg("waiting %d ms for Alice (%s) dialog=%s q=%s", wait, why, dlg, pending);
  if (grace_thread_running) return;
  grace_thread_running = 1;
  pthread_t th;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&th, &attr, grace_thread, NULL) != 0) {
    grace_thread_running = 0;
    grace_armed = 0;
    if (why == WAIT_ANNOUNCE) search_by_request("request", dlg);  /* committed: go now */
  }
  pthread_attr_destroy(&attr);
}

/* The last phrase heard that we did NOT take for a music request (no "включи"
 * etc.).  If Alice then answers it with "Включаю …", it was one after all. */
static char heard[256];
static char heard_dialog[80];
static unsigned heard_at;

static void note_heard(const char *text, uint64_t activity) {
  char t[256];
  size_t n = strnlen(text, sizeof t - 1);
  memcpy(t, text, n);
  t[n] = 0;
  normalize_text(t, n);
  for (char *c = t; *c; c++)
    if (*c == ',') *c = ' ';
  char *body = t;
  while (*body == ' ') body++;
  for (int i = 0; i < g.n_act; i++) {
    size_t pl = strlen(g.act_prefix[i]);
    if (strncmp(body, g.act_prefix[i], pl) == 0) body += pl;
    while (*body == ' ') body++;
  }
  if (!*body) return;
  static int logged;            /* this phrase is already in the log */
  if (strcmp(body, heard) != 0) logged = 0;
  if (activity >= 2 && !logged) {
    log_msg("heard-other activity=%llu dialog=%s text=%s", (unsigned long long)activity,
            cur_dialog, body);
    logged = 1;
  }
  snprintf(heard, sizeof heard, "%s", body);
  if (cur_dialog[0]) snprintf(heard_dialog, sizeof heard_dialog, "%s", cur_dialog);
  else if (now_sec() - heard_at >= 20) heard_dialog[0] = 0;
  heard_at = now_sec();
}

/* «небо славян громкость 5», «громкость 5 небо славян»: Alice sets the volume
 * and plays the rest — the volume clause is not part of the name. */
static void drop_volume_clause(char *s) {
  char *v = strstr(s, "громкост");
  if (!v || (v > s && v[-1] != ' ')) return;
  if (v > s) {                                  /* name first: cut it off */
    while (v > s && (v[-1] == ' ' || v[-1] == ',')) v--;
    size_t il = strlen(" и");
    if ((size_t)(v - s) >= il && !strncmp(v - il, " и", il)) v -= il;
    *v = 0;
    return;
  }
  char *r = strchr(v, ' ');                     /* «громкость» */
  if (r && starts_with(r + 1, "на ")) r += strlen(" на");
  if (r) r = strchr(r + 1, ' ');                /* its value */
  if (!r) { *s = 0; return; }
  while (*r == ' ' || *r == ',') r++;
  if (starts_with(r, "и ")) r += strlen("и ");
  memmove(s, r, strlen(r) + 1);
}

/* Make the phrase heard in this dialog the pending request, minus lead-ins
 * ("хочу послушать", "найди", "включи", "песню" …).  `soft`: Alice has not
 * confirmed it is music yet, so skip questions and smart-home phrases.
 * Returns 1 if adopted (caller holds music_mu). */
static int adopt_heard_request(int soft) {
  if (!heard[0]) return 0;
  const char *dk = cur_dialog[0] ? cur_dialog : heard_dialog;
  int same = cur_dialog[0] && heard_dialog[0] ? !strcmp(heard_dialog, cur_dialog)
                                               : now_sec() - heard_at < 20;
  if (!same) return 0;
  if (soft) {
    /* Alice also carries out commands silently ("громче", "сделай тише",
     * "следующую"): only a title-like phrase of 2+ words counts. */
    if (!strchr(heard, ' ') || is_volume_phrase(heard)) return 0;
    static const char *const cmds[] = {
        "сделай", "выключи", "останови", "установи", "поставь", "включи", "открой",
        "запомни", "напомни", "позвони", "отмени", "стоп", "хватит", "добавь", "убери",
        "перемотай", "промотай", "повтори", "давай", "следующ", "предыдущ", "дальше",
        "назад", "продолж", "пауза", "лайк", "мне нравится", "не нравится", "алиса", NULL};
    for (int i = 0; cmds[i]; i++)
      if (starts_with(heard, cmds[i])) return 0;
    for (int i = 0; i < g.nq; i++)
      if (starts_with(heard, g.questions[i])) return 0;
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s", heard);
    for (char *save = NULL, *word = strtok_r(tmp, " ", &save); word; word = strtok_r(NULL, " ", &save))
      for (int i = 0; i < g.nhome; i++)
        if (starts_with(word, g.home[i])) return 0;
  }
  static const char *const leads[] = {
      "хочу послушать ", "давай послушаем ", "можешь включить ", "можешь поставить ",
      "хочу ", "найди ", "поищи ", "включи-ка ", "поставь-ка ", "включите ", "поставьте ",
      "вруби ", "врубай ", "заведи ", "проиграй ", "воспроизведи ", "послушать ",
      "песня ", "трек ", NULL};
  char req[sizeof heard];
  snprintf(req, sizeof req, "%s", heard);
  drop_volume_clause(req);
  const char *w = req;
  int artist = 0;                /* «группа X», «исполнителя X»: search the artist */
  int album = 0;                 /* «альбом Y»: that album */
  for (int changed = 1; changed;) {
    changed = 0;
    for (int i = 0; leads[i]; i++)
      if (starts_with(w, leads[i])) { w += strlen(leads[i]); changed = 1; }
    for (int i = 0; i < g.nverbs; i++)
      if (starts_with(w, g.verbs[i])) { w += strlen(g.verbs[i]); changed = 1; }
    size_t an = artist_noun(w);  /* before the music nouns: they include «группу » */
    if (an) { w += an; artist = changed = 1; }
    for (int i = 0; i < g.nmusic; i++)
      if (starts_with(w, g.music_prefix[i])) {
        album |= !strcmp(g.music_prefix[i], "альбом ");
        w += strlen(g.music_prefix[i]);
        changed = 1;
      }
    while (*w == ' ') w++;
  }
  if (strlen(w) < 2) return 0;
  if (!album_marker(w, artist, album, pending, sizeof pending))
    snprintf(pending, sizeof pending, "%s%s", artist ? "@artist " : "", w);
  snprintf(pending_dialog, sizeof pending_dialog, "%s", dk);
  pending_at = now_sec();
  pending_sent = 0;
  pending_soft = soft;
  int rw = repeat_window();
  pending_repeat = rw > 0 && last_req[0] && same_request(pending, last_req) &&
                   now_sec() - last_req_at < (unsigned)rw;
  heard[0] = 0;
  log_msg("adopted request (no play verb%s) dialog=%s q=%s%s", soft ? ", alice silent" : "",
          dk, pending, pending_repeat ? " (repeat)" : "");
  return 1;
}

/* The newest words are not a music request (a command, a question, a changed
 * mind): stop waiting to search the older ones. */
static void drop_request_wait(void) {
  if (!grace_armed) return;
  grace_armed = 0;
  pending_sent = 1;
  log_msg("dropped wait: not a music request any more dialog=%s q=%s", grace_dialog, pending);
}

/* Our own "повтори за мной …" request (alice_say, any of its forms) and its
 * dialog: never a user's music request, whatever Alice makes of it. */
static char own_dialog[80];
static int is_own_request(const char *text) {
  char low[160], pre[160];
  vol_lower_copy(low, sizeof low, text);
  int n = g.nsay;
  if (n <= 0) while (say_prefix_def[n]) n++;
  for (int i = 0; i < n; i++) {
    vol_lower_copy(pre, sizeof pre, g.nsay > 0 ? g.say_prefix[i] : say_prefix_def[i]);
    if (pre[0] && starts_with(low, pre)) return 1;
  }
  return 0;
}

static void handle_music_text(const char *text, uint64_t activity) {
  if (is_own_request(text)) {
    if (cur_dialog[0]) snprintf(own_dialog, sizeof own_dialog, "%s", cur_dialog);
    return;
  }
  char *q = music_query(text);
  if (!q) {
    note_heard(text, activity);
    if (activity == 1) {
      drop_request_wait();
    } else if (activity == 3 && g.use_announcement && g.announcement_priority &&
               announcement_wait() > 0) {
      /* Alice is carrying out a phrase without "включи" ("stadler city of
       * living dead") and says nothing: likely music she would play herself.
       * Give her the usual moment to announce it, then search the words. */
      const char *dk = cur_dialog[0] ? cur_dialog : heard_dialog;
      int have = pending[0] && !strcmp(pending_dialog, dk);
      if (!have && strcmp(dk, last_search_dialog) != 0 && adopt_heard_request(1))
        arm_request_wait(dk, announcement_wait(), WAIT_ANNOUNCE, 0);
    }
    return;
  }
  heard[0] = 0;   /* a music request: the request text path has it */

  if (is_transport(q)) {
    if (activity == 1) drop_request_wait();
    static char last_t[24];
    static unsigned last_t_at;
    unsigned tnow = now_sec();
    /* Quasar publishes the same final phrase through several activity stages;
     * keep one transport action for the whole recognition transaction. */
    if (!strcmp(last_t, q) && tnow - last_t_at < 4) {
      free(q);
      return;
    }
    snprintf(last_t, sizeof last_t, "%s", q);
    last_t_at = tnow;
    log_msg("heard activity=%llu text=%s -> %s", (unsigned long long)activity, text, q);
    journal_say(text);
    journal("cmd", "%s", cmd_label(q));
    if (!strcmp(q, "__LIKE__")) {    /* like the current track; don't touch playback */
      spotify_like();
      free(q);
      return;
    }
    if (!strcmp(q, "__HISTORY_BACK__")) {   /* «N песен назад»: that track from the history */
      int n = spoken_count(text);
      if (n > 0) history_back(n);
      else log_msg("history back: no count in \"%s\"", text);
      free(q);
      return;
    }
    if (!strcmp(q, "__NOW_PLAYING__")) {   /* Alice reads out the track; playback untouched */
      say_now_playing();
      free(q);
      return;
    }
    if (!strcmp(q, "__BLACKLIST_ARTIST__") || !strcmp(q, "__BLACKLIST_TRACK__")) {
      blacklist_voice(!strcmp(q, "__BLACKLIST_ARTIST__"), text);   /* add + skip it now */
      free(q);
      return;
    }
    if (!strcmp(q, "__WRONG__")) {       /* «ты поставила не то»: drop it, search again */
      wrong_pick();
      free(q);
      return;
    }
    if (!strcmp(q, "__REBOOT__")) {      /* «перезагрузись»: say so, then reboot */
      reboot_voice();
      free(q);
      return;
    }
    if (!strcmp(q, "__FAVORITES__")) {   /* play the owner's Liked Songs */
      spotify_favorites();
      free(q);
      return;
    }
    if (!strcmp(q, "__SEEK_START__"))
      soloist_seek("start");
    else if (!strcmp(q, "__SEEK_END__"))
      soloist_seek("end");
    else if (!strcmp(q, "__SEEK_FORWARD__"))
      soloist_seek("forward");
    else if (!strcmp(q, "__SEEK_BACK__"))
      soloist_seek("back");
    else if (!strncmp(q, "__SEEK_FORWARD_", 15))
      soloist_seek_seconds("forward", q + 15);
    else if (!strncmp(q, "__SEEK_BACK_", 12))
      soloist_seek_seconds("back", q + 12);
    else if (!strcmp(q, "__PAUSE__")) {
      spotify_sequence_transport("reset");
      soloist_cmd("--pause");
    }
    else if (!strcmp(q, "__CLEAR_QUEUE__"))
      queue_clear();
    else if (!strcmp(q, "__NEXT__"))
      spotify_sequence_transport("next");
    else if (!strcmp(q, "__PREVIOUS__") || !strcmp(q, "__PREV__"))
      spotify_sequence_transport("previous");
    else if (!strcmp(q, "__RESUME__"))
      soloist_cmd("--resume");
    free(q);
    return;
  }

  if (is_play_crumb(q)) {
    log_msg("preempt-crumb activity=%llu text=%s", (unsigned long long)activity, text);
    free(q);
    return;
  }

  /* Accumulate ASR until phrase stabilizes or Alice goes SPEAKING/BUSY.  A new
   * dialog is a new request even with the same words — and the same words as the
   * previous request mean it played the wrong thing. */
  unsigned tnow = now_sec();
  int new_dialog = cur_dialog[0] && strcmp(cur_dialog, pending_dialog) != 0;
  /* The same words heard again well after they were served, in a frame without
   * a dialog id: still a new (repeated) request. */
  int again = activity == 1 && pending_sent && !strcmp(pending, q) && tnow - pending_at >= 8;
  if (new_dialog || again || strcmp(pending, q) != 0) {
    /* Frames do not always carry the dialog id: keep the one already seen for
     * this request; a change after the last request was served is a new one. */
    if (cur_dialog[0])
      snprintf(pending_dialog, sizeof pending_dialog, "%s", cur_dialog);
    else if (pending_sent)
      pending_dialog[0] = 0;
    if (new_dialog) grace_armed = 0;   /* a new request: drop the old one's wait */
    snprintf(pending, sizeof pending, "%s", q);
    pending_at = tnow;
    pending_sent = 0;
    pending_soft = 0;
    int rw = repeat_window();
    pending_repeat = rw > 0 && last_req[0] && same_request(pending, last_req) &&
                     tnow - last_req_at < (unsigned)rw;
    log_msg("pending activity=%llu dialog=%s q=%s%s", (unsigned long long)activity,
            pending_dialog, q, pending_repeat ? " (repeat)" : "");
    /* Without internet Alice never answers (no announcement, no "не нашла"):
     * once the user has been silent this long, search what they said. */
    const char *dk = dialog_key();
    int aw = answer_wait();
    if (activity == 1 && aw > 0 && g.use_announcement && g.announcement_priority &&
        (!grace_armed || grace_why == WAIT_ANSWER) &&
        (!dk[0] || strcmp(dk, last_search_dialog) != 0))
      arm_request_wait(dk, aw, WAIT_ANSWER, 1);
  }

  int ready = 0;
  if (pending_repeat) {
    /* Don't wait for Alice's announcement: last time it named the wrong track. */
    if (activity == 2 || activity == 3) ready = 1;
  } else if (g.use_announcement && g.announcement_priority) {
    /* Alice names the target more accurately in her "Включаю …" reply, so defer
     * to it.  Once she commits (activity 3) the announcement can still lag a
     * moment behind, so wait a short grace before using the user's words. */
    const char *dk = dialog_key();
    if (activity == 3 && !pending_sent && (!dk[0] || strcmp(dk, last_search_dialog) != 0)) {
      int wait = announcement_wait();
      if (wait > 0) arm_request_wait(dk, wait, WAIT_ANNOUNCE, 0);
      else search_by_request("request", dk);
    }
  } else {
    if (activity == 2 || activity == 3) ready = 1;          /* speaking/busy = final enough */
    if ((tnow - pending_at) >= 1 && strlen(pending) >= 3) ready = 1; /* settled partial */
  }

  if (!ready || pending_sent) {
    free(q);
    return;
  }
  log_msg("blocked stock music query activity=%llu text=%s", (unsigned long long)activity, text);
  search_by_request(pending_repeat ? "repeat" : "request", dialog_key());
  free(q);
}

/* Alice couldn't find what was asked ("Не нашла ничего похожего."). */
static int is_not_found(const char *t) {
  static const char *const marks[] = {"не нашл", "не могу найти", "не удалось найти",
                                      "нет такой", "не знаю такой", "ничего похожего", NULL};
  for (int i = 0; marks[i]; i++)
    if (strstr(t, marks[i])) return 1;
  return 0;
}

/* Alice's announcement says more than the track: markup ("<[/domain]>") and an
 * age warning after it ("…«интро». осторожно! детям лучше этого не слышать").
 * Keep only the part naming the track (in place; text is already lowercase). */
static void clean_announcement(char *s) {
  char *r = s, *w = s;
  while (*r) {
    if (*r == '<') {
      char *e = strchr(r, '>');
      if (e) { r = e + 1; *w++ = ' '; continue; }
    }
    *w++ = *r++;
  }
  *w = 0;
  static const char *const trailers[] = {"осторожно", "внимание", "детям ", NULL};
  for (int i = 0; trailers[i]; i++) {
    char *t = strstr(s, trailers[i]);
    if (t) *t = 0;
  }
  /* A sentence ending right after a closing quote ends the track name. */
  static const char *const closing[] = {"\"", "»", "”", NULL};
  for (char *p = s; *p; p++) {
    size_t ql = 0;
    for (int i = 0; closing[i] && !ql; i++)
      if (!strncmp(p, closing[i], strlen(closing[i]))) ql = strlen(closing[i]);
    if (ql && (p[ql] == '.' || p[ql] == '!' || p[ql] == '?')) {
      p[ql] = 0;
      break;
    }
  }
}

/* Quotes around names only get in the search's way; also squeeze spaces. */
static void drop_quotes(char *s) {
  static const char *const quotes[] = {"\"", "«", "»", "“", "”", "„", NULL};
  char *r = s, *w = s;
  while (*r) {
    int skip = 0;
    for (int i = 0; quotes[i] && !skip; i++) {
      size_t n = strlen(quotes[i]);
      if (!strncmp(r, quotes[i], n)) skip = (int)n;
    }
    if (skip) { r += skip; continue; }
    if (*r == ' ' && (w == s || w[-1] == ' ')) { r++; continue; }
    *w++ = *r++;
  }
  *w = 0;
}

/* Fallback source of the query: Alice's own spoken "Включаю …" announcement.
 * Used when platformd committed the music play and spoke the confirmation but we
 * never received the recognised request text (occasional dropped/early frame).
 * Recovers the artist/track from the TTS so playback still happens.  Deduped by
 * dialog id against the request-text path so it never double-fires. */
static void handle_tts_fallback(const char *tts_in) {
  if (!g.use_announcement) return;
  if (!cur_dialog[0] || strcmp(cur_dialog, last_search_dialog) == 0) return;
  if (!strcmp(cur_dialog, own_dialog)) return;   /* her reply to our own request */
  char t[440];
  size_t n = strnlen(tts_in, sizeof t - 1);
  memcpy(t, tts_in, n);
  t[n] = 0;
  n = normalize_text(t, n);                 /* lowercase latin+cyrillic */
  /* "включаю", possibly after a short lead-in ("хорошо, включаю …") or as the
   * next sentence after a carried-out command ("Поставила громкость 5. Включаю …"). */
  char *vk = strstr(t, "\xd0\xb2\xd0\xba\xd0\xbb\xd1\x8e\xd1\x87\xd0\xb0\xd1\x8e");
  int playing = vk && (vk - t <= 40 || (vk - t >= 2 && vk[-1] == ' ' && strchr(".!", vk[-2])));
  /* The user's own request this reply answers, not yet acted on: same dialog,
   * or (frames don't always carry the id) the music request just heard. */
  int ours = pending[0] && !pending_sent &&
             (strcmp(pending_dialog, cur_dialog) == 0 || grace_armed ||
              now_sec() - pending_at < 20);
  /* Alice plays music for a phrase we didn't take for a music request ("хочу
   * послушать …", no "включи"): it was one, so its words are the request. */
  int not_found = !playing && is_not_found(t);
  if (!ours && (playing || not_found)) ours = adopt_heard_request(0);
  if (ours && pending_repeat) {           /* last time Alice's pick was wrong */
    search_by_request("repeat", cur_dialog);
    return;
  }
  if (!playing) {
    if (ours && pending_soft && !not_found) {
      /* A plain answer to a phrase without "включи": it wasn't music. */
      grace_armed = 0;
      pending_sent = 1;
      log_msg("soft request dropped: alice answered dialog=%s text=%s", cur_dialog, t);
      return;
    }
    /* Alice answered the music request with something other than "Включаю …"
     * (didn't find it, can't play it): use what the user actually said. */
    if (ours) {
      log_msg("tts-not-playing dialog=%s text=%s", cur_dialog, t);
      search_by_request("alice did not play it", cur_dialog);
    }
    return;
  }
  log_msg("tts-announce dialog=%s text=%s", cur_dialog, t);
  char *rest = vk + 14;
  clean_announcement(rest);
  while (*rest == ':' || *rest == ' ' || *rest == '.' || *rest == ',') rest++;
  if (strchr(rest, '#') || strchr(rest, '*')) {
    /* Alice hid part of the name ("Включаю: #####, песня …"): the user's own
     * words name it fully; without them drop the masked words. */
    if (ours) {
      search_by_request("masked announcement", cur_dialog);
      return;
    }
    char unmasked[440];
    strip_masked(unmasked, sizeof unmasked, rest);
    snprintf(rest, strlen(rest) + 1, "%s", unmasked);
  }
  if (strlen(rest) < 2) {
    /* "Включаю." with no name: nothing to search for but the user's words. */
    if (ours) search_by_request("empty announcement", cur_dialog);
    return;
  }

  char query[480];
  /* The user asked for a group/artist («включи группу X»): Alice's name for it
   * is the better spelling, but search the artist, never a title. */
  int want_artist = ours && !strncmp(pending, "@artist ", 8);
  /* An album asked for: the user's words name it (and its artist) exactly. */
  if (ours && !strncmp(pending, "@album ", 7)) {
    snprintf(last_search_dialog, sizeof last_search_dialog, "%s", cur_dialog);
    grace_armed = 0;
    pending_sent = 1;
    note_request();
    log_msg("tts-fallback album dialog=%s q=%s", cur_dialog, pending);
    journal_play(pending, "по моим словам", NULL);
    spotify_search_play(pending);
    return;
  }
  /* «Включаю песню «…»» / «… альбом …» without the artist: keep the user's name. */
  if (want_artist && (starts_with(rest, "песню ") || starts_with(rest, "трек ") ||
                      starts_with(rest, "альбом ") || starts_with(rest, "подборку ") ||
                      starts_with(rest, "плейлист "))) {
    static char named[256];
    snprintf(named, sizeof named, "%s", pending + 8);
    rest = named;
  }
  /* "группа X" / "группу X" -> artist search */
  if (strncmp(rest, "\xd0\xb3\xd1\x80\xd1\x83\xd0\xbf\xd0\xbf\xd0\xb0 ", 13) == 0 ||
      strncmp(rest, "\xd0\xb3\xd1\x80\xd1\x83\xd0\xbf\xd0\xbf\xd1\x83 ", 13) == 0) {
    snprintf(query, sizeof query, "@artist %s", rest + 13);
  } else {
    /* "ARTIST, песня TITLE" or "... песня «TITLE»" -> "ARTIST TITLE".
     * Split on the word "песня" regardless of quotes (Alice also says
     * ", трек …" / ", альбом …"); otherwise use the whole phrase (plain
     * artist/track name). */
    static const char *const kinds[] = {"песня", ", трек ", ", альбом ", NULL};
    char *pz = NULL;
    size_t kl = 0;
    int album_kind = 0;                       /* «X, альбом «Y»» */
    for (int i = 0; kinds[i] && !pz; i++)
      if ((pz = strstr(rest, kinds[i])) != NULL) { kl = strlen(kinds[i]); album_kind = i == 2; }
    if (pz) {
      char *a_end = pz;                       /* trim trailing ", " before "песня" */
      while (a_end > rest && (a_end[-1] == ' ' || a_end[-1] == ',')) a_end--;
      *a_end = 0;                             /* rest = artist */
      char *title = pz + kl;                  /* skip the kind word */
      while (*title == ' ') title++;
      if (strncmp(title, "\xc2\xab", 2) == 0) title += 2;   /* strip leading « */
      char *tc = strstr(title, "\xc2\xbb");                 /* strip trailing » */
      if (tc) *tc = 0;
      if (want_artist && *rest) snprintf(query, sizeof query, "@artist %s", rest);
      else if (want_artist) snprintf(query, sizeof query, "%s", pending);  /* no artist named */
      else if (album_kind)
        snprintf(query, sizeof query, "@album %s%s%s", title, *rest ? "|" : "", rest);  /* «X, альбом «Y»» */
      else snprintf(query, sizeof query, "%s %s", rest, title);
    } else {
      snprintf(query, sizeof query, "%s%s", want_artist ? "@artist " : "", rest);
    }
  }
  drop_quotes(query);
  size_t ql = strlen(query);
  while (ql && (query[ql-1]==' '||query[ql-1]=='.'||query[ql-1]=='!'||query[ql-1]=='?'||
                query[ql-1]==','))
    query[--ql] = 0;
  if (ql < 2) {
    if (ours) search_by_request("empty announcement", cur_dialog);
    return;
  }
  snprintf(last_search_dialog, sizeof last_search_dialog, "%s", cur_dialog);
  grace_armed = 0;              /* the announcement came: no fallback needed */
  if (ours) {
    pending_sent = 1;           /* the announcement served this request */
    note_request();             /* keep the user's words to recognise a repeat */
  }
  log_msg("tts-fallback search dialog=%s q=%s", cur_dialog, query);
  journal_play(query, "по анонсу Алисы", NULL);
  spotify_search_play(query);
}

static void handle_frame(uint8_t *data, size_t len) {
  uint64_t type = 0;
  int have_type = get_number(data, len, 253, &type) == 0;

  /* Plum Lite 2: many frames are a bare AliceState wrapped as field 3, not Quasar 252/253.
   * Seen on wire: field3 { field1=activity, field2=utf8 text, ... } e.g. activity=2 SPEAKING. */
  {
    const uint8_t *wrap;
    size_t wrap_len;
    if (get_bytes(data, len, 3, &wrap, &wrap_len) == 0 && wrap_len > 4) {
      pthread_mutex_lock(&music_mu);   /* shared with the announcement-grace timer */
      uint64_t activity = 0;
      get_number(wrap, wrap_len, 1, &activity);
      /* dialog id (field 7) for de-duplication between the two query sources */
      const uint8_t *dlg_b = NULL;
      size_t dlg_len = 0;
      cur_dialog[0] = 0;
      if (get_bytes(wrap, wrap_len, 7, &dlg_b, &dlg_len) == 0 && dlg_len < sizeof cur_dialog) {
        memcpy(cur_dialog, dlg_b, dlg_len);
        cur_dialog[dlg_len] = 0;
      }
      /* Music ducking.  After our own "повтори за мной" reply Alice opens a
       * follow-up listen (~5 s): nobody is talking to her, so no duck. */
      alice_raw_act = (int)activity;
      int own_followup = activity == 1 && cur_dialog[0] && !strcmp(cur_dialog, own_dialog);
      note_alice_activity(own_followup ? 0 : activity);
      const uint8_t *text_b = NULL;
      size_t text_len = 0;
      if (get_bytes(wrap, wrap_len, 2, &text_b, &text_len) == 0 && text_len > 0 && text_len < 500) {
        char text[512];
        memcpy(text, text_b, text_len);
        text[text_len] = 0;
        /* Journal: while listening (1) the text is still growing — keep the
         * latest; once Alice moves on it is the user's phrase. */
        if (activity == 1) snprintf(listen_text, sizeof listen_text, "%s", text);
        else if (activity == 2 || activity == 3) { journal_say(text); listen_text[0] = 0; }
        if (activity == 1 || activity == 2 || activity == 3) {
          handle_music_text(text, activity);
        }
      } else if (activity != 1 && listen_text[0]) {
        journal_say(listen_text);   /* Alice moved on without echoing the phrase */
        listen_text[0] = 0;
      }
      /* Fallback: recover the query from Alice's spoken "Включаю …" confirmation
       * (AliceState field 14 or 13 -> sub-field 1) when the request text never
       * reached us.  Only once the dialog is speaking/committed. */
      if (activity == 2 || activity == 3) {
        const uint8_t *an = NULL;
        size_t an_len = 0;
        const uint8_t *tts = NULL;
        size_t tts_len = 0;
        if ((get_bytes(wrap, wrap_len, 14, &an, &an_len) == 0 ||
             get_bytes(wrap, wrap_len, 13, &an, &an_len) == 0) &&
            get_bytes(an, an_len, 1, &tts, &tts_len) == 0 &&
            tts_len > 7 && tts_len < 400) {
          char tt[420];
          memcpy(tt, tts, tts_len);
          tt[tts_len] = 0;
          if (listen_text[0]) { journal_say(listen_text); listen_text[0] = 0; }
          say_on_alice_answer();
          journal_alice(tt);
          handle_tts_fallback(tt);
        }
      }
      pthread_mutex_unlock(&music_mu);
    }
  }

  if (!have_type) return;

  /* Also accept quick spotter phrases (type 210) for pause/next/resume. */
  if (type == 210) {
    const uint8_t *remoting;
    size_t rem_len;
    if (get_bytes(data, len, 252, &remoting, &rem_len)) return;
    const uint8_t *cap;
    size_t cap_len;
    if (get_bytes(remoting, rem_len, 7, &cap, &cap_len)) return;
    if (cap_len != 25 || memcmp(cap, "CommandSpotterCapability", 25)) return;
    const uint8_t *method;
    size_t method_len;
    if (get_bytes(remoting, rem_len, 15, &method, &method_len)) return;
    uint64_t mid = 0;
    if (get_number(method, method_len, 1, &mid) || mid != 2) return;
    const uint8_t *phrase_b;
    size_t phrase_len;
    if (get_bytes(method, method_len, 3, &phrase_b, &phrase_len)) return;
    if (phrase_len == 0 || phrase_len > 64) return;
    char phrase[80];
    memcpy(phrase, phrase_b, phrase_len);
    phrase[phrase_len] = 0;
    log_msg("spotter phrase=%s", phrase);
    char *q = music_query(phrase);
    if (!q) {
      /* spotter single words: map directly */
      if (!strcmp(phrase, "стоп") || !strcmp(phrase, "хватит") || !strcmp(phrase, "пауза"))
        q = strdup("__PAUSE__");
      else if (!strcmp(phrase, "дальше") || !strcmp(phrase, "следующий"))
        q = strdup("__NEXT__");
      else if (!strcmp(phrase, "назад"))
        q = strdup("__PREVIOUS__");
      else if (!strcmp(phrase, "играй") || !strcmp(phrase, "продолжи") || !strcmp(phrase, "продолжай"))
        q = strdup("__RESUME__");
    }
    if (!q) return;
    journal_say(phrase);
    journal("cmd", "%s", cmd_label(q));
    if (!strcmp(q, "__PAUSE__")) {
      spotify_sequence_transport("reset");
      soloist_cmd("--pause");
    }
    else if (!strcmp(q, "__NEXT__"))
      spotify_sequence_transport("next");
    else if (!strcmp(q, "__PREVIOUS__") || !strcmp(q, "__PREV__"))
      spotify_sequence_transport("previous");
    else if (!strcmp(q, "__RESUME__"))
      soloist_cmd("--resume");
    free(q);
    return;
  }

  if (type != 3) return;
  const uint8_t *state;
  size_t state_len;
  if (get_bytes(data, len, 252, &state, &state_len)) return;

  uint64_t activity = 0;
  get_number(state, state_len, 1, &activity);

  const uint8_t *result;
  size_t result_len;
  int has_result = get_bytes(state, state_len, 15, &result, &result_len) == 0;

  /* Try ASR text from result field 1, else state field 2 (echo). */
  const uint8_t *text_b = NULL;
  size_t text_len = 0;
  uint64_t f2 = 0, f4 = 0;
  if (has_result) {
    get_number(result, result_len, 2, &f2);
    get_number(result, result_len, 4, &f4);
    get_bytes(result, result_len, 1, &text_b, &text_len);
  }
  if ((!text_b || text_len == 0) && get_bytes(state, state_len, 2, &text_b, &text_len) != 0) {
    return;
  }
  if (!text_b || text_len == 0 || text_len > 500) return;

  /* Prefer finals (f2==1), but on Plum also accept when SPEAKING/BUSY with text. */
  int looks_final = (f2 == 1) || (f2 == 0 && (activity == 2 || activity == 3));
  if (!looks_final && f2 != 1) {
    /* Log partials occasionally for debug */
    static int partials;
    if ((++partials % 20) == 1) {
      char tmp[120];
      size_t n = text_len < 100 ? text_len : 100;
      memcpy(tmp, text_b, n);
      tmp[n] = 0;
      log_msg("partial activity=%llu f2=%llu text=%s", (unsigned long long)activity,
              (unsigned long long)f2, tmp);
    }
    return;
  }

  char text[512];
  memcpy(text, text_b, text_len);
  text[text_len] = 0;

  handle_music_text(text, activity);
}

/* Alice's event stream: 4-byte little-endian length + protobuf frame.  Read
 * until the connection drops (spotiya.receiver used to be this client, through
 * :9876; the proxy reads aliced itself now and sends nothing back on it). */
static void read_alice_events(int fd) {
  uint8_t *buf = malloc(MAX_FRAME);
  static unsigned long frame_n;
  if (!buf) return;
  while (running) {
    uint32_t size = 0;
    if (read_full(fd, &size, 4)) break;
    if (size == 0 || size > MAX_FRAME) break;
    if (read_full(fd, buf, size)) break;
    uint64_t t = 0;
    int got = get_number(buf, size, 253, &t);
    frame_n++;
    if (frame_n <= 5 || (frame_n % 30) == 1) {
      log_msg("frame#%lu got253=%d type=%llu size=%u head=%02x%02x%02x%02x%02x%02x%02x%02x",
              frame_n, got, (unsigned long long)t, size, buf[0], buf[1], buf[2], buf[3],
              size > 4 ? buf[4] : 0, size > 5 ? buf[5] : 0, size > 6 ? buf[6] : 0,
              size > 7 ? buf[7] : 0);
    }
    /* Firmware after a cold boot sends AliceState as a bare field 207
     * (fa 0c), without the old outer type=253 envelope.  Preserve the
     * first one verbatim for offline decoding. */
    if (buf[0] == 0xfa && size > 4) {
      static int saved_bare_state;
      if (!saved_bare_state) {
        FILE *f = fopen("/tmp/alice-bare-state.bin", "wb");
        if (f) {
          fwrite(buf, 1, size, f);
          fclose(f);
          saved_bare_state = 1;
        }
      }
    }
    if (frame_n == 1) {
      FILE *f = fopen("/tmp/alice-frame1.bin", "wb");
      if (f) {
        fwrite(buf, 1, size > 4096 ? 4096 : size, f);
        fclose(f);
      }
    }
    handle_frame(buf, size);
  }
  free(buf);
}

/* TERM must stop the proxy at once: musl's signal() restarts a blocking read,
 * so shutdown() the Alice connection to wake it (async-signal-safe). */
static int alice_fd = -1;
static void on_signal(int sig) {
  (void)sig;
  running = 0;
  if (alice_fd >= 0) shutdown(alice_fd, SHUT_RDWR);
}

/* The helpers that run detached (search, like, history back, favorites, queue
 * clear) are never waited for: reap them here, or each one stays a zombie for
 * the proxy's whole uptime (five had piled up within a day).  SA_RESTART keeps
 * the frame pumps' blocking reads going; the few synchronous waitpid() calls
 * just get ECHILD when the child was reaped here first, which they ignore. */
static void on_child(int sig) {
  (void)sig;
  int saved = errno;
  while (waitpid(-1, NULL, WNOHANG) > 0) {}
  errno = saved;
}

static void reap_children(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_child;
  sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
  sigaction(SIGCHLD, &sa, NULL);
}

int main(int argc, char **argv) {
  /* --check FILE: validate an edited voice-commands.json (the monitor runs this
   * before it replaces the live file).  Prints "OK <n> …" or the problem. */
  if (argc == 3 && !strcmp(argv[1], "--check")) {
    int ok = load_commands(argv[2]);
    if (ok && !load_bad_regex) {
      int nr = 0;
      for (int i = 0; i < NREPLY; i++) nr += g.nreply[i];
      printf("OK %d команд, %d глаголов поиска, %d вариантов ответов\n", g.ncmds, g.nverbs, nr);
      return 0;
    }
    printf("%s\n", load_err[0] ? load_err : "не загружается");
    return 1;
  }
  /* One proxy only: two would both act on every phrase.  (The :9876 bind used
   * to guarantee this; there is no listening socket any more.) */
  int lock = open(PROXY_LOCK, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) {
    log_msg("another alice-voice-proxy is running; exiting");
    return 4;
  }
  signal(SIGPIPE, SIG_IGN);
  signal(SIGTERM, on_signal);
  signal(SIGINT, on_signal);
  reap_children();
  srand((unsigned)time(NULL) ^ (unsigned)getpid());

  /* Load editable voice commands. If this fails for any reason the proxy
   * silently falls back to the built-in table, so a bad JSON never breaks it. */
  int cfg_ok = load_commands("/data/opt/spotify-connect/voice-commands.json");

  FILE *log = fopen("/tmp/alice-voice-proxy.log", "a");
  if (log) {
    if (cfg_ok)
      fprintf(log, "voice-commands.json loaded: %d commands, %d verbs\n", g.ncmds, g.nverbs);
    else
      fprintf(log, "voice-commands.json NOT loaded -> using built-in command table\n");
    fclose(log);
  }
  start_music_volume();

  /* aliced pushes its events (ASR, TTS, state) to whoever connects; reconnect
   * every 2 s while it restarts or before it is up at boot. */
  int was_up = -1;
  while (running) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { sleep(2); continue; }
    fcntl(fd, F_SETFD, FD_CLOEXEC);   /* never leak it into forked helpers */
    struct sockaddr_in aaddr = {0};
    aaddr.sin_family = AF_INET;
    aaddr.sin_port = htons(ALICE_PORT);
    aaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&aaddr, sizeof aaddr) < 0) {
      close(fd);
      if (was_up != 0) log_msg("alice events: :%d not reachable, retrying", ALICE_PORT);
      was_up = 0;
      if (running) sleep(2);
      continue;
    }
    alice_fd = fd;
    log_msg("alice events: connected to :%d", ALICE_PORT);
    was_up = 1;
    read_alice_events(fd);
    alice_fd = -1;
    close(fd);
    if (running) {
      log_msg("alice events: connection lost, reconnecting");
      sleep(2);
    }
  }
  return 0;
}
