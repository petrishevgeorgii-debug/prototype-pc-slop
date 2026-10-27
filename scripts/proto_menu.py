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

  * a Display screen for the port's third page (resolution, frame rate...): see "the Display
    screen" below.

The results go to <out>/p1/art/hud/, or for dumps that keep p1 in archives (art.rcf, ...) a patch
per archive, <out>/p1/<archive>.patch (THEORDER_FILE_OVERRIDES, src/runtime_file.c): the same
movies, found by what they hold. The game folder is only read, and nothing written here is part
of a release.
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
VERSION = 9        # of this script's output (part of the cache key)


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

class Reg(int):
    """A register in a Push."""


UNDEF = type('Undefined', (), {'__repr__': lambda self: 'undefined'})()


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
                elif t == 1:
                    values.append(struct.unpack_from('<f', code, p)[0]); p += 4
                elif t == 7:
                    values.append(struct.unpack_from('<i', code, p)[0]); p += 4
                elif t == 2:
                    values.append(None)
                elif t == 3:
                    values.append(UNDEF)
                elif t == 4:
                    values.append(Reg(code[p])); p += 1
                elif t == 5:
                    values.append(bool(code[p])); p += 1
                elif t == 6:  # a double: its two 32-bit halves swapped
                    values.append(struct.unpack('<d', code[p + 4:p + 8] + code[p:p + 4])[0]); p += 8
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
           'NewObject': 0x40, 'InitObject': 0x43, 'DefineLocal': 0x3C, 'StrictEquals': 0x66,
           'Equals2': 0x49, 'Return': 0x3E, 'CallFunction': 0x3D, 'InitArray': 0x42}

    def __init__(self):
        self.parts, self.labels, self.fixups = [], {}, []

    def push(self, *values):
        body = b''
        for v in values:
            if isinstance(v, float) and v == int(v) and -2**31 <= v < 2**31:
                v = int(v)
            if isinstance(v, bool):
                body += b'\x05' + bytes([1 if v else 0])
            elif isinstance(v, Reg):
                body += b'\x04' + bytes([int(v)])
            elif v is UNDEF:
                body += b'\x03'
            elif isinstance(v, float):
                d = struct.pack('<d', v)
                body += b'\x06' + d[4:] + d[:4]
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

    def function(self, body):
        """An anonymous function (DefineFunction, no parameters) of `body` (bytes), left on the stack."""
        record = b'\0' + struct.pack('<HH', 0, len(body))
        self.parts.append(b'\x9b' + struct.pack('<H', len(record)) + record + body)

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
    """code[lo:hi] replaced by `new` (lo == hi: inserted), with the branches and the function,
    With and Try sizes around it moved to match. A branch from outside into the replaced code
    (or to the insertion point) is an error."""
    delta = len(new) - (hi - lo)
    insert = lo == hi
    out = bytearray(code[:lo] + new + code[hi:])

    def at(offset):  # an offset in the old code -> in the new
        return offset if offset < hi else offset + delta

    for pos, op, body, length, _ in acts:
        if lo <= pos < hi:
            continue
        if op in (0x99, 0x9D):  # Jump, If
            after = pos + 5
            target = after + struct.unpack_from('<h', code, body)[0]
            if lo < target < hi or (insert and target == lo):
                raise ValueError('a branch into the replaced code')
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
            if start <= lo and hi <= start + size and (not insert or lo < start + size):
                if not 0 <= size + delta < 0x10000:
                    raise ValueError('a function too large')
                struct.pack_into('<H', out, at(field), size + delta)
            elif start < hi and lo < start + size:
                raise ValueError('a function across the replaced code')
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


# ---- the Display screen

