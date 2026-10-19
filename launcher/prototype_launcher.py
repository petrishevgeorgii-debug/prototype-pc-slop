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
VERSION = '0.5.2'
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
    'game_dir': '', 'render_res': '1920x1080', 'fps60': True, 'anisotropy': 16, 'fsr1': True,
    'rcas': True, 'rcas_strength': 60, 'display_mode': 'borderless', 'vsync': True,
    'show_fps': False, 'language': 1, 'tendril_fix': True, 'tab': 0,
    'controls': {}, 'prompts': 'auto', 'controls_group': 'move', 'mouse_look': True, 'mouse_sensitivity': 100,
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
    if not isinstance(values.get('controls'), dict):
        values['controls'] = {}
    if values['render_res'] not in RESOLUTIONS:
        values['render_res'] = '1920x1080'
    # 0.2.1: the render resolution patch changed; every setup starts again from the PS4's size.
    if values.get('settings_version', 0) < 2:
        values['render_res'] = '1920x1080'
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
    env.update({
        'THEORDER_GAME_DIR': values['game_dir'],
        'THEORDER_FPS60': '1' if values['fps60'] else '0',
        'THEORDER_PROTO_RES': values['render_res'],
        'THEORDER_PROTO_TENDRIL_FIX': '1' if values['tendril_fix'] else '0',
        'THEORDER_ANISOTROPY': str(values['anisotropy']),
        'THEORDER_PROTO_CONTROLS': json.dumps(values.get('controls') or {}),
        'THEORDER_PROMPTS': values.get('prompts', 'auto'),
        'PYTHONIOENCODING': 'utf-8',
    })
    return env


# --------------------------------------------------------------------------------------------
# The game folder.

def file_sha256(path):
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


def check_game(folder):
    """[(label, state, detail)] with state 'ok', 'warn' or 'bad'; and the parsed param.sfo."""
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
            checks.append((label, 'bad' if name == 'eboot.bin' else 'warn', 'missing'))
            continue
        try:
            same = file_sha256(path) == ORIGINAL_SHA256[name]
        except OSError:
            same = False
        checks.append((label, 'ok' if same else 'warn', 'original' if same else 'modified (Restore fixes it)'))
    modules = all((folder / 'sce_module' / m).is_file() for m in ('libc.prx', 'libSceFios2.prx'))
    checks.append(('System modules', 'ok' if modules else 'bad', 'libc, Fios2' if modules else 'sce_module missing'))
    data = (folder / 'p1').is_dir()
    checks.append(('Game data', 'ok' if data else 'bad', 'p1 found' if data else 'p1 folder missing'))
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
# Look.

C = {
    'bg': '#0c0d0f', 'panel': '#111215', 'line': '#232529', 'hair': '#1b1c20', 'text': '#e6e3df',
    'muted': '#8e8c89', 'dim': '#55555a', 'red': '#b0161d', 'red_hot': '#c91f27', 'red_dark': '#3b0d10',
    'track': '#2a2b30', 'ok': '#7db37c', 'warn': '#d2a04c', 'bad': '#d6564c',
}
W, H = 1200, 700
SIDE = 772                      # the art and the play area; the settings panel fills the rest
PANEL = (SIDE, 0, W, H)


def pick_fonts(root):
    from tkinter import font as tkfont
    families = set(tkfont.families(root))
    title = next((f for f in ('Bahnschrift SemiBold Condensed', 'Bahnschrift Condensed', 'Bahnschrift',
                              'Segoe UI Semibold', 'DejaVu Sans Condensed') if f in families), 'TkDefaultFont')
    ui = next((f for f in ('Bahnschrift', 'Segoe UI', 'DejaVu Sans Condensed', 'DejaVu Sans') if f in families),
              'TkDefaultFont')
    return title, ui


def find_art(game_dir):
    for name in ('pic0.png', 'pic1.png'):
        path = Path(game_dir) / 'sce_sys' / name if game_dir else None
        if path and path.is_file():
            return path
    return None


