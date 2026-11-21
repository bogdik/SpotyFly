// Tiny dependency-free HTTP status server for the speaker monitor.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

// Default listen port; the one set in ⚙ Настройки lives in MONCONF (PORT=…).
#ifndef PORT
#define PORT 28088
#endif
#ifndef MONCONF
#define MONCONF "/data/opt/spotify-connect/receiver-state/monitor.conf"
#define SUG_DEF 10   // rows in the search dropdown, MONCONF SUGGEST_N (1..SUG_MAX)
#define SUG_MAX 30
#endif

static int live_pid(const char *file) {
  char p[32], path[64];
  FILE *f = fopen(file, "r");
  if (!f || !fgets(p, sizeof p, f)) { if (f) fclose(f); return 0; }
  fclose(f);
  char *e; long n = strtol(p, &e, 10);
  if (n < 1 || *e != '\n') return 0;
  snprintf(path, sizeof path, "/proc/%ld", n);
  return access(path, F_OK) == 0;
}

static int live_mediad(void) {
  DIR *d = opendir("/proc");
  if (!d) return 0;
  struct dirent *e;
  char path[96], cmd[160];
  while ((e = readdir(d))) {
    if (strspn(e->d_name, "0123456789") != strlen(e->d_name)) continue;
    snprintf(path, sizeof path, "/proc/%s/cmdline", e->d_name);
    FILE *f = fopen(path, "r");
    if (!f) continue;
    size_t n = fread(cmd, 1, sizeof cmd - 1, f);
    fclose(f);
    for (size_t i = 0; i < n; i++) if (!cmd[i]) cmd[i] = ' ';
    cmd[n] = 0;
    if (strstr(cmd, "maind") && strstr(cmd, "--mediad")) { closedir(d); return 1; }
  }
  closedir(d); return 0;
}

static void json_escape(char *dst, size_t cap, const char *src) {
  size_t w = 0;
  for (; *src && w + 2 < cap; src++) {
    unsigned char c = (unsigned char)*src;
    if (c == '"' || c == '\\') { dst[w++]='\\'; dst[w++]=c; }
    else if (c >= 32) dst[w++]=c;
  }
  dst[w] = 0;
}

static void json_field(char *out, size_t cap, const char *json, const char *key) {
  char needle[96];
  snprintf(needle, sizeof needle, "\"%s\":\"", key);
  const char *p = strstr(json, needle);
  if (!p) { *out=0; return; }
  p += strlen(needle);
  size_t n = 0;
  while (p[n] && p[n] != '"' && n + 1 < cap) out[n] = p[n], n++;
  out[n] = 0;
}

static int voice_proxy_running(void);
static pid_t pid_of(const char *pidfile, const char *comm);
// Soloist for the services line: "down" (no process), else what poll-status saw:
// "ok" (logged in), "nologin" (answers but is not logged in to Spotify), or
// "unknown" (no reading yet).
static const char *soloist_state(const char *snapshot, char *buf, size_t cap) {
  if (pid_of("/run/soloist-manual.pid","soloist") <= 0) return "down";
  json_field(buf, cap, snapshot, "soloist");
  return (!strcmp(buf,"ok") || !strcmp(buf,"nologin") || !strcmp(buf,"down")) ? buf : "unknown";
}
static void status(char *out, size_t cap) {
  char snapshot[4096] = "{}";
  FILE *f = fopen("/data/opt/spotify-connect/monitor/status.json", "r");
  if (f) { size_t n=fread(snapshot,1,sizeof snapshot-1,f); snapshot[n]=0; fclose(f); }
  int oauth=0;
  FILE *of = fopen("/data/opt/spotify-connect/receiver-state/web-api/oauth.env", "r");
  if (of) { int ch; while ((ch=fgetc(of))!=EOF) { if (ch>' ') { oauth=1; break; } } fclose(of); }
  char sstate[16];
  // net-probe.sh's latest reading (wifi / internet / Spotify through the proxy)
  char net[1024] = "null";
  FILE *nf = fopen("/tmp/net-status.json", "r");
  if (nf) {
    size_t n = fread(net, 1, sizeof net - 1, nf); net[n] = 0; fclose(nf);
    while (n && (net[n-1] == '\n' || net[n-1] == ' ')) net[--n] = 0;
    if (n < 2 || net[0] != '{' || net[n-1] != '}') snprintf(net, sizeof net, "null");
  }
  snprintf(out, cap,
    "{\"soloist\":%s,"
    "\"oauth\":%s,"
    "\"services\":{\"mediad\":%s,\"voice_proxy\":%s,\"soloist\":\"%s\"},"
    "\"net\":%s,"
    "\"native_music\":{\"isolated_profiles\":5}}\n",
    snapshot, oauth ? "true":"false", live_mediad() ? "true":"false",
    voice_proxy_running() ? "true":"false", soloist_state(snapshot, sstate, sizeof sstate), net);
}