# The port's third page gets a screen of its own: "Display", a fourth category of the pause
# menu's options (Video, Audio, Controller: PROTO_PAUSE_GAME) and a fourth button of the main
# menu's Options (PROTO_GAMEOPTION). The game still registers both screens (the PC version's Game
# options) but has no movies for them (protopausegame.swf, protogameoption.swf), and a new file
# could not be added to the archives some dumps keep p1 in. So the movies are made from the
# Controller screens' own (their rows replaced by the port's) in place of two the PS4 game never
# shows, the developers' pause debug screen and menu (protodebug.gfx, protodevmenu.gfx), and
# protomain registers the two screens with those files. The other options screens get the
# category, the button and the way to it.
DISPLAY = Page(2, 'Display', (), 'Proto_Profile_TargetToggleEnabled', 'mControllerOptions', 'mcControllerOverlay',
               ('Proto_Profile_TargetToggleEnabled',), 'ControllerOptionsOverlay', 'tglInvertX')
PAUSE_SCREENS = ('protopausevideo.gfx', 'protopauseaudio.gfx', 'protopausecontroller.gfx')
MAIN_SCREENS = ('protovideooption.gfx', 'protoaudiooption.gfx', 'protocontrolleroption.gfx')
HUB = 'protooptions.gfx'
MAIN = 'protomain.gfx'
# The Display movies: (the file they replace, the Controller screen they are made from).
NEW_SCREENS = {'protopausegame': ('protodebug.gfx', 'protopausecontroller.gfx'),
               'protogameoption': ('protodevmenu.gfx', 'protocontrolleroption.gfx')}
DISPLAY_INPUTS = (*PAUSE_SCREENS, *MAIN_SCREENS, HUB, MAIN, 'protodebug.gfx', 'protodevmenu.gfx')
# Quit to the desktop: the PC version's Quit button of the main menu and Quit to desktop row of
# the pause menu's Quit screen, which the PS4 game hides (patched when found, each on its own).
QUIT_INPUTS = ('protomainmenu.gfx', 'protoquit.gfx')
CONTROLS = 'protocontrols.gfx'


def main_pointer(data):
    """protomain.gfx: the PC version's mouse cursor (mMouseCursor) for the mouse in the menus
    (game/p1/menu_mouse.cpp): _global.gMouseCursor (the menus' controls light it up), and every
    frame it asks the port (port_Pointer) whether to show, telling it whether the menus are up
    (_global.gPortPointerMode, set where the game switches the menus' sounds between the front end,
    the pause menu and the game), and follows the mouse."""
    data = on_pc(data, lambda v: 'gMouseCursor' in v, 'the PC cursor')
    for name, mode in (('AS_InstallFrontEndFESounds', 1), ('AS_InstallPauseMenuFESounds', 1),
                       ('AS_InstallInGameFESounds', 0)):
        def set_mode(params, mode=mode):
            a = Asm()
            a.get('_global'); a.push('gPortPointerMode', mode); a.op('SetMember')
            return a.bytes()
        data = insert_at_function(data, name, set_mode)

    # var v = DoCallBack("port_Pointer", mode, this._currentframe, this._x, this._y); this._visible = v;
    # if (v) follow the mouse. (The frame, 'enable' over a control, and the place are for the log.)
    body = Asm()
    body.push('v')
    for name in ('_y', '_x', '_currentframe'):
        body.get('this'); body.push(name); body.op('GetMember')
    body.get('_global'); body.push('gPortPointerMode'); body.op('GetMember')
    body.push('port_Pointer', 5, 'commonregisters'); body.op('GetVariable'); body.push('DoCallBack')
    body.op('CallMethod', 'DefineLocal')
    body.get('this'); body.push('_visible'); body.get('v'); body.op('SetMember')
    body.get('v'); body.op('Not'); body.branch('If', 'end')
    for axis in ('x', 'y'):
        body.get('this'); body.push('_' + axis); body.get('this'); body.push('_parent'); body.op('GetMember')
        body.push(f'_{axis}mouse'); body.op('GetMember', 'SetMember')
    body.label('end')

    def follower(acts):
        for i, a in enumerate(acts):
            if is_push(a, exact=['gMouseCursor', 'mMouseCursor']):
                j = next(k for k in range(i, len(acts)) if acts[k][1] == 0x4F)
                at = whole(acts[j])[1]
                code = Asm()
                code.get('mMouseCursor'); code.push('onEnterFrame'); code.function(body.bytes()); code.op('SetMember')
                return at, at, code.bytes()
        return None
    return edit_code(data, follower, 'the PC cursor\'s place')


