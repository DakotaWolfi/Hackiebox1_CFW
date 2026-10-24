#!/usr/bin/env python3
# Settings and state files of the teddyCloud extension for the player harness.
# usage: tbs.py <card> profile <uid hex B0..B7> key=value ...
#        tbs.py <card> global key=value ...
#        tbs.py <card> event <uid hex> action:mode:value ... [tones=0]
#        tbs.py <card> show <uid hex | global>
#        tbs.py <card> state <uid hex> audio_id position seed index chapters flags
#        tbs.py <card> showstate
# keys: resume shuffle sleep spk hp led ears slap tilt vol idlebat idlest
# SPDX-License-Identifier: GPL-3.0-or-later
import os, struct, sys

KEYS = {'resume': 1, 'shuffle': 2, 'sleep': 3, 'spk': 4, 'hp': 5, 'led': 6, 'ears': 7, 'slap': 8, 'tilt': 9, 'vol': 10, 'idlebat': 11, 'idlest': 12}
NAMES = {v: k for k, v in KEYS.items()}


def crc(data):
    c = 0
    for b in data:
        c ^= b << 24
        for _ in range(8):
            c = ((c << 1) ^ 0x04C11DB7) if c & 0x80000000 else c << 1
            c &= 0xFFFFFFFF
    return c


def path(card, uid):
    if uid == 'global':
        return os.path.join(card, 'CFW', 'EXT', 'GLOBAL.TBS')
    return os.path.join(card, 'CFW', 'EXT', uid[:8].upper(), uid[8:].upper() + '.TBS')


def write(p, kind, body):
    f = b'TBS1' + bytes([kind, 1]) + struct.pack('<HI', len(body), 0) + body
    f += struct.pack('<I', crc(f))
    os.makedirs(os.path.dirname(p), exist_ok=True)
    open(p, 'wb').write(f)


def records(args):
    body = b''
    for a in args:
        k, v = a.split('=')
        v = int(v, 0)
        body += bytes([KEYS[k], 2]) + struct.pack('<H', v) if KEYS[k] in (3, 11, 12) else bytes([KEYS[k], 1, v])
    return body


def show(p):
    if not os.path.exists(p):
        print('absent')
        return
    f = open(p, 'rb').read()
    n = struct.unpack('<H', f[6:8])[0]
    ok = crc(f[:12 + n]) == struct.unpack('<I', f[12 + n:16 + n])[0]
    out, i = [], 12
    while i < 12 + n:
        k, l = f[i], f[i + 1]
        v = int.from_bytes(f[i + 2:i + 2 + l], 'little')
        out.append('%s=%d' % (NAMES.get(k, hex(k)), v))
        i += 2 + l
    print('kind %d origin %d %s crc %s' % (f[4], f[5], ' '.join(out) or '(empty)', 'ok' if ok else 'BAD'))


card, cmd = sys.argv[1], sys.argv[2]
if cmd == 'profile':
    write(path(card, sys.argv[3]), 1, records(sys.argv[4:]))
elif cmd == 'global':
    write(path(card, 'global'), 3, records(sys.argv[3:]))
elif cmd == 'event':
    body = b''
    for a in sys.argv[4:]:
        if a.startswith('tones='):
            body += bytes([0x11, 1, int(a[6:])])
            continue
        act, mode, val = (int(x) for x in a.split(':'))
        body += bytes([0x10, 3, act]) + struct.pack('<H', val | mode << 14)
    write(path(card, sys.argv[3]), 2, body)
elif cmd == 'show':
    show(path(card, sys.argv[3]))
elif cmd == 'state':
    uid = bytes.fromhex(sys.argv[3])
    aid, pos, seed, idx, ch, fl = (int(x, 0) for x in sys.argv[4:10])
    r = uid + struct.pack('<IIII', 1, aid, pos, seed) + bytes([idx, ch, fl, 0, 0, 0, 0])
    c = 0xA5
    for b in r:
        c ^= b
    p = os.path.join(card, 'CFW', 'EXT', 'STATE.BIN')
    os.makedirs(os.path.dirname(p), exist_ok=True)
    open(p, 'wb').write(r + bytes([c]) + bytes(2048 - 32))
elif cmd == 'showstate':
    p = os.path.join(card, 'CFW', 'EXT', 'STATE.BIN')
    if not os.path.exists(p):
        print('no state file')
        sys.exit()
    d = open(p, 'rb').read()
    print('state file %d bytes' % len(d))
    for i in range(0, len(d), 32):
        r = d[i:i + 32]
        seq, aid, pos, seed = struct.unpack('<IIII', r[8:24])
        if seq:
            print('  %s seq %d audio_id %08x position %d seed %08x index %d chapters %d flags %d' %
                  (r[:8].hex().upper(), seq, aid, pos, seed, r[24], r[25], r[26]))
