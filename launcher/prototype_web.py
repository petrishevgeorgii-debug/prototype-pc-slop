# SPDX-License-Identifier: GPL-2.0-or-later
"""PROTOTYPE launcher, the web interface: launcher/web in a Microsoft Edge app window.

The page talks to this process over HTTP on 127.0.0.1 (a random port, a token in a SameSite
cookie, JSON requests only): it reads the launcher's state and asks for every change and action
(settings, keys, saves, mods, start). Settings, the game folder checks, the start and the stop
are the classic launcher's own (prototype_launcher.py), so both behave the same; without Edge
(or Chrome, Chromium) the classic launcher opens instead.

The pictures and names come from the player's game files (scripts/launcher_art.py), cached in
out/launcher; nothing from the game ships with the port.
"""
import ctypes
import datetime
import http.server
import json
import os
from pathlib import Path
import secrets
import shutil
import socketserver
import subprocess
import sys
import threading
import time
import urllib.request

RELEASES_API = 'https://api.github.com/repos/petrishevgeorgii-debug/prototype-pc-slop/releases/latest'
RELEASES_PAGE = 'https://github.com/petrishevgeorgii-debug/prototype-pc-slop/releases'
TITLE_ID = 'CUSA06545'
MIME = {'.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8',
        '.jpg': 'image/jpeg', '.png': 'image/png', '.json': 'application/json; charset=utf-8', '.svg': 'image/svg+xml'}


def find_browser(windows):
    """An Edge (else Chrome, Chromium) executable that can open an app window, or None."""
    if windows:
        candidates = []
        try:
            import winreg
            for hive in (winreg.HKEY_LOCAL_MACHINE, winreg.HKEY_CURRENT_USER):
                try:
                    with winreg.OpenKey(hive, r'SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\msedge.exe') as key:
                        candidates.append(Path(winreg.QueryValue(key, None)))
                except OSError:
                    pass
        except ImportError:
            pass
        for base in (os.environ.get('ProgramFiles(x86)'), os.environ.get('ProgramFiles'), os.environ.get('LOCALAPPDATA')):
            if base:
                candidates.append(Path(base) / 'Microsoft' / 'Edge' / 'Application' / 'msedge.exe')
                candidates.append(Path(base) / 'Google' / 'Chrome' / 'Application' / 'chrome.exe')
        return next((p for p in candidates if p.is_file()), None)
    for name in ('microsoft-edge', 'microsoft-edge-stable', 'google-chrome', 'chromium', 'chromium-browser'):
        found = shutil.which(name)
        if found:
            return Path(found)
    return None