def button_hit_boxes(data):
    """protocontrols.gfx: the menus' buttons (protoButton: the main menu's, Extras', the
    difficulty's...) only have their label to be pointed at, a text box twice as tall as a line,
    so it covers the top of the next button down, and the one above is on top: pointing at
    Options lit up Continue. The labels' boxes get the AutoSize flag: as tall as their text."""
    start = movie_start(data)
    top = tags(data, start, len(data))
    button = None
    for code, _, body, end in top:
        if code == 56:  # ExportAssets
            r, count = body + 2, struct.unpack_from('<H', data, body)[0]
            for _ in range(count):
                cid = struct.unpack_from('<H', data, r)[0]
                name_end = data.index(b'\0', r + 2)
                if data[r + 2:name_end] == b'protoButton':
                    button = cid
                r = name_end + 1
    if button is None:
        raise ValueError('no protoButton')

    sprites = {struct.unpack_from('<H', data, body)[0]: (body + 4, end) for code, _, body, end in top if code == 39}

    def placed(cid):
        """The characters a sprite places."""
        if cid not in sprites:
            return set()
        out = set()
        for code, _, body, _ in tags(data, *sprites[cid]):
            if code == 26 and data[body] & 0x02:  # PlaceObject2 with a character
                out.add(struct.unpack_from('<H', data, body + 3)[0])
            elif code == 70 and data[body] & 0x02:  # PlaceObject3
                flags2, r = data[body + 1], body + 4
                if flags2 & 0x08 or (flags2 & 0x10):  # a class name
                    r = data.index(b'\0', r) + 1
                out.add(struct.unpack_from('<H', data, r)[0])
        return out

    labels = {t for child in placed(button) for t in placed(child)}
    out = bytearray(data)
    done = 0
    for code, _, body, end in top:
        if code != 37 or struct.unpack_from('<H', data, body)[0] not in labels:
            continue
        nbits = data[body + 2] >> 3
        flags = body + 2 + (5 + 4 * nbits + 7) // 8
        out[flags + 1] |= 0x40  # AutoSize
        done += 1
    if not done:
        raise ValueError('no protoButton labels')
    return bytes(out)


def patch_main(data):
    """protomain.gfx: the Display screens' files are the debug screens' (see above)."""
    for screen, (target, _) in NEW_SCREENS.items():
        old, new = f'{screen}.swf', target.replace('.gfx', '.swf')
        data = replace_push(data, lambda a, old=old: is_push(a, old),
                            lambda v, old=old, new=new: [new if x == old else x for x in v], f'the {old} screen')
    return data


# What identifies each movie (in an archive, names are only hashes): strings it holds.
MARKERS = {
    'protovideooption.gfx': (b'SettingVideoGammaCorrection', b'mLocalButtonStrings'),
    'protopausevideo.gfx': (b'SettingVideoGammaCorrection', b'mCategoryNames'),
    'protocontrolleroption.gfx': (b'Proto_Profile_TargetToggleEnabled', b'mLocalButtonStrings'),
    'protopausecontroller.gfx': (b'Proto_Profile_TargetToggleEnabled', b'mCategoryNames'),
    'protoaudiooption.gfx': (b'AudioOptionSelection', b'mLocalButtonStrings'),
    'protopauseaudio.gfx': (b'PauseAudioOptionSelection', b'mCategoryNames'),
    'protooptions.gfx': (b'GotoGame', b'GotoController', b'mWidgets'),
    'protomain.gfx': (b'protopausegame.swf', b'protogameoption.swf', b'RegisterScreen'),
    'protodebug.gfx': (b'MakeAlexInvulnerable', b'ActivatePowerList'),
    'protodevmenu.gfx': (b'GotoMissionSelect', b'SELECT MISSION'),
    'protomainmenu.gfx': (b'mcBtnQuit', b'GotoQuit', b'mcBtnSwitchProfile'),
    'protoquit.gfx': (b'QuitToDesktop', b'DetermineQuitOptions'),
    CONTROLS: (b'StdTypeWriter', b'protoPlainListButton', b'UpdateTypeWriter'),
    OVERLAY: (b'VideoOptionsOverlay', b'ControllerOptionsOverlay', b'tglResolution'),
}


