# SPDX-License-Identifier: GPL-2.0-or-later
"""PROTOTYPE launcher, the native window: launcher-ui (SDL3 + Dear ImGui, launcher/native).

The window draws the launcher (the pages of the web one, launcher/web, with their animations);
this process keeps the state and does every action (prototype_web.Backend: settings, keys, saves,
mods, the game folder, the start and the stop). They talk over the window's stdin and stdout, one
JSON object per line each way:

    window   -> {"id": 1, "action": "set", "args": {"key": "vsync", "value": true}}
    launcher -> {"id": 1, "ok": true, "result": {...}}      (or "ok": false, "error": "...")

Each request runs on a thread of its own (an update check can take seconds; the window keeps
asking for news meanwhile). Without launcher-ui (a build without it, or it does not start) the
web window opens instead, and without that the classic one.
"""
import ctypes
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time

import prototype_web


def find_ui(L):
    exe = 'launcher-ui.exe' if L.WINDOWS else 'launcher-ui'
    for candidate in (L.PORT_DIR / 'bin' / exe, L.PORT_DIR / 'out' / exe):
        if candidate.is_file():
            return candidate
    return None


def show_process_windows(pid):
    """Brings the windows of process `pid` back (restored, in front); Windows only."""
    if sys.platform != 'win32' or not pid:
        return False
    user32 = ctypes.windll.user32
    found = []
    proc = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def each(hwnd, _):
        owner = ctypes.c_ulong()
        user32.GetWindowThreadProcessId(ctypes.c_void_p(hwnd), ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(ctypes.c_void_p(hwnd)):
            found.append(hwnd)
        return True
    user32.EnumWindows(proc(each), 0)
    for hwnd in found:
        user32.ShowWindow(ctypes.c_void_p(hwnd), 9)  # SW_RESTORE
        user32.SetForegroundWindow(ctypes.c_void_p(hwnd))
    return bool(found)


def serve(backend, process):
    """Answers the window's requests until it closes its end; returns how many it asked."""
    write_lock = threading.Lock()
    asked = 0

    def answer(message):
        reply = {'id': message.get('id')}
        try:
            args = message.get('args') or {}
            reply['result'] = backend.act(str(message.get('action', '')), args if isinstance(args, dict) else {})
            reply['ok'] = True
        except Exception as error:  # the window shows it
            reply['ok'], reply['error'] = False, str(error) or error.__class__.__name__
        data = (json.dumps(reply, ensure_ascii=False) + '\n').encode('utf-8')
        with write_lock:
            try:
                process.stdin.write(data)
                process.stdin.flush()
            except (OSError, ValueError):
                pass

    for line in process.stdout:
        try:
            message = json.loads(line.decode('utf-8'))
        except ValueError:
            continue
        if not isinstance(message, dict):
            continue
        asked += 1
        threading.Thread(target=answer, args=(message,), daemon=True).start()
    return asked


def run(L):
    """Opens the native window; False when there is none or it does not start (the web window or
    the classic one opens instead)."""
    choice = os.environ.get('THEORDER_LAUNCHER', '').lower()
    if choice in ('classic', 'web') or '--classic' in sys.argv or '--web' in sys.argv:
        return False
    exe = find_ui(L)
    if not exe:
        print('launcher: no launcher-ui; web window')
        return False
    folder = L.CONFIG_DIR / 'window'
    folder.mkdir(parents=True, exist_ok=True)
    lock = folder / 'launcher.lock'
    try:  # one launcher at a time: a second start brings the first one's window back
        other = json.loads(lock.read_text(encoding='utf-8'))
        if prototype_web._pid_alive(other.get('ui')) and show_process_windows(other.get('ui')):
            return True
    except (OSError, ValueError):
        pass
    env = dict(os.environ)
    # Development builds run from the MSYS2 tree: its CLANG64 DLLs (SDL3, FreeType...), as run.py does.
    if L.WINDOWS and not (exe.parent / 'SDL3.dll').is_file():
        clang64 = Path(env.get('MSYS2_ROOT', r'C:\msys64')) / 'clang64' / 'bin'
        if not clang64.is_dir():
            print('launcher: launcher-ui has no DLLs (no MSYS2 CLANG64); web window')
            return False
        env['PATH'] = f'{clang64}{os.pathsep}{env.get("PATH", "")}'
    if L.WINDOWS:  # a missing DLL fails quietly (the web window opens) instead of with an error box
        ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002 | 0x8000)
    backend = prototype_web.Backend(L)
    backend.native = True
    try:
        errors = open(folder / 'launcher-ui.log', 'w', encoding='utf-8', errors='replace')
    except OSError:
        errors = subprocess.DEVNULL
    try:
        process = subprocess.Popen([str(exe)], cwd=L.PORT_DIR, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=errors)
    except OSError as error:
        print(f'launcher: launcher-ui did not start ({error}); web window')
        return False
    try:
        lock.write_text(json.dumps({'pid': os.getpid(), 'ui': process.pid}), encoding='utf-8')
    except OSError:
        pass
    started = time.time()
    asked = serve(backend, process)
    code = process.wait()
    try:
        lock.unlink()
    except OSError:
        pass
    if not asked and code and time.time() - started < 20:
        print(f'launcher: launcher-ui stopped at once (code {code}); web window')
        return False
    while backend.running:  # the window was closed while the game runs: its time still counts
        time.sleep(1)
    backend.save()
    return True