static const char page[] =
"<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>SpotyFly</title><style>"
":root{color-scheme:dark;font:16px/1.45 system-ui,-apple-system,sans-serif;background:#0d0d10;color:#ececf0}"
"*{box-sizing:border-box}body{max-width:560px;margin:0 auto;padding:22px 16px}"
"h1{font-size:19px;font-weight:600;margin:0 0 16px;display:flex;align-items:center;justify-content:space-between}h1 b{color:#1db954}"
"#gear,#cmdb{background:#26262c;border:0;color:#ececf0;font-size:16px;width:34px;height:34px;border-radius:9px;cursor:pointer}#gear:active,#cmdb:active{background:#333}"
".sug{position:relative;flex:1;min-width:0}.sug>input{width:100%}.dd:empty{display:none}.dd{position:absolute;left:0;right:0;top:calc(100% + 6px);background:#1b1b20;border:1px solid #303038;border-radius:12px;padding:5px;overflow:hidden auto;max-height:min(62vh,400px);z-index:9;box-shadow:0 12px 32px #000c}"
".dd>div{display:flex;align-items:center;gap:11px;padding:6px 8px;border-radius:7px;cursor:pointer;min-width:0}"
".dd .im{flex:none;width:42px;height:42px;border-radius:4px;background:#2a2a31 center/cover no-repeat}.dd .ar .im{border-radius:50%}"
".dd .tx{display:flex;flex-direction:column;gap:3px;min-width:0;flex:1}"
".dd .tx b{color:#ececf0;font-weight:500;font-size:15px;line-height:1.2;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}"
".dd .tx .a{color:#a0a0a8;font-size:13px;line-height:1.2;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}"
".dd .e{display:inline-block;background:#a0a0a8;color:#121216;font-size:9px;font-weight:700;line-height:14px;width:14px;text-align:center;border-radius:2px;margin-right:6px;vertical-align:1px}"
".dd>div:hover,.dd>div.sel{background:#ffffff14}.dd>div.sel{box-shadow:inset 2px 0 0 #1db954}"
".ov{position:fixed;inset:0;background:#000b;display:none;align-items:center;justify-content:center;padding:16px;z-index:20}.ov.on{display:flex}#blkb{color:#8a8a92}#blkb:hover{color:#f87171}.bch{display:block;width:100%;text-align:left;background:#1f1f25;border:1px solid #2e2e36;border-radius:12px;padding:12px 14px;margin-top:10px;color:#ececf0;font-size:15px;font-weight:600;cursor:pointer;transition:border-color .15s,background .15s}.bch:hover{border-color:#f87171;background:#26161a}.bch small{display:block;font-weight:400;font-size:12px;color:#9a9aa2;margin-top:3px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.bch:disabled{opacity:.4;cursor:default;border-color:#2e2e36;background:#1f1f25}"
".dlg{background:#17171b;border:1px solid #2a2a30;border-radius:16px;padding:20px;width:100%;max-width:420px;max-height:92vh;overflow-y:auto}.dlg h2{font-size:17px;margin:0 0 4px}"
".dlg label{display:block;font-size:12px;color:#9a9aa2;margin:12px 0 5px}.dlg input{width:100%;background:#0d0d10;border:1px solid #2a2a30;color:#ececf0;border-radius:9px;padding:10px 12px;font-size:14px}.dlg input:focus{outline:0;border-color:#1db954}"
".dlg .row{display:flex;gap:8px;margin-top:18px}.dlg .row button{flex:1;border:0;border-radius:10px;padding:11px;font-size:15px;font-weight:600;cursor:pointer}.dlg .save{background:#1db954;color:#0d0d10}.dlg .cancel{background:#26262c;color:#ececf0}.dlg .hint{font-size:11px;color:#7a7a80;margin-top:4px}"
".c{background:#17171b;border:1px solid #26262c;border-radius:16px;padding:16px;margin:12px 0}"
".np{display:flex;gap:14px;align-items:center}"
"#cv{width:104px;height:104px;border-radius:12px;object-fit:cover;background:#26262c;flex:none}"
".meta{min-width:0;flex:1}#t{font-size:19px;font-weight:600;margin:0;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}"
"#ar{color:#1db954;font-weight:500;margin-top:2px}#al{color:#9a9aa2;font-size:13px;margin-top:2px}"
"#bd{color:#9a9aa2;font-size:12px;margin-top:8px}"
".pbar{height:16px;background:#2a2a30;border-radius:3px;margin:10px 0 2px;cursor:pointer;padding:6px 0;background-clip:content-box}#pf{height:100%;background:#1db954;width:0;border-radius:3px;transition:width .6s linear;pointer-events:none}"
".tm{display:flex;justify-content:space-between;color:#8a8a92;font-size:12px}"
".ctl{display:flex;gap:8px;margin-top:14px}.ctl button{flex:1;background:#26262c;border:0;color:#ececf0;font-size:17px;padding:11px;border-radius:10px;cursor:pointer}.ctl button:active{background:#333}"
"#likeb{color:#555;transition:color .2s,box-shadow .2s,background .2s}#likeb.noauth{color:#555}#likeb.auth{color:#1db954;box-shadow:inset 0 0 0 2px #1db954}#likeb.liked{color:#1db954;background:#1db9541f}"
"#likemsg{overflow:hidden;max-height:0;opacity:0;transition:max-height .25s,opacity .25s,margin .25s;font-size:13px;text-align:center;border-radius:9px}"
"#likemsg.show{max-height:60px;opacity:1;margin:10px 0 2px;padding:9px 12px}#likemsg.warn{background:#3a2a16;color:#f0b86b}#likemsg.ok{background:#15351f;color:#1db954}#likemsg.err{background:#3a1a1a;color:#f87171}"
".srch{display:flex;gap:8px;margin-top:10px}.srch input{flex:1;min-width:0;background:#0d0d10;border:1px solid #2a2a30;color:#ececf0;border-radius:10px;padding:10px 12px;font-size:14px}.srch input:focus{outline:0;border-color:#1db954}.srch button{background:#1db954;color:#0d0d10;border:0;font-weight:600;border-radius:10px;padding:10px 16px;cursor:pointer}"
"#msg{color:#1db954;font-size:13px;margin-top:8px;min-height:16px}"
".m{color:#9a9aa2;font-size:13px}.ok{color:#1db954}.off{color:#f87171}.wn{color:#f0b86b}.unk{color:#6a6a72}#nx{color:#c9c9cf;font-size:13px;margin-top:10px}"
"details{background:#17171b;border:1px solid #26262c;border-radius:16px;margin:12px 0;overflow:hidden}"
"summary{list-style:none;cursor:pointer;padding:14px 16px;font-weight:600;display:flex;justify-content:space-between;align-items:center;user-select:none}"
"summary::-webkit-details-marker{display:none}summary .cnt{color:#8a8a92;font-weight:400;font-size:13px}"
"summary .arw{color:#8a8a92;transition:transform .18s}details[open] summary .arw{transform:rotate(90deg)}"
".lst div{padding:9px 16px;border-top:1px solid #202026;font-size:14px;color:#cfcfd6;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}"
".lst .e{color:#7a7a80;font-size:13px}"
".clr{background:#26262c;border:0;color:#c9c9cf;font-size:12px;font-weight:500;padding:5px 10px;border-radius:8px;cursor:pointer}.clr:active{background:#333}.sr{display:flex;gap:10px;align-items:center}"
".vg{padding:10px 16px;border-top:1px solid #202026}.vt{color:#6a6a72;font-size:11px;margin-bottom:3px}"
".vg .r{display:flex;gap:8px;font-size:14px;line-height:1.35;padding:2px 0}.vg .i{flex:none;width:18px;text-align:center}.vg .tx{min-width:0;overflow-wrap:anywhere}"
".k-say .tx{color:#ececf0;font-weight:500}.k-alice .tx{color:#9a9aa2}.k-play .tx{color:#1db954}.k-cmd .tx{color:#7cb7ff}.k-bl .tx{color:#ff8a65}"
".lst div.b{display:flex;gap:10px;align-items:center;white-space:normal}.b .i{flex:none;width:18px;text-align:center}.b .tx{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.b .kd{color:#6a6a72;font-size:12px;margin-left:8px}"
".x{flex:none;background:none;border:0;color:#8a8a92;font-size:16px;line-height:1;cursor:pointer;padding:4px 6px;border-radius:6px}.x:active{background:#2a2a30;color:#ff5a5a}"
".hb{display:flex;gap:8px}.err{color:#f87171;font-size:13px;margin-top:10px;min-height:0}.err:empty{display:none}"
/* voice-commands editor */
".dlg.wide{max-width:680px;padding:0;display:flex;flex-direction:column;height:min(92vh,900px)}"
".eh{padding:18px 20px 0}.eh .t{display:flex;justify-content:space-between;align-items:center}.eh .t h2{margin:0}"
".tabs{display:flex;gap:4px;background:#0d0d10;border-radius:10px;padding:3px;margin:14px 0 0}.tabs button{flex:1;background:none;border:0;color:#9a9aa2;font-size:13px;font-weight:600;padding:8px 6px;border-radius:8px;cursor:pointer}.tabs button.on{background:#26262c;color:#ececf0}"
".tst{display:flex;gap:8px;align-items:center;margin-top:12px}.tst input{flex:1}.tr{font-size:13px;margin-top:6px;min-height:18px;color:#9a9aa2}.tr b{color:#1db954;font-weight:600}.tr.no b{color:#f0b86b}"
".eb{flex:1;overflow-y:auto;padding:12px 20px 16px}"
".cc{border:1px solid #26262c;border-radius:12px;margin:8px 0;background:#141418}.cc.open{border-color:#34343c}"
".ch{display:flex;align-items:center;gap:8px;padding:11px 14px;cursor:pointer;user-select:none}.ch .cn{font-weight:600;font-size:14px}.ch .cs{color:#7a7a80;font-size:12px;flex:1;min-width:0;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}"
".bdg{font-size:11px;border-radius:6px;padding:2px 6px;flex:none}.bdg.m{background:#3a2a16;color:#f0b86b}.bdg.n{background:#15351f;color:#1db954}"
".cb{padding:0 14px 12px}.cl{font-size:12px;color:#8a8a92;margin:10px 0 6px}.cl i{font-style:normal;color:#6a6a72}"
".chips{display:flex;flex-wrap:wrap;gap:6px;align-items:center}"
".chip{display:inline-flex;align-items:center;gap:2px;background:#22222a;border:1px solid #2e2e36;border-radius:8px;padding:3px 3px 3px 9px;font-size:13px;max-width:100%}"
".chip span{overflow-wrap:anywhere}.chip.re{font-family:ui-monospace,Menlo,monospace;font-size:12px;color:#ffcf8a}.chip.dup{border-color:#f87171}"
".chip button{background:none;border:0;color:#7a7a80;cursor:pointer;font-size:12px;padding:2px 5px;border-radius:5px}.chip button:hover{color:#ff5a5a;background:#2a2a30}"
".chips input.add{flex:1;min-width:140px;width:auto;background:transparent;border:1px dashed #34343c;border-radius:8px;padding:5px 9px;font-size:13px;color:#ececf0}.chips input.add:focus{outline:0;border-color:#1db954;border-style:solid}"
".cr{display:flex;gap:8px;justify-content:flex-end;margin-top:12px}.cr button,.lnk{background:#22222a;border:0;color:#c9c9cf;font-size:12px;padding:6px 10px;border-radius:8px;cursor:pointer}.cr button.del{color:#f87171}"
".nc{display:flex;gap:8px;margin:12px 0 4px}.nc select,.nc input{background:#0d0d10;border:1px solid #2a2a30;color:#ececf0;border-radius:9px;padding:9px 10px;font-size:13px}.nc select{flex:1;min-width:0}.nc input{flex:1}"
".sec{margin:6px 0 16px}.sec h3{font-size:14px;margin:14px 0 2px}.sec .hint{margin:0 0 8px}"
".setg{display:grid;grid-template-columns:1fr auto;gap:10px 12px;align-items:center;font-size:13px}.setg input[type=number]{width:96px;background:#0d0d10;border:1px solid #2a2a30;color:#ececf0;border-radius:8px;padding:7px 9px}.setg input[type=checkbox]{width:18px;height:18px;accent-color:#1db954}"
"#cj{width:100%;height:100%;min-height:360px;background:#0d0d10;border:1px solid #2a2a30;color:#ececf0;border-radius:10px;padding:12px;font:12px/1.5 ui-monospace,Menlo,monospace;resize:none}#cj.bad{border-color:#f87171}"
".ef{display:flex;gap:8px;align-items:center;padding:12px 20px;border-top:1px solid #26262c;background:#17171b;border-radius:0 0 16px 16px}.ef .sp{flex:1;font-size:12px;color:#8a8a92;min-width:0}"
".ef button{border:0;border-radius:10px;padding:10px 14px;font-size:14px;font-weight:600;cursor:pointer}.ef .rst{background:none;color:#f87171;padding:10px 4px}.ef .cancel{background:#26262c;color:#ececf0}.ef .save{background:#1db954;color:#0d0d10}.ef .save:disabled{opacity:.4;cursor:default}"
"@media(max-width:520px){.dlg.wide{height:100%;max-height:none;border-radius:0}.ov.full{padding:0}.ef{border-radius:0;flex-wrap:wrap}.ef .sp{flex-basis:100%;order:-1}}"
"</style>"
"<h1><span>Spoty<b>Fly</b></span><span class=hb><button id=cmdb onclick=openCmd() title='Голосовые команды'>🎙</button><button id=gear onclick=openCfg() title=Настройки>⚙</button></span></h1>"
"<div class=c><div class=np><img id=cv alt=''><div class=meta><div id=t>—</div><div id=ar></div><div id=al></div><div id=bd></div></div></div>"
"<div class=pbar id=pb onclick=seek(event)><div id=pf></div></div><div class=tm><span id=tc>0:00</span><span id=td>0:00</span></div>"
"<div id=likemsg></div>"
"<div class=ctl><button onclick=ctl('prev')>⏮</button><button onclick=ctl('pause')>⏸</button><button onclick=ctl('play')>▶</button><button onclick=ctl('next')>⏭</button><button id=likeb onclick=likeTrack()>♥</button><button id=blkb onclick=openBlk() title='В чёрный список'>⛔</button></div>"
"<div class=srch><div class=sug><input id=q placeholder='Что включить?' autocomplete=off oninput=onType() onkeydown=onKey(event)><div id=dd class=dd></div></div><button onclick=go()>Поиск</button></div>"
"<div id=msg></div><div id=nx></div></div>"
"<div class=c><div id=sv class=m>Проверяю сервисы…</div><div id=netl class=m style='margin-top:6px;font-size:12px;display:none'></div></div>"
"<details open><summary><span>История воспроизведения <span class=cnt id=hc></span></span><span class=arw>▸</span></summary><div id=hs class=lst></div></details>"
"<details><summary><span>История поисков <span class=cnt id=qc></span></span><span class=arw>▸</span></summary><div id=qs class=lst></div></details>"
"<details open id=vd><summary><span>Голосовые запросы <span class=cnt id=vc></span></span><span class=sr><button class=clr onclick=clearVoice(event)>Очистить</button><span class=arw>▸</span></span></summary><div id=vl class=lst></div></details>"
"<details id=blk><summary><span>Чёрный список <span class=cnt id=bc></span></span><span class=arw>▸</span></summary><div id=bls class=lst></div></details>"
"<div class=ov id=ov><div class=dlg><h2>Настройки</h2><div class=hint>Токены Spotify и прокси для поиска. Секрет не показывается; оставь пустым — не меняется.</div>"
"<label>Client ID</label><input id=cid placeholder='Spotify CLIENT_ID'>"
"<label>Client Secret</label><input id=csec type=password placeholder='•••• (оставь пустым — без изменений)'>"
"<label>Soloist API Key</label><input id=skey type=password placeholder='•••• (оставь пустым — без изменений)'><div class=hint>После смены ключа перезагрузи колонку: soloist читает ключ при старте.</div>"
"<label>Прокси: socks5h://ip:порт или http://ip:порт, с паролем — socks5h://логин:пароль@ip:порт (http:// так же); в логине и пароле @ : / % ' \" \\ ` $ — как %XX</label><input id=cpx placeholder=''>"
"<label>Redirect URI (добавь его в Spotify Dashboard → Redirect URIs)</label><input id=crd readonly style='color:#1db954'>"
"<label>Порт веб-монитора</label><input id=cport type=number min=1024 max=65535 inputmode=numeric placeholder=28088><div class=hint id=cporth>По умолчанию 28088. После сохранения страница сама откроется на новом порту; Redirect URI (:8443) не меняется.</div>"
"<label>Строк в подсказках поиска</label><input id=csug type=number min=1 max=30 inputmode=numeric placeholder=10><div class=hint>Сколько треков показывать в выпадающем списке при наборе (1–30, по умолчанию 10).</div>"
"<div class=err id=cerr></div>"
"<div class=row><button class=cancel onclick=closeCfg()>Отмена</button><button class=save onclick=saveCfg()>Сохранить</button></div>"
"<div style='border-top:1px solid #26262c;margin:18px -20px 0;padding:16px 20px 0'>"
"<div style='display:flex;justify-content:space-between;align-items:center'><b style='font-size:14px'>Авторизация Spotify</b><span id=ostat style='font-size:12px;color:#8a8a92'></span></div>"
"<div class=hint>1) Нажми «Авторизоваться», войди в Spotify. 2) Если редирект не открыл устройство — скопируй <b>code</b> из адреса и вставь ниже.</div>"
"<div class=row style='margin-top:10px'><button class=cancel onclick=oauthBegin()>Авторизоваться</button></div>"
"<label>Код авторизации (code=…)</label><input id=ocode placeholder='вставь code из адресной строки'>"
"<div class=row style='margin-top:8px'><button class=save onclick=oauthFinish()>Применить код</button></div>"
"</div></div></div>"
"<div class='ov full' id=ovc><div class='dlg wide'>"
"<div class=eh><div class=t><h2>Голосовые команды</h2><button class=x onclick=closeCmd() title=Закрыть>✕</button></div>"
"<div class=tst><input id=ctq placeholder='Проверь фразу: «алиса, следующий трек»' oninput=cTest() autocomplete=off></div><div class=tr id=ctr></div>"
"<div class=tabs><button id=tb0 onclick=cTab(0)>Команды</button><button id=tb1 onclick=cTab(1)>Поиск музыки</button><button id=tb3 onclick=cTab(3)>Ответы</button><button id=tb2 onclick=cTab(2)>JSON</button></div></div>"
"<div class=eb id=ceb></div>"
"<div class=ef><button class=rst onclick=cReset()>Сбросить по умолчанию</button><span class=sp id=cst></span><button class=cancel onclick=closeCmd()>Отмена</button><button class=save id=csv onclick=cSave() disabled>Сохранить</button></div>"
"</div></div>"
"<div class=ov id=ovb><div class=dlg><h2>В чёрный список</h2><div class=hint id=blkh>Он будет пропускаться и в поиске, и в радио. Убрать можно в блоке «Чёрный список» ниже.</div><button class=bch id=blkt onclick=\"blkAdd('track')\">Только этот трек<small id=blkts></small></button><button class=bch id=blka onclick=\"blkAdd('artist')\">Исполнителя целиком<small id=blkas></small></button><div class=err id=blke></div><div class=row><button class=cancel onclick=closeBlk()>Отмена</button></div></div></div>"
"<script>"
"const dot=x=>x?'<b class=ok>●</b>':'<b class=off>●</b>';"
"const SST={ok:['ok','soloist'],nologin:['wn','soloist — не вошёл в Spotify'],down:['off','soloist — не запущен'],unknown:['unk','soloist']};"
"const sdot=v=>{let a=SST[v]||SST.unknown;return '<span title=\"'+a[1]+'\"><b class='+a[0]+'>●</b> '+a[1]+'</span>'};"
"const NST={ok:['ok','сеть'],noproxy:['wn','сеть — прокси не отвечает, Spotify без связи'],noinet:['off','сеть — нет интернета'],nowifi:['off','сеть — нет связи с роутером']};"
"const NSH={noproxy:'прокси',noinet:'интернет',nowifi:'роутер'};"
"const nHm=t=>{let d=new Date(t*1000);return String(d.getHours()).padStart(2,'0')+':'+String(d.getMinutes()).padStart(2,'0')};"
"const nDur=s=>s<90?Math.round(s)+' с':s<5400?Math.round(s/60)+' мин':(s/3600).toFixed(1)+' ч';"
"function ndot(n){if(!n)return '<span title=\\\"нет данных net-probe\\\"><b class=unk>●</b> сеть</span>';let a=NST[n.state]||['unk','сеть'],t=a[1]+(n.state==='ok'&&n.proxy_ms!=null?' · прокси '+n.proxy_ms+' мс':'');return '<span title=\\\"'+t+'\\\"><b class='+a[0]+'>●</b> '+a[1]+'</span>'}"
"function nline(n){if(!n)return '';let ev=n.events||[],now=Date.now()/1000,out=[];for(let i=ev.length-1;i>=0&&out.length<3;i--){let st=ev[i][1];if(st==='ok')continue;if(st==='boot'){out.push('перезагрузка '+nHm(ev[i][0]));continue}let a=ev[i][0],b=i+1<ev.length?ev[i+1][0]:null;if(!b&&now-a<20)continue;out.push((b?nHm(a)+'–'+nHm(b):'с '+nHm(a))+' '+(NSH[st]||st)+' ('+nDur((b||now)-a)+')')}let h=out.length?'Сбои сети: '+esc(out.join(' · ')):'';if(n.drops){h+=(h?'<br>':'')+'Wi‑Fi рвался '+n.drops+' раз с включения'+(n.fw_resets?', из них сама прошивка («'+esc(n.drop_why==='nointernet'||!n.drop_why?'нет интернета':n.drop_why)+'») — '+n.fw_resets+' с '+nHm(n.fw_since):'')+(n.drop_last?', последний в '+nHm(n.drop_last):'')}if(h)h+=' · <a href=/api/netdiag target=_blank style=\\'color:#9a9aa2\\'>диагностика</a>';return h}"
"const ctl=x=>fetch('/api/ctl/'+x);"
"let likeTmr;function likeMsg(cls,txt){clearTimeout(likeTmr);likemsg.className='';void likemsg.offsetWidth;likemsg.textContent=txt;likemsg.className='show '+cls;likeTmr=setTimeout(()=>{likemsg.className=''},3500)}"
"function paintHeart(){if(!window.AUTH){likeb.className='noauth';likeb.textContent='♡'}else if(window.LIKED){likeb.className='liked';likeb.textContent='♥'}else{likeb.className='auth';likeb.textContent='♡'}}"
"function likeTrack(){if(!window.AUTH){likeMsg('warn','Чтобы ставить лайки, пройди авторизацию в ⚙ Настройках');return}fetch('/api/like',{cache:'no-store'}).then(x=>x.json()).then(r=>{if(r.ok){window.LIKED=1;paintHeart();likeMsg('ok','♥ Добавлено в любимые')}else{likeMsg('err','Spotify отклонил запись (403). Приложению нужен доступ: добавь аккаунт в Dashboard → User Management')}}).catch(()=>likeMsg('err','Нет связи с колонкой'))}"
"let DUR=0;"
"const run=v=>{v=v.trim();if(!v)return;q.value='';hideDD();fetch('/api/search?q='+encodeURIComponent(v));msg.textContent='Ищу: '+v+'…';setTimeout(()=>{msg.textContent='';r()},3000)};"
"const go=()=>run(q.value);"
"function seek(e){if(!DUR)return;let b=pb.getBoundingClientRect(),f=Math.min(1,Math.max(0,(e.clientX-b.left)/b.width)),ms=Math.round(f*DUR);pf.style.width=(f*100)+'%';tc.textContent=fmt(ms);window.seekTarget=ms;window.seekActive=1;window.seekAt=Date.now();fetch('/api/seek?ms='+ms);setTimeout(r,800)}"
"let sel=-1,sugs=[],tmr;function hideDD(){dd.innerHTML='';sel=-1;sugs=[]}"
"function onType(){clearTimeout(tmr);let v=q.value.trim();if(v.length<2){hideDD();return}tmr=setTimeout(()=>sugFetch(v),220)}"
"async function sugFetch(v){try{let a=await(await fetch('/api/suggest?q='+encodeURIComponent(v),{cache:'no-store'})).json();if(q.value.trim()!==v)return;sugs=a||[];sel=-1;dd.innerHTML=sugs.map(sugRow).join('')}catch(e){hideDD()}}"
"function sugRow(it,i){let ar=it.k==='a',im=it.i&&it.i.indexOf('https://i.scdn.co/image/')===0&&/^[A-Za-z0-9]+$/.test(it.i.slice(24))?' style=\"background-image:url('+it.i+')\"':'';return '<div data-i='+i+(ar?' class=ar':'')+' onmousedown=\"pick(event,'+i+')\"><span class=im'+im+'></span><span class=tx><b>'+esc(it.n)+'</b><span class=a>'+(ar?'Исполнитель':(it.e?'<span class=e>E</span>':'')+'Трек • '+esc(it.a))+'</span></span></div>'}"
"function playItem(it){if(!it.u){run(it.q);return}q.value='';hideDD();fetch('/api/play?uri='+encodeURIComponent(it.u)+'&artist='+encodeURIComponent(it.a)+'&title='+encodeURIComponent(it.n));msg.textContent=it.k==='a'?'Включаю исполнителя: '+it.n:'Включаю: '+it.n+' — '+it.a;setTimeout(()=>{msg.textContent='';r()},3000)}"
"function pick(e,i){if(e)e.preventDefault();let it=sugs[i];if(it)playItem(it)}"
"function onKey(e){let n=sugs.length;if(e.keyCode==13){if(sel>=0&&sugs[sel]){playItem(sugs[sel])}else{go()}return}if(e.keyCode==27){hideDD();return}if(!n)return;if(e.keyCode==40){e.preventDefault();sel=(sel+1)%n}else if(e.keyCode==38){e.preventDefault();sel=(sel-1+n)%n}else return;[...dd.children].forEach((c,i)=>{c.classList.toggle('sel',i==sel);if(i==sel)c.scrollIntoView({block:'nearest'})})}"
"document.addEventListener('click',e=>{if(!e.target.closest('.sug'))hideDD()});"
"const defRedir=()=>'https://'+location.hostname+':8443/callback';"
"function openCfg(){fetch('/api/settings',{cache:'no-store'}).then(x=>x.json()).then(s=>{cid.value=s.client_id||'';cpx.value=s.proxy||'';crd.value=defRedir();csec.value='';skey.value='';ocode.value='';csec.placeholder=s.secret_set?'•••• (задан, оставь пустым — без изменений)':'не задан';skey.placeholder=s.soloist_key_set?'•••• (задан, оставь пустым — без изменений)':'не задан';ostat.textContent=s.oauth_set?'● токен есть':'○ не авторизован';ostat.style.color=s.oauth_set?'#1db954':'#8a8a92';cport.value=s.port||'';cport.placeholder=s.port_default||28088;csug.value=s.suggest_n||'';csug.placeholder=s.suggest_default||10;cerr.textContent='';ov.classList.add('on')})}"
"function closeCfg(){ov.classList.remove('on')}"
"function openBlk(){let s=window.NOW||{},t=s.title||'',a=s.artist||'';blkts.textContent=t?t+(a?' — '+a:''):'сейчас ничего не играет';blkas.textContent=a||'исполнитель не известен';blkt.disabled=!t;blka.disabled=!a;blke.textContent='';ovb.classList.add('on')}"
"function closeBlk(){ovb.classList.remove('on')}"
"ovb.addEventListener('click',e=>{if(e.target==ovb)closeBlk()});"
"async function blkAdd(k){blkt.disabled=blka.disabled=true;blke.textContent='';try{let r=await(await fetch('/api/blacklist/add?k='+k,{cache:'no-store'})).json();if(!r.ok)throw 0;closeBlk();let s=window.NOW||{};likeMsg('ok',k==='artist'?'В чёрном списке: '+(s.artist||'исполнитель')+' — пропускаю':'В чёрном списке: '+(s.title||'трек')+' — пропускаю');BJ=null;setTimeout(loadBL,800)}catch(e){blke.textContent='Не получилось — что-то не играет или нет связи с колонкой';blkt.disabled=blka.disabled=false}}"
""
"function saveCfg(){let p='/api/settings/save?client_id='+encodeURIComponent(cid.value.trim())+'&proxy='+encodeURIComponent(cpx.value.trim())+'&redirect_uri='+encodeURIComponent(crd.value.trim());if(csec.value)p+='&secret='+encodeURIComponent(csec.value);if(skey.value)p+='&soloist_key='+encodeURIComponent(skey.value.trim());let pv=cport.value.trim()||cport.placeholder;p+='&port='+encodeURIComponent(pv);p+='&suggest_n='+encodeURIComponent(csug.value.trim()||csug.placeholder);cerr.textContent='';"
"fetch(p).then(x=>x.json()).then(r=>{if(!r.ok){cerr.textContent=r.err||'Не сохранилось';return}closeCfg();"
"if(r.moved&&location.port!=='8443'){msg.textContent='Монитор переезжает на порт '+r.port+'…';let u=location.protocol+'//'+location.hostname+':'+r.port+'/';setTimeout(()=>location.href=u,1200)}else msg.textContent=r.moved?'Порт монитора: '+r.port:'Настройки сохранены'}).catch(()=>{cerr.textContent='Нет связи с колонкой'})}"
"function oauthBegin(){saveCfg0().then(()=>fetch('/api/oauth/begin',{cache:'no-store'}).then(x=>x.json()).then(s=>{if(s.url){window.open(s.url,'_blank');ostat.textContent='жду код…'}else{ostat.textContent='нет Client ID/Secret';ostat.style.color=\"#f87171\"}}))}"
"function saveCfg0(){let p='/api/settings/save?client_id='+encodeURIComponent(cid.value.trim())+'&proxy='+encodeURIComponent(cpx.value.trim())+'&redirect_uri='+encodeURIComponent(crd.value.trim());if(csec.value)p+='&secret='+encodeURIComponent(csec.value);if(skey.value)p+='&soloist_key='+encodeURIComponent(skey.value.trim());return fetch(p)}"
"function oauthFinish(){let v=ocode.value.trim();let m=v.match(/code=([^&]+)/);if(m)v=decodeURIComponent(m[1]);if(!v){ostat.textContent='вставь code';return}ostat.textContent='обмениваю…';fetch('/api/oauth/finish?code='+encodeURIComponent(v)).then(x=>x.json()).then(r=>{ostat.textContent=r.ok?'● токен сохранён':'ошибка обмена';ostat.style.color=r.ok?'#1db954':'#f87171';if(r.ok)ocode.value=''})}"
"ov.addEventListener('click',e=>{if(e.target==ov)closeCfg()});"
"const fmt=ms=>{ms=Math.max(0,Math.floor(ms/1000));return Math.floor(ms/60)+':'+String(ms%60).padStart(2,'0')};"
"const esc=z=>(z||'').replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));"
"function lst(el,cnt,str){let a=(str||'').split('|').filter(Boolean).reverse();cnt.textContent=a.length?'· '+a.length:'';el.innerHTML=a.length?a.map(h=>'<div>'+esc(h)+'</div>').join(''):'<div class=e>Пока пусто</div>'}"
"const VI={say:'🎤',alice:'💬',play:'▶',cmd:'⏯',bl:'⛔'};"
"function vgroups(a){let g=[],cur=null;a.sort((x,y)=>x.t-y.t);for(const e of a){if(!cur||e.k==='say'||e.t-cur.last>30){cur={t:e.t,r:[]};g.push(cur)}cur.r.push(e);cur.last=e.t}return g.reverse()}"
"function hm(t){let d=new Date(t*1000),s=d.toLocaleTimeString('ru-RU',{hour:'2-digit',minute:'2-digit',second:'2-digit'});return d.toDateString()===new Date().toDateString()?s:d.toLocaleDateString('ru-RU',{day:'numeric',month:'short'})+' '+s}"
"let VJ=null,VT=0;async function loadVoice(){VT=Date.now();try{let t=await(await fetch('/api/voicelog',{cache:'no-store'})).text();if(t===VJ)return;VJ=t;let g=vgroups(JSON.parse(t));vc.textContent=g.length?'· '+g.length:'';"
"vl.innerHTML=g.length?g.map(x=>'<div class=vg><div class=vt>'+hm(x.t)+'</div>'+x.r.map(e=>'<div class=\"r k-'+e.k+'\"><span class=i>'+(VI[e.k]||'·')+'</span><span class=tx>'+esc(e.x)+'</span></div>').join('')+'</div>').join(''):'<div class=e>Пока пусто</div>'}catch(e){}}"
"let BJ=null,BT=0,BLA=[];async function loadBL(){BT=Date.now();try{let t=await(await fetch('/api/blacklist',{cache:'no-store'})).text();if(t===BJ)return;BJ=t;BLA=JSON.parse(t).reverse();bc.textContent=BLA.length?'· '+BLA.length:'';"
"bls.innerHTML=BLA.length?BLA.map((e,i)=>'<div class=b><span class=i>'+(e.k==='artist'?'👥':'🎵')+'</span><span class=tx>'+esc(e.n||e.u)+'<span class=kd>'+(e.k==='artist'?'исполнитель':'трек')+'</span></span><button class=x title=Убрать onclick=blDel('+i+')>✕</button></div>').join(''):'<div class=e>Пусто. Скажи «добавь группу в чёрный список» или «добавь трек в чёрный список»</div>'}catch(e){}}"
"function blDel(i){let e=BLA[i];if(!e||!confirm('Убрать из чёрного списка: '+(e.n||e.u)+'?'))return;fetch('/api/blacklist/del?u='+encodeURIComponent(e.u),{cache:'no-store'}).then(()=>{BJ=null;loadBL()})}"
"function clearVoice(e){e.preventDefault();e.stopPropagation();if(!confirm('Очистить лог голосовых запросов?'))return;fetch('/api/voicelog/clear',{cache:'no-store'}).then(()=>{VJ=null;loadVoice()})}"
"async function r(){if(vd.open||Date.now()-VT>10000)loadVoice();if(blk.open||Date.now()-BT>10000)loadBL();try{let x=await(await fetch('/api/status',{cache:'no-store'})).json(),s=x.soloist||{};"
"if(s.cover){cv.style.visibility='visible';if(s.cover!==window.CVURL){window.CVURL=s.cover;fetch('/api/cover',{cache:'no-store'}).then(x=>x.text()).then(t=>{cv.src=t.slice(0,5)==='data:'?t:s.cover}).catch(()=>{cv.src=s.cover})}}else{cv.style.visibility='hidden';window.CVURL=''}"
"t.textContent=s.title||(s.status==='playing'?'Spotify играет':'—');"
"window.NOW=s;ar.textContent=s.artist||'';al.textContent=s.album||'';"
"let d=+s.duration_ms||0,p=Math.min(+s.position_ms||0,d);DUR=d;td.textContent=fmt(d);"
"if(window.seekActive){if(Math.abs(p-window.seekTarget)<=3500||Date.now()-window.seekAt>7000)window.seekActive=0}"
"if(!window.seekActive){pf.style.width=d?(100*p/d)+'%':'0';tc.textContent=fmt(p)}"
"bd.textContent=(s.status==='playing'?'▶ играет':s.status==='paused'?'⏸ пауза':s.status||'—')+(s.volume?'  ·  громк. '+s.volume+'%':'');"
"nx.textContent=s.next?'Далее: '+s.next:'';"
"window.AUTH=!!x.oauth;let tk=(s.title||'')+'|'+(s.artist||'');if(window.AUTH&&tk&&tk!==window.TKEY){window.TKEY=tk;window.LIKED=0;paintHeart();fetch('/api/liked',{cache:'no-store'}).then(y=>y.json()).then(r=>{window.LIKED=r.liked?1:0;paintHeart()})}else if(!window.AUTH){window.TKEY='';window.LIKED=0}paintHeart();"
"lst(hs,hc,s.history);lst(qs,qc,s.search_history);"
"sv.innerHTML=dot(x.services.voice_proxy)+' voice-proxy &nbsp; '+sdot(x.services.soloist)+' &nbsp; '+dot(x.services.mediad)+' Алиса / mediad &nbsp; '+ndot(x.net);{let l=nline(x.net);netl.innerHTML=l;netl.style.display=l?'':'none'}"
"}catch(e){sv.textContent='Нет ответа от monitor'}}"
/* voice-commands editor (🎙) */
"const AL={__WRONG__:'Включила не то — найти заново',__PAUSE__:'Пауза',__RESUME__:'Продолжить',__NEXT__:'Следующий трек',__PREVIOUS__:'Предыдущий трек',__CLEAR_QUEUE__:'Очистить очередь',__SEEK_START__:'В начало трека',__SEEK_END__:'В конец трека','__SEEK_BACK_{1}':'Назад на N секунд','__SEEK_FORWARD_{1}':'Вперёд на N секунд',__SEEK_BACK__:'Перемотать назад',__SEEK_FORWARD__:'Перемотать вперёд',__LIKE__:'Лайк',__HISTORY_BACK__:'N песен назад',__NOW_PLAYING__:'Что играет',__FAVORITES__:'Избранное',__BLACKLIST_ARTIST__:'Исполнителя в чёрный список',__BLACKLIST_TRACK__:'Трек в чёрный список',__REBOOT__:'Перезагрузка колонки'};\n"
"const isSys=a=>/^__.*(__|\\{1\\})$/.test(a);\n"
"const lab=a=>AL[a]||(isSys(a)?a:'Включить: '+a);\n"
"const SL=[['activation_prefixes',0,'Обращения','снимаются в начале фразы: «алиса, …»',1],['verbs',1,'Глаголы запуска','«включи …» — остаток фразы уходит в поиск Spotify',1],['verb_only',1,'Глагол одним словом','просто «включи» без продолжения',0],['music_prefixes',1,'Слова перед названием','«песню …», «группу …» — снимаются и работают даже без глагола',1],['question_words',1,'Вопросы — не музыка','фраза, которая так начинается, остаётся Алисе',0],['home_words',1,'Умный дом — не музыка','слово, которое так начинается, — команда дому, а не поиск',0],['say_prefixes',0,'Как просим Алису повторить','перед каждой нашей фразой — случайная, не два раза подряд. Надёжны только «повтори за мной» и «повторяй за мной»: с «алиса, …», «давай …» она включает песню, «скажи …» перефразирует',0],['reset_phrases',0,'Тихий сброс перед фразой','обрывает её ответ; случайная. Только благодарность: «хорошо»/«ладно» она может принять за «да»',0]];\n"
"const ST=[['use_announcement','b','Брать название из ответа Алисы «Включаю …»'],['announcement_priority','b','Ответ Алисы важнее распознанного текста'],['announcement_wait_ms','n','Ждать анонс Алисы, мс'],['answer_wait_ms','n','Алиса молчит (нет сети) — искать по моим словам через, мс'],['repeat_window_sec','n','Тот же запрос снова в течение, с — значит, включилось не то'],['duck_db','n','Тише музыка, пока Алиса слушает, дБ']];\n"
"let CM=null,CD=null,CO='',CT=0,COP={};\n"
"const cmdsOf=m=>(m&&m.commands)||[];\n"
"const defCmd=a=>cmdsOf(CD).find(c=>c.action===a);\n"
"const same=(a,b)=>JSON.stringify(a)===JSON.stringify(b);\n"
"function cNorm(s){s=(s||'').toLowerCase().replace(/\\s+/g,' ').trim().replace(/[.!?]+$/,'').trim();for(const p of (CM.activation_prefixes||[]))if(p&&s.startsWith(p.toLowerCase()))s=s.slice(p.length).trim();return s}\n"
"function cDirty(){let d=JSON.stringify(CM)!==CO;csv.disabled=!d;if(!cst.dataset.keep)cst.textContent=d?'Есть несохранённые изменения':'';return d}\n"
"function cMsg(t,bad){cst.dataset.keep=1;cst.textContent=t;cst.style.color=bad?'#f87171':'#1db954';clearTimeout(cMsg.t);cMsg.t=setTimeout(()=>{delete cst.dataset.keep;cst.style.color='';cDirty()},4000)}\n"
"async function openCmd(){ovc.classList.add('on');ceb.innerHTML='<div class=e>Загружаю…</div>';try{let[a,b]=await Promise.all([fetch('/api/commands',{cache:'no-store'}).then(x=>x.text()),fetch('/api/commands/default',{cache:'no-store'}).then(x=>x.text())]);CM=JSON.parse(a);try{CD=JSON.parse(b)}catch(e){CD=null}if(!CM||typeof CM!=='object')throw 0;CO=JSON.stringify(CM);COP={};ctq.value='';ctr.textContent='';cTab(CT)}catch(e){ceb.innerHTML='<div class=err>Не удалось прочитать voice-commands.json с колонки</div>'}}\n"
"function closeCmd(){if(CM&&cDirty()&&!confirm('Закрыть без сохранения? Изменения пропадут.'))return;ovc.classList.remove('on')}\n"
"ovc.addEventListener('click',e=>{if(e.target==ovc)closeCmd()});\n"
"function cTab(i){if(CT===2&&i!==2&&!cJsonOk())return;CT=i;[tb0,tb1,tb2,tb3].forEach((b,k)=>b.className=k===i?'on':'');ceb.scrollTop=0;cRender();cDirty()}\n"
"function cRender(){if(!CM)return;if(CT===0)ceb.innerHTML=cCmds();else if(CT===1)ceb.innerHTML=cSearch();else if(CT===3)ceb.innerHTML=cReplies();else{ceb.innerHTML='<textarea id=cj spellcheck=false oninput=cJsonOk()></textarea><div class=err id=cje></div>';cj.value=JSON.stringify(CM,null,2)}}\n"
"function dupMap(){let m={};cmdsOf(CM).forEach((c,i)=>(c.exact||[]).forEach(p=>{(m[p]=m[p]||[]).push(i)}));return m}\n"
"function chip(t,cls,rm,title){return '<span class=\\'chip'+(cls?' '+cls:'')+'\\''+(title?' title=\\''+esc(title).replace(/'/g,'&#39;')+'\\'':'')+'><span>'+esc(t)+'</span><button title=Убрать onclick=\\''+rm+'\\'>✕</button></span>'}\n"
"function cCmds(){let dm=dupMap(),cs=cmdsOf(CM);let h='<div class=hint style=\\'margin:0 0 4px\\'>Сначала проверяются все точные фразы, потом шаблоны — сверху вниз. Фразы пишутся маленькими буквами, без «алиса».</div>';\n"
"cs.forEach((c,i)=>{let op=!!COP[i],d=defCmd(c.action),ex=c.exact||[],re=c.regex||[];\n"
"let bd=!d&&CD?'<span class=\\'bdg n\\'>новая</span>':d&&!same(c,d)?'<span class=\\'bdg m\\'>изменено</span>':'';\n"
"let sum=ex.length?ex.slice(0,4).join(', ')+(ex.length>4?' …':''):re.length?re.length+' шабл.':'нет фраз';\n"
"h+='<div class=\\'cc'+(op?' open':'')+'\\'><div class=ch onclick=cOpen('+i+')><span class=cn>'+esc(lab(c.action))+'</span><span class=cs>'+esc(sum)+'</span>'+bd+'<span class=arw style=\\'color:#8a8a92\\'>'+(op?'▾':'▸')+'</span></div>';\n"
"if(op){h+='<div class=cb>';\n"
"if(!isSys(c.action))h+='<div class=cl>Что включить <i>— как будто сказал «включи …»</i></div><input class=add style=\\'width:100%;border-style:solid\\' value=\\''+esc(c.action).replace(/'/g,'&#39;')+'\\' onchange=cAct('+i+',this.value)>';\n"
"h+='<div class=cl>Точные фразы <i>— фраза целиком</i></div><div class=chips>'+ex.map((p,j)=>{let o=(dm[p]||[]).filter(k=>k!==i);return chip(p,o.length?'dup':'','cRm('+i+',\"exact\",'+j+')',o.length?'Эта же фраза есть в «'+lab(cs[o[0]].action)+'» — сработает первая':'')}).join('')+'<input class=add placeholder=\\'+ фраза, Enter\\' onkeydown=\\'cAdd(event,'+i+',\"exact\")\\'></div>';\n"
"h+='<div class=cl>Шаблоны <i>— regex: варианты, числа, «… в чёрный список»</i></div><div class=chips>'+re.map((p,j)=>chip(p,'re','cRm('+i+',\"regex\",'+j+')')).join('')+'<input class=add placeholder=\\'+ шаблон, Enter\\' style=\\'font-family:ui-monospace,monospace\\' onkeydown=\\'cAdd(event,'+i+',\"regex\")\\'></div>';\n"
"h+='<div class=cr>'+(d&&!same(c,d)?'<button onclick=cRevert('+i+')>↺ Как было по умолчанию</button>':'')+'<button class=del onclick=cDel('+i+')>Удалить команду</button></div></div>'}\n"
"h+='</div>'});\n"
"let used=new Set(cs.map(c=>c.action)),opts=Object.keys(AL).map(a=>'<option value=\\''+a+'\\'>'+AL[a]+(used.has(a)?' (ещё одна)':'')+'</option>').join('');\n"
"h+='<div class=cl style=\\'margin-top:16px\\'>Новая команда</div><div class=nc><select id=ncs onchange=\\'ncq.style.display=this.value===\"*\"?\"\":\"none\"\\'><option value=\\'*\\'>Включить музыку по фразе…</option>'+opts+'</select><input id=ncq placeholder=\\'что включить, напр. «группу кино»\\'><button class=lnk onclick=cNew()>Добавить</button></div>';\n"
"return h}\n"
"function cOpen(i){COP[i]=!COP[i];cRender()}\n"
"function cAdd(e,i,k){if(e.key!=='Enter')return;e.preventDefault();let el=e.target,v=k==='exact'?cNorm(el.value):el.value.trim();if(!v)return;\n"
"if(k==='regex'){try{new RegExp(v)}catch(x){cMsg('Шаблон с ошибкой: '+x.message,1);return}}\n"
"let c=cmdsOf(CM)[i],a=c[k]=c[k]||[];if(a.includes(v)){cMsg('Такая фраза уже есть',1);return}\n"
"if(k==='exact'){let o=cmdsOf(CM).findIndex((x,j)=>j!==i&&(x.exact||[]).includes(v));if(o>=0){cMsg('«'+v+'» уже есть в «'+lab(cmdsOf(CM)[o].action)+'»',1);return}}\n"
"a.push(v);cRender();let n=[...ceb.querySelectorAll('.cc.open')].find(x=>x.querySelector('.ch').getAttribute('onclick')==='cOpen('+i+')');n=n&&n.querySelectorAll('input.add');if(n&&n.length){n[k==='exact'?n.length-2:n.length-1].focus()}cDirty();cTest()}\n"
"function cRm(i,k,j){let c=cmdsOf(CM)[i];c[k].splice(j,1);if(!c[k].length)delete c[k];cRender();cDirty();cTest()}\n"
"function cAct(i,v){v=cNorm(v);if(!v){cRender();return}cmdsOf(CM)[i].action=v;cRender();cDirty()}\n"
"function cRevert(i){let c=cmdsOf(CM)[i],d=defCmd(c.action);if(!d)return;cmdsOf(CM)[i]=JSON.parse(JSON.stringify(d));cRender();cDirty();cTest()}\n"
"function cDel(i){let c=cmdsOf(CM)[i];if(!confirm('Удалить команду «'+lab(c.action)+'»?'))return;CM.commands.splice(i,1);let o={};for(const k in COP){let n=+k;if(n<i)o[n]=COP[k];else if(n>i)o[n-1]=COP[k]}COP=o;cRender();cDirty();cTest()}\n"
"function cNew(){let a=ncs.value;if(a==='*'){a=cNorm(ncq.value);if(!a){cMsg('Напиши, что включать',1);return}}CM.commands=CM.commands||[];CM.commands.push({action:a,exact:[]});COP[CM.commands.length-1]=true;cRender();cDirty();setTimeout(()=>{let l=ceb.querySelectorAll('.cc');l=l[l.length-1];if(l){l.scrollIntoView({block:'center'});let x=l.querySelector('.chips input.add');if(x)x.focus()}},0)}\n"
"function cSearch(){let h='';CM.search=CM.search||{};\n"
"for(const[k,inS,t,hint,sp]of SL){let a=(inS?CM.search[k]:CM[k])||[];h+='<div class=sec><h3>'+t+'</h3><div class=hint>'+hint+'</div><div class=chips>'+a.map((p,j)=>chip(p.trim(),'','cLrm(\"'+k+'\",'+j+')')).join('')+'<input class=add placeholder=\\'+ добавить, Enter\\' onkeydown=\\'cLadd(event,\"'+k+'\")\\'></div></div>'}\n"
"let s=CM.settings=CM.settings||{};h+='<div class=sec><h3>Поведение</h3><div class=setg>';\n"
"for(const[k,ty,t]of ST){h+='<label for=st_'+k+' style=\\'margin:0;color:#cfcfd6;font-size:13px\\'>'+t+'</label>';h+=ty==='b'?'<input type=checkbox id=st_'+k+(s[k]!==false?' checked':'')+' onchange=\\'cSet(\"'+k+'\",this.checked)\\'>':'<input type=number min=0 id=st_'+k+' value=\\''+(s[k]==null?'':s[k])+'\\' onchange=\\'cSet(\"'+k+'\",this.value===\"\"?undefined:Math.max(0,+this.value))\\'>'}\n"
"return h+'</div></div>'}\n"
"const slOf=k=>SL.find(x=>x[0]===k);\n"
"function cLarr(k,mk){let d=slOf(k),o=d[1]?(CM.search=CM.search||{}):CM;if(!o[k]&&mk)o[k]=[];return o[k]}\n"
"function cLadd(e,k){if(e.key!=='Enter')return;e.preventDefault();let v=e.target.value.toLowerCase().replace(/\\s+/g,' ').trim();if(!v)return;if(slOf(k)[4])v+=' ';let a=cLarr(k,1);if(a.includes(v)){cMsg('Уже есть',1);return}a.push(v);cRender();let i=[...ceb.querySelectorAll('input.add')].find(x=>x.getAttribute('onkeydown').includes('\"'+k+'\"'));if(i)i.focus();cDirty();cTest()}\n"
"function cLrm(k,j){cLarr(k).splice(j,1);cRender();cDirty();cTest()}\n"
"function cSet(k,v){if(v===undefined)delete CM.settings[k];else CM.settings[k]=v;cDirty()}\n"
"const RL=[['now_playing','Что играет','ответ на «что сейчас играет?»','это {track}'],['now_paused','Что играет — на паузе','то же, когда музыка на паузе','на паузе {track}'],['nothing_playing','Ничего не играет','«что играет?» или «в чёрный список», а музыки нет','ничего не играет'],['blacklist_artist','Исполнитель в чёрный список','«группу … в чёрный список»','{artist} теперь в чёрном списке'],['blacklist_track','Трек в чёрный список','«трек в чёрный список»','{track} теперь в чёрном списке'],['reboot','Перезагрузка','«Алиса, перезагрузись»','перезагружаюсь'],['reboot_refused','Перезагрузка не нужна','колонка включилась меньше 2 минут назад','я только что перезагрузилась'],['clear_start','Очистка очереди — начало','«очисти очередь»; {count} — сколько треков','начинаю чистить очередь, подождите окончания'],['clear_done','Очистка очереди — готово','когда очередь опустела','я всё очистила'],['clear_empty','Очистка очереди — пусто','очередь и так пустая','очередь и так пустая'],['clear_partial','Очистка очереди — не до конца','Spotify перестал убирать треки; {count} — сколько осталось','очистила не всё, осталось {count}'],['wrong_retry','«Ты поставила не то»','перед новым поиском того, что ты просил','сейчас поищу другое'],['wrong_unknown','«Не то», а просьбы не было','последняя просьба старше 3 часов или её нет','не помню, что ты просил, скажи ещё раз']];\n"
"const rPrev=p=>p.replace(/\\{track\\}/g,'Kino, Звезда').replace(/\\{artist\\}/g,'Kino').replace(/\\{title\\}/g,'Звезда').replace(/\\{count\\}/g,'12');\n"
"function cReplies(){let R=CM.replies=CM.replies||{},h='<div class=hint style=\\'margin:0 0 4px\\'>Это Алиса говорит за нас («повтори за мной …»). Каждый раз берётся случайный вариант, один и тот же подряд не повторяется. Подстановки: <b>{track}</b> — «исполнитель, название», <b>{artist}</b> — исполнитель, <b>{title}</b> — название, <b>{count}</b> — число треков (очистка очереди). Не пиши «включи», «играет», «песня», «трек» рядом с названием — Алиса примет за музыкальный запрос.</div>';\n"
"for(const[k,t,hint,def]of RL){let a=R[k]||[];h+='<div class=sec><h3>'+esc(t)+'</h3><div class=hint>'+esc(hint)+(a.length?'':' · сейчас по умолчанию: «'+esc(def)+'»')+'</div><div class=chips>'+a.map((p,j)=>chip(p,'','cRrm(\\\"'+k+'\\\",'+j+')','Например: '+rPrev(p))).join('')+'<input class=add placeholder=\\'+ вариант, Enter\\' onkeydown=\\'cRadd(event,\\\"'+k+'\\\")\\'></div></div>'}\n"
"return h}\n"
"function cRadd(e,k){if(e.key!=='Enter')return;e.preventDefault();let v=e.target.value.replace(/\\s+/g,' ').trim();if(!v)return;\n"
"if(/(^|[\\s,])(включи|играет|песн|трек)/i.test(v.replace(/\\{[a-z]+\\}/g,''))&&!confirm('Слова «включи», «играет», «песня», «трек» могут заставить Алису включить музыку вместо ответа. Всё равно добавить?'))return;\n"
"let R=CM.replies=CM.replies||{},a=R[k]=R[k]||[];if(a.includes(v)){cMsg('Такой вариант уже есть',1);return}a.push(v);cRender();let i=[...ceb.querySelectorAll('input.add')].find(x=>x.getAttribute('onkeydown').includes('\\\"'+k+'\\\"'));if(i)i.focus();cDirty()}\n"
"function cRrm(k,j){let a=CM.replies[k];a.splice(j,1);if(!a.length)delete CM.replies[k];cRender();cDirty()}\n"
"function cJsonOk(){let e=document.getElementById('cje');try{let v=JSON.parse(cj.value);if(!v||typeof v!=='object'||Array.isArray(v))throw new Error('нужен объект { … }');CM=v;cj.className='';if(e)e.textContent='';cDirty();cTest();return true}catch(x){cj.className='bad';if(e)e.textContent='Ошибка JSON: '+x.message;csv.disabled=true;return false}}\n"
"function cTest(){let raw=ctq.value;if(!raw.trim()||!CM){ctr.textContent='';return}let s=cNorm(raw.replace(/^([^,]{1,15}), /,'$1 ')),cs=cmdsOf(CM),r=null;\n"
"for(const c of cs)if((c.exact||[]).includes(s)){r=[c.action,'точная фраза']; break}\n"
"if(!r)for(const c of cs){for(const p of(c.regex||[])){let ok=false;try{ok=new RegExp(p).test(s)}catch(e){}if(ok){if(/\\{1\\}/.test(c.action)&&!/[0-9]/.test(s))continue;r=[c.action,'шаблон '+p];break}}if(r)break}\n"
"let out;if(r)out=(isSys(r[0])?'<b>'+esc(lab(r[0]))+'</b>':'<b>включить «'+esc(r[0])+'»</b>')+' · '+esc(r[1]);\n"
"else{let q=cSearchQ(s);out=q?'<b>поиск в Spotify: «'+esc(q.q)+'»'+(q.a?' (исполнитель)':'')+'</b>':null}\n"
"ctr.className='tr'+(out?'':' no');ctr.innerHTML=out?'→ '+out:'→ <b>не команда</b> — ответит сама Алиса'}\n"
"function cSearchQ(s){let S=CM.search||{},rest=null,ex=false,art=false;for(const v of S.verbs||[])if(s.startsWith(v)){rest=s.slice(v.length);break}\n"
"if(rest===null){if((S.verb_only||[]).includes(s))return {q:s};rest=s}rest=rest.trim();\n"
"let m=rest.match(/^((песни|треки|музыку) )?(группу|группа|группы|исполнителя|исполнитель|артиста|артист|певца|певец|певицу|певица|певицы|рэпера|рэпер|музыканта) /);if(m){rest=rest.slice(m[0].length);ex=art=true}\n"
"else for(const p of S.music_prefixes||[])if(rest.startsWith(p)){rest=rest.slice(p.length);ex=true;break}\n"
"if(rest===s&&!ex)return null;if(rest.length<2)return null;for(const q of S.question_words||[])if(rest.startsWith(q))return null;\n"
"if(!ex)for(const w of rest.split(' '))for(const h of S.home_words||[])if(w.startsWith(h))return null;return {q:rest,a:art}}\n"
"async function cPost(u,body){cst.dataset.keep=1;cst.style.color='';cst.textContent='Проверяю и перезапускаю прокси…';csv.disabled=true;try{let r=await(await fetch(u,{method:'POST',headers:{'Content-Type':'application/json'},body:body||''})).json();delete cst.dataset.keep;return r}catch(e){delete cst.dataset.keep;return {ok:false,err:'Нет связи с колонкой'}}}\n"
"async function cSave(){if(CT===2&&!cJsonOk())return;let r=await cPost('/api/commands/save',JSON.stringify(CM,null,2));if(r.ok){CO=JSON.stringify(CM);cRender();cMsg('Сохранено: '+(r.msg||'').replace(/^OK /,'')+(r.restarted?' · прокси перезапущен':' · прокси не поднялся!'),!r.restarted)}else{cDirty();cMsg('Не сохранено: '+(r.err||'ошибка'),1)}}\n"
"async function cReset(){if(!CD){cMsg('На колонке нет файла по умолчанию',1);return}if(!confirm('Вернуть все команды, слова поиска и настройки к заводским?\\nВсе твои изменения пропадут.'))return;let r=await cPost('/api/commands/reset');if(r.ok){CM=JSON.parse(JSON.stringify(CD));CO=JSON.stringify(CM);COP={};cRender();cMsg('Вернул по умолчанию'+(r.restarted?' · прокси перезапущен':''),!r.restarted)}else{cDirty();cMsg('Не сброшено: '+(r.err||'ошибка'),1)}}\n"
"r();setInterval(r,2000)</script>";