def identify(movie):
    """The name of a movie this script patches, or None."""
    found = [name for name, marks in MARKERS.items() if all(m in movie for m in marks)]
    if len(found) == 1 and found[0] == OVERLAY:
        try:
            patch_overlay(movie)
        except ValueError:
            return None  # a screen importing the overlay
    return found[0] if len(found) == 1 else None


def patch_all(movies, label):
    """{name: patched movie} from {name: movie}: the Video and Controller rows, the overlay's
    toggles and, with `label` and every movie it needs, the Display screen."""
    out = {}
    for name in (*SCREENS, OVERLAY):
        movie = movies[name]
        out[name] = patch_overlay(movie) if name == OVERLAY else patch_screen(movie, screen_page(movie))
    if label:
        for name in PAUSE_SCREENS:
            out[name] = pause_categories(out.get(name) or movies[name], label)
        for name in MAIN_SCREENS:
            out[name] = main_buttons(out.get(name) or movies[name], label)
        out[HUB] = patch_hub(movies[HUB], label)
        out[MAIN] = patch_main(movies[MAIN])
        for screen, (target, source) in NEW_SCREENS.items():
            out[target] = display_screen(movies[source], screen == 'protopausegame', label)
    if MAIN in movies:
        try:
            out[MAIN] = main_pointer(out.get(MAIN) or movies[MAIN])
        except (ValueError, StopIteration, IndexError):
            pass  # another build of protomain: no mouse cursor
    if CONTROLS in movies:
        try:
            out[CONTROLS] = button_hit_boxes(movies[CONTROLS])
        except (ValueError, struct.error, IndexError):
            pass  # another build: the buttons stay as they are
    for name in QUIT_INPUTS:
        if name in movies:
            try:
                out[name] = (main_menu_quit if name == 'protomainmenu.gfx' else pause_quit)(movies[name])
            except (ValueError, StopIteration, IndexError):
                pass  # another build of the screen: it stays as it is
    return out


def on_pc(data, follows, what, within=4):
    """The movie with one `if (_global.PLATFORM == _global.RAD_WIN32)` made always true: the
    first one whose next `within` actions have a Push whose values `follows` takes."""
    def locate(acts):
        for i in range(4, len(acts) - 3):
            if acts[i][1] != 0x49 or acts[i + 1][1] != 0x12 or acts[i + 2][1] != 0x9D:
                continue
            before = [v for a in acts[max(0, i - 6):i] if a[1] == 0x96 and a[4] for v in a[4]]
            if 'PLATFORM' in before and 'RAD_WIN32' in before and \
                    any(a[1] == 0x96 and a[4] and follows(a[4]) for a in acts[i + 3:i + 3 + within]):
                lo, hi = whole(acts[i + 2])
                return lo, hi, bytes([0x17])  # Pop the condition instead of branching on it
        return None
    return edit_code(data, locate, what)


def main_menu_quit(data):
    """protomainmenu.gfx: the PC version's Quit button (fe_ExitGame, game/p1/options_menu.cpp)
    after Extras, named Quit game, asking "Quit to desktop?"."""
    quit_button = lambda v: 'mcBtnQuit' in v or 'GotoQuit' in v
    for what in ('the Quit button', 'the Quit button\'s label', 'the Quit button\'s action'):
        data = on_pc(data, quit_button, what)
    data = replace_push(data, lambda a: is_push(a, exact=['$QUIT', 1, 'mcBtnQuit']),
                        lambda v: ['$QUIT_GAME', 1, 'mcBtnQuit'], 'the Quit button\'s label')
    # The PC text $QUITTODESKTOP is not in the PS4's text bible; $QUITDESKTOP (the pause menu's) is.
    return replace_push(data, lambda a: is_push(a, '$QUITTODESKTOP'),
                        lambda v: ['$QUITDESKTOP' if x == '$QUITTODESKTOP' else x for x in v], 'the Quit question')


