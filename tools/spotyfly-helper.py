#!/usr/bin/env python3
"""PC-side helper for installer.sh / uninstaller.sh (python3, stdlib only).

  soloist DIR        download the official Soloist (arm32) from Spotify into DIR,
                     with only its ELF interpreter pointed at /data/opt/solo/ld.so
  compat DIR         download the Soloist compatibility runtime (glibc 2.38 armhf,
                     apulse) published by spotiya, checked against a pinned SHA-256
  cfg-patch IN OUT ORIG    quasar.cfg: the stock music players play local silence,
                     mediad talks to the local relay; the replaced values -> ORIG
  cfg-unpatch IN OUT ORIG  put the ORIG values back
  cfg-info IN        print deviceType and whether the patch is in place
  elf FILE           print the ELF class/arch of FILE (from `od` output on stdin
                     when FILE is "-": the first 20 bytes as hex)

Nothing here sends anything anywhere except the two downloads above.
"""
import hashlib
import io
import json
import os
import struct
import sys
import tarfile
import urllib.request

SOLOIST_URL = 'https://soloist-builds.spotifycdn.com/soloist_release_arm32.tar.gz'
COMPAT_URL = ('https://github.com/klockky/spotiya/releases/download/v0.10.1/'
              'spotiya-soloist-compat-armv7.tar.gz')
COMPAT_SHA256 = '6b312cd44d71572ddf2a46f1900fe3e8a1196feb51301cdd55ead4fae9f7082e'
SOLO_DIR = '/data/opt/solo'
SILENCE = ('filesrc location=/data/opt/spotify-connect/silence.wav ! decodebin ! '
           'audioconvert ! audioresample ! fakesink sync=true')
RELAY_URL = 'ws://127.0.0.1:9/quasar/websocket'
# (path in quasar.cfg, our value)
PATCH = [
    (('audioclient', 'controller', 'playbackParams', 'RADIO', 'gstPipeline'), SILENCE),
    (('audioclient', 'controller', 'playbackParams', 'TRACK', 'gstPipeline'), SILENCE),
    (('audioclient', 'controller', 'playbackParams', 'TRACK:CACHE', 'gstPipeline'), SILENCE),
    (('audioclient', 'controller', 'playbackParams', 'TRACK:DBUF', 'gstPipeline'), SILENCE),
    (('mediad', 'playbackParams', 'gstPipeline'), SILENCE),
    (('mediad', 'apiUrl'), RELAY_URL),
]
LIMIT = 64 * 1024 * 1024


def die(msg):
    print('spotyfly-helper: ' + msg, file=sys.stderr)
    sys.exit(1)


def fetch(url):
    with urllib.request.urlopen(url, timeout=60) as r:
        data = r.read(LIMIT + 1)
    if len(data) > LIMIT:
        die('download larger than 64 MiB: ' + url)
    return data


def set_interpreter(binary, path):
    data = bytearray(binary)
    if data[:6] != b'\x7fELF\x01\x01' or struct.unpack_from('<H', data, 18)[0] != 40:
        die('not the official ARM32 little-endian Soloist ELF')
    phoff = struct.unpack_from('<I', data, 28)[0]
    size, count = struct.unpack_from('<HH', data, 42)
    for i in range(count):
        entry = struct.unpack_from('<8I', data, phoff + i * size)
        if entry[0] != 3:                       # PT_INTERP
            continue
        value = path.encode() + b'\0'
        if len(value) > entry[4]:
            die('interpreter path does not fit the ELF header')
        data[entry[1]:entry[1] + entry[4]] = value.ljust(entry[4], b'\0')
        return bytes(data)
    die('ELF interpreter not found')


def soloist(out):
    archive = fetch(SOLOIST_URL)
    os.makedirs(out, exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(archive), mode='r:gz') as tar:
        member = tar.getmember('soloist')
        if not member.isfile() or not 0 < member.size <= LIMIT:
            die('bad soloist member in the archive')
        binary = tar.extractfile(member).read()
        # Only PT_INTERP changes; running ld.so directly would break Crashpad.
        patched = set_interpreter(binary, SOLO_DIR + '/ld.so')
        with open(os.path.join(out, 'soloist'), 'wb') as f:
            f.write(patched)
        os.chmod(os.path.join(out, 'soloist'), 0o755)
        for name in ('CHANGELOG.md', 'THIRD_PARTY_LICENSES.txt'):
            m = tar.getmember(name)
            if m.isfile() and m.size <= 4 * 1024 * 1024:
                with open(os.path.join(out, name), 'wb') as f:
                    f.write(tar.extractfile(m).read())
    with open(os.path.join(out, 'installed.json'), 'w') as f:
        json.dump({'format': 1, 'owner': '/data/opt/spotify-connect', 'source': SOLOIST_URL,
                   'archive_sha256': hashlib.sha256(archive).hexdigest(),
                   'original_sha256': hashlib.sha256(binary).hexdigest(),
                   'installed_sha256': hashlib.sha256(patched).hexdigest()}, f, indent=2)
    print('soloist: %.1f MiB' % (len(patched) / 1048576))


