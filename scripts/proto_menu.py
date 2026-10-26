#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""PROTOTYPE: the port's settings in the game's own Options > Video and Options > Controller
screens, made from the player's own game files.

The game's menus are Scaleform movies (p1/art/hud/*.gfx, SWF with ActionScript 2). The Video
screen (protovideooption.gfx, and protopausevideo.gfx in the pause menu) still holds the PC
version's rows (resolution, anti-aliasing, V-Sync...) behind `_global.PLATFORM == RAD_WIN32`, so
the PS4 shows only Brightness; the Controller screen (protocontrolleroption.gfx,
protopausecontroller.gfx) ends with the PS3's Rumble row and the PC's rows (mouse sensitivity,
keyboard...), which the PS4 skips too. Here:

  * the screens' code after Brightness (Video) or Target toggle (Controller) is replaced: it asks
    the port for its rows of that page (commonregisters.DoCallBack, the game's own way to call
    native code) and adds one game toggle per row. The port answers those calls itself
    (game/p1/options_menu.cpp, rows in gpu/shim/port_game_menu.cpp); without it the loop adds
    nothing and the screen is the PS4's.
  * protooptionsoverlay.gfx gets the toggles those rows use (tglPort0...), copies of the screens'
    own toggles placed in its VideoOptionsOverlay and ControllerOptionsOverlay.

The results go to <out>/p1/art/hud/, or for dumps that keep p1 in archives (art.rcf, ...) a patch
per archive, <out>/p1/<archive>.patch (THEORDER_FILE_OVERRIDES, src/runtime_file.c). The game
folder is only read, and nothing written here is part of a release.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
import zlib

HUD = Path('p1') / 'art' / 'hud'
OVERLAY = 'protooptionsoverlay.gfx'
ROWS = 12          # toggles placed for the port's rows (the port uses as many as it has rows)
VERSION = 3        # of this script's output (part of the cache key)


class Page:
    """A screen the port adds rows to: `page` is the port's page number (port_Count(page)...)."""

    def __init__(self, page, name, screens, after, options, overlay, removed, sprite, toggle):
        self.page, self.name, self.screens = page, name, screens
        self.after = after          # the game database name of the row the port's rows follow
        self.options = options      # the screen's array of rows
        self.overlay = overlay      # the screen's variable for its overlay sprite
        self.removed = removed      # names the replaced (non-PS4) rows must hold
        self.sprite, self.toggle = sprite, toggle  # the overlay sprite and the toggle copied