def pause_quit(data):
    """protoquit.gfx (pause menu > Quit): Quit to desktop after Quit to main menu."""
    return on_pc(data, lambda v: '$QUITDESKTOP' in v, 'the Quit to desktop row', within=16)


def display_label(language):
    return 'ДИСПЛЕЙ' if str(language) == '8' else 'DISPLAY'


def edit_code(data, locate, what):
    """The movie with one change to the code of one of its DoAction tags: locate(acts) gives
    (lo, hi, new bytes) in the movie's offsets, or None for another tag."""
    for code, head, body, end in tags(data, movie_start(data), len(data)):
        if code != 12:
            continue
        acts = actions(data, body, end, [])
        found = locate(acts)
        if not found:
            continue
        lo, hi, new = found
        rel = [(pos - body, op, b - body, length, v) for pos, op, b, length, v in acts]
        code_bytes = splice(data[body:end], rel, lo - body, hi - body, new)
        return with_length(data[:head] + tag(12, code_bytes) + data[end:])
    raise ValueError(f'{what} not found')


def is_push(a, *strings, exact=None):
    if a[1] != 0x96 or not a[4]:
        return False
    if exact is not None:
        return list(a[4]) == list(exact)
    return all(s in a[4] for s in strings)


def push_bytes(*values):
    asm = Asm()
    asm.push(*values)
    return asm.bytes()


def whole(a):
    """(start, end) of an action."""
    return a[0], a[2] + a[3]


def function_body(acts, data, name):
    """(body start, {parameter: register}) of DefineFunction2 `name`, or None."""
    for a in acts:
        if a[1] != 0x8E:
            continue
        p = a[2]
        e = data.index(b'\0', p)
        if data[p:e].decode('latin-1') != name:
            continue
        p = e + 1
        n = struct.unpack_from('<H', data, p)[0]
        p += 5
        params = {}
        for _ in range(n):
            reg = data[p]
            e = data.index(b'\0', p + 1)
            params[data[p + 1:e].decode('latin-1')] = Reg(reg)
            p = e + 1
        return a[2] + a[3], params
    return None


def replace_push(data, match, values, what):
    def locate(acts):
        for a in acts:
            if match(a):
                lo, hi = whole(a)
                return lo, hi, push_bytes(*values(a[4]))
        return None
    return edit_code(data, locate, what)


def insert_at_function(data, name, build):
    def locate(acts):
        found = function_body(acts, data, name)
        if not found:
            return None
        at, params = found
        return at, at, build(params)
    return edit_code(data, locate, f'function {name}')


def insert_after_pop(data, match, build, what):
    """`build()` inserted after the statement (the Pop) that follows the first Push `match` takes."""
    def locate(acts):
        for i, a in enumerate(acts):
            if match(a):
                j = next(k for k in range(i, len(acts)) if acts[k][1] == 0x17)
                at = whole(acts[j])[1]
                return at, at, build()
        return None
    return edit_code(data, locate, what)


def pause_categories(data, label):
    """A pause options screen: Display as its fourth category."""
    data = replace_push(data, lambda a: is_push(a, 'mCategoryNames', '$PAUSE_CONTROLLER', '$AUDIO', '$VIDEO'),
                        lambda v: ['mCategoryNames', label, '$PAUSE_CONTROLLER', '$AUDIO', '$VIDEO', 4],
                        'the pause categories')

    def index(params):  # if (screenName === "protopausegame") return 3;
        a = Asm()
        a.push(params.get('screenName', Reg(1)), 'protopausegame'); a.op('StrictEquals', 'Not')
        a.branch('If', 'skip'); a.push(3); a.op('Return'); a.label('skip')
        return a.bytes()
    data = insert_at_function(data, 'GetCategoryIndexFromScreenName', index)

    def change(params):  # the fourth category: SwitchScreen(PROTO_PAUSE_GAME)
        a = Asm()
        a.push(0, 'mcCategoryList'); a.op('GetVariable'); a.push('GetSelectIndex'); a.op('CallMethod')
        a.push(3); a.op('Equals2', 'Not'); a.branch('If', 'skip')
        a.get('mCategoryIndex'); a.push(3); a.op('Equals2'); a.branch('If', 'skip')
        a.push('mCategoryIndex', 3); a.op('SetVariable')
        a.get('_global'); a.push('PROTO_PAUSE_GAME'); a.op('GetMember')
        a.push(1); a.get('_global'); a.push('managers'); a.op('GetMember'); a.push('screen'); a.op('GetMember')
        a.push('SwitchScreen'); a.op('CallMethod', 'Pop')
        a.push(UNDEF); a.op('Return')
        a.label('skip')
        return a.bytes()
    return insert_at_function(data, 'OnCategoryChange', change)