def compat(out):
    data = fetch(COMPAT_URL)
    if hashlib.sha256(data).hexdigest() != COMPAT_SHA256:
        die('compat runtime checksum mismatch')
    os.makedirs(out, exist_ok=True)
    want = ('ld.so', 'lib/', 'licenses/')
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:gz') as tar:
        for m in tar.getmembers():
            rel = m.name.split('/', 1)[1] if '/' in m.name else ''
            if not rel or not rel.startswith(want) or '..' in rel.split('/'):
                continue
            dest = os.path.join(out, rel)
            if m.isdir():
                os.makedirs(dest, exist_ok=True)
            elif m.isfile():
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                with open(dest, 'wb') as f:
                    f.write(tar.extractfile(m).read())
                os.chmod(dest, 0o755 if not rel.startswith('licenses/') else 0o644)
    print('compat runtime: ok')


def load_cfg(path):
    with open(path, 'rb') as f:
        raw = f.read()
    try:
        return json.loads(raw.decode('utf-8'))
    except ValueError as e:
        die('quasar.cfg is not valid JSON: %s' % e)


def save_cfg(cfg, path):
    text = json.dumps(cfg, indent=3, ensure_ascii=False, separators=(',', ' : ')) + '\n'
    if json.loads(text) != cfg:
        die('round trip of quasar.cfg changed it')
    with open(path, 'w', encoding='utf-8') as f:
        f.write(text)


def node(cfg, path):
    cur = cfg
    for key in path[:-1]:
        if not isinstance(cur, dict) or key not in cur:
            return None
        cur = cur[key]
    return cur if isinstance(cur, dict) and path[-1] in cur else None


def cfg_patch(src, dst, orig):
    cfg = load_cfg(src)
    missing = ['/'.join(p) for p, _ in PATCH if node(cfg, p) is None]
    if missing:
        die('unknown quasar.cfg layout, missing: ' + ', '.join(missing))
    saved = {}
    if os.path.exists(orig):
        with open(orig) as f:
            saved = json.load(f)
    for path, value in PATCH:
        n = node(cfg, path)
        key = '/'.join(path)
        if n[path[-1]] != value and key not in saved:
            saved[key] = n[path[-1]]
        n[path[-1]] = value
    with open(orig, 'w') as f:
        json.dump(saved, f, indent=2, ensure_ascii=False)
    save_cfg(cfg, dst)
    print('patched: %d values (%d originals known)' % (len(PATCH), len(saved)))


def device_type(cfg):
    dev = cfg.get('common', {}).get('deviceType', '')
    if not dev:
        for v in cfg.values():
            if isinstance(v, dict) and isinstance(v.get('deviceType'), str):
                return v['deviceType']
    return dev


def cfg_unpatch(src, dst, orig):
    cfg = load_cfg(src)
    saved = {}
    if os.path.exists(orig):
        with open(orig) as f:
            saved = json.load(f)
    done = 0
    for path, value in PATCH:
        key = '/'.join(path)
        n = node(cfg, path)
        if n is None or key not in saved:
            continue
        if n[path[-1]] == value:
            n[path[-1]] = saved[key]
            done += 1
    save_cfg(cfg, dst)
    left = [('/'.join(p)) for p, v in PATCH if node(cfg, p) is not None and node(cfg, p)[p[-1]] == v]
    print('restored: %d values' % done)
    if left:
        print('still ours (no original known): ' + ', '.join(left))


def cfg_info(src):
    cfg = load_cfg(src)
    dev = device_type(cfg)
    known = [node(cfg, p) is not None for p, _ in PATCH]
    ours = [node(cfg, p) is not None and node(cfg, p)[p[-1]] == v for p, v in PATCH]
    state = 'patched' if all(ours) else ('unknown-layout' if not all(known) else
                                        ('partly' if any(ours) else 'stock'))
    print('deviceType=%s patch=%s' % (dev or '?', state))


def elf(hexbytes):
    b = bytes(int(x, 16) for x in hexbytes.split())
    if b[:4] != b'\x7fELF' or len(b) < 20:
        print('unknown')
        return
    cls = {1: '32', 2: '64'}.get(b[4], '?')
    mach = struct.unpack_from('<H', b, 18)[0]
    print({(40, '32'): 'armv7', (183, '64'): 'aarch64', (62, '64'): 'x86_64',
           (8, '32'): 'mips'}.get((mach, cls), 'machine%d-%s' % (mach, cls)))


def main(a):
    if len(a) == 2 and a[0] == 'soloist':
        soloist(a[1])
    elif len(a) == 2 and a[0] == 'compat':
        compat(a[1])
    elif len(a) == 4 and a[0] == 'cfg-patch':
        cfg_patch(*a[1:])
    elif len(a) == 4 and a[0] == 'cfg-unpatch':
        cfg_unpatch(*a[1:])
    elif len(a) == 2 and a[0] == 'cfg-info':
        cfg_info(a[1])
    elif len(a) == 2 and a[0] == 'elf':
        elf(sys.stdin.read() if a[1] == '-' else a[1])
    else:
        print(__doc__)
        sys.exit(2)


if __name__ == '__main__':
    main(sys.argv[1:])