static void send_all(int fd, const char *s) { size_t n=strlen(s); while(n){ ssize_t w=send(fd,s,n,0); if(w<=0)return; s+=w;n-=w; } }
static void solo_ctl(const char *op) {
  if(!fork()) { execl("/data/opt/solo/run.sh","ctl", "ctl",op,"-D","/data/opt/spotify-connect/receiver-state/soloist/data",(char*)0); _exit(127); }
}
static void search_query(const char *q) {
  char text[401]; size_t n=0;
  for(;*q && *q!=' ' && *q!='\r' && n<400;q++) { char c=*q; if(c=='+' ) c=' '; if(c=='%'&&q[1]&&q[2]){int a=(q[1]|32)-'0',b=(q[2]|32)-'0'; if(a>9)a-=39;if(b>9)b-=39;if(a>=0&&a<16&&b>=0&&b<16){c=(char)(a*16+b);q+=2;}} if((unsigned char)c>=32)text[n++]=c; } text[n]=0;
  if(n && !fork()){execl("/data/opt/spotify-connect/alice-spotify-search.sh","alice-spotify-search",text,(char*)0);_exit(127);}
}
#define CREDS "/data/opt/spotify-connect/receiver-state/web-api/credentials.env"
#define NETCONF "/data/opt/spotify-connect/receiver-state/web-api/net.conf"
#define SOLOKEY "/data/opt/spotify-connect/receiver-state/soloist/api-key"
#define OAUTHF  "/data/opt/spotify-connect/receiver-state/web-api/oauth.env"
#define OAUTHSH "/data/opt/spotify-connect/spotify-oauth.sh"
#define DEF_PROXY ""
#define DEF_REDIR "http://127.0.0.1:8088/callback"