def show_fourth_button(data, widgets):
    """mcOptionBtn3, hidden on the PS4, goes into `widgets` like the other three."""
    def locate(acts):
        for i in range(len(acts) - 3):
            if is_push(acts[i], 'mcOptionBtn3') and acts[i + 1][1] == 0x4E and \
                    is_push(acts[i + 2], exact=['_visible', False]) and acts[i + 3][1] == 0x4F:
                a = Asm()
                a.push(1, widgets); a.op('GetVariable'); a.push('push'); a.op('CallMethod', 'Pop')
                return acts[i + 2][0], whole(acts[i + 3])[1], a.bytes()
        return None
    return edit_code(data, locate, 'the hidden fourth button')


def main_buttons(data, label):
    """A main menu options screen: Display as the fourth button of its list."""
    data = show_fourth_button(data, 'mButtons')
    data = replace_push(data, lambda a: is_push(a, 'mLocalButtonStrings', '$PAUSE_CONTROLLER', '$AUDIO', '$VIDEO'),
                        lambda v: ['mLocalButtonStrings', label, '$PAUSE_CONTROLLER', '$AUDIO', '$VIDEO', 4, 'Array'],
                        'the button names')

    def names(acts):
        for i, a in enumerate(acts):
            if is_push(a, exact=['mOptionScreenNames', '_global']):
                j = next(k for k in range(i + 1, len(acts)) if is_push(acts[k], exact=[3, 'Array']))
                lo, hi = whole(acts[j])
                return lo, hi, push_bytes(4, 'Array')
        return None
    data = edit_code(data, names, 'the screen names')

    def first(acts):  # _global.PROTO_GAMEOPTION, the new last one, goes in first
        for a in acts:
            if is_push(a, exact=['mOptionScreenNames', '_global']):
                asm = Asm()
                asm.push('mOptionScreenNames', '_global'); asm.op('GetVariable'); asm.push('PROTO_GAMEOPTION')
                asm.op('GetMember'); asm.push('_global')
                lo, hi = whole(a)
                return lo, hi, asm.bytes()
        return None
    return edit_code(data, first, 'the screen names')


def patch_hub(data, label):
    """protooptions.gfx (Options in the main menu): a fourth button, Display, to PROTO_GAMEOPTION
    through its GotoGame (the PC version's Game options)."""
    data = show_fourth_button(data, 'mWidgets')

    def set_label():
        a = Asm()
        a.push(label, 1, 'mcOptionButtons'); a.op('GetVariable'); a.push('mcOptionBtn3'); a.op('GetMember')
        a.push('SetLabel'); a.op('CallMethod', 'Pop')
        return a.bytes()
    data = insert_after_pop(data, lambda a: is_push(a, '$PAUSE_CONTROLLER', 'mcOptionButtons'), set_label,
                            'the Controller button\'s label')

    def on_select():
        a = Asm()
        a.get('GotoGame'); a.get('mcOptionButtons'); a.push('mcOptionBtn3'); a.op('GetMember')
        a.push(2); a.get('_global'); a.push('managers'); a.op('GetMember'); a.push('focus'); a.op('GetMember')
        a.push('SetOnSelect'); a.op('CallMethod', 'Pop')
        return a.bytes()
    data = insert_after_pop(data, lambda a: is_push(a, exact=['GotoController']), on_select,
                            'the Controller button\'s action')

    def confirm(params):  # the confirm hint on the fourth button
        a = Asm()
        a.push(0); a.get('_global'); a.push('managers'); a.op('GetMember'); a.push('focus'); a.op('GetMember')
        a.push('GetSelectedIndex'); a.op('CallMethod'); a.push(3); a.op('Equals2', 'Not'); a.branch('If', 'skip')
        a.push(None, 'A', None, 3, 'GotoGame'); a.op('CallFunction', 'Pop')
        a.push(UNDEF); a.op('Return')
        a.label('skip')
        return a.bytes()
    return insert_at_function(data, 'OnButtonHintConfirm', confirm)