PAGES = (
    Page(0, 'Video', ('protovideooption.gfx', 'protopausevideo.gfx'), 'SettingVideoGammaCorrection',
         'mVideoOptions', 'mcVideoOverlay', ('SettingVideoVSyncEnabled', 'SettingVideoGraphicQuality'),
         'VideoOptionsOverlay', 'tglResolution'),
    Page(1, 'Controller', ('protocontrolleroption.gfx', 'protopausecontroller.gfx'),
         'Proto_Profile_TargetToggleEnabled', 'mControllerOptions', 'mcControllerOverlay',
         ('Proto_Profile_GamepadRumbleEnabled', 'SettingsMouseSensitivity'),
         'ControllerOptionsOverlay', 'tglInvertX'),
)
SCREENS = tuple(screen for page in PAGES for screen in page.screens)
MOVIES = len(SCREENS) + 1  # and the overlay


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
        variable names (str, pushed with GetVariable) or numbers."""
        for a in reversed(args):
            if isinstance(a, str):
                self.get(a)
            else:
                self.push(a)
        self.push(function, len(args) + 1, 'commonregisters'); self.op('GetVariable')
        self.push('DoCallBack'); self.op('CallMethod')


def port_rows(page):
    """The code that adds the port's rows of `page` to the screen's rows."""
    a = Asm()
    n = page.page
    # var __pn = commonregisters.DoCallBack("port_Count", page); var __pi = 0;
    a.push('__pn'); a.callback('port_Count', n); a.op('DefineLocal')
    a.push('__pi', 0); a.op('DefineLocal')
    a.label('row')
    a.get('__pi'); a.get('__pn'); a.op('Less2', 'Not'); a.branch('If', 'done')
    # var __pv = new Array(); var __pc = DoCallBack("port_Choices", page, __pi); var __pj = 0;
    a.push('__pv', 0, 'Array'); a.op('NewObject', 'DefineLocal')
    a.push('__pc'); a.callback('port_Choices', n, '__pi'); a.op('DefineLocal')
    a.push('__pj', 0); a.op('DefineLocal')
    a.label('choice')
    a.get('__pj'); a.get('__pc'); a.op('Less2', 'Not'); a.branch('If', 'choices_done')
    # __pv.push({mLabel: DoCallBack("port_Choice", page, __pi, __pj), mValue: __pj});
    a.push('mLabel'); a.callback('port_Choice', n, '__pi', '__pj')
    a.push('mValue'); a.get('__pj')
    a.push(2); a.op('InitObject')
    a.push(1); a.get('__pv'); a.push('push'); a.op('CallMethod', 'Pop')
    a.push('__pj'); a.get('__pj'); a.op('Increment', 'SetVariable')
    a.branch('Jump', 'choice')
    a.label('choices_done')
    # <options>.push(new optionscontainer(<overlay>["tglPort" + __pi], "PortSetting<page>_" + __pi,
    #     optionscontainer.DBTYPE_INT, DoCallBack("port_Label", page, __pi), __pv));
    a.get('__pv')
    a.callback('port_Label', n, '__pi')
    a.get('optionscontainer'); a.push('DBTYPE_INT'); a.op('GetMember')
    a.push(f'PortSetting{n}_'); a.get('__pi'); a.op('Add2')
    a.get(page.overlay); a.push('tglPort'); a.get('__pi'); a.op('Add2', 'GetMember')
    a.push(5, 'optionscontainer'); a.op('NewObject')
    a.push(1); a.get(page.options); a.push('push'); a.op('CallMethod', 'Pop')
    a.push('__pi'); a.get('__pi'); a.op('Increment', 'SetVariable')
    a.branch('Jump', 'row')
    a.label('done')
    return a.bytes()


def splice(code, acts, lo, hi, new):
    """code[lo:hi] replaced by `new`, with the branches and the function, With and Try sizes
    around it moved to match. A branch from outside into the replaced code is an error."""
    delta = len(new) - (hi - lo)
    out = bytearray(code[:lo] + new + code[hi:])

    def at(offset):  # an offset in the old code -> in the new
        return offset if offset < hi else offset + delta

    for pos, op, body, length, _ in acts:
        if lo <= pos < hi:
            continue
        if op in (0x99, 0x9D):  # Jump, If
            after = pos + 5
            target = after + struct.unpack_from('<h', code, body)[0]
            if lo < target < hi:
                raise ValueError('a branch into the replaced rows')
            moved = at(target) - at(after)
            if not -0x8000 <= moved < 0x8000:
                raise ValueError('a branch too far')
            struct.pack_into('<h', out, at(body), moved)
        elif op in (0x9B, 0x8E, 0x94, 0x8F):  # DefineFunction(2), With, Try: sized blocks after them
            if op == 0x8F:  # flags, then the try, catch and finally sizes
                total = sum(struct.unpack_from('<HHH', code, body + 1))
                if body + length < hi and lo < body + length + total:
                    raise ValueError('a Try block around the replaced rows')
                continue
            field = body if op == 0x94 else body + length - 2
            start, size = body + length, struct.unpack_from('<H', code, field)[0]
            if start <= lo and hi <= start + size:
                if not 0 <= size + delta < 0x10000:
                    raise ValueError('a function too large')
                struct.pack_into('<H', out, at(field), size + delta)
            elif start < hi and lo < start + size:
                raise ValueError('a function across the replaced rows')
    return bytes(out)


def patch_screen(data, page):
    """A Video or Controller screen: the rows after Brightness / Target toggle come from the port."""
    start = movie_start(data)
    for code, head, body, end in tags(data, start, len(data)):
        if code != 12:  # DoAction
            continue
        pool = []
        acts = actions(data, body, end, pool)
        anchor = next((i for i, a in enumerate(acts) if a[1] == 0x96 and a[4] and page.after in a[4]), None)
        if anchor is None:
            continue
        first = next(i for i in range(anchor, len(acts)) if acts[i][1] == 0x17) + 1   # after the row's Pop
        last = next((i for i in range(first, len(acts)) if acts[i][1] == 0x96 and acts[i][4] and acts[i][4][0] == 'mConfirmParams'), None)
        if last is None:
            raise ValueError(f'the end of the {page.name} rows was not found')
        names = [v for a in acts[first:last] if a[1] == 0x96 and a[4] for v in a[4]]
        if not all(name in names for name in page.removed):
            raise ValueError(f'the {page.name} screen\'s other rows are not where they were expected')
        lo, hi = acts[first][0] - body, acts[last][0] - body
        code_bytes = data[body:end]
        rel = [(pos - body, op, b - body, length, v) for pos, op, b, length, v in acts]
        code_bytes = splice(code_bytes, rel, lo, hi, port_rows(page))
        return with_length(data[:head] + tag(12, code_bytes) + data[end:])
    raise ValueError(f'the {page.name} screen\'s row {page.after} was not found')


def screen_page(data):
    """The page of a screen movie (by the rows it makes), or None."""
    for page in PAGES:
        if page.after.encode() in data and page.options.encode() in data:
            return page
    return None


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
    """protooptionsoverlay.gfx: toggles tglPort0... in each page's overlay sprite, below its list."""
    start = movie_start(data)
    top = tags(data, start, len(data))
    exported = {}
    for code, head, body, end in top:
        if code == 56:
            n = struct.unpack_from('<H', data, body)[0]
            p = body + 2
            for _ in range(n):
                cid = struct.unpack_from('<H', data, p)[0]
                e = data.index(b'\0', p + 2)
                exported[data[p + 2:e].decode('latin-1')] = cid
                p = e + 1
    for page in PAGES:
        if page.sprite not in exported:
            raise ValueError(f'{page.sprite} not found')
        data = patch_sprite(data, exported[page.sprite], page)
    return data


