#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""PROTOTYPE: the port's settings in the game's own Options > Video screen, made from the player's
own game files.

The game's menus are Scaleform movies (p1/art/hud/*.gfx, SWF with ActionScript 2). The Video
screen (protovideooption.gfx, and protopausevideo.gfx in the pause menu) still holds the PC
version's rows (resolution, anti-aliasing, V-Sync...) behind `_global.PLATFORM == RAD_WIN32`, so
the PS4 shows only Brightness. Here:

  * the screens' code after the Brightness row is replaced: it asks the port for its rows
    (commonregisters.DoCallBack, the game's own way to call native code) and adds one game
    toggle per row. The port answers those calls itself (game/p1/options_menu.cpp, rows in
    gpu/shim/port_game_menu.cpp); without it the loop adds nothing and the screen is the PS4's.
  * protooptionsoverlay.gfx gets the toggles those rows use (tglPort0...), copies of the screen's
    own toggle placed in its VideoOptionsOverlay.

The results go to <out>/p1/art/hud/ (THEORDER_FILE_OVERRIDES, src/runtime_file.c). The game
folder is only read, and nothing written here is part of a release.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

HUD = Path('p1') / 'art' / 'hud'
SCREENS = ('protovideooption.gfx', 'protopausevideo.gfx')
OVERLAY = 'protooptionsoverlay.gfx'
ROWS = 12          # toggles placed for the port's rows (the port uses as many as it has rows)
VERSION = 1        # of this script's output (part of the cache key)


# ---- SWF structure

def tags(data, pos, end):
    """[(code, header start, body start, body end)] of the tags in data[pos:end]."""
    out = []
    while pos < end:
        code_len = struct.unpack_from('<H', data, pos)[0]
        code, length = code_len >> 6, code_len & 0x3f
        body = pos + 2
        if length == 0x3f:
            length = struct.unpack_from('<I', data, body)[0]
            body += 4
        out.append((code, pos, body, body + length))
        pos = body + length
        if code == 0:
            break
    return out


def tag(code, body):
    """A tag with its long header (any length)."""
    return struct.pack('<HI', (code << 6) | 0x3f, len(body)) + body


def movie_start(data):
    """Offset of the first tag (after the header, frame rect, rate and count)."""
    if data[:3] not in (b'GFX', b'FWS'):
        raise ValueError('not an uncompressed GFX/SWF movie')
    nbits = data[8] >> 3
    return 8 + (5 + 4 * nbits + 7) // 8 + 4


def with_length(data):
    return data[:4] + struct.pack('<I', len(data)) + data[8:]


# ---- ActionScript 2 bytecode

def actions(code, pos, end, pool):
    """[(offset, op, body offset, body length, values)] (values: Push's, ConstantPool's)."""
    out = []
    while pos < end:
        op = code[pos]
        if op == 0:
            out.append((pos, 0, pos + 1, 0, None))
            pos += 1
            continue
        length = struct.unpack_from('<H', code, pos + 1)[0] if op >= 0x80 else 0
        body = pos + 3 if op >= 0x80 else pos + 1
        values = None
        if op == 0x88:
            n = struct.unpack_from('<H', code, body)[0]
            p, pool[:] = body + 2, []
            for _ in range(n):
                e = code.index(b'\0', p)
                pool.append(code[p:e].decode('utf-8', 'replace'))
                p = e + 1
            values = list(pool)
        elif op == 0x96:
            values, p = [], body
            while p < body + length:
                t = code[p]; p += 1
                if t == 0:
                    e = code.index(b'\0', p); values.append(code[p:e].decode('utf-8', 'replace')); p = e + 1
                elif t in (1, 7):
                    values.append(None); p += 4
                elif t in (2, 3):
                    values.append(None)
                elif t in (4, 5):
                    values.append(None); p += 1
                elif t == 6:
                    values.append(None); p += 8
                elif t == 8:
                    i = code[p]; p += 1; values.append(pool[i] if i < len(pool) else None)
                elif t == 9:
                    i = struct.unpack_from('<H', code, p)[0]; p += 2; values.append(pool[i] if i < len(pool) else None)
                else:
                    raise ValueError(f'unknown Push type {t}')
        elif op in (0x9B, 0x8E):
            # A function: its body follows the definition; it is walked as part of the code.
            pass
        out.append((pos, op, body, length, values))
        pos = body + length
    return out


class Asm:
    """A tiny ActionScript 2 assembler: Push, plain actions, Jump/If to labels."""
    OPS = {'Pop': 0x17, 'GetVariable': 0x1C, 'SetVariable': 0x1D, 'Not': 0x12, 'Less2': 0x48,
           'Add2': 0x47, 'GetMember': 0x4E, 'SetMember': 0x4F, 'Increment': 0x50, 'CallMethod': 0x52,
           'NewObject': 0x40, 'InitObject': 0x43, 'DefineLocal': 0x3C}

    def __init__(self):
        self.parts, self.labels, self.fixups = [], {}, []

    def push(self, *values):
        body = b''
        for v in values:
            if isinstance(v, bool):
                body += b'\x05' + bytes([1 if v else 0])
            elif isinstance(v, int):
                body += b'\x07' + struct.pack('<i', v)
            elif isinstance(v, str):
                body += b'\x00' + v.encode('utf-8') + b'\0'
            elif v is None:
                body += b'\x02'
            else:
                raise TypeError(v)
        self.parts.append(b'\x96' + struct.pack('<H', len(body)) + body)

    def op(self, *names):
        for name in names:
            self.parts.append(bytes([self.OPS[name]]))

    def label(self, name):
        self.labels[name] = sum(len(p) for p in self.parts)

    def branch(self, kind, name):
        self.fixups.append((len(self.parts), name))
        self.parts.append(bytes([0x99 if kind == 'Jump' else 0x9D]) + b'\x02\x00\x00\x00')

    def bytes(self):
        offsets, at = [], 0
        for p in self.parts:
            offsets.append(at)
            at += len(p)
        parts = list(self.parts)
        for index, name in self.fixups:
            after = offsets[index] + 5
            parts[index] = parts[index][:3] + struct.pack('<h', self.labels[name] - after)
        return b''.join(parts)

    # helpers
    def get(self, name):
        self.push(name); self.op('GetVariable')

    def callback(self, function, *args):
        """commonregisters.DoCallBack(function, args...): the value is left on the stack. args are
        variable names (str) pushed with GetVariable."""
        for a in reversed(args):
            self.get(a)
        self.push(function, len(args) + 1, 'commonregisters'); self.op('GetVariable')
        self.push('DoCallBack'); self.op('CallMethod')


def port_rows():
    """The code that adds the port's rows to mVideoOptions (after the game's Brightness row)."""
    a = Asm()
    # var __pn = commonregisters.DoCallBack("port_Count"); var __pi = 0;
    a.push('__pn'); a.callback('port_Count'); a.op('DefineLocal')
    a.push('__pi', 0); a.op('DefineLocal')
    a.label('row')
    a.get('__pi'); a.get('__pn'); a.op('Less2', 'Not'); a.branch('If', 'done')
    # var __pv = new Array(); var __pc = DoCallBack("port_Choices", __pi); var __pj = 0;
    a.push('__pv', 0, 'Array'); a.op('NewObject', 'DefineLocal')
    a.push('__pc'); a.callback('port_Choices', '__pi'); a.op('DefineLocal')
    a.push('__pj', 0); a.op('DefineLocal')
    a.label('choice')
    a.get('__pj'); a.get('__pc'); a.op('Less2', 'Not'); a.branch('If', 'choices_done')
    # __pv.push({mLabel: DoCallBack("port_Choice", __pi, __pj), mValue: __pj});
    a.push('mLabel'); a.callback('port_Choice', '__pi', '__pj')
    a.push('mValue'); a.get('__pj')
    a.push(2); a.op('InitObject')
    a.push(1); a.get('__pv'); a.push('push'); a.op('CallMethod', 'Pop')
    a.push('__pj'); a.get('__pj'); a.op('Increment', 'SetVariable')
    a.branch('Jump', 'choice')
    a.label('choices_done')
    # mVideoOptions.push(new optionscontainer(mcVideoOverlay["tglPort" + __pi], "PortSetting" + __pi,
    #     optionscontainer.DBTYPE_INT, DoCallBack("port_Label", __pi), __pv));
    a.get('__pv')
    a.callback('port_Label', '__pi')
    a.get('optionscontainer'); a.push('DBTYPE_INT'); a.op('GetMember')
    a.push('PortSetting'); a.get('__pi'); a.op('Add2')
    a.get('mcVideoOverlay'); a.push('tglPort'); a.get('__pi'); a.op('Add2', 'GetMember')
    a.push(5, 'optionscontainer'); a.op('NewObject')
    a.push(1); a.get('mVideoOptions'); a.push('push'); a.op('CallMethod', 'Pop')
    a.push('__pi'); a.get('__pi'); a.op('Increment', 'SetVariable')
    a.branch('Jump', 'row')
    a.label('done')
    return a.bytes()


def patch_screen(data):
    """protovideooption.gfx / protopausevideo.gfx: the rows after Brightness come from the port."""
    start = movie_start(data)
    for code, head, body, end in tags(data, start, len(data)):
        if code != 12:  # DoAction
            continue
        pool = []
        acts = actions(data, body, end, pool)
        gamma = next((i for i, a in enumerate(acts) if a[1] == 0x96 and a[4] and 'SettingVideoGammaCorrection' in a[4]), None)
        if gamma is None:
            continue
        first = next(i for i in range(gamma, len(acts)) if acts[i][1] == 0x17) + 1   # after the row's Pop
        last = next((i for i in range(first, len(acts)) if acts[i][1] == 0x96 and acts[i][4] and acts[i][4][0] == 'mConfirmParams'), None)
        if last is None:
            raise ValueError('the end of the Video rows was not found')
        names = [v for a in acts[first:last] if a[1] == 0x96 and a[4] for v in a[4]]
        if 'SettingVideoVSyncEnabled' not in names or 'SettingVideoGraphicQuality' not in names:
            raise ValueError('the PC rows are not where they were expected')
        lo, hi = acts[first][0], acts[last][0]
        code_bytes = data[body:lo] + port_rows() + data[hi:end]
        return with_length(data[:head] + tag(12, code_bytes) + data[end:])
    raise ValueError('the Brightness row was not found')


def matrix(tx, ty):
    """A MATRIX record with a translation only."""
    nbits = max(int(v).bit_length() + 1 for v in (tx, ty, 1))
    bits = '0' + '0' + format(nbits, '05b') + format(tx & ((1 << nbits) - 1), f'0{nbits}b') + \
        format(ty & ((1 << nbits) - 1), f'0{nbits}b')
    bits += '0' * (-len(bits) % 8)
    return bytes(int(bits[i:i + 8], 2) for i in range(0, len(bits), 8))


def place(depth, character, name, tx, ty):
    return tag(26, bytes([0x26]) + struct.pack('<HH', depth, character) + matrix(tx, ty) + name.encode() + b'\0')


def patch_overlay(data):
    """protooptionsoverlay.gfx: toggles tglPort0... in VideoOptionsOverlay, below its list."""
    start = movie_start(data)
    top = tags(data, start, len(data))
    exported = None
    for code, head, body, end in top:
        if code == 56:
            n = struct.unpack_from('<H', data, body)[0]
            p = body + 2
            for _ in range(n):
                cid = struct.unpack_from('<H', data, p)[0]
                e = data.index(b'\0', p + 2)
                if data[p + 2:e] == b'VideoOptionsOverlay':
                    exported = cid
                p = e + 1
    if exported is None:
        raise ValueError('VideoOptionsOverlay not found')
    for code, head, body, end in top:
        if code != 39 or struct.unpack_from('<H', data, body)[0] != exported:
            continue
        inner = tags(data, body + 4, end)
        toggle = depth_list = None
        out = b''
        for c, h, b, e in inner:
            chunk = data[h:e]
            if c == 26:
                flags, depth = data[b], struct.unpack_from('<H', data, b + 1)[0]
                if flags & 0x02:
                    cid = struct.unpack_from('<H', data, b + 3)[0]
                    text = data[b:e]  # the instance name is the tag's last string
                    if b'tglResolution\0' in text:
                        toggle = cid
                    if b'mcList\0' in text:
                        depth_list = depth
                        # The list goes above the new toggles: depth 200.
                        chunk = tag(26, data[b:b + 1] + struct.pack('<H', 200) + data[b + 3:e])
            if c == 1 and toggle is not None:  # before the frame is shown: the port's toggles
                for i in range(ROWS):
                    out += place(100 + i, toggle, f'tglPort{i}', 40, -1418 + 600 * i)
                toggle = None
            out += chunk
        if depth_list is None:
            raise ValueError('mcList not found in VideoOptionsOverlay')
        sprite = data[body:body + 4] + out
        return with_length(data[:head] + tag(39, sprite) + data[end:])
    raise ValueError('VideoOptionsOverlay sprite not found')


def generate(game, out, log=print):
    """Writes the patched menu movies into `out` (cached); returns `out`, or None when the game
    folder has no loose menu files (an archive dump) or they are not the expected ones."""
    hud = Path(game) / HUD
    sources = [hud / n for n in (*SCREENS, OVERLAY)]
    if not all(s.is_file() for s in sources):
        log('Menu: p1/art/hud has no loose Video screen files (archive dump?): the game\'s Video screen stays')
        return None
    key = hashlib.sha256(json.dumps({
        'v': VERSION, 'files': [(s.name, s.stat().st_size, int(s.stat().st_mtime)) for s in sources]},
        sort_keys=True).encode()).hexdigest()
    out = Path(out)
    stamp = out / 'menu.json'
    try:
        if json.loads(stamp.read_text(encoding='utf-8')).get('key') == key:
            return out
    except (OSError, ValueError):
        pass
    target = out / str(HUD).replace('\\', '/').lower()
    target.mkdir(parents=True, exist_ok=True)
    written = []
    try:
        results = {s.name.lower(): (patch_overlay if s.name == OVERLAY else patch_screen)(s.read_bytes())
                   for s in sources}
    except (ValueError, OSError, struct.error, StopIteration, IndexError) as error:
        log(f'Menu: the game\'s Video screen stays ({error})')
        for s in sources:
            (target / s.name.lower()).unlink(missing_ok=True)
        return None
    for name, data in results.items():
        (target / name).write_bytes(data)
        written.append(name)
    stamp.write_text(json.dumps({'key': key}), encoding='utf-8')
    log(f'Menu: the port\'s settings in Options > Video ({", ".join(written)})')
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('game', type=Path, help='the game folder')
    parser.add_argument('out', type=Path, help='the overrides folder')
    args = parser.parse_args()
    return 0 if generate(args.game, args.out) else 1


if __name__ == '__main__':
    sys.exit(main())