def display_screen(data, pause, label):
    """The Display screen from a Controller screen's movie: its rows are the port's page 2."""
    def rows(acts):
        for i, a in enumerate(acts):
            if is_push(a, exact=['mControllerOptions', 0, 'Array']) and acts[i + 1][1] == 0x40 \
                    and acts[i + 2][1] in (0x3C, 0x1D):
                lo = whole(acts[i + 2])[1]
                last = next(k for k in range(i, len(acts)) if acts[k][1] == 0x96 and acts[k][4]
                            and acts[k][4][0] == 'mConfirmParams')
                names = [v for b in acts[i:last] if b[1] == 0x96 and b[4] for v in b[4]]
                if 'Proto_Profile_TargetToggleEnabled' not in names:
                    raise ValueError('the Controller rows are not where they were expected')
                return lo, acts[last][0], port_rows(DISPLAY)
        return None
    data = edit_code(data, rows, 'the Controller rows')
    old = 'protopausecontroller' if pause else 'protocontrolleroption'
    new = 'protopausegame' if pause else 'protogameoption'
    rename = {old: new, '$PAUSE_CONTROLLER': label,
              'PauseControllerOptionSelection': 'PauseDisplayOptionSelection',
              'ControllerOptionSelection': 'DisplayOptionSelection'}
    data = replace_push(data, lambda a: is_push(a, old, 'InitializeOptions'),
                        lambda v: [rename.get(x, x) if isinstance(x, str) else x for x in v], 'the screen\'s name')
    data = replace_push(data, lambda a: is_push(a, 'PauseControllerOptionSelection' if pause else 'ControllerOptionSelection'),
                        lambda v: [rename.get(x, x) if isinstance(x, str) else x for x in v], 'the selection\'s name')
    if not pause:  # no "view the controller" button on it
        data = replace_push(data, lambda a: is_push(a, exact=['mGamepadAvailable', True]),
                            lambda v: ['mGamepadAvailable', False], 'the gamepad button')
    return pause_categories(data, label) if pause else main_buttons(data, label)


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


def archive_movies(archives):
    """{name: [(archive, entry offset, movie, packed)]} of the movies this script patches in
    `archives` (a movie can be in more than one)."""
    found = {}
    for path in archives:
        size = path.stat().st_size
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
                name = identify(data)
                if name:
                    found.setdefault(name, []).append((path, entry_at, data, packed))
    return found


def write_archive_patch(path, target, base_patch, patched):
    """Writes `target` for archive `path`: `patched` [(entry offset, member bytes)] added past
    its end, with `base_patch`'s regions."""
    size = path.stat().st_size
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


def fingerprint(path):
    try:
        st = Path(path).stat()
        return [Path(path).name, st.st_size, int(st.st_mtime)]
    except OSError:
        return None


def loose_movie(path):
    movie, _ = unpack(path.read_bytes())
    if movie is None:
        raise ValueError(f'{path.name} is not a movie')
    return movie


def made_display(out):
    """Whether the last generate() into `out` made the Display screen."""
    try:
        return bool(json.loads((Path(out) / 'menu.json').read_text(encoding='utf-8')).get('display'))
    except (OSError, ValueError):
        return False