def render_background(game_dir):
    """The window background (PIL image), or None without Pillow: the game's own key art from
    the player's dump, muted to greys with its reds kept, darkened under the text."""
    try:
        from PIL import Image, ImageDraw, ImageEnhance
    except ImportError:
        return None
    canvas = Image.new('RGB', (W, H), C['bg'])
    art_path = find_art(game_dir)
    if art_path:
        try:
            art = Image.open(art_path).convert('RGB')
            # The key art's left half (Alex Mercer), filling the play area.
            art = art.crop((0, 0, art.width // 2, art.height))
            scale = SIDE / art.width
            art = art.resize((SIDE, int(art.height * scale)), Image.LANCZOS)
            top = max(0, min(art.height - H, int(art.height * 0.04)))
            art = art.crop((0, top, SIDE, top + H))
            # Greys everywhere but the reds (the virus), then darker overall.
            gray = Image.merge('RGB', [art.convert('L')] * 3)
            hsv = art.convert('HSV').split()
            red_mask = Image.eval(hsv[1], lambda v: 0 if v < 90 else min(255, (v - 90) * 3))
            hue_mask = Image.eval(hsv[0], lambda h: 255 if h < 14 or h > 238 else 0)
            from PIL import ImageChops
            mask = ImageChops.multiply(red_mask, hue_mask)
            art = Image.composite(ImageEnhance.Color(art).enhance(0.85), gray, mask)
            art = ImageEnhance.Brightness(art).enhance(0.42)
            art = ImageEnhance.Contrast(art).enhance(1.12)
            canvas.paste(art, (0, 0))
        except Exception as error:
            print(f'launcher: art not used: {error}')
    # Shade: the lower part (title, play) and the edge toward the panel go to the background.
    shade = Image.new('L', (SIDE, H), 0)
    px = shade.load()
    for y in range(H):
        bottom = max(0.0, (y - H * 0.38) / (H * 0.62)) ** 1.3
        for x in range(SIDE):
            right = max(0.0, (x - SIDE * 0.72) / (SIDE * 0.28)) ** 1.6
            px[x, y] = int(255 * min(1.0, bottom * 0.92 + right * 0.85))
    left = canvas.crop((0, 0, SIDE, H))
    left = Image.composite(Image.new('RGB', (SIDE, H), C['bg']), left, shade)
    canvas.paste(left, (0, 0))
    draw = ImageDraw.Draw(canvas)
    draw.rectangle((SIDE, 0, W, H), fill=C['panel'])
    draw.line((SIDE, 0, SIDE, H), fill=C['line'])
    return canvas


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
# Not bindable: the port's own keys (Insert opens its menu, F7 mouse look, F9 pad recording) and
# the characters THEORDER_KEY_MAP uses as separators.
RESERVED_KEYS = {'Insert', 'F7', 'F9', ',', '=', '+'}


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

class Launcher:
    def __init__(self, root):
        import tkinter as tk
        self.tk = tk
        self.root = root
        self.values = load_settings()
        self.process = None
        self.busy = None
        self.checks, self.sfo = [], {}
        root.title('PROTOTYPE')
        root.geometry(f'{W}x{H}')
        root.resizable(False, False)
        root.configure(bg=C['bg'])
        self.title_font, self.ui_font = pick_fonts(root)
        self.canvas = tk.Canvas(root, width=W, height=H, highlightthickness=0, bd=0, bg=C['bg'])
        self.canvas.pack()
        self.bg_image = None
        self.window_icon()
        self.refresh_game(redraw=False)
        self.draw()
        root.protocol('WM_DELETE_WINDOW', self.close)
        root.after(400, self.poll)
        if not self.values['game_dir']:
            root.after(300, self.choose_folder)

    # ---- helpers
    def f(self, size, weight='normal', family=None):
        return (family or self.ui_font, size, weight)

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
        if hasattr(self, 'status_item'):
            self.canvas.itemconfigure(self.status_item, text=text, fill=self.status_color)

    def refresh_game(self, redraw=True):
        self.checks, self.sfo = check_game(self.values['game_dir'])
        bad = [c for c in self.checks if c[1] == 'bad']
        if not self.values['game_dir']:
            self.set_status('Select your game folder to begin', C['warn'])
        elif bad:
            self.set_status(f'{bad[0][0]}: {bad[0][2]}', C['bad'])
        else:
            self.set_status('Ready to play', C['muted'])
        if redraw:
            self.bg_image = None
            self.window_icon()
            self.draw()

    @property
    def ready(self):
        return bool(self.checks) and not any(state == 'bad' for _, state, _ in self.checks)

    def hover(self, item, normal, hot):
        cv = self.canvas
        cv.tag_bind(item, '<Enter>', lambda e: (cv.itemconfigure(item, fill=hot), cv.configure(cursor='hand2')))
        cv.tag_bind(item, '<Leave>', lambda e: (cv.itemconfigure(item, fill=normal), cv.configure(cursor='')))

    # ---- drawing
    def draw(self):
        cv = self.canvas
        cv.delete('all')
        if self.bg_image is None:
            image = render_background(self.values['game_dir'])
            if image is not None:
                from PIL import ImageTk
                self.bg_image = ImageTk.PhotoImage(image)
        if self.bg_image is not None:
            cv.create_image(0, 0, image=self.bg_image, anchor='nw')
        else:
            cv.create_rectangle(SIDE, 0, W, H, fill=C['panel'], outline='')
            cv.create_line(SIDE, 0, SIDE, H, fill=C['line'])
        self.draw_left()
        self.draw_panel()

    def draw_left(self):
        cv = self.canvas
        x = 56
        kicker = cv.create_text(x, 42, text='PROTOTYPE BIOHAZARD BUNDLE', anchor='w', fill=C['muted'],
                                font=self.f(9, 'bold'))
        cv.create_text(cv.bbox(kicker)[2] + 18, 42, text='NATIVE PC PORT', anchor='w', fill=C['dim'], font=self.f(9))
        # Game selection.
        gy = 392
        active = cv.create_text(x, gy, text='PROTOTYPE', anchor='w', fill=C['text'], font=self.f(10, 'bold'))
        bbox = cv.bbox(active)
        cv.create_rectangle(bbox[0], gy + 13, bbox[2], gy + 15, fill=C['red'], outline='')
        other = cv.create_text(bbox[2] + 26, gy, text='PROTOTYPE 2', anchor='w', fill=C['dim'], font=self.f(10, 'bold'))
        cv.create_text(cv.bbox(other)[2] + 8, gy, text='SOON', anchor='w', fill=C['dim'], font=self.f(7, 'bold'))
        # Title.
        cv.create_text(x - 2, 452, text='PROTOTYPE', anchor='w', fill=C['text'], font=(self.title_font, 46, 'bold'))
        version = self.sfo.get('APP_VER')
        sub = 'Biohazard Bundle' + (f'   ·   {TITLE_ID}   ·   v{version}' if version else '')
        cv.create_text(x, 496, text=sub, anchor='w', fill=C['muted'], font=self.f(10))
        # Play.
        by = 534
        running = self.process is not None and self.process.poll() is None
        if running:
            self.button(x, by, 220, 50, 'RUNNING', None, fill=C['track'], hot=C['track'])
            stop = cv.create_text(x + 240, by + 25, text='STOP', anchor='w', fill=C['muted'], font=self.f(9, 'bold'))
            self.hover(stop, C['muted'], C['text'])
            cv.tag_bind(stop, '<Button-1>', lambda e: self.stop())
            sx = x + 290
        elif self.busy:
            self.button(x, by, 220, 50, self.busy.upper() + '…', None, fill=C['red_dark'], hot=C['red_dark'])
            sx = x + 240
        else:
            enabled = self.ready
            self.button(x, by, 220, 50, 'PLAY', self.play if enabled else None,
                        fill=C['red'] if enabled else C['red_dark'], hot=C['red_hot'])
            sx = x + 240
        self.status_item = cv.create_text(sx, by + 25, text=getattr(self, 'status_text', ''), anchor='w',
                                          fill=getattr(self, 'status_color', C['muted']), font=self.f(10),
                                          width=SIDE - sx - 40)
        # Footer links.
        fy = H - 40
        cv.create_line(x, fy - 22, SIDE - 56, fy - 22, fill=C['hair'])
        lx = x
        for text, command in (('Game folder', self.choose_folder), ('Saves', self.open_saves),
                              ('Save backups', self.open_backups), ('Log', self.open_log),
                              ('Port folder', lambda: self.open_path(PORT_DIR))):
            item = cv.create_text(lx, fy, text=text, anchor='w', fill=C['muted'], font=self.f(9))
            self.hover(item, C['muted'], C['text'])
            cv.tag_bind(item, '<Button-1>', lambda e, c=command: c())
            lx = cv.bbox(item)[2] + 24
        cv.create_text(SIDE - 56, fy, text=f'v{VERSION}', anchor='e', fill=C['dim'], font=self.f(8))

    def button(self, x, y, w, h, text, command, fill, hot):
        cv = self.canvas
        tag = f'btn{x}_{y}'
        rect = cv.create_rectangle(x, y, x + w, y + h, fill=fill, outline='', tags=(tag,))
        cv.create_text(x + w / 2, y + h / 2, text=text, fill=C['text'] if command else C['muted'],
                       font=self.f(12, 'bold'), tags=(tag,))
        if command:
            cv.tag_bind(tag, '<Enter>', lambda e: (cv.itemconfigure(rect, fill=hot), cv.configure(cursor='hand2')))
            cv.tag_bind(tag, '<Leave>', lambda e: (cv.itemconfigure(rect, fill=fill), cv.configure(cursor='')))
            cv.tag_bind(tag, '<Button-1>', lambda e: command())

    def small_button(self, x, y, text, command, desc=''):
        """An outlined button; returns its right edge."""
        cv = self.canvas
        label = cv.create_text(x + 12, y, text=text, anchor='w', fill=C['text'], font=self.f(9, 'bold'))
        b = cv.bbox(label)
        rect = cv.create_rectangle(x, y - 13, b[2] + 12, y + 13, outline=C['line'], fill='')
        cv.tag_raise(label)
        for item in (rect, label):
            cv.tag_bind(item, '<Enter>', lambda e: (cv.itemconfigure(rect, outline=C['muted']), self.describe(desc),
                                                    cv.configure(cursor='hand2')))
            cv.tag_bind(item, '<Leave>', lambda e: (cv.itemconfigure(rect, outline=C['line']), self.describe(''),
                                                    cv.configure(cursor='')))
            cv.tag_bind(item, '<Button-1>', lambda e: command())
        return b[2] + 12

    # ---- settings panel
    TABS = ('GRAPHICS', 'DISPLAY', 'CONTROLS', 'GAME')

    def draw_panel(self):
        cv = self.canvas
        x0, y0, x1, y1 = PANEL
        tx = x0 + 32
        for i, name in enumerate(self.TABS):
            active = self.values['tab'] == i
            item = cv.create_text(tx, 40, text=name, anchor='w', fill=C['text'] if active else C['muted'],
                                  font=self.f(10, 'bold'))
            bbox = cv.bbox(item)
            if active:
                cv.create_rectangle(bbox[0], 63, bbox[2], 65, fill=C['red'], outline='')
            else:
                self.hover(item, C['muted'], C['text'])
                cv.tag_bind(item, '<Button-1>', lambda e, i=i: self.switch_tab(i))
            tx += 96
        cv.create_line(x0, 65, x1, 65, fill=C['line'])
        self.row_y = 108
        # Description of the row under the pointer.
        cv.create_line(x0 + 32, y1 - 92, x1 - 32, y1 - 92, fill=C['hair'])
        self.desc_item = cv.create_text(x0 + 32, y1 - 76, text='', anchor='nw', fill=C['muted'],
                                        font=self.f(9), width=x1 - x0 - 64)
        tabs = [self.tab_graphics, self.tab_display, self.tab_controls, self.tab_game]
        tabs[min(self.values['tab'], len(tabs) - 1)]()

    def switch_tab(self, i):
        self.capture_stop()
        self.values['tab'] = i
        self.save()
        self.draw()

    def describe(self, text):
        self.canvas.itemconfigure(self.desc_item, text=text or '')

    def row(self, label, desc, height=44, locked=False):
        cv = self.canvas
        x0, _, x1, _ = PANEL
        y = self.row_y
        tag = f'row{y}'
        cv.create_rectangle(x0 + 1, y - height / 2, x1, y + height / 2, fill='', outline='', tags=(tag,))
        cv.create_text(x0 + 32, y, text=label, anchor='w', fill=C['dim'] if locked else C['text'],
                       font=self.f(10), tags=(tag,))
        cv.create_line(x0 + 32, y + height / 2, x1 - 32, y + height / 2, fill=C['hair'])
        cv.tag_bind(tag, '<Enter>', lambda e: self.describe(desc))
        cv.tag_bind(tag, '<Leave>', lambda e: self.describe(''))
        self.row_y += height
        return y, tag

    def stepper(self, label, desc, key, options, locked=False):
        """‹ value › over a list of (value, text)."""
        cv = self.canvas
        y, tag = self.row(label, desc, locked=locked)
        x1 = PANEL[2] - 32
        width = 176
        current = self.values.get(key)
        index = next((i for i, (v, _) in enumerate(options) if v == current), 0)
        cv.create_text(x1 - width / 2, y, text=options[index][1], fill=C['dim'] if locked else C['text'],
                       font=self.f(10, 'bold'), tags=(tag,))
        for direction, ax in ((-1, x1 - width + 6), (1, x1 - 6)):
            arrow = cv.create_text(ax, y - 1, text='‹' if direction < 0 else '›', fill=C['dim'] if locked else C['muted'],
                                   font=self.f(15), tags=(tag,))
            if locked:
                continue

            def step(e, d=direction):
                self.values[key] = options[(index + d) % len(options)][0]
                self.save()
                self.draw()
                self.describe(desc)
            cv.tag_bind(arrow, '<Button-1>', step)
            self.hover(arrow, C['muted'], C['text'])

    def toggle(self, label, desc, key, locked=False):
        cv = self.canvas
        y, tag = self.row(label, desc, locked=locked)
        x1 = PANEL[2] - 32
        on = bool(self.values.get(key)) and not locked
        w, h = 34, 16
        x = x1 - w
        sw = f'sw{y}'
        cv.create_rectangle(x, y - h / 2, x + w, y + h / 2, fill=C['red'] if on else C['track'], outline='',
                            tags=(tag, sw))
        kx = x + w - h + 2 if on else x + 2
        cv.create_rectangle(kx, y - h / 2 + 2, kx + h - 4, y + h / 2 - 2, fill=C['text'] if not locked else C['dim'],
                            outline='', tags=(tag, sw))
        cv.create_text(x - 12, y, text='On' if on else 'Off', anchor='e', fill=C['muted'], font=self.f(9), tags=(tag,))
        if not locked:
            def flip(e):
                self.values[key] = not self.values.get(key)
                self.save()
                self.draw()
                self.describe(desc)
            cv.tag_bind(sw, '<Button-1>', flip)
            cv.tag_bind(sw, '<Enter>', lambda e: cv.configure(cursor='hand2'))
            cv.tag_bind(sw, '<Leave>', lambda e: cv.configure(cursor=''))

    def slider(self, label, desc, key, low, high, locked=False):
        cv = self.canvas
        y, tag = self.row(label, desc, locked=locked)
        x1 = PANEL[2] - 32
        width = 176
        x = x1 - width
        span = width - 44
        value = int(self.values.get(key, low))
        cv.create_line(x, y, x + span, y, fill=C['track'], width=2, tags=(tag,))
        kx = x + (value - low) / (high - low) * span
        bar = cv.create_line(x, y, kx, y, fill=C['dim'] if locked else C['red'], width=2, tags=(tag,))
        knob = cv.create_rectangle(kx - 4, y - 7, kx + 4, y + 7, fill=C['text'] if not locked else C['dim'],
                                   outline='', tags=(tag,))
        num = cv.create_text(x1, y, text=f'{value}', anchor='e', fill=C['muted'], font=self.f(9), tags=(tag,))
        if locked:
            return
        hit = cv.create_rectangle(x - 6, y - 10, x + span + 6, y + 10, fill='', outline='', tags=(tag,))

        def move(e):
            frac = min(1.0, max(0.0, (e.x - x) / span))
            v = int(round(low + frac * (high - low)))
            self.values[key] = v
            nx = x + frac * span
            cv.coords(bar, x, y, nx, y)
            cv.coords(knob, nx - 4, y - 7, nx + 4, y + 7)
            cv.itemconfigure(num, text=f'{v}')

        for item in (hit, bar, knob):
            cv.tag_bind(item, '<Button-1>', move)
            cv.tag_bind(item, '<B1-Motion>', move)
            cv.tag_bind(item, '<ButtonRelease-1>', lambda e: self.save())

    def note(self, text, color=None, height=30):
        self.canvas.create_text(PANEL[0] + 32, self.row_y - 16, text=text, anchor='nw', fill=color or C['muted'],
                                font=self.f(9), width=PANEL[2] - PANEL[0] - 64)
        self.row_y += height

    def section(self, text, first=False):
        if not first:
            self.row_y += 14
        self.canvas.create_text(PANEL[0] + 32, self.row_y - 8, text=text, anchor='w', fill=C['dim'],
                                font=self.f(8, 'bold'))
        self.row_y += 24

    def tab_graphics(self):
        res = [(r, r.replace('x', ' × ')) for r in RESOLUTIONS]
        self.section('IMAGE', first=True)
        self.stepper('Render resolution', 'Experimental. The resolution the game renders at; 1920 × 1080 is '
                     'the PS4\'s. Higher supersamples (sharper, heavier); lower is scaled up by FSR 1. If the '
                     'game misbehaves, set it back to 1920 × 1080.', 'render_res', res)
        self.toggle('FSR 1 upscaling', 'AMD FidelityFX Super Resolution 1: scales the game image to the window '
                    'with edge-aware filtering. Only active when the window is larger than the image.', 'fsr1')
        self.toggle('Sharpening', 'Contrast-adaptive sharpening (RCAS) of the final image.', 'rcas')
        self.slider('Sharpness', 'Strength of the sharpening.', 'rcas_strength', 0, 100, locked=not self.values['rcas'])
        self.stepper('Texture filtering', 'Anisotropic filtering: sharper ground and walls at steep angles.',
                     'anisotropy', [(0, 'Game default'), (2, '2× AF'), (4, '4× AF'), (8, '8× AF'), (16, '16× AF')])
        self.stepper('Frame rate', '60 FPS: gameplay at 60; videos and menus keep their normal speed.', 'fps60',
                     [(False, '30 FPS'), (True, '60 FPS')])

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
        self.toggle('Mouse look', 'The mouse turns the camera (F7 switches it in game).', 'mouse_look')
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
                bx1 = x1 - 32 - (proto_controls.SLOTS - 1 - slot) * 96
                bx0 = bx1 - 88
                capturing = getattr(self, 'capture', None) == (action, slot)
                if capturing:
                    self.capture_box = (bx0, y - 12, bx1, y + 12)
                key = keys[slot] if slot < len(keys) else ''
                text = 'Press a key…' if capturing else (self.key_text(key) if key else '—')
                color = C['warn'] if key and key.casefold() in clashes else (C['text'] if key else C['dim'])
                box = cv.create_rectangle(bx0, y - 12, bx1, y + 12, outline=C['red'] if capturing else C['line'],
                                          fill=C['track'] if capturing else '', tags=(tag,))
                item = cv.create_text((bx0 + bx1) / 2, y, text=text, fill=color, font=self.f(9, 'bold'),
                                      tags=(tag,))
                for part in (box, item):
                    cv.tag_bind(part, '<Button-1>', lambda e, a=action, s=slot: self.capture_start(a, s))
                    cv.tag_bind(part, '<Enter>', lambda e, b=box: (cv.itemconfigure(b, outline=C['muted']),
                                                                   cv.configure(cursor='hand2')))
                    cv.tag_bind(part, '<Leave>', lambda e, b=box, c=capturing: (
                        cv.itemconfigure(b, outline=C['red'] if c else C['line']), cv.configure(cursor='')))
        self.row_y += 22
        bx = self.small_button(x0 + 32, self.row_y, 'RESET KEYS', self.reset_controls,
                               'Puts every key back to the port\'s layout.')
        if clashes:
            cv.create_text(bx + 14, self.row_y, text='Orange: a key on two different buttons', anchor='w',
                           fill=C['warn'], font=self.f(8))

    @staticmethod
    def key_text(key):
        short = {'Return': 'Enter', 'Escape': 'Esc', 'Mouse Left': 'Left click', 'Mouse Right': 'Right click', 'Mouse Middle': 'Middle click',
                 'Mouse X1': 'Mouse 4', 'Mouse X2': 'Mouse 5', 'Left Shift': 'L-Shift', 'Right Shift': 'R-Shift',
                 'Left Ctrl': 'L-Ctrl', 'Right Ctrl': 'R-Ctrl', 'Left Alt': 'L-Alt', 'Right Alt': 'R-Alt'}
        return short.get(key, key)

    def capture_start(self, action, slot):
        if getattr(self, 'capture', None) == (action, slot):
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
        if not getattr(self, 'capture', None):
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

    def tab_game(self):
        cv = self.canvas
        x0, _, x1, _ = PANEL
        self.section('GAME FOLDER', first=True)
        folder = self.values['game_dir'] or 'Not selected'
        if len(folder) > 44:
            folder = '…' + folder[-43:]
        cv.create_text(x0 + 32, self.row_y - 6, text=folder, anchor='w', fill=C['text'], font=self.f(10))
        change = cv.create_text(x1 - 32, self.row_y - 6, text='Change', anchor='e', fill=C['muted'], font=self.f(9, 'bold'))
        self.hover(change, C['muted'], C['text'])
        cv.tag_bind(change, '<Button-1>', lambda e: self.choose_folder())
        self.row_y += 26
        for label, state, detail in self.checks:
            cv.create_rectangle(x0 + 33, self.row_y - 9, x0 + 37, self.row_y - 5, fill=C[state], outline='')
            cv.create_text(x0 + 48, self.row_y - 7, text=label, anchor='w', fill=C['text'], font=self.f(9))
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
                          'Deletes files earlier fixes and debugging left in the game folder.')
        self.row_y += 30
        self.section('SAVES')
        bx = self.small_button(x0 + 32, self.row_y, 'OPEN SAVE FOLDER', self.open_saves,
                               'Your saves: user\\savedata in the port folder.') + 10
        self.small_button(bx, self.row_y, 'OPEN BACKUPS', self.open_backups,
                          'A copy of your saves is made at every start (the latest ones are kept).')
        self.row_y += 30
        self.section('FIXES')
        self.toggle('White tendrils fix', 'Consume tendrils keep their red veins (a GPU emulation difference). '
                    'Keep it on.', 'tendril_fix')

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
        out = PORT_DIR / 'out'

        def work():
            out.mkdir(exist_ok=True)
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

    def play(self):
        if self.process is not None and self.process.poll() is None:
            return
        if not self.ready:
            self.set_status('Fix the game folder first (GAME tab)', C['bad'])
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
                self.set_status('PROTOTYPE 2 was chosen in the bundle menu: not supported yet', C['warn'])
            else:
                self.set_status(f'The game stopped (code {code}). OPEN LOG shows why.', C['bad'])
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
    if WINDOWS:
        root.tk.call('tk', 'scaling', 1.333)
    Launcher(root)
    if os.environ.get('PROTO_LAUNCHER_SCREENSHOT'):
        root.after(1500, root.destroy)
    root.mainloop()


if __name__ == '__main__':
    main()