// Extract &key=value from a request line, URL-decoding into out (until & or space).
static int get_param(const char *req, const char *key, char *out, size_t cap) {
  char needle[48]; snprintf(needle, sizeof needle, "%s=", key);
  const char *p = strstr(req, needle);
  if (!p) { out[0]=0; return 0; }
  p += strlen(needle);
  size_t n=0;
  for (; *p && *p!='&' && *p!=' ' && *p!='\r' && n+1<cap; p++) {
    char ch=*p;
    if (ch=='+') ch=' ';
    else if (ch=='%'&&p[1]&&p[2]) { int a=(p[1]|32)-'0',b=(p[2]|32)-'0'; if(a>9)a-=39; if(b>9)b-=39; if(a>=0&&a<16&&b>=0&&b<16){ch=(char)(a*16+b);p+=2;} }
    if ((unsigned char)ch>=32) out[n++]=ch;
  }
  out[n]=0; return 1;
}
static void read_kv(const char *file, const char *key, char *out, size_t cap) {
  out[0]=0; FILE *f=fopen(file,"r"); if(!f) return;
  char line[512], needle[64]; snprintf(needle,sizeof needle,"%s=",key);
  size_t kl=strlen(needle);
  while (fgets(line,sizeof line,f)) {
    if (!strncmp(line,needle,kl)) {
      char *v=line+kl; v[strcspn(v,"\r\n")]=0;
      size_t L=strlen(v); if (L>=2 && (v[0]=='"'||v[0]=='\'') && v[L-1]==v[0]) { v[L-1]=0; v++; }
      snprintf(out,cap,"%s",v); break;
    }
  }
  fclose(f);
}
static void solo_seek(const char *ms) {
  for (const char *p=ms; *p; p++) if (*p<'0'||*p>'9') return;
  if (!*ms) return;
  if(!fork()){ execl("/data/opt/solo/run.sh","ctl","ctl","seek",ms,"-D","/data/opt/spotify-connect/receiver-state/soloist/data",(char*)0); _exit(127); }
}
/* A suggestion picked on the page: play exactly that track (or that artist's
   tracks, for an artist row), no text search. */
