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
import random

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
if sys.platform == 'win32':
    import _winapi  # noqa: F401
    import winreg  # noqa: F401

WINDOWS = sys.platform == 'win32'
FROZEN = getattr(sys, 'frozen', False)
PORT_DIR = Path(sys.executable).resolve().parent if FROZEN else Path(__file__).resolve().parent.parent
sys.path.insert(0, str(PORT_DIR / 'scripts'))

APP_NAME = 'PROTOTYPE'
VERSION = '0.2.0'
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
}


def load_settings():
    values = dict(DEFAULTS)
    try:
        values.update(json.loads(CONFIG_FILE.read_text(encoding='utf-8')))
    except (OSError, ValueError):
        pass
    if values['render_res'] not in RESOLUTIONS:
        values['render_res'] = '1920x1080'
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
            'rcas_strength': int(values['rcas_strength']), 'upscaler': 'off',
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
        'THEORDER_LANGUAGE': str(values['language']),
        'THEORDER_ANISOTROPY': str(values['anisotropy']),
        'THEORDER_UPSCALER': 'none',
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
    'bg': '#09090a', 'panel': '#111113', 'line': '#2a2a2e', 'text': '#ece7e2', 'muted': '#8d8a87',
    'dim': '#55524f', 'red': '#c8141c', 'red_hot': '#ec2129', 'red_dark': '#5e0a0e', 'ok': '#6fbf73',
    'warn': '#e0a43a', 'bad': '#e0453a',
}
W, H = 1200, 690
PANEL = (700, 64, 1162, 636)


