#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""PROTOTYPE BIOHAZARD BUNDLE native port: the launcher.

Picks and checks the game folder (the player's own dump, never modified), prepares the native
image, holds the graphics settings and starts the game through run.py. The packaged
executable (PyInstaller) also runs run.py (--run) and the preparation scripts (--script), so
players need no Python.
"""
import json
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import threading
import time
import hashlib

# Modules run.py and scripts/ use: imported here so the packaged executable carries them.
import argparse  # noqa: F401
import base64  # noqa: F401
import binascii  # noqa: F401
import collections  # noqa: F401
import ctypes  # noqa: F401
import dataclasses  # noqa: F401
import datetime  # noqa: F401
import locale  # noqa: F401
import re  # noqa: F401
import shlex  # noqa: F401
import struct  # noqa: F401
import tempfile  # noqa: F401
import xml.etree.ElementTree  # noqa: F401
import io  # noqa: F401
import zlib  # noqa: F401
if sys.platform == 'win32':
    import _winapi  # noqa: F401
    import winreg  # noqa: F401
try:  # scripts/proto_prompts.py draws the key caps with these
    import PIL.DdsImagePlugin  # noqa: F401
    import PIL.ImageDraw  # noqa: F401
    import PIL.ImageFont  # noqa: F401
except ImportError:
    pass

WINDOWS = sys.platform == 'win32'
FROZEN = getattr(sys, 'frozen', False)
PORT_DIR = Path(sys.executable).resolve().parent if FROZEN else Path(__file__).resolve().parent.parent
sys.path.insert(0, str(PORT_DIR / 'scripts'))

APP_NAME = 'PROTOTYPE'
VERSION = '1.0.1'
TITLE_ID = 'CUSA06545'
# sha256 of the untouched executables of CUSA06545 01.00 (the port patches them in memory only).
ORIGINAL_SHA256 = {
    'eboot.bin': '664269bbadb9681d9e69db4306ae788435c67799d0867aeb635ce18137311473',
    'prototype2_ps4f.elf': 'da64c2ce9b323de0ad903d0fb2afb842a6f41c19e1da400177d53375ae2f1df5',
}
# Files earlier shadPS4 fixes and debugging left in the game folder (never part of the dump).
LEFTOVER_PATTERNS = ('*.orig', '*.allfixes', 'proto_*.txt', 'proto_timefix.py', 'sprx.py',
                     f'{TITLE_ID}_frame*.rdc', f'{TITLE_ID}_frame*.7z')
CONFIG_DIR = (Path(os.environ.get('APPDATA', Path.home())) if WINDOWS else
              Path(os.environ.get('XDG_CONFIG_HOME') or Path.home() / '.config')) / 'prototype-port'
CONFIG_FILE = CONFIG_DIR / 'launcher.json'
LOG_FILE = PORT_DIR / 'last_run.log'
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)

# --------------------------------------------------------------------------------------------
# Command line roles of the packaged executable.


def attach_stdio():
    """A windowed executable starts without sys.stdout; the handles the launcher passed (the
    log file) still exist: reopen them."""
    for name, std in (('stdout', -11), ('stderr', -12)):
        if getattr(sys, name) is not None:
            continue
        stream = None
        if WINDOWS:
            try:
                import msvcrt
                handle = ctypes.windll.kernel32.GetStdHandle(std)
                if handle and handle != ctypes.c_void_p(-1).value:
                    stream = open(msvcrt.open_osfhandle(handle, os.O_WRONLY), 'w', encoding='utf-8',
                                  errors='replace', buffering=1)
            except OSError:
                stream = None
        setattr(sys, name, stream or open(os.devnull, 'w'))


def run_role(argv):
    """--run [args]: run.py; --script <file> [args]: a preparation script."""
    attach_stdio()
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding='utf-8', line_buffering=True)
        except (AttributeError, ValueError):
            pass
    if argv[0] == '--script':
        path, sys.argv = argv[1], argv[1:]
    else:
        path = str(PORT_DIR / 'run.py')
        sys.argv = [path, *argv[1:]]
    sys.path.insert(0, str(Path(path).parent))
    try:
        runpy.run_path(path, run_name='__main__')
    except SystemExit as stop:
        return stop.code if isinstance(stop.code, int) else (0 if stop.code is None else 1)
    return 0


def run_command():
    return [sys.executable, '--run'] if FROZEN else [sys.executable, str(PORT_DIR / 'run.py')]


def script_command(name, *args):
    script = str(PORT_DIR / 'scripts' / name)
    return ([sys.executable, '--script', script] if FROZEN else [sys.executable, script]) + [str(a) for a in args]


# --------------------------------------------------------------------------------------------
# Settings.

RESOLUTIONS = ['1280x720', '1600x900', '1920x1080', '2560x1440', '3200x1800', '3840x2160']
LANGUAGES = [('English', 1), ('Français', 2), ('Español', 3), ('Deutsch', 4), ('Italiano', 5)]
DEFAULTS = {
    'game_dir': '', 'anisotropy': 16, 'fsr1': True,
    'rcas': True, 'rcas_strength': 60, 'display_mode': 'borderless', 'vsync': True,
    'show_fps': False, 'language': 1, 'panel': 'GAME',
    'controls': {}, 'prompts': 'auto', 'controls_group': 'move', 'mouse_look': True, 'mouse_sensitivity': 100,
    'game': 'p1',
}
# The two games of the bundle: name, the executable run.py starts (THEORDER_EBOOT), the data
# folder, the line under the name on its tab, and the part of the bundle's key art (pic0) it shows.
GAMES = {
    'p1': ('PROTOTYPE', 'eboot.bin', 'p1', '2009  ·  Alex Mercer', 'left'),
    'p2': ('PROTOTYPE 2', 'prototype2_ps4f.elf', 'p2', '2012  ·  James Heller  ·  experimental', 'right'),
}
# Settings each game keeps for itself (its patches); everything else is shared.
GAME_DEFAULTS = {
    'p1': {'render_res': '1920x1080', 'fps60': True, 'tendril_fix': True},
    'p2': {'render_res': '1920x1080', 'fps60': False},
}
PROMPTS = [('auto', 'Automatic'), ('keyboard', 'Keyboard & mouse'), ('gamepad', 'PlayStation')]


def load_settings():
    values = dict(DEFAULTS)
    try:
        values.update(json.loads(CONFIG_FILE.read_text(encoding='utf-8')))
    except (OSError, ValueError):
        pass
    # The in-game menu saves into theorder.ini: its values win over the launcher's last ones.
    try:
        import graphics_options
        ini = graphics_options.read_ini(PORT_DIR / 'theorder.ini')
        for key, kind in (('display_mode', str), ('vsync', bool), ('show_fps', bool), ('fsr1', bool),
                          ('rcas', bool), ('rcas_strength', int), ('anisotropy', int),
                          ('mouse_look', bool), ('mouse_sensitivity', int)):
            if key in ini:
                raw = ini[key]
                values[key] = raw if kind is str else (raw not in ('0', '') if kind is bool else int(float(raw)))
    except Exception:
        pass
    values.pop('ssr', None)  # water reflections: no setting since 1.0.1
    if not isinstance(values.get('controls'), dict):
        values['controls'] = {}
    if values.get('game') not in GAMES:
        values['game'] = 'p1'
    # 0.6: the patch settings are per game; PROTOTYPE's come from the earlier single values.
    per_game = values.get('per_game') if isinstance(values.get('per_game'), dict) else {}
    if 'p1' not in per_game:
        per_game['p1'] = {k: values.get(k, v) for k, v in GAME_DEFAULTS['p1'].items()}
    for key, defaults in GAME_DEFAULTS.items():
        settings = per_game.setdefault(key, {})
        for name, value in defaults.items():
            settings.setdefault(name, value)
        if settings['render_res'] not in RESOLUTIONS:
            settings['render_res'] = '1920x1080'
    for settings in per_game.values():  # the 0.7 shadow options, scrapped
        settings.pop('shadows', None)
        settings.pop('shadow_distance', None)
    values['per_game'] = per_game
    for key in ('render_res', 'fps60', 'tendril_fix', 'tab'):  # the 0.5 single values
        values.pop(key, None)
    # 0.2.1: the render resolution patch changed; every setup starts again from the PS4's size.
    if values.get('settings_version', 0) < 2:
        per_game['p1']['render_res'] = '1920x1080'
        values['settings_version'] = 2
    return values


def save_settings(values):
    try:
        CONFIG_DIR.mkdir(parents=True, exist_ok=True)
        tmp = CONFIG_FILE.with_suffix('.tmp')
        tmp.write_text(json.dumps(values, indent=2), encoding='utf-8')
        tmp.replace(CONFIG_FILE)
    except OSError:
        pass


def write_ini(values):
    """The GPU layer's settings file (theorder.ini beside run.py), read at every start."""
    try:
        import graphics_options
        graphics_options.write_keys(PORT_DIR / 'theorder.ini', {
            'display_mode': values['display_mode'], 'vsync': int(values['vsync']),
            'show_fps': int(values['show_fps']), 'anisotropy': int(values['anisotropy']),
            'fsr1': int(values['fsr1']), 'rcas': int(values['rcas']),
            'rcas_strength': int(values['rcas_strength']),
            'mouse_look': int(values['mouse_look']), 'mouse_sensitivity': int(values['mouse_sensitivity']),
        })
    except Exception as error:  # the game still starts with its defaults
        print(f'launcher: theorder.ini not written: {error}')


def game_environment(values):
    env = dict(os.environ)
    p1, p2 = values['per_game']['p1'], values['per_game']['p2']
    env.update({
        'THEORDER_GAME_DIR': values['game_dir'],
        # Both games' patch settings: a start from the bundle menu runs the other game (run.py).
        'THEORDER_FPS60': '1' if p1['fps60'] else '0',
        'THEORDER_PROTO_RES': p1['render_res'],
        'THEORDER_PROTO_TENDRIL_FIX': '1' if p1['tendril_fix'] else '0',
        'THEORDER_PROTO2_FPS60': '1' if p2['fps60'] else '0',
        'THEORDER_PROTO2_RES': p2['render_res'],
        'THEORDER_ANISOTROPY': str(values['anisotropy']),
        'THEORDER_PROTO_CONTROLS': json.dumps(values.get('controls') or {}),
        'THEORDER_PROMPTS': values.get('prompts', 'auto'),
        'THEORDER_EBOOT': GAMES[values.get('game', 'p1')][1],
        'PYTHONIOENCODING': 'utf-8',
    })
    return env


# --------------------------------------------------------------------------------------------
# The game folder.

_SHA_CACHE = {}


def file_sha256(path):
    """The file's SHA-256; remembered while its size and time stay (switching games re-checks)."""
    st = os.stat(path)
    key = (str(path), st.st_size, st.st_mtime_ns)
    if key not in _SHA_CACHE:
        _SHA_CACHE[key] = _sha256(path)
    return _SHA_CACHE[key]


def _sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def read_sfo(folder):
    try:
        from prepare import read_sfo as reader
        return reader(Path(folder) / 'sce_sys' / 'param.sfo') or {}
    except Exception:
        return {}


def check_game(folder, game='p1'):
    """[(label, state, detail)] with state 'ok', 'warn' or 'bad' (bad: `game` cannot start); and the
    parsed param.sfo."""
    folder = Path(folder) if folder else None
    if not folder or not folder.is_dir():
        return [('Game folder', 'bad', 'not selected')], {}
    sfo = read_sfo(folder)
    checks = []
    title = sfo.get('TITLE_ID')
    if title == TITLE_ID:
        checks.append(('Game', 'ok', f'{TITLE_ID} · version {sfo.get("APP_VER", "?")}'))
    elif title:
        checks.append(('Game', 'bad', f'{title} is not {TITLE_ID}'))
    else:
        checks.append(('Game', 'bad', 'sce_sys/param.sfo missing'))
    for name, label in (('eboot.bin', 'PROTOTYPE executable'), ('prototype2_ps4f.elf', 'PROTOTYPE 2 executable')):
        path = folder / name
        if not path.is_file():
            # eboot.bin is the bundle menu, which starts either game.
            checks.append((label, 'bad' if name in ('eboot.bin', GAMES[game][1]) else 'warn', 'missing'))
            continue
        try:
            same = file_sha256(path) == ORIGINAL_SHA256[name]
        except OSError:
            same = False
        checks.append((label, 'ok' if same else 'warn', 'original' if same else 'modified (Restore fixes it)'))
    modules = all((folder / 'sce_module' / m).is_file() for m in ('libc.prx', 'libSceFios2.prx'))
    checks.append(('System modules', 'ok' if modules else 'bad', 'libc, Fios2' if modules else 'sce_module missing'))
    for key in ('p1', 'p2'):
        data_dir = GAMES[key][2]
        data = (folder / data_dir).is_dir()
        checks.append((f'{GAMES[key][0]} data', 'ok' if data else ('bad' if key == game else 'warn'),
                       f'{data_dir} found' if data else f'{data_dir} folder missing'))
    return checks, sfo


def restorable(folder):
    """[(executable, backup)] whose backup holds the original bytes and the executable does not."""
    out = []
    for name, sha in ORIGINAL_SHA256.items():
        path, backup = Path(folder) / name, Path(folder) / (name + '.orig')
        try:
            if backup.is_file() and file_sha256(backup) == sha and (not path.is_file() or file_sha256(path) != sha):
                out.append((path, backup))
        except OSError:
            pass
    return out


def leftovers(folder):
    folder = Path(folder)
    found = []
    for pattern in LEFTOVER_PATTERNS:
        found += [p for p in folder.glob(pattern) if p.is_file()]
    # A backup goes only once the executable it backs up is the original again.
    keep = {p for _, p in restorable(folder)}
    return sorted(set(found) - keep)


# --------------------------------------------------------------------------------------------
# Look: a Blackwatch containment terminal. Monospace type, CRT scanlines, hazard stripes, the
# bundle's key art as a surveillance feed, and the odd glitch.

STATE = {'ok': '#86c27f', 'warn': '#e3a43a', 'bad': '#ef4b41'}
# Each game's colours: PROTOTYPE blood red, PROTOTYPE 2 cold blue (its menus). 'red' is the
# accent, 'ink' the text on it, 'ghost' the other colour of a glitch.
THEMES = {
    'p1': {'bg': '#09090b', 'top': '#040405', 'panel': '#0c0c0f', 'card': '#151518', 'hi': '#1e0f11',
           'grid': '#111114', 'line': '#2e2e34', 'hair': '#1c1c21', 'track': '#26262b',
           'text': '#e8e1d8', 'muted': '#968f87', 'dim': '#5d5853', 'ink': '#0a0505',
           'red': '#c8161d', 'red_hot': '#ff3b33', 'red_dark': '#3c0b0e', 'ghost': '#1fd2ff',
           'feed': ('#000000', '#6a625c', '#1e1c1b')},
    'p2': {'bg': '#070a0d', 'top': '#030507', 'panel': '#0a0e12', 'card': '#10171d', 'hi': '#0c1a24',
           'grid': '#0e1317', 'line': '#23313b', 'hair': '#162029', 'track': '#1d2830',
           'text': '#dfe7ec', 'muted': '#8799a5', 'dim': '#51616b', 'ink': '#02080c',
           'red': '#1f93cc', 'red_hot': '#4cc6ff', 'red_dark': '#0b2838', 'ghost': '#ff3b30',
           'feed': ('#000000', '#55697a', '#171f25')},
}
C = dict(STATE, **THEMES['p1'])
W, H = 1200, 720
HEAD = 40                           # the terminal's header
BODY = HEAD + 8                     # under the hazard stripes
FOOT = 34                           # the status line
SPLIT = 470                         # the subject file (left) | the console (right)
FEED = (22, 104, 448, 372)          # the subject feed
PANEL = (SPLIT, BODY, W, H - FOOT)  # the console
INFO = 96                           # the info box at the bottom of the console
# Where the feed's face reticle sits (fractions of the feed), per game.
RETICLE = {'p1': (0.39, 0.47), 'p2': (0.62, 0.30)}
# The subject file under the feed.
DOSSIER = {
    'p1': (('SUBJECT', 'ALEX MERCER'), ('CODENAME', 'ZEUS'), ('FILED', '2009')),
    'p2': (('SUBJECT', 'SGT. JAMES HELLER'), ('FILED', '2012'), ('STATUS', 'EXPERIMENTAL')),
}


def pick_fonts(root):
    """(title, monospace) font families: what Windows has, then common Linux ones."""
    from tkinter import font as tkfont
    families = {f.casefold(): f for f in tkfont.families(root)}

    def first(names, default):
        return next((families[n.casefold()] for n in names if n.casefold() in families), default)
    title = first(('Bahnschrift SemiBold Condensed', 'Bahnschrift Condensed', 'Bahnschrift', 'DejaVu Sans Condensed',
                   'Arial Narrow', 'DejaVu Sans', 'Arial'), 'Helvetica')
    mono = first(('Consolas', 'Cascadia Mono', 'Lucida Console', 'DejaVu Sans Mono', 'Liberation Mono',
                  'Courier New'), 'Courier')
    return title, mono


def find_art(game_dir):
    for name in ('pic0.png', 'pic1.png'):
        path = Path(game_dir) / 'sce_sys' / name if game_dir else None
        if path and path.is_file():
            return path
    return None


def render_background(game):
    """The window's fixed parts (a PIL image), or None without Pillow: header, hazard stripes,
    the console's grid, the status line, CRT scanlines over all of it."""
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        return None
    t = THEMES[game]
    image = Image.new('RGB', (W, H), t['bg'])
    draw = ImageDraw.Draw(image)
    x0, y0, x1, y1 = PANEL
    draw.rectangle((x0, y0, x1, y1), fill=t['panel'])
    for x in range(x0 + 24, x1, 24):
        draw.line((x, y0, x, y1), fill=t['grid'])
    for y in range(y0 + 24, y1, 24):
        draw.line((x0, y, x1, y), fill=t['grid'])
    draw.line((x0, y0, x0, y1), fill=t['line'])
    draw.rectangle((0, 0, W, HEAD - 1), fill=t['top'])
    draw.rectangle((0, HEAD, W, BODY - 1), fill='#000000')
    for x in range(-24, W + 24, 24):
        draw.polygon([(x, BODY - 1), (x + 8, HEAD), (x + 20, HEAD), (x + 12, BODY - 1)], fill=t['red'])
    draw.rectangle((0, H - FOOT, W, H), fill=t['top'])
    draw.line((0, H - FOOT, W, H - FOOT), fill=t['line'])
    fx0, fy0, fx1, fy1 = FEED
    draw.rectangle((fx0 - 6, fy0 - 6, fx1 + 6, fy1 + 6), outline=t['hair'])
    for y in range(BODY + 8, H - FOOT - 4, 8):  # a ruler down the left edge
        draw.line((5, y, 9 if (y - BODY) % 40 else 13, y), fill=t['line'])
    shade = Image.new('L', (W, H), 0)
    lines = ImageDraw.Draw(shade)
    for y in range(0, H, 3):
        lines.line((0, y, W, y), fill=34)
    return Image.composite(Image.new('RGB', (W, H), '#000000'), image, shade)


def render_feed(game_dir, game):
    """The subject feed and three glitched copies of it (PIL images), or [] without Pillow: the
    game's half of the bundle's key art from the player's dump (Alex Mercer left, James Heller
    right) as a surveillance camera shows it, the reds (the virus) kept; static without it."""
    try:
        from PIL import Image, ImageChops, ImageDraw, ImageOps
    except ImportError:
        return []
    import random
    theme = THEMES[game]
    fw, fh = FEED[2] - FEED[0], FEED[3] - FEED[1]
    rng = random.Random(1886)
    frame = None
    path = find_art(game_dir)
    if path:
        try:
            art = Image.open(path).convert('RGB')
            bw = int(art.width * 0.41)
            bh = min(art.height, int(bw * fh / fw))
            x = int(art.width * (0.083 if GAMES[game][4] == 'left' else 0.52))
            art = art.crop((x, 0, x + bw, bh)).resize((fw, fh), Image.LANCZOS)
            black, mid, white = theme['feed']
            frame = ImageOps.colorize(art.convert('L'), black=black, mid=mid, white=white, midpoint=90)
            hsv = art.convert('HSV').split()
            red = Image.eval(hsv[1], lambda v: 0 if v < 90 else min(255, (v - 90) * 3))
            hue = Image.eval(hsv[0], lambda v: 255 if v < 14 or v > 238 else 0)
            frame = Image.composite(art, frame, ImageChops.multiply(red, hue))
        except Exception as error:
            print(f'launcher: art not used: {error}')
            frame = None
    if frame is None:  # no signal
        frame = Image.merge('RGB', [Image.effect_noise((fw, fh), 70).point(lambda v: v // 4)] * 3)
    grain = Image.merge('RGB', [Image.effect_noise((fw, fh), 40)] * 3)
    frame = Image.blend(frame, grain, 0.05)
    lines = Image.new('L', (fw, fh), 0)
    draw = ImageDraw.Draw(lines)
    for y in range(0, fh, 2):
        draw.line((0, y, fw, y), fill=48)
    vignette = Image.radial_gradient('L').resize((fw, fh)).point(lambda v: min(220, max(0, v - 120) * 2))
    frame = Image.composite(Image.new('RGB', (fw, fh), '#000000'), frame, ImageChops.lighter(lines, vignette))
    frames = [frame]
    for _ in range(3):
        glitched = frame.copy()
        for _ in range(rng.randint(3, 6)):
            y0 = rng.randrange(0, fh - 8)
            y1 = min(fh, y0 + rng.randint(3, 28))
            shift = rng.choice((-1, 1)) * rng.randint(6, 44)
            glitched.paste(ImageChops.offset(frame.crop((0, y0, fw, y1)), shift, 0), (0, y0))
        r, g, b = glitched.split()
        frames.append(Image.merge('RGB', (ImageChops.offset(r, 4, 0), g, ImageChops.offset(b, -4, 0))))
    return frames


# --------------------------------------------------------------------------------------------
# Keys for the Controls page: Tk key events to SDL key names (what the runtime reads).

# Windows virtual-key codes: the key's position, whatever the keyboard layout (a Russian layout
# still gives VK 'A' for the key the runtime's SDL scancode "A" is).
VK_NAMES = {0x08: 'Backspace', 0x09: 'Tab', 0x0D: 'Return', 0x14: 'CapsLock', 0x1B: 'Escape', 0x20: 'Space',
            0x21: 'PageUp', 0x22: 'PageDown', 0x23: 'End', 0x24: 'Home', 0x25: 'Left', 0x26: 'Up',
            0x27: 'Right', 0x28: 'Down', 0x2D: 'Insert', 0xBA: ';', 0xBD: '-', 0xBE: '.', 0xBF: '/',
            0xC0: '`', 0xDB: '[', 0xDC: '\\', 0xDD: ']', 0xDE: "'"}
VK_NAMES.update({0x30 + i: str(i) for i in range(10)})
VK_NAMES.update({0x41 + i: chr(0x41 + i) for i in range(26)})
VK_NAMES.update({0x60 + i: f'Keypad {i}' for i in range(10)})
VK_NAMES.update({0x70 + i: f'F{i + 1}' for i in range(12)})
KEYSYM_NAMES = {'space': 'Space', 'Tab': 'Tab', 'Return': 'Return', 'BackSpace': 'Backspace',
                'Escape': 'Escape', 'Caps_Lock': 'CapsLock', 'Shift_L': 'Left Shift', 'Shift_R': 'Right Shift',
                'Control_L': 'Left Ctrl', 'Control_R': 'Right Ctrl', 'Alt_L': 'Left Alt', 'Alt_R': 'Right Alt',
                'Up': 'Up', 'Down': 'Down', 'Left': 'Left', 'Right': 'Right', 'Prior': 'PageUp', 'Next': 'PageDown',
                'Home': 'Home', 'End': 'End', 'semicolon': ';', 'minus': '-', 'period': '.', 'slash': '/',
                'grave': '`', 'bracketleft': '[', 'backslash': '\\', 'bracketright': ']', 'apostrophe': "'"}
# Not bindable: the port's own keys (Insert opens its menu, F7 mouse look, F9 pad recording, F10
# hang report) and the characters THEORDER_KEY_MAP uses as separators.
RESERVED_KEYS = {'Insert', 'F7', 'F9', 'F10', ',', '=', '+'}


def sdl_key_name(event):
    name = KEYSYM_NAMES.get(event.keysym)
    if name is None and WINDOWS:
        name = VK_NAMES.get(event.keycode)
    if name is None and len(event.keysym) == 1 and event.keysym.isascii() and event.keysym.isalnum():
        name = event.keysym.upper()
    if name is None and event.keysym.startswith('F') and event.keysym[1:].isdigit():
        name = event.keysym
    return None if name in RESERVED_KEYS else name


# --------------------------------------------------------------------------------------------
# The window.

STATE_TAGS = {'ok': '[ OK ]', 'warn': '[WARN]', 'bad': '[FAIL]'}


class Launcher:
    TABS = ('GAME', 'GRAPHICS', 'DISPLAY', 'CONTROLS', 'FOLDER')

    def __init__(self, root):
        import random
        import tkinter as tk
        self.tk = tk
        self.root = root
        self.random = random.Random()
        self.values = load_settings()
        self.process = None
        self.started = 0.0
        self.busy = None
        self.capture = None
        self.checks, self.sfo = [], {}
        self.images = {}
        self.regions, self.hot = [], None
        self.desc_target, self.desc_shown, self.desc_job = '', 0, None
        self.blink, self.scan, self.busy_x = True, 0, 0
        self.next_glitch = time.time() + 2.5
        self.status_text, self.status_color = '', C['muted']
        root.title('PROTOTYPE')
        root.geometry(f'{W}x{H}')
        root.resizable(False, False)
        root.configure(bg=C['bg'])
        self.title_font, self.mono = pick_fonts(root)
        self.canvas = tk.Canvas(root, width=W, height=H, highlightthickness=0, bd=0, bg=C['bg'])
        self.canvas.pack()
        self.canvas.bind('<Motion>', self.motion)
        self.canvas.bind('<Leave>', lambda e: self.set_hot(None))
        root.bind('<Return>', self.key_play)
        root.bind('<KP_Enter>', self.key_play)
        for i, name in enumerate(self.TABS):
            root.bind(f'<Key-{i + 1}>', lambda e, n=name: self.key_page(e, n))
        self.window_icon()
        self.refresh_game(redraw=False)
        self.draw()
        root.protocol('WM_DELETE_WINDOW', self.close)
        root.after(400, self.poll)
        root.after(40, self.tick_fast)
        root.after(500, self.tick_slow)
        if not self.values['game_dir']:
            root.after(300, self.choose_folder)

    # ---- helpers
    def f(self, size, weight='normal'):
        return (self.mono, size, weight)

    def window_icon(self):
        try:
            from PIL import Image, ImageTk
            path = Path(self.values['game_dir']) / 'sce_sys' / 'icon0.png'
            if path.is_file():
                icon = ImageTk.PhotoImage(Image.open(path).convert('RGB').resize((64, 64)))
                self.root.iconphoto(True, icon)
                self._icon = icon
        except Exception:
            pass

    def save(self):
        save_settings(self.values)

    def set_status(self, text, color=None):
        self.status_text = text
        self.status_color = color or C['muted']
        item = getattr(self, 'status_item', None)
        if item and self.canvas.type(item):
            self.canvas.itemconfigure(item, text=self.status_line(), fill=self.status_color)
            self.place_cursor()

    def status_line(self):
        text = self.status_text.upper()
        return text if len(text) <= 92 else text[:91] + '…'

    def refresh_game(self, redraw=True):
        self.checks, self.sfo = check_game(self.values['game_dir'], self.game)
        bad = [c for c in self.checks if c[1] == 'bad']
        if not self.values['game_dir']:
            self.set_status('Select your game folder to begin', C['warn'])
        elif bad:
            self.set_status(f'{bad[0][0]}: {bad[0][2]}', C['bad'])
        else:
            self.set_status('Ready to play', C['ok'])
        if redraw:
            self.images = {}
            self.window_icon()
            self.draw()

    @property
    def ready(self):
        return bool(self.checks) and not any(state == 'bad' for _, state, _ in self.checks)

    @property
    def running(self):
        return self.process is not None and self.process.poll() is None

    @property
    def game(self):
        return self.values.get('game', 'p1')

    def getv(self, key, default=None):
        """A setting; 'game.<name>' is one of the selected game's own (its patches)."""
        if key.startswith('game.'):
            return self.values['per_game'][self.game].get(key[5:], default)
        return self.values.get(key, default)

    def setv(self, key, value):
        if key.startswith('game.'):
            self.values['per_game'][self.game][key[5:]] = value
        else:
            self.values[key] = value

    def hover(self, item, normal, hot):
        cv = self.canvas
        cv.tag_bind(item, '<Enter>', lambda e: (cv.itemconfigure(item, fill=hot), cv.configure(cursor='hand2')))
        cv.tag_bind(item, '<Leave>', lambda e: (cv.itemconfigure(item, fill=normal), cv.configure(cursor='')))

    def clickable(self, tag, command, parts=()):
        """Hand cursor over `tag`; parts: (item, option, normal, hot) changed while the pointer is on it."""
        cv = self.canvas

        def enter(e):
            cv.configure(cursor='hand2')
            for item, option, _, hot in parts:
                cv.itemconfigure(item, **{option: hot})

        def leave(e):
            cv.configure(cursor='')
            for item, option, normal, _ in parts:
                cv.itemconfigure(item, **{option: normal})
        cv.tag_bind(tag, '<Enter>', enter)
        cv.tag_bind(tag, '<Leave>', leave)
        cv.tag_bind(tag, '<Button-1>', lambda e: command(e))

    def photos(self, kind):
        """The cached Tk images of the background or the feed (and its glitches) for this game."""
        key = (kind, self.game)
        if key not in self.images:
            made = render_background(self.game) if kind == 'bg' else render_feed(self.values['game_dir'], self.game)
            made = [] if made is None else made if isinstance(made, list) else [made]
            if made:
                from PIL import ImageTk
                made = [ImageTk.PhotoImage(m) for m in made]
            self.images[key] = made
        return self.images[key]

    def text_width(self, text, font):
        from tkinter import font as tkfont
        return tkfont.Font(root=self.root, font=font).measure(text)

    # ---- drawing
    def draw(self):
        cv = self.canvas
        cv.delete('all')
        C.update(STATE)
        C.update(THEMES[self.game])
        self.root.configure(bg=C['bg'])
        cv.configure(bg=C['bg'])
        self.regions, self.hot = [], None
        background = self.photos('bg')
        if background:
            cv.create_image(0, 0, image=background[0], anchor='nw')
        else:
            self.draw_plain_background()
        self.draw_header()
        self.draw_file()
        self.draw_panel()
        self.draw_footer()
        self.rehover()

    def draw_plain_background(self):
        """Without Pillow: the same frame in flat colours."""
        cv = self.canvas
        cv.create_rectangle(0, 0, W, HEAD, fill=C['top'], outline='')
        cv.create_rectangle(0, HEAD, W, BODY, fill='#000000', outline='')
        for x in range(-24, W + 24, 24):
            cv.create_polygon(x, BODY, x + 8, HEAD, x + 20, HEAD, x + 12, BODY, fill=C['red'], outline='')
        cv.create_rectangle(*PANEL, fill=C['panel'], outline='')
        cv.create_line(SPLIT, BODY, SPLIT, H - FOOT, fill=C['line'])
        cv.create_rectangle(0, H - FOOT, W, H, fill=C['top'], outline='')
        cv.create_line(0, H - FOOT, W, H - FOOT, fill=C['line'])

    def draw_header(self):
        cv = self.canvas
        y = HEAD // 2
        cv.create_rectangle(16, y - 5, 26, y + 5, fill=C['red'], outline='', tags=('beacon',))
        item = cv.create_text(38, y, text='BLACKWATCH', anchor='w', fill=C['text'], font=self.f(11, 'bold'))
        cv.create_text(cv.bbox(item)[2] + 12, y, text='//  CONTAINMENT TERMINAL 7-A  ·  MANHATTAN ISLAND',
                       anchor='w', fill=C['muted'], font=self.f(9))
        cv.create_text(W - 270, y, text=f'BIOHAZARD BUNDLE  ·  NATIVE PC PORT v{VERSION}', anchor='e',
                       fill=C['dim'], font=self.f(8))
        cv.create_text(W - 16, y, text=self.clock(), anchor='e', fill=C['text'], font=self.f(10, 'bold'),
                       tags=('clock',))

    @staticmethod
    def clock():
        return time.strftime('%a %d %b %Y  %H:%M:%S').upper()

    def draw_file(self):
        """The left column: the game tabs, the subject feed, the file, PLAY and the folder scan."""
        cv = self.canvas
        locked = self.running or bool(self.busy)
        # The two games as file tabs.
        x, y0, y1, w = FEED[0], BODY + 14, BODY + 46, 206
        for i, (key, (name, *_)) in enumerate(GAMES.items()):
            theme = THEMES[key]
            active = key == self.game
            tag = f'game_{key}'
            shape = cv.create_polygon(x, y1, x, y0 + 8, x + 8, y0, x + w - 16, y0, x + w, y0 + 16, x + w, y1,
                                      fill=theme['card'] if active else C['bg'],
                                      outline=theme['red'] if active else C['line'], tags=(tag,))
            number = cv.create_text(x + 14, (y0 + y1) / 2, text=f'FILE {i + 1:02d}', anchor='w',
                                    fill=theme['red'] if active else C['dim'], font=self.f(8, 'bold'), tags=(tag,))
            label = cv.create_text(cv.bbox(number)[2] + 10, (y0 + y1) / 2, text=name, anchor='w',
                                   fill=C['text'] if active else C['muted'], font=self.f(10, 'bold'), tags=(tag,))
            if not active and not locked:
                self.clickable(tag, lambda e, k=key: self.choose_game(k),
                               ((shape, 'fill', C['bg'], theme['card']), (shape, 'outline', C['line'], theme['red']),
                                (label, 'fill', C['muted'], C['text']), (number, 'fill', C['dim'], theme['red'])))
            x += w + 14
        self.draw_feed()
        # The file.
        fx0, _, fx1, fy1 = FEED
        x, y = fx0, fy1 + 22
        for label, value in DOSSIER[self.game]:
            item = cv.create_text(x, y, text=label, anchor='w', fill=C['dim'], font=self.f(8))
            item = cv.create_text(cv.bbox(item)[2] + 7, y, text=value, anchor='w',
                                  fill=C['warn'] if value == 'EXPERIMENTAL' else C['text'], font=self.f(8, 'bold'))
            x = cv.bbox(item)[2] + 16
        name = GAMES[self.game][0]
        base, _, number = name.partition(' ')
        title = cv.create_text(fx0 - 3, fy1 + 58, text=base, anchor='w', fill=C['text'],
                               font=(self.title_font, 40, 'bold'), tags=('title',))
        if number:
            cv.create_text(cv.bbox(title)[2] + 10, fy1 + 58, text=number, anchor='w', fill=C['red'],
                           font=(self.title_font, 40, 'bold'), tags=('title',))
        settings = self.values['per_game'][self.game]
        version = self.sfo.get('APP_VER')
        facts = [TITLE_ID] + ([f'APP {version}'] if version else []) + [
            settings['render_res'].replace('x', ' × '), '60 FPS' if settings['fps60'] else '30 FPS']
        item = cv.create_text(fx0, fy1 + 98, text=' · '.join(facts), anchor='w', fill=C['muted'], font=self.f(9))
        change = cv.create_text(cv.bbox(item)[2] + 14, fy1 + 98, text='[ CHANGE ]', anchor='w', fill=C['red'],
                                font=self.f(8, 'bold'))
        self.clickable(change, lambda e: self.switch_tab('GAME'), ((change, 'fill', C['red'], C['red_hot']),))
        self.draw_play(fy1 + 118)
        self.draw_scan(fy1 + 196)

    def draw_feed(self):
        cv = self.canvas
        fx0, fy0, fx1, fy1 = FEED
        frames = self.photos('feed')
        signal = find_art(self.values['game_dir']) is not None
        if frames:
            cv.create_image(fx0, fy0, image=frames[0], anchor='nw', tags=('feed',))
        else:
            cv.create_rectangle(fx0, fy0, fx1, fy1, fill='#000000', outline='')
        cv.create_line(fx0, fy0, fx1, fy0, fill=C['red'], tags=('scan',), state='hidden')
        for cx, cy, dx, dy in ((fx0, fy0, 1, 1), (fx1, fy0, -1, 1), (fx0, fy1, 1, -1), (fx1, fy1, -1, -1)):
            cv.create_line(cx, cy + dy * 18, cx, cy, cx + dx * 18, cy, fill=C['red'], width=2)
        self.tag_label(fx0 + 10, fy0 + 14, 'CAM 04  //  SUBJECT FEED')
        item = cv.create_text(fx1 - 12, fy0 + 14, text='REC', anchor='e', fill=C['red_hot'], font=self.f(8, 'bold'),
                              tags=('rec',))
        x = cv.bbox(item)[0] - 8
        cv.create_oval(x - 7, fy0 + 10, x + 1, fy0 + 18, fill=C['red_hot'], outline='', tags=('rec',))
        cv.create_text(fx0 + 12, fy1 - 13, text='40.7128 N   74.0060 W', anchor='w', fill=C['muted'], font=self.f(8))
        cv.create_text(fx1 - 12, fy1 - 13, text=time.strftime('%H:%M:%S'), anchor='e', fill=C['muted'],
                       font=self.f(8), tags=('feedtime',))
        if not signal:
            cx, cy = (fx0 + fx1) / 2, (fy0 + fy1) / 2
            cv.create_text(cx, cy - 12, text='NO SIGNAL', fill=C['text'], font=(self.title_font, 28, 'bold'),
                           tags=('title',))
            link = cv.create_text(cx, cy + 22, text='> SELECT THE GAME FOLDER', fill=C['red'], font=self.f(9, 'bold'))
            self.clickable(link, lambda e: self.choose_folder(), ((link, 'fill', C['red'], C['red_hot']),))
            return
        # A reticle on the face, and the file's stamp.
        rx = fx0 + (fx1 - fx0) * RETICLE[self.game][0]
        ry = fy0 + (fy1 - fy0) * RETICLE[self.game][1]
        size = 40
        for dx, dy in ((-1, -1), (1, -1), (-1, 1), (1, 1)):
            cx, cy = rx + dx * size, ry + dy * size
            cv.create_line(cx, cy - dy * 10, cx, cy, cx - dx * 10, cy, fill=C['red_hot'], width=1)
        cv.create_line(rx - 4, ry, rx + 5, ry, fill=C['red_hot'])
        cv.create_line(rx, ry - 4, rx, ry + 5, fill=C['red_hot'])
        self.tag_label(rx - size, ry + size + 12, 'ID MATCH 99.2%', color=C['red_hot'])
        self.stamp(fx1 - 92, fy1 - 52, 'CLASSIFIED', 11)

    def tag_label(self, x, y, text, color=None):
        """Small text on a black plate, over the feed."""
        cv = self.canvas
        item = cv.create_text(x + 5, y, text=text, anchor='w', fill=color or C['text'], font=self.f(8, 'bold'))
        b = cv.bbox(item)
        plate = cv.create_rectangle(b[0] - 5, b[1] - 1, b[2] + 5, b[3] + 1, fill='#000000', outline='')
        cv.tag_lower(plate, item)

    def stamp(self, cx, cy, text, angle):
        """A rubber stamp: rotated text in a rotated double frame."""
        import math
        cv = self.canvas
        font = (self.title_font, 17, 'bold')
        tw = self.text_width(text, font)
        a = math.radians(angle)
        ca, sa = math.cos(a), math.sin(a)
        for grow, width in ((0, 2), (5, 1)):
            hw, hh = tw / 2 + 12 + grow, 15 + grow
            points = []
            for dx, dy in ((-hw, -hh), (hw, -hh), (hw, hh), (-hw, hh)):
                points += [cx + dx * ca + dy * sa, cy - dx * sa + dy * ca]
            cv.create_polygon(points, fill='', outline=C['red_hot'], width=width)
        cv.create_text(cx, cy, text=text, angle=angle, fill=C['red_hot'], font=font)

    def draw_play(self, y):
        cv = self.canvas
        x0, x1, h = FEED[0], FEED[2], 54
        y1 = y + h

        def chamfer(a, b, cut=12):
            return (a, y + cut, a + cut, y, b, y, b, y1 - cut, b - cut, y1, a, y1)
        if self.running:
            cv.create_polygon(chamfer(x0, x1 - 118), fill=C['track'], outline='')
            cv.create_rectangle(x0 + 22, y + h / 2 - 7, x0 + 36, y + h / 2 + 7, fill=C['red'], outline='')
            cv.create_text(x0 + 50, y + h / 2, text='RUNNING', anchor='w', fill=C['text'], font=self.f(15, 'bold'))
            cv.create_text(x1 - 136, y + h / 2, text=self.elapsed(), anchor='e', fill=C['muted'], font=self.f(9),
                           tags=('elapsed',))
            shape = cv.create_polygon(chamfer(x1 - 108, x1, 8), fill=C['bg'], outline=C['red'], width=2, tags=('stop',))
            label = cv.create_text(x1 - 54, y + h / 2, text='STOP', fill=C['red'], font=self.f(12, 'bold'), tags=('stop',))
            self.clickable('stop', lambda e: self.stop(), ((shape, 'fill', C['bg'], C['red_dark']),
                                                           (label, 'fill', C['red'], C['text'])))
        elif self.busy:
            cv.create_polygon(chamfer(x0, x1), fill=C['red_dark'], outline='')
            self.busy_x = 0
            cv.create_rectangle(x0, y + h - 5, x0 + 90, y + h - 2, fill=C['red'], outline='', tags=('busybar',))
            cv.create_text(x0 + 22, y + h / 2, text=self.busy.upper() + '…', anchor='w', fill=C['text'],
                           font=self.f(15, 'bold'))
        else:
            ready = self.ready
            shape = cv.create_polygon(chamfer(x0, x1), fill=C['red'] if ready else C['red_dark'], outline='',
                                      tags=('play',))
            ink = C['ink'] if ready else C['dim']
            cv.create_polygon(x0 + 24, y + h / 2 - 10, x0 + 41, y + h / 2, x0 + 24, y + h / 2 + 10, fill=ink,
                              outline='', tags=('play',))
            cv.create_text(x0 + 56, y + h / 2, text='PLAY', anchor='w', fill=ink, font=self.f(18, 'bold'), tags=('play',))
            cv.create_text(x1 - 22, y + h / 2, text='[ ENTER ]' if ready else 'LOCKED', anchor='e',
                           fill=C['ink'] if ready else C['dim'], font=self.f(8, 'bold'), tags=('play',))
            if ready:
                self.clickable('play', lambda e: self.play(), ((shape, 'fill', C['red'], C['red_hot']),))

    def elapsed(self):
        seconds = int(time.time() - self.started) if self.started else 0
        return f'T+{seconds // 3600:02d}:{seconds // 60 % 60:02d}:{seconds % 60:02d}'

    def draw_scan(self, y):
        """The game folder's checks, as a terminal prints them."""
        cv = self.canvas
        x0, x1 = FEED[0], FEED[2]
        item = cv.create_text(x0, y, text='> INTEGRITY SCAN', anchor='w', fill=C['dim'], font=self.f(8, 'bold'))
        cv.create_line(cv.bbox(item)[2] + 10, y, x1, y, fill=C['hair'])
        y += 17
        for label, state, detail in self.checks[:6]:
            cv.create_text(x0, y, text=STATE_TAGS[state], anchor='w', fill=C[state], font=self.f(8, 'bold'))
            cv.create_text(x0 + 52, y, text=label.upper(), anchor='w', fill=C['muted'], font=self.f(8))
            cv.create_text(x1, y, text=detail, anchor='e', fill=C['dim'], font=self.f(8))
            y += 15

    def draw_footer(self):
        cv = self.canvas
        y = H - FOOT / 2
        cv.create_text(14, y, text='>', anchor='w', fill=C['red'], font=self.f(10, 'bold'))
        self.status_item = cv.create_text(30, y, text=self.status_line(), anchor='w', fill=self.status_color,
                                          font=self.f(9))
        cv.create_rectangle(0, 0, 0, 0, fill=C['text'], outline='', tags=('cursor',))
        self.place_cursor()
        x = W - 14
        for text, command in reversed((('GAME FOLDER', self.choose_folder), ('SAVES', self.open_saves),
                                       ('BACKUPS', self.open_backups), ('LOG', self.open_log),
                                       ('PORT FOLDER', lambda: self.open_path(PORT_DIR)))):
            item = cv.create_text(x, y, text=f'[ {text} ]', anchor='e', fill=C['muted'], font=self.f(8, 'bold'))
            self.clickable(item, lambda e, c=command: c(), ((item, 'fill', C['muted'], C['text']),))
            x = cv.bbox(item)[0] - 10

    def place_cursor(self):
        b = self.canvas.bbox(self.status_item)
        if b:
            self.canvas.coords('cursor', b[2] + 4, b[1] + 2, b[2] + 11, b[3] - 2)

    # ---- the console (settings)
    def draw_panel(self):
        cv = self.canvas
        x0, y0, x1, y1 = PANEL
        current = self.values.get('panel') if self.values.get('panel') in self.TABS else 'GAME'
        tx, ty = x0 + 28, y0 + 30
        for i, name in enumerate(self.TABS):
            active = name == current
            item = cv.create_text(tx + 10, ty, text=f'{i + 1} {name}', anchor='w', fill=C['ink'] if active else C['muted'],
                                  font=self.f(9, 'bold'))
            b = cv.bbox(item)
            box = cv.create_rectangle(b[0] - 10, ty - 13, b[2] + 10, ty + 13, fill=C['red'] if active else C['panel'],
                                      outline='' if active else C['hair'])
            cv.tag_lower(box, item)
            if not active:
                tag = f'tab_{name}'
                cv.addtag_withtag(tag, item)
                cv.addtag_withtag(tag, box)
                self.clickable(tag, lambda e, n=name: self.switch_tab(n),
                               ((item, 'fill', C['muted'], C['text']), (box, 'outline', C['hair'], C['red'])))
            tx = b[2] + 18
        cv.create_text(x1 - 28, ty, text='KEYS 1-5  ·  ENTER PLAYS', anchor='e', fill=C['dim'], font=self.f(8))
        cv.create_line(x0 + 28, ty + 24, x1 - 28, ty + 24, fill=C['line'])
        self.row_y = ty + 52
        # The info box: what the setting under the pointer does.
        ix0, iy0, ix1, iy1 = x0 + 28, y1 - INFO + 6, x1 - 28, y1 - 14
        cv.create_rectangle(ix0, iy0, ix1, iy1, outline=C['line'], fill=C['bg'])
        item = cv.create_text(ix0 + 16, iy0, text='INFO', anchor='w', fill=C['ink'], font=self.f(8, 'bold'))
        b = cv.bbox(item)
        cv.tag_lower(cv.create_rectangle(b[0] - 6, b[1], b[2] + 6, b[3], fill=C['red'], outline=''), item)
        self.desc_item = cv.create_text(ix0 + 16, iy0 + 18, text='', anchor='nw', fill=C['muted'], font=self.f(9),
                                        width=ix1 - ix0 - 32)
        self.desc_target, self.desc_shown = None, 0
        {'GAME': self.tab_game, 'GRAPHICS': self.tab_graphics, 'DISPLAY': self.tab_display,
         'CONTROLS': self.tab_controls, 'FOLDER': self.tab_folder}[current]()

    def switch_tab(self, name):
        self.capture_stop()
        self.values['panel'] = name
        self.save()
        self.draw()

    def key_page(self, event, name):
        if self.capture:
            return self.capture_key(event)
        self.switch_tab(name)

    def key_play(self, event):
        if self.capture:
            return self.capture_key(event)
        if self.ready and not self.running and not self.busy:
            self.play()

    # ---- the info box and the row under the pointer
    def region(self, box, desc, items=()):
        """A part of the console that describes itself in the info box; items: (item, fill) lit with it."""
        self.regions.append((box, desc, items))

    def motion(self, event):
        x, y = self.canvas.canvasx(event.x), self.canvas.canvasy(event.y)
        self.set_hot(next((r for r in self.regions if r[0][0] <= x <= r[0][2] and r[0][1] <= y <= r[0][3]), None))

    def rehover(self):
        """After a redraw: light the row still under the pointer."""
        try:
            x = self.root.winfo_pointerx() - self.canvas.winfo_rootx()
            y = self.root.winfo_pointery() - self.canvas.winfo_rooty()
        except self.tk.TclError:
            x = y = -1
        hit = next((r for r in self.regions if r[0][0] <= x <= r[0][2] and r[0][1] <= y <= r[0][3]), None)
        self.set_hot(hit, instant=True)

    def set_hot(self, region, instant=False):
        if region is self.hot and region is not None:
            return
        cv = self.canvas
        if self.hot:
            for item, _ in self.hot[2]:
                cv.itemconfigure(item, fill='')
        self.hot = region
        if region:
            for item, fill in region[2]:
                cv.itemconfigure(item, fill=fill)
        self.describe(region[1] if region else '', instant)

    def describe(self, text, instant=False):
        """Types the text into the info box, as a terminal would."""
        text = text or 'Point at a setting to read what it does.'
        if text == self.desc_target:
            return
        self.desc_target = text
        self.desc_shown = len(text) if instant else 0
        if self.desc_job:
            self.root.after_cancel(self.desc_job)
            self.desc_job = None
        self.type_desc()

    def type_desc(self):
        self.desc_job = None
        text = self.desc_target or ''
        done = self.desc_shown >= len(text)
        try:
            self.canvas.itemconfigure(self.desc_item, text=text if done else text[:self.desc_shown] + '█',
                                      fill=C['muted'] if self.hot else C['dim'])
        except self.tk.TclError:
            return
        if not done:
            self.desc_shown += 7
            self.desc_job = self.root.after(12, self.type_desc)

    # ---- settings rows
    def row(self, label, desc, height=40, locked=False):
        cv = self.canvas
        x0, _, x1, _ = PANEL
        y = self.row_y
        top, bottom = y - height / 2 + 1, y + height / 2 - 1
        glow = cv.create_rectangle(x0 + 16, top, x1 - 16, bottom, fill='', outline='')
        tick = cv.create_rectangle(x0 + 16, top, x0 + 19, bottom, fill='', outline='')
        cv.create_text(x0 + 32, y, text=label, anchor='w', fill=C['dim'] if locked else C['text'], font=self.f(10))
        cv.create_line(x0 + 28, y + height / 2, x1 - 28, y + height / 2, fill=C['hair'])
        self.region((x0 + 16, top, x1 - 16, bottom), desc, ((glow, C['hi']), (tick, C['red'])))
        self.row_y += height
        return y, f'row{y}'

    def stepper(self, label, desc, key, options, locked=False):
        """< value > over a list of (value, text): the left half steps back, the right half on."""
        cv = self.canvas
        y, tag = self.row(label, desc, locked=locked)
        x1 = PANEL[2] - 32
        width = 210
        x0 = x1 - width
        current = self.getv(key)
        index = next((i for i, (v, _) in enumerate(options) if v == current), 0)
        box = cv.create_rectangle(x0, y - 13, x1, y + 13, outline=C['line'], fill=C['bg'], tags=(tag,))
        cv.create_text((x0 + x1) / 2, y, text=options[index][1], fill=C['dim'] if locked else C['text'],
                       font=self.f(10, 'bold'), tags=(tag,))
        arrows = [cv.create_text(ax, y, text=t, fill=C['dim'] if locked else C['red'], font=self.f(11, 'bold'),
                                 tags=(tag,)) for ax, t in ((x0 + 14, '<'), (x1 - 14, '>'))]
        if locked:
            return

        def step(e):
            direction = -1 if self.canvas.canvasx(e.x) < (x0 + x1) / 2 else 1
            self.setv(key, options[(index + direction) % len(options)][0])
            self.save()
            self.draw()
        self.clickable(tag, step, ((box, 'outline', C['line'], C['red']),) +
                       tuple((a, 'fill', C['red'], C['red_hot']) for a in arrows))

    def toggle(self, label, desc, key, locked=False):
        """[ ON | OFF ]"""
        cv = self.canvas
        y, tag = self.row(label, desc, locked=locked)
        x1 = PANEL[2] - 32
        on = bool(self.getv(key)) and not locked
        half = 52
        parts = []
        for i, (text, lit) in enumerate((('ON', on), ('OFF', not on))):
            sx = x1 - 2 * half + i * half
            fill = (C['red'] if text == 'ON' else C['track']) if lit and not locked else C['bg']
            box = cv.create_rectangle(sx, y - 12, sx + half, y + 12, fill=fill, outline=C['line'], tags=(tag,))
            color = (C['ink'] if text == 'ON' else C['text']) if lit and not locked else C['dim']
            cv.create_text(sx + half / 2, y, text=text, fill=color, font=self.f(9, 'bold'), tags=(tag,))
            parts.append((box, 'outline', C['line'], C['red']))
        if locked:
            return

        def flip(e):
            self.setv(key, not self.getv(key))
            self.save()
            self.draw()
        self.clickable(tag, flip, tuple(parts))

    def slider(self, label, desc, key, low, high, locked=False):
        """A bar of blocks; click or drag."""
        cv = self.canvas
        y, tag = self.row(label, desc, locked=locked)
        x1 = PANEL[2] - 32
        count, step = 22, 8
        bx0 = x1 - 50 - count * step
        value = int(self.getv(key, low))
        blocks = []
        for i in range(count):
            bx = bx0 + i * step
            blocks.append(cv.create_rectangle(bx, y - 9, bx + step - 2, y + 9, outline='', tags=(tag,)))
        num = cv.create_text(x1, y, text=f'{value}', anchor='e', fill=C['dim'] if locked else C['text'],
                             font=self.f(10, 'bold'))

        def paint(v):
            lit = round((v - low) / (high - low) * count)
            for i, block in enumerate(blocks):
                cv.itemconfigure(block, fill=(C['dim'] if locked else C['red']) if i < lit else C['track'])
            cv.itemconfigure(num, text=f'{v}')
        paint(value)
        if locked:
            return

        def move(e):
            frac = min(1.0, max(0.0, (self.canvas.canvasx(e.x) - bx0) / (count * step - 2)))
            v = int(round(low + frac * (high - low)))
            self.setv(key, v)
            paint(v)
        cv.tag_bind(tag, '<Button-1>', move)
        cv.tag_bind(tag, '<B1-Motion>', move)
        cv.tag_bind(tag, '<ButtonRelease-1>', lambda e: self.save())
        cv.tag_bind(tag, '<Enter>', lambda e: cv.configure(cursor='hand2'))
        cv.tag_bind(tag, '<Leave>', lambda e: cv.configure(cursor=''))

    def note(self, text, color=None, height=30):
        self.canvas.create_text(PANEL[0] + 32, self.row_y - 16, text=text, anchor='nw', fill=color or C['muted'],
                                font=self.f(9), width=PANEL[2] - PANEL[0] - 64)
        self.row_y += height

    def section(self, text, first=False):
        cv = self.canvas
        if not first:
            self.row_y += 14
        y = self.row_y - 8
        cv.create_rectangle(PANEL[0] + 28, y - 4, PANEL[0] + 36, y + 4, fill=C['red'], outline='')
        item = cv.create_text(PANEL[0] + 44, y, text=text, anchor='w', fill=C['red'], font=self.f(8, 'bold'))
        cv.create_line(cv.bbox(item)[2] + 10, y, PANEL[2] - 28, y, fill=C['hair'])
        self.row_y += 24

    def small_button(self, x, y, text, command, desc=''):
        """An outlined button; returns its right edge."""
        cv = self.canvas
        tag = f'button_{x}_{y}'
        label = cv.create_text(x + 14, y, text=text, anchor='w', fill=C['text'], font=self.f(9, 'bold'), tags=(tag,))
        b = cv.bbox(label)
        box = cv.create_rectangle(x, y - 13, b[2] + 14, y + 13, outline=C['line'], fill=C['bg'], tags=(tag,))
        cv.tag_lower(box, label)
        self.clickable(tag, lambda e: command(), ((box, 'outline', C['line'], C['red']),
                                                  (box, 'fill', C['bg'], C['red_dark'])))
        self.region((x, y - 13, b[2] + 14, y + 13), desc)
        return b[2] + 14

    # ---- pages
    def tab_game(self):
        """The selected game's own settings: the patches made for it."""
        name = GAMES[self.game][0]
        other = GAMES['p2' if self.game == 'p1' else 'p1'][0]
        res = [(r, r.replace('x', ' × ')) for r in RESOLUTIONS]
        self.section(f'{name} PATCHES', first=True)
        if self.game == 'p2':
            self.stepper('Render resolution', 'Experimental. The resolution PROTOTYPE 2 renders at; 1920 × 1080 '
                         'is the PS4\'s. Above it the game is given more video memory. Lower is scaled up by '
                         'FSR 1. If the game misbehaves, set it back to 1920 × 1080.', 'game.render_res', res)
            self.stepper('Frame rate', 'Experimental. 60 FPS lifts the game\'s 30 FPS cap. If the game runs too '
                         'fast or a menu misbehaves, go back to 30 FPS.', 'game.fps60',
                         [(False, '30 FPS'), (True, '60 FPS')])
        else:
            self.stepper('Render resolution', 'Experimental. The resolution the game renders at; 1920 × 1080 is '
                         'the PS4\'s. Higher supersamples (sharper, heavier); lower is scaled up by FSR 1. If the '
                         'game misbehaves, set it back to 1920 × 1080.', 'game.render_res', res)
            self.stepper('Frame rate', '60 FPS: gameplay at 60; videos and menus keep their normal speed.',
                         'game.fps60', [(False, '30 FPS'), (True, '60 FPS')])
            self.toggle('White tendrils fix', 'Consume tendrils keep their red veins (a GPU emulation '
                        'difference). Keep it on.', 'game.tendril_fix')
        self.row_y += 20
        self.note(f'These apply to {name} only: {other} keeps its own (switch with the file tabs on the left). '
                  'Graphics, display and controls are shared by both games.', C['dim'], height=48)
        self.profile()
        if self.game == 'p2':
            self.section('EXPERIMENTAL')
            self.note('PROTOTYPE 2 runs, but is newer in the port than PROTOTYPE: expect rough edges. '
                      'Keyboard button icons are not in yet; the game shows PlayStation buttons. '
                      '[ LOG ] at the bottom right shows what happened if it stops.', C['muted'], height=70)

    def profile(self):
        """The shared settings the game starts with, each a click away from its page."""
        cv = self.canvas
        v = self.values
        self.section('LAUNCH PROFILE  ·  SHARED')
        modes = {'windowed': 'WINDOWED', 'borderless': 'BORDERLESS', 'fullscreen': 'FULLSCREEN'}
        lines = (('UPSCALING', 'FSR 1' if v['fsr1'] else 'OFF', 'GRAPHICS'),
                 ('DISPLAY', modes.get(v['display_mode'], v['display_mode']).upper(), 'DISPLAY'),
                 ('SHARPENING', f"{v['rcas_strength']}" if v['rcas'] else 'OFF', 'GRAPHICS'),
                 ('V-SYNC', 'ON' if v['vsync'] else 'OFF', 'DISPLAY'),
                 ('FILTERING', f"{v['anisotropy']}× AF" if v['anisotropy'] else 'GAME', 'GRAPHICS'),
                 ('MOUSE LOOK', f"ON · {v['mouse_sensitivity']}" if v['mouse_look'] else 'OFF', 'CONTROLS'),
                 ('BUTTON ICONS', dict(PROMPTS).get(v.get('prompts'), 'Automatic').upper(), 'CONTROLS'),
                 ('FRAME COUNTER', 'ON' if v['show_fps'] else 'OFF', 'DISPLAY'))
        x0 = PANEL[0] + 32
        half = (PANEL[2] - PANEL[0] - 64) // 2
        for i, (label, value, page) in enumerate(lines):
            x = x0 + (i % 2) * half
            y = self.row_y - 8 + (i // 2) * 22
            tag = f'profile_{i}'
            cv.create_text(x, y, text=label, anchor='w', fill=C['dim'], font=self.f(8), tags=(tag,))
            item = cv.create_text(x + 130, y, text=value, anchor='w', fill=C['text'], font=self.f(9, 'bold'),
                                  tags=(tag,))
            self.clickable(tag, lambda e, p=page: self.switch_tab(p), ((item, 'fill', C['text'], C['red_hot']),))
        self.row_y += (len(lines) + 1) // 2 * 22

    def tab_graphics(self):
        self.section('IMAGE  ·  BOTH GAMES', first=True)
        self.toggle('FSR 1 upscaling', 'AMD FidelityFX Super Resolution 1: scales the game image to the window '
                    'with edge-aware filtering. Only active when the window is larger than the image.', 'fsr1')
        self.toggle('Sharpening', 'Contrast-adaptive sharpening (RCAS) of the final image.', 'rcas')
        self.slider('Sharpness', 'Strength of the sharpening.', 'rcas_strength', 0, 100, locked=not self.values['rcas'])
        self.stepper('Texture filtering', 'Anisotropic filtering: sharper ground and walls at steep angles.',
                     'anisotropy', [(0, 'Game default'), (2, '2× AF'), (4, '4× AF'), (8, '8× AF'), (16, '16× AF')])
        self.row_y += 20
        self.note('Render resolution and frame rate are on the GAME page: each game has its own.', C['dim'])

    def tab_display(self):
        self.section('WINDOW', first=True)
        self.stepper('Display mode', 'Borderless fills the screen and switches instantly with Alt+Tab.',
                     'display_mode', [('windowed', 'Windowed'), ('borderless', 'Borderless'), ('fullscreen', 'Fullscreen')])
        self.toggle('V-Sync', 'Waits for the display refresh: no tearing.', 'vsync')
        self.toggle('Frame counter', 'Shows the frame rate in a corner of the screen.', 'show_fps')
        self.section('IN GAME')
        self.note('Insert (keyboard) or R3 + L2 (controller) opens the port menu while playing: '
                  'FSR 3.1 / FSR 4 upscaling and the other graphics options.')

    # ---- controls
    def layout(self):
        import proto_controls
        return proto_controls.layout(self.values.get('controls'))

    def tab_controls(self):
        import proto_controls
        cv = self.canvas
        x0, _, x1, _ = PANEL
        self.section('KEYBOARD AND MOUSE', first=True)
        self.stepper('Button icons in game', 'The buttons the game shows in its hints and tutorials: your keys '
                     'and mouse buttons (below), or the PlayStation buttons. Automatic: the ones of what you '
                     'played with last time. Takes effect at the next start.', 'prompts', PROMPTS)
        self.toggle('Mouse look', 'The mouse turns the camera as in PC games: by how far you move it, with '
                    'no dead zone or speed limit (F7 switches it in game).', 'mouse_look')
        self.slider('Mouse sensitivity', 'How fast the mouse turns the camera (also in the in-game menu).',
                    'mouse_sensitivity', 10, 400, locked=not self.values['mouse_look'])
        groups = [(g, en) for g, en, _ in proto_controls.GROUPS]
        self.stepper('Keys for', 'Click a key box, then press the key or mouse button for it (click the box '
                     'again for the left button). Delete clears the box; a click anywhere else cancels. A key is '
                     'only on one action at a time.', 'controls_group', groups)
        full = self.layout()
        clashes = proto_controls.conflicts(full)
        group = self.values.get('controls_group', 'move')
        for action, _, label, _, _, action_group in proto_controls.ACTIONS:
            if action_group != group:
                continue
            y, tag = self.row(label, 'Click a box, then press a key or a mouse button (click the box again '
                              'for the left button). Delete clears it; a click anywhere else cancels.', height=34)
            keys = full.get(action, [])
            for slot in range(proto_controls.SLOTS):
                bx1 = x1 - 32 - (proto_controls.SLOTS - 1 - slot) * 106
                bx0 = bx1 - 98
                capturing = self.capture == (action, slot)
                if capturing:
                    self.capture_box = (bx0, y - 12, bx1, y + 12)
                key = keys[slot] if slot < len(keys) else ''
                text = 'PRESS A KEY_' if capturing else (self.key_text(key) if key else '—')
                color = (C['red_hot'] if capturing else C['warn'] if key and key.casefold() in clashes
                         else C['text'] if key else C['dim'])
                box_tag = f'key_{action}_{slot}'
                box = cv.create_rectangle(bx0, y - 12, bx1, y + 12, outline=C['red_hot'] if capturing else C['line'],
                                          fill=C['red_dark'] if capturing else C['bg'], tags=(box_tag,))
                cv.create_text((bx0 + bx1) / 2, y, text=text, fill=color, font=self.f(9, 'bold'), tags=(box_tag,))
                normal = C['red_hot'] if capturing else C['line']
                self.clickable(box_tag, lambda e, a=action, s=slot: self.capture_start(a, s),
                               ((box, 'outline', normal, C['red']),))
        self.row_y += 22
        bx = self.small_button(x0 + 32, self.row_y, 'RESET KEYS', self.reset_controls,
                               'Puts every key back to the port\'s layout.')
        if clashes:
            cv.create_text(bx + 14, self.row_y, text='Orange: a key on two different buttons', anchor='w',
                           fill=C['warn'], font=self.f(8))

    @staticmethod
    def key_text(key):
        short = {'Return': 'Enter', 'Escape': 'Esc', 'Mouse Left': 'Left click', 'Mouse Right': 'Right click',
                 'Mouse Middle': 'Middle click', 'Mouse X1': 'Mouse 4', 'Mouse X2': 'Mouse 5', 'Left Shift': 'L-Shift',
                 'Right Shift': 'R-Shift', 'Left Ctrl': 'L-Ctrl', 'Right Ctrl': 'R-Ctrl', 'Left Alt': 'L-Alt',
                 'Right Alt': 'R-Alt'}
        return short.get(key, key)

    def capture_start(self, action, slot):
        if self.capture == (action, slot):
            return
        self.capture_stop()
        self.capture = (action, slot)
        self.root.bind('<KeyPress>', self.capture_key)
        self.root.bind('<MouseWheel>', self.capture_wheel)
        self.root.bind('<Button-4>', lambda e: self.capture_set('Wheel Up'))   # X11 wheel
        self.root.bind('<Button-5>', lambda e: self.capture_set('Wheel Down'))
        # The click that started the capture must not bind the left button: listen from the next one.
        self.root.after(250, lambda: self.root.bind('<ButtonPress-1>', self.capture_click) if self.capture else None)
        self.root.bind('<ButtonPress-2>', lambda e: self.capture_set('Mouse Middle'))
        self.root.bind('<ButtonPress-3>', lambda e: self.capture_set('Mouse Right'))
        self.draw()

    def capture_stop(self):
        self.capture = None
        for sequence in ('<KeyPress>', '<MouseWheel>', '<Button-4>', '<Button-5>', '<ButtonPress-1>',
                         '<ButtonPress-2>', '<ButtonPress-3>'):
            self.root.unbind(sequence)

    def capture_click(self, event):
        """Left click: on the box being set it is the left mouse button, anywhere else a cancel."""
        box = getattr(self, 'capture_box', None)
        if box and event.widget is self.canvas and box[0] <= event.x <= box[2] and box[1] <= event.y <= box[3]:
            self.capture_set('Mouse Left')
        else:
            self.capture_stop()
            self.draw()

    def capture_wheel(self, event):
        self.capture_set('Wheel Up' if event.delta > 0 else 'Wheel Down')

    def capture_key(self, event):
        if event.keysym == 'Delete':
            self.capture_set('')
            return 'break'
        key = sdl_key_name(event)
        if key:
            self.capture_set(key)
        return 'break'

    def capture_set(self, key):
        import proto_controls
        if not self.capture:
            return
        action, slot = self.capture
        self.capture_stop()
        full = self.layout()
        if key:  # a key is on one action only: taken from wherever it was
            for other, keys in full.items():
                full[other] = [k for k in keys if k.casefold() != key.casefold()]
        keys = list(full.get(action, []))
        while len(keys) <= slot:
            keys.append('')
        keys[slot] = key
        full[action] = [k for k in keys if k]
        self.values['controls'] = proto_controls.changes(full)
        self.save()
        self.draw()

    def reset_controls(self):
        self.capture_stop()
        self.values['controls'] = {}
        self.save()
        self.draw()

    def tab_folder(self):
        cv = self.canvas
        x0, _, x1, _ = PANEL
        self.section('GAME FOLDER', first=True)
        folder = self.values['game_dir'] or 'Not selected'
        if len(folder) > 62:
            folder = '…' + folder[-61:]
        y = self.row_y - 4
        cv.create_rectangle(x0 + 28, y - 14, x1 - 120, y + 14, outline=C['line'], fill=C['bg'])
        cv.create_text(x0 + 40, y, text=folder, anchor='w', fill=C['text'], font=self.f(9))
        self.small_button(x1 - 112, y, 'CHANGE', self.choose_folder, 'Choose the folder of your game dump '
                          '(the one with eboot.bin, sce_sys, sce_module, p1 and p2).')
        self.row_y += 34
        for label, state, detail in self.checks:
            cv.create_text(x0 + 32, self.row_y - 7, text=STATE_TAGS[state], anchor='w', fill=C[state],
                           font=self.f(9, 'bold'))
            cv.create_text(x0 + 100, self.row_y - 7, text=label, anchor='w', fill=C['text'], font=self.f(9))
            cv.create_text(x1 - 32, self.row_y - 7, text=detail, anchor='e', fill=C['muted'], font=self.f(9))
            self.row_y += 22
        self.row_y += 18
        bx = x0 + 32
        bx = self.small_button(bx, self.row_y, 'PREPARE', self.prepare,
                               'Converts the game into the native image now (also done at every start).') + 10
        if self.values['game_dir'] and restorable(self.values['game_dir']):
            bx = self.small_button(bx, self.row_y, 'RESTORE ORIGINAL', self.restore,
                                   'Puts the original executables back from the .orig backups.') + 10
        self.small_button(bx, self.row_y, 'CLEAN UP', self.clean,
                          'Deletes files earlier fixes and debugging left in the game folder. The game files '
                          'themselves are kept.')
        self.row_y += 30
        self.section('SAVES')
        bx = self.small_button(x0 + 32, self.row_y, 'OPEN SAVE FOLDER', self.open_saves,
                               'Your saves: user\\savedata in the port folder.') + 10
        self.small_button(bx, self.row_y, 'OPEN BACKUPS', self.open_backups,
                          'A copy of your saves is made at every start (the latest ones are kept).')
        self.row_y += 30

    # ---- animation
    def tick_fast(self):
        """25 times a second: the feed's scan line, the busy bar."""
        cv = self.canvas
        fx0, fy0, fx1, fy1 = FEED
        self.scan = (self.scan + 3) % (fy1 - fy0 + 90)
        y = fy0 + self.scan
        if y < fy1:
            cv.coords('scan', fx0, y, fx1, y)
            cv.itemconfigure('scan', state='normal')
        else:
            cv.itemconfigure('scan', state='hidden')
        if self.busy:
            self.busy_x = (self.busy_x + 9) % (FEED[2] - FEED[0] - 90)
            for item in cv.find_withtag('busybar'):
                c = cv.coords(item)
                cv.coords(item, FEED[0] + self.busy_x, c[1], FEED[0] + self.busy_x + 90, c[3])
        self.root.after(40, self.tick_fast)

    def tick_slow(self):
        """Twice a second: the clocks, the blinking cursor and REC light, the odd glitch."""
        cv = self.canvas
        self.blink = not self.blink
        state = 'normal' if self.blink else 'hidden'
        for tag in ('cursor', 'rec'):
            cv.itemconfigure(tag, state=state)
        cv.itemconfigure('beacon', fill=C['red'] if self.blink else C['red_dark'])
        cv.itemconfigure('clock', text=self.clock())
        cv.itemconfigure('feedtime', text=time.strftime('%H:%M:%S'))
        cv.itemconfigure('elapsed', text=self.elapsed())
        if time.time() >= self.next_glitch:
            self.glitch()
            self.next_glitch = time.time() + self.random.uniform(3.5, 9.0)
        self.root.after(500, self.tick_slow)

    def glitch(self, again=True):
        """A moment of a broken signal: the feed tears, the title splits into two colours."""
        cv = self.canvas
        frames = self.photos('feed')
        if len(frames) > 1:
            cv.itemconfigure('feed', image=self.random.choice(frames[1:]))
        shift = self.random.choice((3, 4, 5))
        for item in cv.find_withtag('title'):
            x, y = cv.coords(item)
            for dx, color in ((-shift, C['red_hot']), (shift, C['ghost'])):
                copy = cv.create_text(x + dx, y, text=cv.itemcget(item, 'text'), anchor=cv.itemcget(item, 'anchor'),
                                      font=cv.itemcget(item, 'font'), fill=color, tags=('glitchfx',))
                cv.tag_lower(copy, item)

        def restore():
            if frames:
                cv.itemconfigure('feed', image=frames[0])
            cv.delete('glitchfx')
            if again and self.random.random() < 0.6:
                self.root.after(self.random.randint(60, 160), lambda: self.glitch(again=False))
        self.root.after(self.random.randint(70, 130), restore)

    # ---- actions
    def choose_folder(self):
        from tkinter import filedialog
        if self.busy:
            return
        start = self.values['game_dir'] or str(Path.home())
        folder = filedialog.askdirectory(parent=self.root, initialdir=start,
                                         title='PROTOTYPE BIOHAZARD BUNDLE folder (eboot.bin, sce_sys, p1, p2)')
        if not folder:
            return
        self.values['game_dir'] = str(Path(folder))
        self.save()
        self.refresh_game()

    def open_path(self, path):
        try:
            if WINDOWS:
                os.startfile(str(path))
            else:
                subprocess.Popen(['xdg-open', str(path)])
        except OSError as error:
            self.set_status(f'Cannot open {path}: {error}', C['bad'])

    def save_folder(self):
        root = PORT_DIR / 'user' / 'savedata'
        candidates = sorted(root.glob(f'*/{TITLE_ID}')) if root.is_dir() else []
        return candidates[0] if candidates else root

    def open_saves(self):
        folder = self.save_folder()
        if folder.is_dir():
            self.open_path(folder)
        else:
            self.set_status('No saves yet: they appear after the first save in game', C['muted'])

    def open_backups(self):
        folder = PORT_DIR / 'user' / 'savedata_backups'
        if folder.is_dir():
            self.open_path(folder)
        else:
            self.set_status('No save backups yet', C['muted'])

    def open_log(self):
        if LOG_FILE.is_file():
            self.open_path(LOG_FILE)
        else:
            self.set_status('No log yet: start the game once', C['muted'])

    def background(self, label, work, done):
        """Runs work() in a thread; done(result) back in the window."""
        self.busy = label
        self.draw()

        def runner():
            try:
                result = work()
            except Exception as error:
                result = error
            self.root.after(0, lambda: self.finish(done, result))
        threading.Thread(target=runner, daemon=True).start()

    def finish(self, done, result):
        self.busy = None
        done(result)
        self.draw()

    def prepare(self):
        if not self.ready:
            self.set_status('Fix the game folder first', C['bad'])
            return
        game = self.values['game_dir']
        exe = GAMES[self.game][1]
        out = PORT_DIR / 'out' if exe == 'eboot.bin' else PORT_DIR / 'out' / Path(exe).stem  # as run.py

        def work():
            out.mkdir(parents=True, exist_ok=True)
            env = game_environment(self.values)
            for name in ('prepare.py', 'link_libc.py', 'link_modules.py'):
                r = subprocess.run(script_command(name, game, '--out', out), cwd=PORT_DIR, env=env,
                                   stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   text=True, encoding='utf-8', errors='replace', creationflags=NO_WINDOW)
                if r.returncode:
                    return f'{name} failed: ' + (r.stdout.strip().splitlines() or ['?'])[-1]
            return None

        def done(error):
            if error:
                self.set_status(str(error), C['bad'])
            else:
                self.set_status('Game prepared: native image ready', C['ok'])
        self.background('Preparing', work, done)

    def restore(self):
        from tkinter import messagebox
        items = restorable(self.values['game_dir'])
        if not items:
            return
        names = '\n'.join(p.name for p, _ in items)
        if not messagebox.askyesno('Restore original files', f'Replace these with their original backups?\n\n{names}',
                                   parent=self.root):
            return
        try:
            for path, backup in items:
                shutil.copy2(backup, path)
            self.set_status('Original executables restored', C['ok'])
        except OSError as error:
            self.set_status(f'Restore failed: {error}', C['bad'])
        self.refresh_game()

    def clean(self):
        from tkinter import messagebox
        files = leftovers(self.values['game_dir']) if self.values['game_dir'] else []
        if not files:
            self.set_status('Nothing to clean: the game folder holds only the game', C['ok'])
            return
        size = sum(p.stat().st_size for p in files) / 1e6
        names = '\n'.join(p.name for p in files[:14]) + ('\n…' if len(files) > 14 else '')
        if not messagebox.askyesno('Clean up the game folder',
                                   f'Delete {len(files)} leftover files ({size:.0f} MB)?\n\n{names}\n\n'
                                   'The game files themselves are kept.', parent=self.root):
            return
        failed = 0
        for p in files:
            try:
                p.unlink()
            except OSError:
                failed += 1
        self.set_status(f'Removed {len(files) - failed} files' + (f', {failed} failed' if failed else ''),
                        C['ok'] if not failed else C['warn'])
        self.refresh_game()

    def choose_game(self, key):
        if self.process is not None and self.process.poll() is None or self.busy or key == self.game:
            return
        self.capture_stop()
        self.values['game'] = key
        self.save()
        self.refresh_game(redraw=False)
        self.draw()

    def play(self):
        if self.process is not None and self.process.poll() is None:
            return
        if not self.ready:
            self.set_status('Fix the game folder first (5 FOLDER)', C['bad'])
            return
        write_ini(self.values)
        env = game_environment(self.values)
        try:
            log = open(LOG_FILE, 'w', encoding='utf-8', errors='replace')
            flags = NO_WINDOW | getattr(subprocess, 'CREATE_NEW_PROCESS_GROUP', 0)
            self.process = subprocess.Popen(run_command(), cwd=PORT_DIR, env=env, stdin=subprocess.DEVNULL,
                                            stdout=log, stderr=subprocess.STDOUT, creationflags=flags)
            log.close()
            self.started = time.time()
            self.set_status('Starting… the game window opens in a moment', C['text'])
        except OSError as error:
            self.process = None
            self.set_status(f'Could not start: {error}', C['bad'])
        self.draw()

    def stop(self):
        if self.process is None or self.process.poll() is not None:
            return
        if WINDOWS:
            subprocess.run(['taskkill', '/T', '/F', '/PID', str(self.process.pid)], creationflags=NO_WINDOW,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        else:
            self.process.terminate()

    def poll(self):
        if self.process is not None and self.process.poll() is not None:
            code = self.process.returncode
            self.process = None
            if code == 0:
                self.set_status('Game closed', C['muted'])
            elif code == 42:
                self.set_status('The game asked for an executable that is not in the game folder', C['warn'])
            else:
                self.set_status(f'The game stopped (code {code}). [ LOG ] shows why.', C['bad'])
            self.draw()
        self.root.after(500, self.poll)

    def close(self):
        self.save()
        self.root.destroy()


def main():
    if len(sys.argv) > 1 and sys.argv[1] in ('--run', '--script'):
        sys.exit(run_role(sys.argv[1:]))
    if WINDOWS:
        try:
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except (AttributeError, OSError):
            pass
    import tkinter as tk
    root = tk.Tk()
    if WINDOWS or os.environ.get('PROTO_LAUNCHER_SCREENSHOT'):
        root.tk.call('tk', 'scaling', 1.333)
    Launcher(root)
    if os.environ.get('PROTO_LAUNCHER_SCREENSHOT'):  # closes by itself: seconds (1.5 for 1)
        seconds = float(os.environ['PROTO_LAUNCHER_SCREENSHOT'])
        root.after(int((seconds if seconds > 1 else 1.5) * 1000), root.destroy)
    root.mainloop()


if __name__ == '__main__':
    main()