def patch_sprite(data, sprite, page):
    for code, head, body, end in tags(data, movie_start(data), len(data)):
        if code != 39 or struct.unpack_from('<H', data, body)[0] != sprite:
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
                    if page.toggle.encode() + b'\0' in text:
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
            raise ValueError(f'mcList not found in {page.sprite}')
        if b'tglPort0' not in out:
            raise ValueError(f'{page.toggle} not found in {page.sprite}')
        sprite_body = data[body:body + 4] + out
        return with_length(data[:head] + tag(39, sprite_body) + data[end:])
    raise ValueError(f'{page.sprite} sprite not found')


# ---- archives

# Dumps straight from the PS4 package keep p1's files in Radical's archives (art.rcf,
# art_play_go.rcf, ...): "ATG CORE CEMENT LIBRARY", u32 directory offset at 0x24 and its length at
# 0x28, entries of u32 name hash, u32 offset, u32 length (as scripts/proto_prompts.py). The
# patched movies are added past the archive's end and their entries point there: a G3PATCH1 file
# the runtime lays over the archive (src/runtime_file.c). The keyboard prompts may have patched
# the same archive: their patch is taken into this one, which the runtime then reads first.
RCF_MAGIC = b'ATG CORE CEMENT LIBRARY'


def rcf_entries(f):
    head = f.read(0x3c)
    if not head.startswith(RCF_MAGIC) or head[0x22] != 0:  # little-endian archives only
        return []
    offset, length = struct.unpack_from('<II', head, 0x24)
    if length % 12 or length > 64 << 20:
        return []
    f.seek(offset)
    table = f.read(length)
    return [(offset + i, *struct.unpack_from('<III', table, i)) for i in range(0, len(table), 12)]


def read_patch(path, base_size):
    """[(offset, length, bytes)] and the new size of a G3PATCH1 made for `base_size`, or ([], 0)."""
    try:
        data = Path(path).read_bytes()
    except OSError:
        return [], 0
    if len(data) < 32 or not data.startswith(b'G3PATCH1'):
        return [], 0
    size, vsize, count = struct.unpack_from('<QQI', data, 8)
    if size != base_size:
        return [], 0
    regions = []
    for i in range(count):
        offset, length, at = struct.unpack_from('<QQQ', data, 32 + 24 * i)
        regions.append((offset, length, data[at:at + length]))
    return regions, vsize


def unpack(member):
    """(movie bytes, packed) of an archive member, or (None, False) when it is not a movie."""
    packed = member.startswith(b'RZ\0\0')
    if packed:
        try:
            member = zlib.decompress(member[16:])
        except zlib.error:
            return None, False
    if member[:3] in (b'CFX', b'CWS'):  # a zlib-compressed movie: stored uncompressed after patching
        try:
            member = (b'GFX' if member[:3] == b'CFX' else b'FWS') + member[3:8] + zlib.decompress(member[8:])
        except zlib.error:
            return None, False
    return (member, packed) if member[:3] in (b'GFX', b'FWS') else (None, False)


def patch_movie(data):
    """The patched movie, or None when it is none of the screens and the overlay."""
    if b'VideoOptionsOverlay' in data and b'ControllerOptionsOverlay' in data and b'tglResolution' in data:
        try:
            return patch_overlay(data)
        except ValueError:
            pass  # a screen importing the overlay
    page = screen_page(data)
    return patch_screen(data, page) if page else None