def _pid_alive(pid):
    if not pid:
        return False
    if sys.platform == 'win32':
        handle = ctypes.windll.kernel32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
        if not handle:
            return False
        code = ctypes.c_ulong()
        ctypes.windll.kernel32.GetExitCodeProcess(handle, ctypes.byref(code))
        ctypes.windll.kernel32.CloseHandle(handle)
        return code.value == 259  # STILL_ACTIVE
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def show_windows(pid, show):
    """Minimises (show False) or restores the browser's windows (Windows only)."""
    if sys.platform != 'win32' or not pid:
        return
    user32 = ctypes.windll.user32
    found = []
    proc = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def each(hwnd, _):
        owner = ctypes.c_ulong()
        user32.GetWindowThreadProcessId(ctypes.c_void_p(hwnd), ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(ctypes.c_void_p(hwnd)) and \
                user32.GetWindowTextLengthW(ctypes.c_void_p(hwnd)) > 0:
            found.append(hwnd)
        return True
    user32.EnumWindows(proc(each), 0)
    for hwnd in found:
        user32.ShowWindow(ctypes.c_void_p(hwnd), 9 if show else 6)  # SW_RESTORE / SW_MINIMIZE
        if show:
            user32.SetForegroundWindow(ctypes.c_void_p(hwnd))


def dir_size(path):
    total = count = 0
    for root, _, files in os.walk(path):
        for name in files:
            try:
                total += os.path.getsize(os.path.join(root, name))
                count += 1
            except OSError:
                pass
    return total, count


class Backend:
    """The launcher's state and actions for the page (`L`: the prototype_launcher module)."""

    def __init__(self, L):
        self.L = L
        self.values = L.load_settings()
        self.lang = L.lang.set_language(self.values.get('ui_language', 'auto'))
        self.process = None
        self.started = 0.0
        self.exit = None          # the last run's end: {'code', 'game'}
        self.busy = None
        self.notice = None        # a message for the page's next poll
        self.art = None
        self.art_dir = L.PORT_DIR / 'out' / 'launcher'
        self.pad = None
        self.update = None
        self.browser_pid = None
        self.checks, self.sfo = [], {}
        self.lock = threading.RLock()
        self.refresh()
        threading.Thread(target=self.load_art, daemon=True).start()
        threading.Thread(target=self.watch_pad, daemon=True).start()
        threading.Thread(target=self.check_update, daemon=True).start()

    # ---- background
    def load_art(self):
        game = self.values.get('game_dir')
        if not game or not Path(game).is_dir():
            self.art = {'art': [], 'p1': [], 'p2': [], 'missions': {}, 'districts': []}
            return
        try:
            import launcher_art
            self.art = launcher_art.generate(game, self.art_dir, log=print)
        except Exception as error:  # the page shows no pictures
            print(f'launcher: art: {error}')
            self.art = {'art': [], 'p1': [], 'p2': [], 'missions': {}, 'districts': []}

    def watch_pad(self):
        """The connected gamepad (theorder-probe --gamepad), every 20 s."""
        exe = 'theorder-probe.exe' if self.L.WINDOWS else 'theorder-probe'
        probe = next((p for p in (self.L.PORT_DIR / 'bin' / exe, self.L.PORT_DIR / 'out' / exe) if p.is_file()), None)
        while probe:
            if not self.running:
                try:
                    r = subprocess.run([str(probe), '--gamepad'], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                       stderr=subprocess.DEVNULL, text=True, timeout=10, creationflags=self.L.NO_WINDOW)
                    line = next((ln for ln in r.stdout.splitlines() if ln.startswith('Gamepad:')), 'Gamepad: none')
                    if line.strip() == 'Gamepad: none':
                        self.pad = None
                    else:
                        fields = dict(f.split('=', 1) for f in line[len('Gamepad:'):].split() if '=' in f)
                        name = line.split('name=', 1)[1].strip() if 'name=' in line else ''
                        kind = 'xbox' if fields.get('south') == 'a' else 'playstation'
                        self.pad = {'kind': kind, 'type': fields.get('type', ''), 'name': name}
                except (OSError, subprocess.TimeoutExpired, ValueError):
                    pass
            time.sleep(20)

    def check_update(self):
        try:
            request = urllib.request.Request(RELEASES_API, headers={'User-Agent': 'PROTOTYPE-PC-Port-launcher',
                                                                    'Accept': 'application/vnd.github+json'})
            with urllib.request.urlopen(request, timeout=8) as reply:
                data = json.loads(reply.read().decode('utf-8'))
            tag = str(data.get('tag_name', '')).lstrip('vV')
            self.update = {'ok': True, 'latest': tag, 'url': data.get('html_url') or RELEASES_PAGE,
                           'newer': self._newer(tag, self.L.VERSION)}
        except Exception as error:
            self.update = {'ok': False, 'error': str(error)[:160], 'url': RELEASES_PAGE}

    @staticmethod
    def _newer(a, b):
        def parts(v):
            return [int(x) if x.isdigit() else 0 for x in v.split('.')[:3]]
        try:
            return parts(a) > parts(b)
        except ValueError:
            return False

    # ---- state
    def refresh(self):
        self.checks, self.sfo = self.L.check_game(self.values.get('game_dir'), self.game)

    @property
    def game(self):
        return self.values.get('game', 'p1')

    @property
    def running(self):
        return self.process is not None and self.process.poll() is None

    @property
    def ready(self):
        return bool(self.checks) and not any(state == 'bad' for _, state, _ in self.checks)

    def save(self):
        self.L.save_settings(self.values)

    def saves(self):
        """{p1: [...], p2: [...]}: each game's save slots as the game wrote them."""
        root = self.L.PORT_DIR / 'user' / 'savedata'
        folders = sorted(root.glob(f'*/{TITLE_ID}')) if root.is_dir() else []
        out = {'p1': [], 'p2': []}
        if not folders:
            return out
        folder = folders[0]
        for meta in sorted(folder.glob('SLOTP?-*.BIN.sce_sys')):
            slot_dir = folder / meta.name[:-len('.sce_sys')]
            game = 'p1' if meta.name.upper().startswith('SLOTP1') else 'p2'
            try:
                param = (meta / 'param.bin').read_bytes()
            except OSError:
                continue
            text = lambda a, b: param[a:b].split(b'\0')[0].decode('utf-8', 'replace').strip()  # noqa: E731
            subtitle, detail = text(128, 256), text(256, 1280)
            mtime = int.from_bytes(param[1288:1296], 'little', signed=True) if len(param) >= 1296 else 0
            if mtime <= 0:
                mtime = int((meta / 'param.bin').stat().st_mtime)
            mission = None
            if game == 'p1':
                for data_file in slot_dir.glob('*.bin'):
                    try:
                        with open(data_file, 'rb') as f:
                            f.seek(0x524)
                            ident = f.read(6).decode('ascii', 'replace')
                        if len(ident) == 6 and ident[0] == 'e' and ident[3] == 'm':
                            mission = ident
                    except OSError:
                        pass
                    break
            title, number, auto = subtitle, None, False
            if game == 'p1' and '(' in subtitle:  # "PAMYAT V SMERTI( 3 )"
                head, _, tail = subtitle.rpartition('(')
                if tail.strip(' )').isdigit():
                    title, number = head.strip(), int(tail.strip(' )'))
            elif game == 'p2':  # "(1)(AUTO) (+) MURDER YOUR MAKER": number, autosave, New Game+
                rest, plus = subtitle.strip(), False
                while rest.startswith('(') and ')' in rest:
                    part, _, rest = rest[1:].partition(')')
                    part, rest = part.strip(), rest.strip()
                    if part.isdigit():
                        number = int(part)
                    elif part == '+':
                        plus = True
                    elif part.upper() in ('AUTO', 'АВТО'):
                        auto = True
                title = rest + (' (+)' if plus else '')
            out[game].append({'slot': meta.name.split('-')[1][:1], 'title': title or slot_dir.name, 'number': number,
                              'detail': detail, 'time': mtime, 'mission': mission, 'auto': auto, 'dir': slot_dir.name})
        return out

    def backups(self):
        try:
            import save_backup
            user = self.L.PORT_DIR / 'user'
            out = []
            for name in save_backup.list_backups(user)[:8]:
                path = save_backup.backups_dir(user) / name
                slots = sum(1 for _ in path.rglob('SLOTP?-*.BIN')) if path.is_dir() else 0
                try:
                    when = datetime.datetime.strptime(name[:15], save_backup.STAMP).timestamp()
                except ValueError:
                    when = path.stat().st_mtime
                out.append({'name': name, 'time': int(when), 'slots': slots})
            return out
        except (OSError, ImportError):
            return []

    def mods(self):
        root = self.L.PORT_DIR / 'mods'
        disabled = set(self._mods_config().get('disabled', []))
        named, loose = [], 0
        if root.is_dir():
            for folder in sorted(p for p in root.iterdir() if p.is_dir() and not p.name.startswith(('_', '.'))):
                shaders = folder / 'shaders'
                if folder.name == 'shaders':
                    loose = sum(1 for p in folder.iterdir() if p.is_file() and p.suffix in ('.frag', '.vert', '.comp', '.spv'))
                elif shaders.is_dir():
                    files = sum(1 for p in shaders.iterdir() if p.is_file() and p.suffix in ('.frag', '.vert', '.comp', '.spv'))
                    named.append({'name': folder.name, 'files': files, 'enabled': folder.name not in disabled})
        return {'named': named, 'loose': loose, 'dir': str(root)}

    def _mods_config(self):
        try:
            data = json.loads((self.L.PORT_DIR / 'mods.json').read_text(encoding='utf-8'))
            return data if isinstance(data, dict) else {}
        except (OSError, ValueError):
            return {}

    def controls(self):
        import proto_controls
        full = proto_controls.layout(self.values.get('controls'))
        clashes = sorted(proto_controls.conflicts(full))
        actions = [{'id': a[0], 'pad': a[1], 'en': a[2], 'ru': a[3], 'group': a[5], 'keys': full.get(a[0], [])}
                   for a in proto_controls.ACTIONS]
        return {'actions': actions, 'groups': [list(g) for g in proto_controls.GROUPS], 'clashes': clashes,
                'slots': proto_controls.SLOTS}

    def prepared(self, game):
        out = self.L.PORT_DIR / 'out'
        if game == 'p1':
            return (out / 'eboot.elf').is_file() or any(out.glob('eboot*.elf'))
        return any((out / 'prototype2_ps4f').glob('*.elf')) if (out / 'prototype2_ps4f').is_dir() else False

    def launch_state(self):
        lines = []
        try:
            with open(self.L.LOG_FILE, 'r', encoding='utf-8', errors='replace') as f:
                lines = [ln.rstrip() for ln in f.readlines()[-14:]]
        except OSError:
            pass
        return {'running': self.running, 'since': self.started if self.running else 0, 'game': self.values.get('playing'),
                'log': lines, 'exit': self.exit, 'busy': self.busy}

    def state(self):
        L = self.L
        v = self.values
        size, files = dir_size(self.L.PORT_DIR / 'user' / 'cache')
        notice, self.notice = self.notice, None
        keys = ('anisotropy', 'fsr1', 'rcas', 'rcas_strength', 'display_mode', 'vsync', 'show_fps', 'prompts',
                'mouse_look', 'mouse_sensitivity', 'menu_mouse', 'vibration', 'light_bar', 'menu_vibration',
                'extra_vibration', 'haptics', 'haptics_sound', 'discord', 'ui_language', 'minimize_on_play')
        return {
            'version': self.L.VERSION, 'beta': self.L.BETA, 'lang': self.lang, 'game': self.game,
            'gameDir': v.get('game_dir', ''), 'values': {k: v.get(k) for k in keys}, 'perGame': v['per_game'],
            'checks': [[L.T(label), state, L.T(detail)] for label, state, detail in self.checks], 'ready': self.ready, 'sfo': {k: self.sfo.get(k) for k in ('TITLE_ID', 'APP_VER')},
            'restorable': bool(v.get('game_dir')) and bool(self.L.restorable(v['game_dir'])),
            'art': self.art, 'saves': self.saves(), 'backups': self.backups(), 'mods': self.mods(),
            'controls': self.controls(), 'pad': self.pad, 'update': self.update,
            'cache': {'mb': round(size / 1e6), 'files': files}, 'prepared': {g: self.prepared(g) for g in ('p1', 'p2')},
            'playTime': v.get('play_time', {}), 'launch': self.launch_state(), 'notice': notice,
            'releases': RELEASES_PAGE,
        }

    # ---- actions (from the page)
    def act(self, name, args):
        handler = getattr(self, 'do_' + name, None)
        if handler is None:
            raise ValueError(f'unknown action {name}')
        with self.lock:
            return handler(**args)

    def do_state(self):
        return self.state()

    def do_poll(self):
        notice, self.notice = self.notice, None
        return {'launch': self.launch_state(), 'pad': self.pad, 'update': self.update, 'notice': notice,
                'artReady': self.art is not None}

    def do_set(self, key, value):
        L = self.L
        if key.startswith('game.'):
            name = key[5:]
            settings = self.values['per_game'][self.game]
            if name == 'render_res' and value not in L.RESOLUTIONS:
                raise ValueError('resolution')
            if name == 'fps':
                value = int(value)
                if value not in L.P1_FRAME_RATES:
                    raise ValueError('frame rate')
                settings['fps60'] = value > 30
            settings[name] = value
        else:
            if key not in ('anisotropy', 'fsr1', 'rcas', 'rcas_strength', 'display_mode', 'vsync', 'show_fps',
                           'prompts', 'mouse_look', 'mouse_sensitivity', 'menu_mouse', 'vibration', 'light_bar',
                           'menu_vibration', 'extra_vibration', 'haptics', 'haptics_sound', 'discord', 'ui_language',
                           'minimize_on_play'):
                raise ValueError(f'unknown setting {key}')
            self.values[key] = value
            if key == 'ui_language':
                self.lang = L.lang.set_language(value)
        self.save()
        L.write_ini(self.values)  # the game's own menus read and change these too
        return self.state()

    def do_game(self, game):
        if game not in ('p1', 'p2') or self.running:
            return self.state()
        self.values['game'] = game
        self.save()
        self.refresh()
        return self.state()

    def do_key(self, action, slot, key):
        import proto_controls
        full = proto_controls.layout(self.values.get('controls'))
        if action not in full or not 0 <= int(slot) < proto_controls.SLOTS:
            raise ValueError('action')
        if key:
            for other, keys in full.items():  # a key is on one action only
                full[other] = [k for k in keys if k.casefold() != key.casefold()]
        keys = list(full.get(action, []))
        while len(keys) <= int(slot):
            keys.append('')
        keys[int(slot)] = key or ''
        full[action] = [k for k in keys if k]
        self.values['controls'] = proto_controls.changes(full)
        self.save()
        return self.state()

    def do_reset_keys(self):
        self.values['controls'] = {}
        self.save()
        return self.state()

    def do_play(self):
        L = self.L
        if self.running:
            return self.state()
        self.refresh()
        if not self.ready:
            self.notice = {'kind': 'bad', 'key': 'fixFolder'}
            return self.state()
        L.write_ini(self.values)
        env = L.game_environment(self.values)
        try:
            log = open(L.LOG_FILE, 'w', encoding='utf-8', errors='replace')
            flags = L.NO_WINDOW | getattr(subprocess, 'CREATE_NEW_PROCESS_GROUP', 0)
            self.process = subprocess.Popen(L.run_command(), cwd=L.PORT_DIR, env=env, stdin=subprocess.DEVNULL,
                                            stdout=log, stderr=subprocess.STDOUT, creationflags=flags)
            log.close()
            self.started = time.time()
            self.exit = None
            self.values['playing'] = self.game
            threading.Thread(target=self.wait_game, args=(self.process, self.game), daemon=True).start()
        except OSError as error:
            self.process = None
            self.notice = {'kind': 'bad', 'text': f'{error}'}
        return self.state()

    def wait_game(self, process, game):
        if self.values.get('minimize_on_play', True):
            time.sleep(6)  # the game window is up by then
            if process.poll() is None:
                show_windows(self.browser_pid, False)
        code = process.wait()
        with self.lock:
            played = int(time.time() - self.started)
            times = self.values.setdefault('play_time', {})
            times[game] = int(times.get(game, 0)) + max(0, played)
            self.values.pop('playing', None)
            self.exit = {'code': code, 'game': game, 'played': played}
            self.save()
        show_windows(self.browser_pid, True)

    def do_stop(self):
        if not self.running:
            return self.state()
        if self.L.WINDOWS:
            subprocess.run(['taskkill', '/T', '/F', '/PID', str(self.process.pid)], creationflags=self.L.NO_WINDOW,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        else:
            self.process.terminate()
        return self.state()

    def _open(self, path):
        try:
            if self.L.WINDOWS:
                os.startfile(str(path))
            else:
                subprocess.Popen(['xdg-open', str(path)])
            return True
        except OSError:
            return False

    def do_open(self, what):
        L = self.L
        root = L.PORT_DIR / 'user' / 'savedata'
        paths = {'saves': next(iter(sorted(root.glob(f'*/{TITLE_ID}'))), root) if root.is_dir() else root,
                 'backups': L.PORT_DIR / 'user' / 'savedata_backups', 'log': L.LOG_FILE, 'port': L.PORT_DIR,
                 'mods': L.PORT_DIR / 'mods', 'game': Path(self.values.get('game_dir') or L.PORT_DIR)}
        path = paths.get(what)
        if what == 'mods' and path:
            (path / 'shaders').mkdir(parents=True, exist_ok=True)
        ok = bool(path) and path.exists() and self._open(path)
        return {'ok': ok}

    def do_url(self, url):
        if url not in (RELEASES_PAGE,) and not url.startswith('https://github.com/petrishevgeorgii-debug/'):
            raise ValueError('url')
        import webbrowser
        webbrowser.open(url)
        return {'ok': True}

    def do_choose_folder(self):
        result = {}

        def ask():
            try:
                import tkinter as tk
                from tkinter import filedialog
                root = tk.Tk()
                root.withdraw()
                root.attributes('-topmost', True)
                result['path'] = filedialog.askdirectory(parent=root, initialdir=self.values.get('game_dir') or str(Path.home()),
                                                         title=self.L.T('PROTOTYPE BIOHAZARD BUNDLE folder (eboot.bin, sce_sys, p1, p2)'))
                root.destroy()
            except Exception as error:
                result['error'] = str(error)
        thread = threading.Thread(target=ask)
        thread.start()
        thread.join()
        if result.get('path'):
            self.values['game_dir'] = str(Path(result['path']))
            self.save()
            self.refresh()
            self.art = None
            threading.Thread(target=self.load_art, daemon=True).start()
        return self.state()

    def do_prepare(self):
        L = self.L
        if not self.ready or self.busy:
            return self.state()
        game, exe = self.values['game_dir'], L.GAMES[self.game][1]
        out = L.PORT_DIR / 'out' if exe == 'eboot.bin' else L.PORT_DIR / 'out' / Path(exe).stem

        def work():
            error = None
            try:
                out.mkdir(parents=True, exist_ok=True)
                env = L.game_environment(self.values)
                for name in ('prepare.py', 'link_libc.py', 'link_modules.py'):
                    r = subprocess.run(L.script_command(name, game, '--out', out), cwd=L.PORT_DIR, env=env,
                                       stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                       text=True, encoding='utf-8', errors='replace', creationflags=L.NO_WINDOW)
                    if r.returncode:
                        error = f'{name}: ' + (r.stdout.strip().splitlines() or ['?'])[-1]
                        break
            except OSError as e:
                error = str(e)
            self.busy = None
            self.notice = {'kind': 'bad', 'text': error} if error else {'kind': 'ok', 'key': 'prepared'}
        self.busy = 'prepare'
        threading.Thread(target=work, daemon=True).start()
        return self.state()

    def do_clean_preview(self):
        files = self.L.leftovers(self.values['game_dir']) if self.values.get('game_dir') else []
        return {'files': [p.name for p in files], 'mb': round(sum(p.stat().st_size for p in files) / 1e6)}

    def do_clean(self):
        files = self.L.leftovers(self.values['game_dir']) if self.values.get('game_dir') else []
        failed = 0
        for p in files:
            try:
                p.unlink()
            except OSError:
                failed += 1
        self.refresh()
        self.notice = {'kind': 'ok' if not failed else 'warn', 'key': 'cleaned', 'n': len(files) - failed, 'failed': failed}
        return self.state()

    def do_restore_originals(self):
        items = self.L.restorable(self.values['game_dir']) if self.values.get('game_dir') else []
        try:
            for path, backup in items:
                shutil.copy2(backup, path)
            self.notice = {'kind': 'ok', 'key': 'restored'}
        except OSError as error:
            self.notice = {'kind': 'bad', 'text': str(error)}
        self.refresh()
        return self.state()

    def do_backup(self):
        import save_backup
        try:
            made = save_backup.backup(self.L.PORT_DIR / 'user')
            self.notice = {'kind': 'ok', 'key': 'backedUp'} if made else {'kind': 'warn', 'key': 'noSaves'}
        except OSError as error:
            self.notice = {'kind': 'bad', 'text': str(error)}
        return self.state()

    def do_restore_backup(self, name):
        import save_backup
        if self.running:
            self.notice = {'kind': 'warn', 'key': 'closeGameFirst'}
            return self.state()
        if name not in save_backup.list_backups(self.L.PORT_DIR / 'user'):
            raise ValueError('backup')
        try:
            save_backup.restore(self.L.PORT_DIR / 'user', name)
            self.notice = {'kind': 'ok', 'key': 'backupRestored'}
        except OSError as error:
            self.notice = {'kind': 'bad', 'text': str(error)}
        return self.state()

    def do_mod(self, name, enabled):
        config = self._mods_config()
        disabled = [n for n in config.get('disabled', []) if isinstance(n, str) and n != name]
        if not enabled:
            disabled.append(name)
        config['disabled'] = sorted(set(disabled))
        path = self.L.PORT_DIR / 'mods.json'
        tmp = path.with_suffix('.tmp')
        tmp.write_text(json.dumps(config, indent=2), encoding='utf-8')
        tmp.replace(path)
        return self.state()

    def do_check_update(self):
        self.update = None
        self.check_update()
        return {'update': self.update}

    def do_close(self):
        threading.Timer(0.2, self.close_browser).start()
        return {'ok': True}

    def close_browser(self):
        process = getattr(self, 'browser', None)
        if process and process.poll() is None:
            process.terminate()


def make_handler(backend, web_dir, token):
    class Handler(http.server.BaseHTTPRequestHandler):
        server_version = 'PrototypeLauncher'

        def log_message(self, *args):  # quiet
            pass

        def _allowed(self):
            host = self.headers.get('Host', '')
            if host not in (f'127.0.0.1:{self.server.server_address[1]}', f'localhost:{self.server.server_address[1]}'):
                return False
            cookie = self.headers.get('Cookie', '')
            return f't={token}' in [c.strip() for c in cookie.split(';')] or self.headers.get('X-Token') == token

        def _send(self, code, body, kind='application/json; charset=utf-8', extra=()):
            self.send_response(code)
            self.send_header('Content-Type', kind)
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Cache-Control', 'no-store')
            self.send_header('X-Content-Type-Options', 'nosniff')
            for k, v in extra:
                self.send_header(k, v)
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            backend.seen = True
            path = self.path.split('#')[0]
            if path.startswith('/?') or path == '/':
                query = dict(p.split('=', 1) for p in path[2:].split('&') if '=' in p)
                if query.get('t') == token:  # the first load: the token becomes a cookie, the address drops it
                    self._send(302, b'', 'text/plain', (('Location', '/app'), ('Set-Cookie', f't={token}; Path=/; SameSite=Strict; HttpOnly')))
                    return
            if not self._allowed():
                self._send(403, b'forbidden', 'text/plain')
                return
            path = path.split('?')[0]
            if path in ('/', '/app'):
                path = '/index.html'
            if path.startswith('/art/'):
                target = (backend.art_dir / path[len('/art/'):]).resolve()
                root = backend.art_dir.resolve()
            elif path == '/icon.png':
                target = Path(backend.values.get('game_dir') or '.') / 'sce_sys' / 'icon0.png'
                root = target.parent
            else:
                target = (web_dir / path.lstrip('/')).resolve()
                root = web_dir.resolve()
            if root not in target.parents and target != root or not target.is_file():
                self._send(404, b'not found', 'text/plain')
                return
            self._send(200, target.read_bytes(), MIME.get(target.suffix, 'application/octet-stream'))

        def do_POST(self):
            if not self._allowed() or not self.headers.get('Content-Type', '').startswith('application/json'):
                self._send(403, b'{}')
                return
            if not self.path.startswith('/api/'):
                self._send(404, b'{}')
                return
            length = int(self.headers.get('Content-Length') or 0)
            try:
                args = json.loads(self.rfile.read(length) or b'{}') if length else {}
                result = backend.act(self.path[len('/api/'):], args if isinstance(args, dict) else {})
                self._send(200, json.dumps(result, ensure_ascii=False).encode('utf-8'))
            except Exception as error:
                self._send(400, json.dumps({'error': str(error)}).encode('utf-8'))
    return Handler


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = False


def web_dir(L):
    for candidate in (L.PORT_DIR / 'web', L.PORT_DIR / 'launcher' / 'web', Path(__file__).resolve().parent / 'web'):
        if (candidate / 'index.html').is_file():
            return candidate
    return None


def run(L):
    """Opens the web launcher; False when it cannot (the classic one opens instead)."""
    if os.environ.get('THEORDER_LAUNCHER', '').lower() == 'classic' or '--classic' in sys.argv:
        return False
    folder = web_dir(L)
    browser = find_browser(L.WINDOWS)
    if not folder or not browser:
        print(f'launcher: classic window ({"no web files" if not folder else "no Edge or Chrome"})')
        return False
    profile = L.CONFIG_DIR / 'window'
    profile.mkdir(parents=True, exist_ok=True)
    lock = profile / 'launcher.lock'
    try:  # one launcher at a time: a second start brings the first one's window back
        other = json.loads(lock.read_text(encoding='utf-8'))
        if _pid_alive(other.get('pid')) and _pid_alive(other.get('browser')):
            show_windows(other.get('browser'), True)
            return True
    except (OSError, ValueError):
        pass
    backend = Backend(L)
    backend.seen = False
    token = secrets.token_urlsafe(24)
    server = Server(('127.0.0.1', 0), make_handler(backend, folder, token))
    port = server.server_address[1]
    threading.Thread(target=server.serve_forever, daemon=True).start()
    args = [str(browser), f'--app=http://127.0.0.1:{port}/?t={token}', f'--user-data-dir={profile}',
            '--window-size=1280,820', '--no-first-run', '--no-default-browser-check', '--disable-sync',
            '--disable-extensions', '--disable-features=Translate,msEdgeTranslate,msImplicitSignin,msEdgeSidebarV2',
            '--disable-background-networking']
    try:
        process = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except OSError as error:
        print(f'launcher: {browser.name} did not start: {error}')
        server.shutdown()
        return False
    backend.browser = process
    backend.browser_pid = process.pid
    try:
        lock.write_text(json.dumps({'pid': os.getpid(), 'browser': process.pid, 'port': port}), encoding='utf-8')
    except OSError:
        pass
    started = time.time()
    process.wait()
    if time.time() - started < 4 and not backend.seen:  # the window never opened
        server.shutdown()
        print(f'launcher: {browser.name} closed at once; classic window')
        return False
    # The game may still be running: keep its log reader alive until it ends.
    while backend.running:
        time.sleep(1)
    server.shutdown()
    backend.save()
    try:
        lock.unlink()
    except OSError:
        pass
    return True