static void play_track(const char *req) {
  char uri[96], artist[256], title[256];
  get_param(req,"uri",uri,sizeof uri);
  get_param(req,"artist",artist,sizeof artist);
  get_param(req,"title",title,sizeof title);
  int art=!strncmp(uri,"spotify:artist:",15);
  const char *id=uri+(art?15:14);
  if ((!art && strncmp(uri,"spotify:track:",14)) || !*id) return;
  for (const char *p=id; *p; p++)
    if (!((*p>='A'&&*p<='Z')||(*p>='a'&&*p<='z')||(*p>='0'&&*p<='9'))) return;
  if(!fork()){
    if (art) execl("/data/opt/spotify-connect/alice-spotify-search.sh","alice-spotify-search","--artist",uri,title,(char*)0);
    else execl("/data/opt/spotify-connect/alice-spotify-search.sh","alice-spotify-search","--track",uri,artist,title,(char*)0);
    _exit(127);
  }
}
static int conf_sug(void);
static void send_suggest(int c, const char *req) {
  char q[401], n[8]; get_param(req,"q",q,sizeof q);
  snprintf(n,sizeof n,"%d",conf_sug());
  send_all(c,"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");
  if (!q[0]) { send_all(c,"[]"); return; }
  pid_t pid=fork();
  if (pid==0) { dup2(c,1); dup2(c,2); execl("/data/opt/spotify-connect/alice-suggest.sh","alice-suggest",q,n,(char*)0); _exit(0); }
  if (pid>0) waitpid(pid,0,0);
}
// True if the file exists and holds at least one non-whitespace byte.
static int file_filled(const char *path) {
  FILE *f=fopen(path,"r"); if(!f) return 0;
  int ch, filled=0;
  while ((ch=fgetc(f))!=EOF) { if(ch>' '){filled=1;break;} }
  fclose(f); return filled;
}
// net.conf / credentials.env are SOURCED by the shell scripts (as root): a value
// with a newline, quote, $ or ` would run as a command.  URLs: printable, none of
// ' " \\ ` $ and no spaces, written in '…'.  Spotify IDs/secrets: letters/digits.
static int url_ok(const char *v) {
  size_t n=strlen(v); if(!n || n>240) return 0;
  for(const unsigned char *p=(const unsigned char*)v;*p;p++)
    if(*p<=' ' || *p==0x7f || strchr("'\"\\`$",*p)) return 0;
  return 1;
}
static int proxy_ok(const char *v) {
  static const char *const sch[]={"socks5h://","socks5://","socks4a://","socks4://","http://","https://",NULL};
  if(!url_ok(v)) return 0;
  for(int i=0;sch[i];i++) if(!strncmp(v,sch[i],strlen(sch[i])) && v[strlen(sch[i])]) return 1;
  return 0;
}
static int alnum_ok(const char *v) {
  if(!*v || strlen(v)>128) return 0;
  for(const char *p=v;*p;p++) if(!isalnum((unsigned char)*p)) return 0;
  return 1;
}
// "scheme://user:PASS@host" -> the PASS span (start, length), or 0 if none.
static int proxy_pass(const char *v, size_t *at, size_t *len) {
  const char *s=strstr(v,"://"); if(!s) return 0; s+=3;
  const char *a=strrchr(s,'@'); if(!a) return 0;
  const char *c=memchr(s,':',(size_t)(a-s)); if(!c) return 0;
  *at=(size_t)(c+1-v); *len=(size_t)(a-c-1); return 1;
}
#define PASS_MASK "***"
static void write_netconf(const char *proxy, const char *redir) {
  FILE *f=fopen(NETCONF ".tmp","w");
  if (!f) return;
  fprintf(f,"PROXY='%s'\nREDIRECT_URI='%s'\n", proxy, redir);
  fclose(f); rename(NETCONF ".tmp",NETCONF); chmod(NETCONF,0600);
}
static int oauth_run(const char *sub, const char *arg, char *out, size_t cap);
static void send_cover(int c) {
  send_all(c,"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");
  pid_t pid=fork();
  if (pid==0) { dup2(c,1); int n=open("/dev/null",1); if(n>=0)dup2(n,2); execl("/data/opt/spotify-connect/alice-cover.sh","alice-cover",(char*)0); _exit(0); }
  if (pid>0) waitpid(pid,0,0);
}
// Voice journal written by alice-voice-proxy: "epoch\tkind\ttext" per line.
// /api/voicelog -> [{"t":epoch,"k":"say|alice|play|cmd","x":"…"}, …] (oldest first).
#ifndef VOICE_JOURNAL
#define VOICE_JOURNAL "/tmp/alice-voice-journal.log"
#endif
static void send_voicelog(int c) {
  send_all(c,"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");
  static char buf[48*1024];
  size_t n=0;
  FILE *f=fopen(VOICE_JOURNAL,"rb");
  if(f){ if(fseek(f,-(long)(sizeof buf-1),SEEK_END)!=0) fseek(f,0,SEEK_SET); n=fread(buf,1,sizeof buf-1,f); fclose(f); }
  buf[n]=0;
  char *p=buf;
  if(n==sizeof buf-1){ char *nl=strchr(p,'\n'); p=nl?nl+1:p+n; }   // cut-off first line
  send_all(c,"[");
  int first=1;
  for(char *save=0,*line=strtok_r(p,"\n",&save); line; line=strtok_r(0,"\n",&save)){
    char *k=strchr(line,'\t'); if(!k) continue; *k++=0;
    char *x=strchr(k,'\t'); if(!x) continue; *x++=0;
    if(strspn(line,"0123456789")!=strlen(line) || strlen(k)>8) continue;
    char xe[1300], ke[20], one[1400];
    json_escape(xe,sizeof xe,x); json_escape(ke,sizeof ke,k);
    snprintf(one,sizeof one,"%s{\"t\":%s,\"k\":\"%s\",\"x\":\"%s\"}",first?"":",",line,ke,xe);
    send_all(c,one); first=0;
  }
  send_all(c,"]");
}
static void clear_voicelog(void) { int fd=open(VOICE_JOURNAL,O_WRONLY|O_TRUNC); if(fd>=0) close(fd); }
// Чёрный список (alice-blacklist.sh): "kind\turi\tname\tepoch" per line.
// /api/blacklist -> [{"k":"artist|track","u":uri,"n":name,"t":epoch}, …] (oldest first).
#ifndef BLACKLIST
#define BLACKLIST "/data/opt/spotify-connect/receiver-state/blacklist.txt"
#endif
static void send_blacklist(int c) {
  send_all(c,"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n[");
  FILE *f=fopen(BLACKLIST,"r"); int first=1; char line[1024];
  while(f && fgets(line,sizeof line,f)){
    line[strcspn(line,"\r\n")]=0;
    char *u=strchr(line,'\t'); if(!u) continue; *u++=0;
    char *n=strchr(u,'\t'); if(!n) continue; *n++=0;
    char *t=strchr(n,'\t'); if(t) *t++=0; else t="0";
    if(strcmp(line,"artist") && strcmp(line,"track")) continue;
    char ue[200], ne[900], one[1200];
    json_escape(ue,sizeof ue,u); json_escape(ne,sizeof ne,n);
    snprintf(one,sizeof one,"%s{\"k\":\"%s\",\"u\":\"%s\",\"n\":\"%s\",\"t\":%ld}",first?"":",",line,ue,ne,strtol(t,0,10));
    send_all(c,one); first=0;
  }
  if(f) fclose(f);
  send_all(c,"]");
}
// /api/netdiag -> net-probe.sh's Wi‑Fi snapshots (receiver-state/netdiag/*.txt),
// newest first, as one text page.
#define NETDIAG "/data/opt/spotify-connect/receiver-state/netdiag"
static int cmp_desc(const void *a, const void *b) { return strcmp(*(char*const*)b, *(char*const*)a); }
static void send_netdiag(int c) {
  send_all(c,"HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");
  DIR *d=opendir(NETDIAG); if(!d){ send_all(c,"Снимков пока нет.\n"); return; }
  char *names[64]; int n=0; struct dirent *e;
  while((e=readdir(d)) && n<64){ size_t l=strlen(e->d_name); if(l>4 && !strcmp(e->d_name+l-4,".txt")) names[n++]=strdup(e->d_name); }
  closedir(d);
  qsort(names,(size_t)n,sizeof *names,cmp_desc);
  if(!n) send_all(c,"Снимков пока нет.\n");
  size_t total=0; char path[256], buf[4096];
  for(int i=0;i<n;i++){
    snprintf(path,sizeof path,NETDIAG "/%s",names[i]);
    FILE *f=fopen(path,"r");
    if(f && total<120000){ size_t r; while((r=fread(buf,1,sizeof buf-1,f))>0){ buf[r]=0; send_all(c,buf); total+=r; } send_all(c,"\n"); }
    if(f) fclose(f);
    free(names[i]);
  }
}
// /api/blacklist/add?k=track|artist -> alice-blacklist.sh add-track | add-artist current
// (what plays now; it is skipped at once).  argv, no shell.  -> {"ok":…}
static int blacklist_add(const char *req) {
  char k[16]; get_param(req,"k",k,sizeof k);
  const char *sub = !strcmp(k,"track") ? "add-track" : !strcmp(k,"artist") ? "add-artist" : 0;
  if(!sub) return 0;
  pid_t pid=fork();
  if(pid==0){ int nf=open("/dev/null",O_RDWR); if(nf>=0){dup2(nf,0);dup2(nf,1);dup2(nf,2);}
    if(!strcmp(sub,"add-artist")) execl("/data/opt/spotify-connect/alice-blacklist.sh","alice-blacklist",sub,"current",(char*)0);
    else execl("/data/opt/spotify-connect/alice-blacklist.sh","alice-blacklist",sub,(char*)0);
    _exit(127); }
  if(pid<0) return 0;
  int st=0; if(waitpid(pid,&st,0)!=pid) return 1;   /* reaped elsewhere: assume it ran */
  return WIFEXITED(st) && WEXITSTATUS(st)==0;
}
// /api/blacklist/del?u=URI -> alice-blacklist.sh remove URI (argv, no shell; URI checked).
static void blacklist_del(const char *req) {
  char u[128]; get_param(req,"u",u,sizeof u);
  const char *id = !strncmp(u,"spotify:artist:",15) ? u+15 : !strncmp(u,"spotify:track:",14) ? u+14 : 0;
  if(!id || !*id || strspn(id,"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789")!=strlen(id)) return;
  pid_t pid=fork();
  if(pid==0){ int nf=open("/dev/null",O_RDWR); if(nf>=0){dup2(nf,0);dup2(nf,1);dup2(nf,2);} execl("/data/opt/spotify-connect/alice-blacklist.sh","alice-blacklist","remove",u,(char*)0); _exit(127); }
  if(pid>0) waitpid(pid,0,0);
}
// {"liked":true|false} for the now-playing track (spotify-oauth.sh liked -> 1/0).
static void send_liked(int c) {
  char out[16]; int rc=oauth_run("liked","current",out,sizeof out);
  send_all(c,"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");
  send_all(c, (rc==0 && out[0]=='1') ? "{\"liked\":true}" : "{\"liked\":false}");
}
static int conf_port(void);
static int conf_sug(void);
static void settings_json(char *out, size_t cap) {
  char cid[256]="", proxy[256]="", secret[256]="", redir[256]="";
  read_kv(CREDS,"CLIENT_ID",cid,sizeof cid);
  read_kv(NETCONF,"PROXY",proxy,sizeof proxy);
  if (!proxy[0]) snprintf(proxy,sizeof proxy,DEF_PROXY);
  read_kv(NETCONF,"REDIRECT_URI",redir,sizeof redir);
  if (!redir[0]) snprintf(redir,sizeof redir,DEF_REDIR);
  read_kv(CREDS,"CLIENT_SECRET",secret,sizeof secret);
  // the proxy's password never leaves the speaker: "user:***@host"
  size_t pa, pl;
  if (proxy_pass(proxy,&pa,&pl) && pl) {
    char m[256]; snprintf(m,sizeof m,"%.*s" PASS_MASK "%s",(int)pa,proxy,proxy+pa+pl);
    snprintf(proxy,sizeof proxy,"%s",m);
  }
  char cidE[300], proxyE[300], redirE[300];
  json_escape(cidE,sizeof cidE,cid); json_escape(proxyE,sizeof proxyE,proxy); json_escape(redirE,sizeof redirE,redir);
  snprintf(out,cap,"{\"client_id\":\"%s\",\"proxy\":\"%s\",\"redirect_uri\":\"%s\",\"secret_set\":%s,\"soloist_key_set\":%s,\"oauth_set\":%s,\"port\":%d,\"port_default\":%d,\"suggest_n\":%d,\"suggest_default\":%d}",
    cidE, proxyE, redirE, secret[0]?"true":"false", file_filled(SOLOKEY)?"true":"false", file_filled(OAUTHF)?"true":"false", conf_port(), PORT, conf_sug(), SUG_DEF);
}
static void settings_save(const char *req, char *err, size_t ecap) {
  char cid[256], sec[256], proxy[256], redir[256], skey[4200];
  err[0]=0;
  get_param(req,"client_id",cid,sizeof cid);
  get_param(req,"secret",sec,sizeof sec);
  get_param(req,"proxy",proxy,sizeof proxy);
  get_param(req,"redirect_uri",redir,sizeof redir);
  get_param(req,"soloist_key",skey,sizeof skey);
  // the page got "user:***@host": the same password, unless one was typed in
  size_t pa, pl;
  if (proxy_pass(proxy,&pa,&pl) && pl==strlen(PASS_MASK) && !strncmp(proxy+pa,PASS_MASK,pl)) {
    char cur[256]; size_t ca, cl;
    read_kv(NETCONF,"PROXY",cur,sizeof cur);
    if (proxy_pass(cur,&ca,&cl)) {
      char m[256]; snprintf(m,sizeof m,"%.*s%.*s%s",(int)pa,proxy,(int)cl,cur+ca,proxy+pa+pl);
      snprintf(proxy,sizeof proxy,"%s",m);
    }
  }
  if (proxy[0] && !proxy_ok(proxy)) { snprintf(err,ecap,"Прокси: нужен адрес вида socks5h://ip:порт или http://ip:порт (с паролем: socks5h://логин:пароль@ip:порт), без пробелов и символов ' \" \\ ` $ (их — в виде %%XX)"); return; }
  if (redir[0] && !url_ok(redir)) { snprintf(err,ecap,"Redirect URI: без пробелов и символов ' \" \\ ` $"); return; }
  if ((cid[0] && !alnum_ok(cid)) || (sec[0] && !alnum_ok(sec))) { snprintf(err,ecap,"Client ID / Secret — только латинские буквы и цифры"); return; }
  if (cid[0] || sec[0]) {
    char curid[256], cursec[256];
    read_kv(CREDS,"CLIENT_ID",curid,sizeof curid);
    read_kv(CREDS,"CLIENT_SECRET",cursec,sizeof cursec);
    FILE *f=fopen(CREDS ".tmp","w");
    if (f) { fprintf(f,"CLIENT_ID=%s\nCLIENT_SECRET=%s\n", cid[0]?cid:curid, sec[0]?sec:cursec); fclose(f); rename(CREDS ".tmp",CREDS); chmod(CREDS,0600); }
  }
  if (proxy[0] || redir[0]) {
    char curpx[256], currd[256];
    read_kv(NETCONF,"PROXY",curpx,sizeof curpx); if(!curpx[0]) snprintf(curpx,sizeof curpx,DEF_PROXY);
    read_kv(NETCONF,"REDIRECT_URI",currd,sizeof currd); if(!currd[0]) snprintf(currd,sizeof currd,DEF_REDIR);
    write_netconf(proxy[0]?proxy:curpx, redir[0]?redir:currd);
  }
  // Soloist API key: owner-only plaintext in the soloist state dir (mode 600).
  if (skey[0]) {
    int ok=1; for(const char*p=skey;*p;p++) if((unsigned char)*p<=' '){ok=0;break;}
    if (ok) {
      FILE *f=fopen(SOLOKEY ".tmp","w");
      if (f) { fprintf(f,"%s\n",skey); fclose(f); rename(SOLOKEY ".tmp",SOLOKEY); chmod(SOLOKEY,0600); }
    }
  }
}
// Run spotify-oauth.sh <sub> [arg] and capture its stdout into out. argv-exec
// (no shell) so the attacker-controlled code/arg is never interpreted.
// Returns the child's exit status (0 on success), or -1 on spawn failure.
static int oauth_run(const char *sub, const char *arg, char *out, size_t cap) {
  out[0]=0;
  int pfd[2]; if (pipe(pfd)) return -1;
  pid_t pid=fork();
  if (pid<0) { close(pfd[0]); close(pfd[1]); return -1; }
  if (pid==0) {
    close(pfd[0]); dup2(pfd[1],1); close(pfd[1]);
    if (arg) execl(OAUTHSH,"spotify-oauth",sub,arg,(char*)0);
    else     execl(OAUTHSH,"spotify-oauth",sub,(char*)0);
    _exit(127);
  }
  close(pfd[1]);
  size_t n=0; ssize_t r;
  char buf[256];
  while ((r=read(pfd[0],buf,sizeof buf))>0) { for(ssize_t i=0;i<r&&n+1<cap;i++) out[n++]=buf[i]; }
  out[n]=0; close(pfd[0]);
  while (out[0] && (out[strlen(out)-1]=='\n'||out[strlen(out)-1]=='\r')) out[strlen(out)-1]=0;
  int st=0; waitpid(pid,&st,0);
  return (WIFEXITED(st)?WEXITSTATUS(st):-1);
}
// Currently-playing spotify track id from status.json, "" if none.
static void current_track_id(char *out, size_t cap) {
  out[0]=0; FILE *f=fopen("/data/opt/spotify-connect/monitor/status.json","r"); if(!f) return;
  char buf[4096]; size_t n=fread(buf,1,sizeof buf-1,f); buf[n]=0; fclose(f);
  char *p=strstr(buf,"spotify:track:"); if(!p) return;
  p+=14; size_t i=0;
  while (p[i] && ((p[i]>='A'&&p[i]<='Z')||(p[i]>='a'&&p[i]<='z')||(p[i]>='0'&&p[i]<='9')) && i+1<cap) { out[i]=p[i]; i++; }
  out[i]=0;
}
// ---- Listen port (⚙ Настройки) ----
// 8443 is the TLS front (socat) for the OAuth redirect; it forwards to this port.
static int port_ok(long p) { return p >= 1024 && p <= 65535 && p != 8443; }
static int conf_port(void) {
  char v[16]; read_kv(MONCONF,"PORT",v,sizeof v);
  long p=strtol(v,0,10); return port_ok(p) ? (int)p : PORT;
}
// Rows in the search dropdown (⚙ Настройки, MONCONF SUGGEST_N=…).
static int conf_sug(void) {
  char v[16]; read_kv(MONCONF,"SUGGEST_N",v,sizeof v);
  long n=strtol(v,0,10); return n>=1 && n<=SUG_MAX ? (int)n : SUG_DEF;
}
static int write_monconf(long port, int sug) {
  FILE *f=fopen(MONCONF ".tmp","w"); if(!f) return 0;
  fprintf(f,"PORT=%ld\nSUGGEST_N=%d\n",port,sug); fclose(f); rename(MONCONF ".tmp",MONCONF);
  return 1;
}
static void save_sug(const char *v, char *err, size_t cap) {
  err[0]=0; if(!v[0]) return;
  char *e; long n=strtol(v,&e,10);
  if(*e || n<1 || n>SUG_MAX){ snprintf(err,cap,"Строк в подсказках — число 1–%d",SUG_MAX); return; }
  if(n!=conf_sug() && !write_monconf(conf_port(),(int)n)) snprintf(err,cap,"Не записать настройки");
}
static int open_listener(int port) {
  int s=socket(AF_INET,SOCK_STREAM,0), yes=1; if(s<0) return -1;
  setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof yes);
  struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(port),.sin_addr.s_addr=htonl(INADDR_ANY)};
  if(bind(s,(struct sockaddr*)&a,sizeof a)<0 || listen(s,8)<0){ close(s); return -1; }
  fcntl(s,F_SETFD,FD_CLOEXEC);
  return s;
}
#ifndef TLS_PID
#define TLS_PID "/run/spotify-monitor-tls.pid"
#endif
#ifndef START_MONITOR
#define START_MONITOR "/data/opt/spotify-connect/monitor/start-monitor.sh"
#endif
// pid from a pidfile if that process's name starts with `comm`, else 0 (never
// signal a stale pid that now belongs to something else).
static pid_t pid_of(const char *pidfile, const char *comm) {
  char b[32], path[48], name[32]; FILE *f=fopen(pidfile,"r");
  if(!f) return 0; if(!fgets(b,sizeof b,f)){ fclose(f); return 0; } fclose(f);
  long n=strtol(b,0,10); if(n<2) return 0;
  snprintf(path,sizeof path,"/proc/%ld/comm",n); f=fopen(path,"r"); if(!f) return 0;
  if(!fgets(name,sizeof name,f)) name[0]=0; fclose(f);
  return strncmp(name,comm,strlen(comm)) ? 0 : (pid_t)n;
}
static int alive(pid_t p) { char path[32]; snprintf(path,sizeof path,"/proc/%d",(int)p); return p>0 && access(path,F_OK)==0; }
static void stop_pid(pid_t p) { // TERM, then KILL after 3 s
  if(p<=0) return; kill(p,SIGTERM);
  for(int i=0;i<30 && alive(p);i++) usleep(100000);
  if(alive(p)){ kill(p,SIGKILL); for(int i=0;i<10 && alive(p);i++) usleep(100000); }
}
static void spawn_detached(const char *path, const char *arg0, const char *out) {
  pid_t p=fork(); if(p) return;
  setsid(); int nf=open("/dev/null",O_RDWR), of=open(out,O_WRONLY|O_CREAT|O_APPEND,0644);
  if(nf>=0) dup2(nf,0); if(of>=0){ dup2(of,1); dup2(of,2); }
  execl(path,arg0,(char*)0); _exit(127);
}
// The TLS front on :8443 forwards to the monitor port: restart it on the new one
// (start-monitor.sh reads the port from MONCONF and starts only what is down).
static void restart_tls_front(void) {
  stop_pid(pid_of(TLS_PID,"socat"));
  spawn_detached(START_MONITOR,"start-monitor","/tmp/spotify-monitor-tls.log");
}
// Save a new port: checked here (range, free) so the page can say why not.
// Returns 1 when the port changes; the parent rebinds on SIGUSR1.
static int save_port(const char *v, char *err, size_t cap) {
  err[0]=0; if(!v[0]) return 0;
  char *e; long p=strtol(v,&e,10);
  if(*e || !port_ok(p)){ snprintf(err,cap,"Порт — число 1024–65535, кроме 8443"); return 0; }
  if(p==conf_port()) return 0;
  int t=open_listener((int)p);
  if(t<0){ snprintf(err,cap,"Порт %ld уже занят другой программой",p); return 0; }
  close(t);
  if(!write_monconf(p,conf_sug())){ snprintf(err,cap,"Не записать настройки"); return 0; }
  return 1;
}