def pick_fonts(root):
    from tkinter import font as tkfont
    families = set(tkfont.families(root))
    title = next((f for f in ('Impact', 'Bahnschrift SemiBold Condensed', 'Oswald', 'DejaVu Sans Condensed')
                  if f in families), 'TkDefaultFont')
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
    the player's dump, toned black and red, faded into the panels."""
    try:
        from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageOps
    except ImportError:
        return None
    canvas = Image.new('RGB', (W, H), C['bg'])
    # A dim red glow behind the figure.
    glow = Image.new('L', (W, H), 0)
    ImageDraw.Draw(glow).ellipse((300, 40, 980, 760), fill=255)
    glow = glow.filter(ImageFilter.GaussianBlur(140))
    canvas = Image.composite(Image.new('RGB', (W, H), '#3a0609'), canvas, Image.eval(glow, lambda v: int(v * 0.55)))
    art_path = find_art(game_dir)
    if art_path:
        try:
            art = Image.open(art_path).convert('RGB')
            # The key art's left half: Alex Mercer (PROTOTYPE). Its white backdrop becomes
            # transparent; the figure is toned black and blood red, its own reds kept hot.
            half = art.crop((0, 0, art.width // 2, art.height))
            scale = H * 1.06 / half.height
            half = half.resize((int(half.width * scale), int(half.height * scale)), Image.LANCZOS)
            gray = ImageOps.autocontrast(ImageOps.grayscale(half), cutoff=1)
            toned = ImageOps.colorize(gray, black='#060505', mid='#5c1216', white='#ddd5ce', midpoint=105)
            sat = half.convert('HSV').split()[1]
            red = ImageOps.autocontrast(Image.eval(sat, lambda v: max(0, v - 60) * 2))
            toned = Image.composite(Image.new('RGB', half.size, '#d0141c'), toned,
                                    Image.eval(red, lambda v: int(v * 0.85)))
            lum = ImageOps.grayscale(half)
            alpha = Image.eval(lum, lambda v: 255 if v < 170 else max(0, int(255 * (236 - v) / 66)))
            alpha = alpha.filter(ImageFilter.GaussianBlur(1.2))
            # Fade the figure out toward the bottom and the left.
            fade = Image.linear_gradient('L').resize(half.size)  # 0 at the top, 255 at the bottom
            fade = Image.eval(fade, lambda v: 255 if v < 184 else max(0, int(255 * (255 - v) / 71)))
            side = Image.linear_gradient('L').rotate(90, expand=True).transpose(Image.FLIP_LEFT_RIGHT).resize(half.size)
            side = Image.eval(side, lambda v: min(255, v * 4))
            alpha = ImageChops.multiply(ImageChops.multiply(alpha, fade), side)
            canvas.paste(toned, (330, H - half.height + 20), alpha)
        except Exception as error:
            print(f'launcher: art not used: {error}')
    draw = ImageDraw.Draw(canvas, 'RGBA')
    # Veins: a few branching red strands on the left, glowing.
    veins = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    vd = ImageDraw.Draw(veins)
    rng = random.Random(2009)
    for strand in range(7):
        x, y = rng.uniform(-40, 120), rng.uniform(0, H)
        angle = rng.uniform(-0.6, 0.6)
        width = rng.uniform(2.0, 4.5)
        for _ in range(140):
            import math
            angle += rng.uniform(-0.25, 0.25)
            nx, ny = x + math.cos(angle) * 6, y + math.sin(angle) * 6
            vd.line([(x, y), (nx, ny)], fill=(150, 10, 18, int(70 + 80 * width / 4.5)), width=max(1, int(width)))
            x, y, width = nx, ny, max(0.6, width * 0.985)
            if rng.random() < 0.03:
                bx, by, ba = x, y, angle + rng.choice((-0.9, 0.9))
                for _ in range(30):
                    ba += rng.uniform(-0.3, 0.3)
                    nbx, nby = bx + math.cos(ba) * 5, by + math.sin(ba) * 5
                    vd.line([(bx, by), (nbx, nby)], fill=(120, 8, 14, 90), width=1)
                    bx, by = nbx, nby
    glow = veins.filter(ImageFilter.GaussianBlur(6))
    canvas.paste(glow, (0, 0), glow)
    canvas.paste(veins, (0, 0), veins)
    # Settings panel: translucent dark card with a thin border.
    x0, y0, x1, y1 = PANEL
    draw.rectangle((x0, y0, x1, y1), fill=(10, 10, 11, 205))
    draw.rectangle((x0, y0, x1, y1), outline=(255, 255, 255, 24))
    draw.rectangle((x0, y0, x0 + 3, y0 + 48), fill=(200, 20, 28, 255))
    # Film grain.
    noise = Image.effect_noise((W, H), 18).convert('L')
    canvas = Image.blend(canvas, Image.merge('RGB', (noise, noise, noise)), 0.035)
    return canvas


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
        self.hover_desc = None
        self.checks, self.sfo = [], {}
        root.title('PROTOTYPE — Native PC Port')
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
            self.set_status('Select your PROTOTYPE BIOHAZARD BUNDLE folder to begin', C['warn'])
        elif bad:
            self.set_status(f'{bad[0][0]}: {bad[0][2]}', C['bad'])
        else:
            self.set_status('Ready', C['ok'])
        if redraw:
            self.bg_image = None
            self.window_icon()
            self.draw()

    @property
    def ready(self):
        return bool(self.checks) and not any(state == 'bad' for _, state, _ in self.checks)

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
            x0, y0, x1, y1 = PANEL
            cv.create_rectangle(x0, y0, x1, y1, fill=C['panel'], outline=C['line'])
            cv.create_rectangle(x0, y0, x0 + 3, y0 + 48, fill=C['red'], outline='')
        self.draw_left()
        self.draw_panel()

    def draw_left(self):
        cv = self.canvas
        x = 64
        cv.create_text(x, 86, text='B I O H A Z A R D   B U N D L E', anchor='w', fill=C['red'],
                       font=self.f(11, 'bold'))
        cv.create_text(x - 3, 146, text='PROTOTYPE', anchor='w', fill=C['text'],
                       font=(self.title_font, 64))
        cv.create_rectangle(x, 208, x + 120, 211, fill=C['red'], outline='')
        cv.create_text(x + 132, 210, text='NATIVE PC PORT', anchor='w', fill=C['muted'], font=self.f(10, 'bold'))
        # Game tiles.
        self.tile(x, 248, 'PROTOTYPE', 'READY' if self.ready else 'NEEDS SETUP', selected=True)
        self.tile(x + 214, 248, 'PROTOTYPE 2', 'COMING SOON', selected=False, disabled=True)
        # Play.
        running = self.process is not None and self.process.poll() is None
        if running:
            self.slant_button(x, 350, 404, 74, 'RUNNING', None, fill=C['line'], hot=C['line'], size=24)
            self.link(x + 404 - 4, 448, 'STOP GAME', self.stop, anchor='e')
        elif self.busy:
            self.slant_button(x, 350, 404, 74, self.busy.upper(), None, fill=C['red_dark'], hot=C['red_dark'], size=22)
        else:
            enabled = self.ready
            self.slant_button(x, 350, 404, 74, 'PLAY', self.play if enabled else None,
                              fill=C['red'] if enabled else C['red_dark'], hot=C['red_hot'], size=28,
                              icon=True)
        self.status_item = self.canvas.create_text(x, 448, text=getattr(self, 'status_text', ''), anchor='w',
                                                   fill=getattr(self, 'status_color', C['muted']),
                                                   font=self.f(11), width=320)
        # Footer.
        fy = H - 46
        self.link(x, fy, 'GAME FOLDER', self.choose_folder)
        self.link(x + 128, fy, 'OPEN LOG', self.open_log)
        self.link(x + 226, fy, 'PORT FOLDER', lambda: self.open_path(PORT_DIR))
        cv.create_text(x, H - 20, text=f'v{VERSION} · requires your own game dump · not affiliated with the publisher',
                       anchor='w', fill=C['dim'], font=self.f(8))

    def tile(self, x, y, title, sub, selected, disabled=False):
        cv = self.canvas
        w, h = 198, 72
        cv.create_rectangle(x, y, x + w, y + h, fill='#0e0e10', outline=C['red'] if selected else C['line'],
                            width=2 if selected else 1)
        if selected:
            cv.create_rectangle(x, y + h - 4, x + w, y + h, fill=C['red'], outline='')
        cv.create_text(x + 16, y + 26, text=title, anchor='w', fill=C['dim'] if disabled else C['text'],
                       font=(self.title_font, 18))
        color = C['dim'] if disabled else (C['ok'] if sub == 'READY' else C['warn'])
        cv.create_text(x + 16, y + 52, text=sub, anchor='w', fill=color, font=self.f(9, 'bold'))

    def slant_button(self, x, y, w, h, text, command, fill, hot, size=20, icon=False):
        cv = self.canvas
        s = 18
        pts = (x + s, y, x + w, y, x + w - s, y + h, x, y + h)
        tag = f'btn{id(command)}{x}{y}'
        poly = cv.create_polygon(*pts, fill=fill, outline='', tags=(tag,))
        label = f'▶  {text}' if icon else text
        cv.create_text(x + w / 2, y + h / 2, text=label, fill=C['text'] if command else C['muted'],
                       font=self.f(size, 'bold'), tags=(tag,))
        if command:
            cv.tag_bind(tag, '<Enter>', lambda e: (cv.itemconfigure(poly, fill=hot), cv.configure(cursor='hand2')))
            cv.tag_bind(tag, '<Leave>', lambda e: (cv.itemconfigure(poly, fill=fill), cv.configure(cursor='')))
            cv.tag_bind(tag, '<Button-1>', lambda e: command())

    def link(self, x, y, text, command, anchor='w', size=10, color=None):
        cv = self.canvas
        item = cv.create_text(x, y, text=text, anchor=anchor, fill=color or C['muted'], font=self.f(size, 'bold'))
        cv.tag_bind(item, '<Enter>', lambda e: (cv.itemconfigure(item, fill=C['red_hot']), cv.configure(cursor='hand2')))
        cv.tag_bind(item, '<Leave>', lambda e: (cv.itemconfigure(item, fill=color or C['muted']), cv.configure(cursor='')))
        cv.tag_bind(item, '<Button-1>', lambda e: command())
        return item

    # ---- settings panel
    TABS = ('GRAPHICS', 'DISPLAY', 'GAME')

    def draw_panel(self):
        cv = self.canvas
        x0, y0, x1, y1 = PANEL
        tx = x0 + 26
        for i, name in enumerate(self.TABS):
            active = self.values['tab'] == i
            item = cv.create_text(tx, y0 + 26, text=name, anchor='w', fill=C['text'] if active else C['muted'],
                                  font=self.f(13, 'bold'))
            bbox = cv.bbox(item)
            if active:
                cv.create_rectangle(bbox[0], y0 + 42, bbox[2], y0 + 45, fill=C['red'], outline='')
            else:
                cv.tag_bind(item, '<Enter>', lambda e, it=item: (cv.itemconfigure(it, fill=C['text']), cv.configure(cursor='hand2')))
                cv.tag_bind(item, '<Leave>', lambda e, it=item: (cv.itemconfigure(it, fill=C['muted']), cv.configure(cursor='')))
                cv.tag_bind(item, '<Button-1>', lambda e, i=i: self.switch_tab(i))
            tx += 124
        cv.create_line(x0 + 20, y0 + 56, x1 - 20, y0 + 56, fill=C['line'])
        self.row_y = y0 + 86
        # Description area.
        cv.create_line(x0 + 20, y1 - 74, x1 - 20, y1 - 74, fill=C['line'])
        self.desc_item = cv.create_text(x0 + 24, y1 - 62, text='', anchor='nw', fill=C['muted'],
                                        font=self.f(10), width=x1 - x0 - 48)
        [self.tab_graphics, self.tab_display, self.tab_game][self.values['tab']]()

    def switch_tab(self, i):
        self.values['tab'] = i
        self.save()
        self.draw()

    def describe(self, text):
        self.canvas.itemconfigure(self.desc_item, text=text or '')

    def row(self, label, desc, height=46, locked=False):
        cv = self.canvas
        x0, _, x1, _ = PANEL
        y = self.row_y
        tag = f'row{y}'
        hit = cv.create_rectangle(x0 + 14, y - height / 2 + 2, x1 - 14, y + height / 2 - 2, fill='', outline='',
                                  tags=(tag,))
        cv.create_text(x0 + 26, y, text=label, anchor='w', fill=C['dim'] if locked else C['text'],
                       font=self.f(12), tags=(tag,))
        cv.tag_bind(tag, '<Enter>', lambda e: self.describe(desc))
        cv.tag_bind(tag, '<Leave>', lambda e: self.describe(''))
        self.row_y += height
        del hit
        return y, tag

    def stepper(self, label, desc, key, options, locked=False):
        """◀ value ▶ over a list of (value, text)."""
        cv = self.canvas
        y, tag = self.row(label, desc, locked=locked)
        x1 = PANEL[2] - 26
        width = 224
        current = self.values.get(key)
        index = next((i for i, (v, _) in enumerate(options) if v == current), 0)
        text = options[index][1]
        cx = x1 - width / 2
        cv.create_text(cx, y, text=text, fill=C['dim'] if locked else C['text'], font=self.f(12, 'bold'), tags=(tag,))
        for direction, ax in ((-1, x1 - width + 8), (1, x1 - 8)):
            arrow = cv.create_text(ax, y, text='◀' if direction < 0 else '▶', fill=C['dim'] if locked else C['red'],
                                   font=self.f(12), tags=(tag,))
            if locked:
                continue
            def step(e, d=direction):
                self.values[key] = options[(index + d) % len(options)][0]
                self.save()
                self.draw()
                self.describe(desc)
            cv.tag_bind(arrow, '<Button-1>', step)
            cv.tag_bind(arrow, '<Enter>', lambda e, a=arrow: (cv.itemconfigure(a, fill=C['red_hot']), cv.configure(cursor='hand2')))
            cv.tag_bind(arrow, '<Leave>', lambda e, a=arrow: (cv.itemconfigure(a, fill=C['red']), cv.configure(cursor='')))
        cv.create_line(x1 - width + 22, y + 13, x1 - 22, y + 13, fill=C['line'], tags=(tag,))

    def toggle(self, label, desc, key, locked=False):
        cv = self.canvas
        y, tag = self.row(label, desc, locked=locked)
        x1 = PANEL[2] - 26
        on = bool(self.values.get(key)) and not locked
        w, h = 46, 22
        x = x1 - w
        fill = C['red'] if on else '#26262a'
        sw = f'sw{y}'
        cv.create_oval(x, y - h / 2, x + h, y + h / 2, fill=fill, outline='', tags=(tag, sw))
        cv.create_oval(x + w - h, y - h / 2, x + w, y + h / 2, fill=fill, outline='', tags=(tag, sw))
        cv.create_rectangle(x + h / 2, y - h / 2, x + w - h / 2, y + h / 2, fill=fill, outline='', tags=(tag, sw))
        kx = x + w - h + 3 if on else x + 3
        cv.create_oval(kx, y - h / 2 + 3, kx + h - 6, y + h / 2 - 3, fill=C['text'] if not locked else C['dim'],
                       outline='', tags=(tag, sw))
        cv.create_text(x - 12, y, text='ON' if on else 'OFF', anchor='e', fill=C['muted'], font=self.f(9, 'bold'),
                       tags=(tag,))
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
        x1 = PANEL[2] - 26
        width = 196
        x = x1 - width
        value = int(self.values.get(key, low))
        cv.create_line(x, y, x1 - 40, y, fill='#2c2c30', width=4, tags=(tag,))
        frac = (value - low) / (high - low)
        kx = x + frac * (width - 40)
        bar = cv.create_line(x, y, kx, y, fill=C['dim'] if locked else C['red'], width=4, tags=(tag,))
        knob = cv.create_oval(kx - 7, y - 7, kx + 7, y + 7, fill=C['text'] if not locked else C['dim'], outline='', tags=(tag,))
        num = cv.create_text(x1, y, text=f'{value}%', anchor='e', fill=C['muted'], font=self.f(10, 'bold'), tags=(tag,))
        if locked:
            return

        def move(e):
            frac = min(1.0, max(0.0, (e.x - x) / (width - 40)))
            v = int(round(low + frac * (high - low)))
            self.values[key] = v
            nx = x + frac * (width - 40)
            cv.coords(bar, x, y, nx, y)
            cv.coords(knob, nx - 7, y - 7, nx + 7, y + 7)
            cv.itemconfigure(num, text=f'{v}%')

        for item in (bar, knob):
            cv.tag_bind(item, '<Button-1>', move)
            cv.tag_bind(item, '<B1-Motion>', move)
            cv.tag_bind(item, '<ButtonRelease-1>', lambda e: self.save())

    def note(self, text, color=None, height=30):
        y = self.row_y - 14
        self.canvas.create_text(PANEL[0] + 26, y, text=text, anchor='nw', fill=color or C['dim'], font=self.f(10),
                                width=PANEL[2] - PANEL[0] - 52)
        self.row_y += height

    def section(self, text):
        y = self.row_y - 8
        self.canvas.create_text(PANEL[0] + 26, y, text=text, anchor='w', fill=C['red'], font=self.f(9, 'bold'))
        self.row_y += 22

    def tab_graphics(self):
        res = [(r, r.replace('x', ' × ') + ('  (PS4)' if r == '1920x1080' else '')) for r in RESOLUTIONS]
        self.section('IMAGE')
        self.stepper('Render resolution', 'The resolution the game renders at. Above your window size it '
                     'supersamples (sharper, heavier); below it, FSR 1 scales it up.', 'render_res', res)
        self.toggle('FSR 1 upscaling', 'AMD FidelityFX Super Resolution 1: scales the game image to your window '
                    'with edge-aware upscaling instead of plain filtering.', 'fsr1')
        self.toggle('Sharpening (RCAS)', 'Contrast-adaptive sharpening of the final image.', 'rcas')
        self.slider('Sharpness', 'Strength of the sharpening.', 'rcas_strength', 0, 100, locked=not self.values['rcas'])
        self.stepper('Anisotropic filtering', 'Sharper textures at grazing angles (ground, walls). '
                     'Cheap on any modern GPU.', 'anisotropy',
                     [(0, 'Game default'), (2, '2×'), (4, '4×'), (8, '8×'), (16, '16×')])
        self.stepper('Frame rate', '60 FPS: gameplay at 60, videos and menus keep their normal speed.', 'fps60',
                     [(False, '30 FPS (PS4)'), (True, '60 FPS')])
        self.section('IN DEVELOPMENT')
        self.stepper('Temporal upscaler', 'FSR 3.1, FSR 4 and FSR 4.1 need motion vectors from the game\'s '
                     'camera: being hooked up for PROTOTYPE, coming in a later update.', 'none',
                     [(None, 'FSR 3.1 · 4 · 4.1')], locked=True)

    def tab_display(self):
        self.section('WINDOW')
        self.stepper('Window mode', 'Borderless fills the screen and switches instantly with Alt+Tab.',
                     'display_mode', [('windowed', 'Windowed'), ('borderless', 'Borderless'), ('fullscreen', 'Fullscreen')])
        self.toggle('V-Sync', 'Waits for the display refresh: no tearing.', 'vsync')
        self.toggle('Show FPS', 'Frame rate counter in the corner.', 'show_fps')
        self.section('IN GAME')
        self.note('Insert (keyboard) or R3 + L2 (controller) opens the port menu while playing.', C['muted'], 40)

    def tab_game(self):
        cv = self.canvas
        x0, _, x1, _ = PANEL
        self.section('GAME FOLDER')
        folder = self.values['game_dir'] or 'not selected'
        if len(folder) > 46:
            folder = '…' + folder[-45:]
        cv.create_text(x0 + 26, self.row_y - 4, text=folder, anchor='w', fill=C['text'], font=self.f(11))
        self.link(x1 - 26, self.row_y - 4, 'CHANGE', self.choose_folder, anchor='e', color=C['red'])
        self.row_y += 30
        for label, state, detail in self.checks:
            mark = {'ok': '●', 'warn': '▲', 'bad': '✖'}[state]
            cv.create_text(x0 + 30, self.row_y - 6, text=mark, anchor='w', fill=C[state], font=self.f(11, 'bold'))
            cv.create_text(x0 + 52, self.row_y - 6, text=label, anchor='w', fill=C['text'], font=self.f(11))
            cv.create_text(x1 - 26, self.row_y - 6, text=detail, anchor='e', fill=C['muted'], font=self.f(10))
            self.row_y += 24
        self.row_y += 8
        bx = x0 + 26
        actions = [('PREPARE', self.prepare, 'Converts the game into the native image now (also done at every start).')]
        if self.values['game_dir'] and restorable(self.values['game_dir']):
            actions.append(('RESTORE ORIGINAL', self.restore, 'Puts the original executables back from the .orig backups.'))
        actions.append(('CLEAN UP', self.clean, 'Deletes files earlier fixes and debugging left in the game folder.'))
        for text, command, desc in actions:
            item = self.link(bx, self.row_y, f'[ {text} ]', command, color=C['text'])
            cv.tag_bind(item, '<Enter>', lambda e, d=desc, it=item: (self.describe(d), cv.itemconfigure(it, fill=C['red_hot']),
                                                                     cv.configure(cursor='hand2')), add=False)
            bx = cv.bbox(item)[2] + 18
        self.row_y += 36
        self.section('OPTIONS')
        self.stepper('Language', 'Text and voice language of the game.', 'language',
                     [(code, name) for name, code in LANGUAGES])
        self.toggle('Fix white tendrils', 'Consume tendrils show their red veins (a fix for a GPU emulation '
                    'difference; keep it on).', 'tendril_fix')

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