def patch_archive(path, target, base_patch):
    """Writes `target` for archive `path` (with `base_patch`'s regions); returns the number of
    movies patched (0: nothing written)."""
    size = path.stat().st_size
    patched = []
    with open(path, 'rb') as f:
        for entry_at, _, offset, length in rcf_entries(f):
            if length < 64 or offset + length > size:
                continue
            f.seek(offset)
            head = f.read(16)
            if not (head.startswith(b'RZ\0\0') or head[:3] in (b'GFX', b'FWS', b'CFX', b'CWS')):
                continue
            f.seek(offset)
            data, packed = unpack(f.read(length))
            if data is None:
                continue
            new = patch_movie(data)
            if new is None:
                continue
            if packed:
                new = b'RZ\0\0' + struct.pack('<III', 0, len(new), 0) + zlib.compress(new, 6)
            patched.append((entry_at, new))
    if not patched:
        return 0
    regions, vsize = read_patch(base_patch, size) if base_patch else ([], 0)
    end = (max(size, vsize) + 0x7ff) & ~0x7ff
    for entry_at, data in patched:
        if end + len(data) > 0xffffffff:
            raise ValueError(f'{path.name} is too large to add to')
        regions.append((entry_at + 4, 8, struct.pack('<II', end, len(data))))
        regions.append((end, len(data), data))
        end = (end + len(data) + 0x7ff) & ~0x7ff
    header = 32 + 24 * len(regions)
    table, payload, at = [], b'', header
    for offset, length, data in regions:
        table.append(struct.pack('<QQQ', offset, length, at))
        payload += data
        at += len(data)
    target.parent.mkdir(parents=True, exist_ok=True)
    tmp = target.with_suffix('.tmp')
    tmp.write_bytes(b'G3PATCH1' + struct.pack('<QQII', size, end, len(regions), 0) + b''.join(table) + payload)
    tmp.replace(target)
    return len(patched)


def fingerprint(path):
    try:
        st = Path(path).stat()
        return [Path(path).name, st.st_size, int(st.st_mtime)]
    except OSError:
        return None


def generate(game, out, log=print, base_roots=()):
    """Writes the patched menu movies into `out` (cached); returns `out`, or None when they could
    not be made. Loose files (p1/art/hud/*.gfx) become files; archives get a patch each, which
    includes the patch of the same archive in the first of `base_roots` that has one (the
    keyboard prompts'): `out` must then come first in THEORDER_FILE_OVERRIDES."""
    hud = Path(game) / HUD
    sources = [hud / n for n in (*SCREENS, OVERLAY)]
    archives = sorted(p for p in (Path(game) / 'p1').glob('*.rcf') if p.is_file())
    loose = all(s.is_file() for s in sources)
    if not loose and not archives:
        log('Menu: neither p1/art/hud/*.gfx nor p1/*.rcf in the game folder: the game\'s screens stay')
        return None
    bases = {}
    for archive in archives:
        for root in base_roots:
            candidate = Path(root) / 'p1' / (archive.name.lower() + '.patch')
            if candidate.is_file():
                bases[archive.name] = candidate
                break
    key = hashlib.sha256(json.dumps({
        'v': VERSION, 'loose': loose,
        'files': [fingerprint(s) for s in (sources if loose else archives)],
        'bases': {name: fingerprint(p) for name, p in bases.items()}},
        sort_keys=True).encode()).hexdigest()
    out = Path(out)
    stamp = out / 'menu.json'
    try:
        if json.loads(stamp.read_text(encoding='utf-8')).get('key') == key:
            return out
    except (OSError, ValueError):
        pass
    target = out / str(HUD).replace('\\', '/').lower()
    for old in [*(target / s.name.lower() for s in sources), *(out / 'p1').glob('*.rcf.patch')]:
        old.unlink(missing_ok=True)
    try:
        if loose:
            target.mkdir(parents=True, exist_ok=True)
            for s in sources:
                movie, _ = unpack(s.read_bytes())
                if movie is None:
                    raise ValueError(f'{s.name} is not a movie')
                data = patch_overlay(movie) if s.name == OVERLAY else patch_screen(movie, screen_page(movie))
                (target / s.name.lower()).write_bytes(data)
            done = MOVIES
        else:
            done = 0
            for archive in archives:
                n = patch_archive(archive, out / 'p1' / (archive.name.lower() + '.patch'), bases.get(archive.name))
                if n:
                    log(f'Menu: {archive.name}: {n} menu movies')
                done += n
    except (ValueError, OSError, struct.error, StopIteration, IndexError, zlib.error) as error:
        log(f'Menu: the game\'s Video and Controller screens stay ({error})')
        for old in [*(target / s.name.lower() for s in sources), *(out / 'p1').glob('*.rcf.patch')]:
            old.unlink(missing_ok=True)
        return None
    if done != MOVIES:
        log(f'Menu: {done} menu movies found, not {MOVIES}: the game\'s Video and Controller screens stay')
        for old in (out / 'p1').glob('*.rcf.patch'):
            old.unlink(missing_ok=True)
        return None
    stamp.write_text(json.dumps({'key': key}), encoding='utf-8')
    log(f'Menu: the port\'s settings in Options > Video and Controller ({"loose files" if loose else "archive patches"})')
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('game', type=Path, help='the game folder')
    parser.add_argument('out', type=Path, help='the overrides folder')
    args = parser.parse_args()
    return 0 if generate(args.game, args.out) else 1


if __name__ == '__main__':
    sys.exit(main())