// ---- Голосовые команды (voice-commands.json of alice-voice-proxy) ----
#ifndef COMMANDS
#define COMMANDS "/data/opt/spotify-connect/voice-commands.json"
#endif
#ifndef COMMANDS_DEF
#define COMMANDS_DEF "/data/opt/spotify-connect/voice-commands.default.json"
#endif
#ifndef PROXY_BIN
#define PROXY_BIN "/data/opt/spotify-connect/alice-voice-proxy"
#endif
#ifndef PROXY_PID
#define PROXY_PID "/run/alice-voice-proxy.pid"
#endif
// The voice proxy has no port of its own (it is aliced's client): its pidfile.
// (9877, checked before, is maind's own port, not the proxy's.)
static int voice_proxy_running(void) { return pid_of(PROXY_PID,"alice-voice-pro") > 0; }
#define COMMANDS_MAX (256*1024)
static void send_file_json(int c, const char *path) {
  send_all(c,"HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");
  int fd=open(path,O_RDONLY); if(fd<0){ send_all(c,"null"); return; }
  char b[4096]; ssize_t n; while((n=read(fd,b,sizeof b))>0){ if(send(c,b,(size_t)n,0)<=0) break; } close(fd);
}
// Run `alice-voice-proxy --check FILE` (the proxy's own loader: JSON + every
// regex as the proxy compiles it); its one-line verdict goes to msg.
static int check_commands(const char *file, char *msg, size_t cap) {
  msg[0]=0; int pfd[2]; if(pipe(pfd)) return -1;
  pid_t p=fork(); if(p<0){ close(pfd[0]); close(pfd[1]); return -1; }
  if(p==0){ close(pfd[0]); dup2(pfd[1],1); dup2(pfd[1],2); execl(PROXY_BIN,"alice-voice-proxy","--check",file,(char*)0); _exit(127); }
  close(pfd[1]); size_t n=0; ssize_t r; char b[256];
  while((r=read(pfd[0],b,sizeof b))>0) for(ssize_t i=0;i<r && n+1<cap;i++) msg[n++]=b[i];
  msg[n]=0; close(pfd[0]); msg[strcspn(msg,"\r\n")]=0;
  int st=0; waitpid(p,&st,0); return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
// The proxy reads the JSON once at start: restart it (the receiver reconnects
// to :9876 by itself within a second).  Returns 1 if the new one is running.
static int restart_proxy(void) {
  stop_pid(pid_of(PROXY_PID,"alice-voice-pro"));
  pid_t p=fork(); if(p<0) return 0;
  if(p==0){ setsid(); int nf=open("/dev/null",O_RDWR), of=open("/tmp/alice-voice-proxy.out",O_WRONLY|O_CREAT|O_TRUNC,0644);
    if(nf>=0) dup2(nf,0); if(of>=0){ dup2(of,1); dup2(of,2); }
    execl(PROXY_BIN,"alice-voice-proxy",(char*)0); _exit(127); }
  FILE *f=fopen(PROXY_PID,"w"); if(f){ fprintf(f,"%d\n",(int)p); fclose(f); }
  usleep(700000);   // a failed bind exits at once
  return waitpid(p,0,WNOHANG)==0;
}
// Validate `body` (or the default file when body==NULL), make it the live file,
// restart the proxy; answers {"ok":…,"msg"|"err":…}.
static void apply_commands(int c, const char *body, size_t len) {
  char msg[400], one[700], me[460]; int ok=0, run=0;
  const char *tmp=COMMANDS ".new";
  int fd=open(tmp,O_WRONLY|O_CREAT|O_TRUNC,0644), wr=fd>=0;
  if(wr && body){ size_t o=0; while(o<len){ ssize_t w=write(fd,body+o,len-o); if(w<=0){ wr=0; break; } o+=(size_t)w; } }
  else if(wr){ int s=open(COMMANDS_DEF,O_RDONLY); char b[4096]; ssize_t n; if(s<0) wr=0;
    else { while((n=read(s,b,sizeof b))>0) if(write(fd,b,(size_t)n)!=n){ wr=0; break; } close(s); } }
  if(fd>=0) close(fd);
  if(!wr) snprintf(msg,sizeof msg,body?"Не удалось записать файл":"Нет файла по умолчанию");
  else if(check_commands(tmp,msg,sizeof msg)!=0){ if(!msg[0]) snprintf(msg,sizeof msg,"Прокси не принял файл"); }
  else if(rename(tmp,COMMANDS)==0){ ok=1; sync(); run=restart_proxy(); }
  else snprintf(msg,sizeof msg,"Не удалось заменить файл");
  if(!ok) unlink(tmp);
  json_escape(me,sizeof me,msg);
  snprintf(one,sizeof one,ok?"{\"ok\":true,\"msg\":\"%s\",\"restarted\":%s}":"{\"ok\":false,\"err\":\"%s\"%s}",me,ok?(run?"true":"false"):"");
  send_all(c,"HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");
  send_all(c,one);
}
// Rest of a POST: headers up to the blank line, then Content-Length bytes.
// `got` bytes of the request are already in req.  Returns malloc'd body or NULL.
static char *read_body(int c, const char *req, size_t got, size_t *len) {
  size_t cap=COMMANDS_MAX+8192, n=got; char *b=malloc(cap+1); if(!b) return 0;
  memcpy(b,req,got); b[n]=0;
  char *hend;
  while(!(hend=strstr(b,"\r\n\r\n"))){ ssize_t r=recv(c,b+n,cap-n,0); if(r<=0){ free(b); return 0; } n+=(size_t)r; b[n]=0; }
  long cl=-1;
  for(char *h=b; h<hend; h=strstr(h,"\r\n")+2) if(!strncasecmp(h,"Content-Length:",15)){ cl=strtol(h+15,0,10); break; }
  size_t hl=(size_t)(hend+4-b);
  if(cl<0 || cl>COMMANDS_MAX){ free(b); return 0; }
  while(n<hl+(size_t)cl){ ssize_t r=recv(c,b+n,cap-n,0); if(r<=0){ free(b); return 0; } n+=(size_t)r; }
  memmove(b,b+hl,(size_t)cl); b[cl]=0; *len=(size_t)cl; return b;
}

// Minimal HTML page returned to the browser after the OAuth redirect.
static void send_callback(int c, const char *req) {
  char code[512], out[256];
  get_param(req,"code",code,sizeof code);
  const char *msg; int ok=0;
  if (!code[0]) msg="Нет кода авторизации в ссылке.";
  else if (oauth_run("finish",code,out,sizeof out)==0 && !strcmp(out,"OK")) { msg="Авторизация завершена — токен сохранён на устройстве. Можно закрыть вкладку."; ok=1; }
  else msg="Не удалось обменять код. Проверь Client ID/Secret, Redirect URI и прокси, затем повтори.";
  send_all(c,"HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n");
  send_all(c,"<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
             "<body style='font:17px/1.5 system-ui;background:#0d0d10;color:#ececf0;max-width:460px;margin:40px auto;padding:0 16px'>");
  send_all(c, ok ? "<h2 style='color:#1db954'>✓ Готово</h2>" : "<h2 style='color:#f87171'>Ошибка</h2>");
  send_all(c,"<p>"); send_all(c,msg); send_all(c,"</p>");
}

// A request child saved a new port: SIGUSR1 -> the server moves there (the old
// listener closes only once the new one is up; no SA_RESTART so accept() wakes).
static volatile sig_atomic_t rebind;
static void on_usr1(int sig) { (void)sig; rebind=1; }

int main(void) {
  int port=conf_port(), s=open_listener(port);
  if(s<0 && port!=PORT){ port=PORT; s=open_listener(port); }   // configured port taken: default
  if(s<0) return 1;
  struct sigaction sa; memset(&sa,0,sizeof sa); sa.sa_handler=on_usr1; sigaction(SIGUSR1,&sa,0);
  for(;;){ int c=accept(s,0,0);
    if(c<0){
      if(errno!=EINTR) break;
      if(rebind){ rebind=0; int np=conf_port(), ns=np!=port?open_listener(np):-1; if(ns>=0){ close(s); s=ns; port=np; } }
      continue;
    }
    fcntl(c,F_SETFD,FD_CLOEXEC);   // a proxy/socat restarted from here must not hold the page's socket
    pid_t child=fork();
    if(child<0){close(c);continue;}
    if(child>0){close(c);while(waitpid(-1,0,WNOHANG)>0){}continue;}
    close(s);
    struct timeval timeout={.tv_sec=2,.tv_usec=0}; setsockopt(c,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);
    char req[1024]={0}; ssize_t rn=recv(c,req,sizeof req-1,0); if(rn<=0){close(c);continue;}
    if(!strncmp(req,"GET /api/status ",16)){ char body[8192];status(body,sizeof body);send_all(c,"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");send_all(c,body); }
    else if(!strncmp(req,"GET /api/ctl/",13)){ char op[16]={0}; sscanf(req+13,"%15[^ ?]",op); if(!strcmp(op,"pause")||!strcmp(op,"play")||!strcmp(op,"next")||!strcmp(op,"prev"))solo_ctl(op);send_all(c,"HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n"); }
    else if(!strncmp(req,"GET /api/search?q=",18)){ search_query(req+18);send_all(c,"HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n"); }
    else if(!strncmp(req,"GET /api/play?",14)){ play_track(req);send_all(c,"HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n"); }
    else if(!strncmp(req,"GET /api/seek?ms=",17)){ char ms[16]; get_param(req,"ms",ms,sizeof ms); solo_seek(ms);send_all(c,"HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n"); }
    else if(!strncmp(req,"GET /api/suggest?q=",19)){ send_suggest(c,req); }
    else if(!strncmp(req,"GET /api/cover ",15)){ send_cover(c); }
    else if(!strncmp(req,"GET /api/liked ",15)){ send_liked(c); }
    else if(!strncmp(req,"GET /api/voicelog ",18)){ send_voicelog(c); }
    else if(!strncmp(req,"GET /api/blacklist/del?",23)){ blacklist_del(req);send_all(c,"HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n"); }
    else if(!strncmp(req,"GET /api/blacklist ",19)){ send_blacklist(c); }
    else if(!strncmp(req,"GET /api/netdiag ",17)){ send_netdiag(c); }
    else if(!strncmp(req,"GET /api/blacklist/add?",23)){ int ok=blacklist_add(req);send_all(c,"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");send_all(c,ok?"{\"ok\":true}":"{\"ok\":false}"); }
    else if(!strncmp(req,"GET /api/voicelog/clear ",24)){ clear_voicelog();send_all(c,"HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n"); }
    else if(!strncmp(req,"GET /api/favorites ",19)){ char o[16];int rc=oauth_run("favorites",0,o,sizeof o);send_all(c,rc==0&&!strcmp(o,"OK")?"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"ok\":true}":"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"ok\":false}"); }
    else if(!strncmp(req,"GET /api/settings/save?",23)){ char serr[200]; settings_save(req,serr,sizeof serr);
      char pv[16], err[200], j[480]; int moved=0; snprintf(err,sizeof err,"%s",serr);
      if(!err[0]){ get_param(req,"suggest_n",pv,sizeof pv); save_sug(pv,err,sizeof err); }
      if(!err[0]){ get_param(req,"port",pv,sizeof pv); moved=save_port(pv,err,sizeof err); }
      if(err[0]){ char eE[420]; json_escape(eE,sizeof eE,err); snprintf(j,sizeof j,"{\"ok\":false,\"err\":\"%s\"}",eE); } else snprintf(j,sizeof j,"{\"ok\":true,\"port\":%d,\"moved\":%s}",conf_port(),moved?"true":"false");
      send_all(c,"HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");send_all(c,j);
      if(moved){ shutdown(c,SHUT_WR); kill(getppid(),SIGUSR1); restart_tls_front(); } }
    else if(!strncmp(req,"GET /api/commands ",18)){ send_file_json(c,COMMANDS); }
    else if(!strncmp(req,"GET /api/commands/default ",26)){ send_file_json(c,COMMANDS_DEF); }
    else if(!strncmp(req,"POST /api/commands/save ",24)){ size_t bl=0; char *b=read_body(c,req,(size_t)rn,&bl);
      if(b){ apply_commands(c,b,bl); free(b); } else send_all(c,"HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nConnection: close\r\n\r\n{\"ok\":false,\"err\":\"Запрос не дошёл целиком или больше 256 КБ\"}"); }
    else if(!strncmp(req,"POST /api/commands/reset ",25)){ apply_commands(c,0,0); }
    else if(!strncmp(req,"GET /api/settings ",18)){ char body[900];settings_json(body,sizeof body);send_all(c,"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");send_all(c,body); }
    else if(!strncmp(req,"GET /api/oauth/begin ",21)){ char url[1200];int rc=oauth_run("begin",0,url,sizeof url);char j[1300];char uE[1250];json_escape(uE,sizeof uE,url);snprintf(j,sizeof j,"{\"url\":\"%s\"}",rc==0?uE:"");send_all(c,"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");send_all(c,j); }
    else if(!strncmp(req,"GET /api/oauth/finish?code=",27)){ char code[512],out[256];get_param(req,"code",code,sizeof code);int rc=oauth_run("finish",code,out,sizeof out);send_all(c,rc==0&&!strcmp(out,"OK")?"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"ok\":true}":"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"ok\":false}"); }
    else if(!strncmp(req,"GET /callback",13)){ send_callback(c,req); }
    else if(!strncmp(req,"GET /api/like ",14)){ char out[64];int rc=oauth_run("like","current",out,sizeof out);send_all(c,rc==0&&!strcmp(out,"OK")?"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"ok\":true}":"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"ok\":false}"); }
    else { send_all(c,"HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n");send_all(c,page); } close(c);
    _exit(0);
  } return 0;
}