def generate(game, out, log=print, base_roots=(), language=None):
    """Writes the patched menu movies into `out` (cached); returns `out`, or None when they could
    not be made. Loose files (p1/art/hud/*.gfx) become files; archives get a patch each, which
    includes the patch of the same archive in the first of `base_roots` that has one (the
    keyboard prompts'): `out` must then come first in THEORDER_FILE_OVERRIDES. The Display screen
    is made when every movie it needs is there (made_display). `language`: the game's
    (THEORDER_LANGUAGE), for the screen's name."""
    hud = Path(game) / HUD
    sources = [hud / n for n in (*SCREENS, OVERLAY)]
    display_sources = [hud / n for n in DISPLAY_INPUTS]
    quit_sources = [hud / n for n in (*QUIT_INPUTS, MAIN, CONTROLS)]
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
    label = display_label(language)
    inputs = sorted(set(sources + [d for d in (*display_sources, *quit_sources) if d.is_file()]))
    key = hashlib.sha256(json.dumps({
        'v': VERSION, 'loose': loose, 'label': label,
        'files': [fingerprint(s) for s in (inputs if loose else archives)],
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
    names = [*SCREENS, OVERLAY, *DISPLAY_INPUTS, *QUIT_INPUTS, CONTROLS, 'protopausegame.gfx', 'protogameoption.gfx']

    def clean():
        stamp.unlink(missing_ok=True)
        for old in [*(target / n.lower() for n in names), *(out / 'p1').glob('*.rcf.patch')]:
            old.unlink(missing_ok=True)
    clean()
    try:
        if loose:
            movies = {s.name: loose_movie(s) for s in sources}
            display = all(d.is_file() for d in display_sources)
            if display:
                movies.update({d.name: loose_movie(d) for d in display_sources})
            movies.update({q.name: loose_movie(q) for q in quit_sources if q.is_file() and q.name not in movies})
            target.mkdir(parents=True, exist_ok=True)
            patched = patch_all(movies, label if display else None)
            for name, data in patched.items():
                (target / name.lower()).write_bytes(data)
        else:
            found = archive_movies(archives)
            missing = [n for n in (*SCREENS, OVERLAY) if n not in found]
            if missing:
                log(f'Menu: {", ".join(missing)} not found in the archives: the game\'s options screens stay')
                clean()
                return None
            display = all(n in found for n in DISPLAY_INPUTS)
            patched = patch_all({n: places[0][2] for n, places in found.items()}, label if display else None)
            per_archive = {}
            for name, data in patched.items():
                for path, entry_at, _, packed in found[name]:
                    member = b'RZ\0\0' + struct.pack('<III', 0, len(data), 0) + zlib.compress(data, 6) if packed else data
                    per_archive.setdefault(path, []).append((entry_at, member))
            for path, members in per_archive.items():
                write_archive_patch(path, out / 'p1' / (path.name.lower() + '.patch'), bases.get(path.name), members)
                log(f'Menu: {path.name}: {len(members)} menu movies')
    except (ValueError, OSError, struct.error, StopIteration, IndexError, KeyError, zlib.error) as error:
        log(f'Menu: the game\'s options screens stay ({error})')
        clean()
        return None
    stamp.write_text(json.dumps({'key': key, 'display': display}), encoding='utf-8')
    log(f'Menu: the port\'s settings in Options > Video and Controller{" and a Display screen" if display else ""} '
        f'({"loose files" if loose else "archive patches"})')
    log('Menu: the mouse in the menus, with the PC cursor' if b'port_Pointer' in patched.get(MAIN, b'') else
        'Menu: no mouse in the menus (protomain.gfx not found or not recognised)')
    quits = [n for n in QUIT_INPUTS if n in patched]
    log(f'Menu: quit to the desktop from {" and ".join({"protomainmenu.gfx": "the main menu", "protoquit.gfx": "the pause menu"}[n] for n in quits)}'
        if quits else 'Menu: no quit to the desktop (the main menu and quit screens were not found or not recognised)')
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('game', type=Path, help='the game folder')
    parser.add_argument('out', type=Path, help='the overrides folder')
    parser.add_argument('--language', default=None, help='the game\'s language (THEORDER_LANGUAGE; 8: Russian)')
    args = parser.parse_args()
    return 0 if generate(args.game, args.out, language=args.language) else 1


if __name__ == '__main__':
    sys.exit(main())
