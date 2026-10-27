#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prepares the game image and starts the native probe (Windows and Linux; run.sh wraps it on Linux).

Environment variables (THEORDER_GAME_DIR, THEORDER_DATA_DIR,
THEORDER_PATCHES, THEORDER_MODS_* / THEORDER_MODS_*, THEORDER_USER_DIR, ...).
Extra arguments go to the game probe.
The in-game "Apply and restart" runs this script again through THEORDER_RESTART_COMMAND;
`--after PID` waits for the previous game process to end first (its GPU device and memory).
"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import time

WINDOWS = sys.platform == 'win32'
EXE_SUFFIX = '.exe' if WINDOWS else ''
if WINDOWS:
    import ctypes

PORT = Path(__file__).resolve().parent


def fail(message):
    print(message, file=sys.stderr)
    sys.exit(1)


def wait_for(pid):
    if not WINDOWS:
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                return
            except PermissionError:
                pass
            time.sleep(0.1)
        return
    kernel32 = ctypes.windll.kernel32
    SYNCHRONIZE = 0x00100000
    handle = kernel32.OpenProcess(SYNCHRONIZE, False, pid)
    if handle:
        kernel32.WaitForSingleObject(handle, 30000)
        kernel32.CloseHandle(handle)


def no_console():
    """Started without a console (the launcher): console programs must not open their own."""
    if not WINDOWS:
        return 0
    return 0 if ctypes.windll.kernel32.GetConsoleWindow() else subprocess.CREATE_NO_WINDOW


def parse_cpu_features(output):
    """Instruction sets `theorder-probe --cpu-features` reports missing ([] when complete)."""
    for line in output.splitlines():
        if line.startswith('CPU: missing '):
            return [name.strip() for name in line[len('CPU: missing '):].split(',') if name.strip()]
    return []


def isa_rewrite_wanted(missing, env):
    """THEORDER_ISA_REWRITE=1 forces the ISA rewrite table, 0 disables it; otherwise the CPU decides."""
    policy = get_env(env, 'ISA_REWRITE')
    if policy in ('0', '1'):
        return policy == '1'
    return bool(missing)


def run_script(name, *args, capture=False, check=True):
    # Inside the packaged launcher executable (PyInstaller) the executable runs scripts itself.
    script = ['--script'] if getattr(sys, 'frozen', False) else []
    command = [sys.executable, *script, str(PORT / 'scripts' / name), *map(str, args)]
    env = {**os.environ, 'PYTHONIOENCODING': 'utf-8'}
    if capture:
        result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, text=True,
                                encoding='utf-8', env=env, creationflags=no_console())
        if not check:
            return result.returncode, result.stdout
        if result.returncode:
            sys.exit(result.returncode)
        return result.stdout
    # stdin given: the output handles are passed explicitly (a windowed launcher child would
    # get none otherwise).
    result = subprocess.run(command, stdin=subprocess.DEVNULL, env=env, creationflags=no_console())
    if result.returncode:
        sys.exit(result.returncode)
    return None


def launch_prefix(env):
    """Linux integrations chosen in the launcher: MangoHud is its Vulkan layer (an environment
    variable), GameMode a command that starts the game."""
    if WINDOWS:
        return []
    if get_env(env, 'MANGOHUD') == '1':
        env.setdefault('MANGOHUD', '1')
    if get_env(env, 'GAMEMODE') == '1':
        if gamemode := shutil.which('gamemoderun'):
            return [gamemode]
        print('GameMode: gamemoderun not found, starting without it', flush=True)
    return []


def get_env(env, key, default=None):
    """The environment variable THEORDER_<key>, or default."""
    return env.get(f'THEORDER_{key}', default)


def find_executable(name):
    """bin/ (packaged), else out/ (built with build.sh)."""
    for candidate in (PORT / 'bin' / name, PORT / 'out' / name):
        if candidate.is_file():
            return candidate
    return None


PS4_TO_ENGINE_LANG = {
    0: 18, 1: 0, 2: 2, 3: 5, 4: 4, 5: 3, 6: 6, 7: 7, 8: 8,
    9: 19, 10: 21, 11: 20, 12: 12, 13: 10, 14: 13, 15: 11,
    16: 9, 17: 17, 18: 1, 19: 15, 20: 16
}

LANG_CODE_TO_PS4 = {
    'ja': 0, 'japanese': 0,
    'en': 1, 'en-us': 1, 'us': 1, 'english': 1,
    'fr': 2, 'french': 2,
    'es': 3, 'es-es': 3, 'spanish': 3,
    'de': 4, 'german': 4,
    'it': 5, 'italian': 5,
    'nl': 6, 'dutch': 6,
    'pt-pt': 7,
    'ru': 8, 'russian': 8,
    'ko': 9, 'korean': 9,
    'zh-hant': 10, 'zh-hans': 11,
    'fi': 12, 'finnish': 12,
    'sv': 13, 'swedish': 13,
    'da': 14, 'danish': 14,
    'no': 15, 'norwegian': 15,
    'pl': 16, 'polish': 16,
    'pt': 17, 'pt-br': 17, 'br': 17, 'portuguese': 17,
    'en-gb': 18, 'uk': 18,
    'tr': 19, 'turkish': 19,
    'es-419': 20, 'es-la': 20, 'la': 20,
    'ar': 21, 'arabic': 21
}


def resolve_language(cli_lang, env, ini_path, graphics_options=None):
    for candidate in (cli_lang, get_env(env, 'LANGUAGE')):
        if candidate is not None:
            val = str(candidate).strip().lower()
            if val in LANG_CODE_TO_PS4:
                return LANG_CODE_TO_PS4[val]
            if val.isdigit():
                return int(val)
    if graphics_options:
        ini_vals = graphics_options.read_ini(ini_path)
        if 'language' in ini_vals:
            val = str(ini_vals['language']).strip().lower()
            if val in LANG_CODE_TO_PS4:
                return LANG_CODE_TO_PS4[val]
            if val.isdigit():
                return int(val)
    try:
        appdata = os.environ.get('APPDATA')
        if appdata:
            s_file = Path(appdata) / 'theorder1886-launcher' / 'settings.json'
            if s_file.is_file():
                s_data = json.loads(s_file.read_text(encoding='utf-8'))
                if 'language' in s_data:
                    val = str(s_data['language']).strip().lower()
                    if val in LANG_CODE_TO_PS4:
                        return LANG_CODE_TO_PS4[val]
                    if val.isdigit():
                        return int(val)
    except Exception:
        pass
    try:
        import locale
        loc = (locale.getdefaultlocale()[0] or '').lower()
        if any(p in loc for p in ('pt', 'brazil', 'portuguese')):
            return 17
        if any(p in loc for p in ('es', 'spanish')):
            return 20
        if any(p in loc for p in ('fr', 'french')):
            return 2
        if any(p in loc for p in ('de', 'german')):
            return 4
        if any(p in loc for p in ('it', 'italian')):
            return 5
        if any(p in loc for p in ('ru', 'russian')):
            return 8
        if any(p in loc for p in ('pl', 'polish')):
            return 16
    except Exception:
        pass
    return 1


def gamepad_connected(probe):
    """Whether SDL sees a gamepad now (probe --gamepad prints 'Gamepad: none' without one)."""
    try:
        result = subprocess.run([str(probe), '--gamepad'], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL, text=True, timeout=10, creationflags=no_console())
    except (OSError, subprocess.TimeoutExpired):
        return False
    return any(line.startswith('Gamepad:') and line.strip() != 'Gamepad: none'
               for line in result.stdout.splitlines())


def prototype_controls(env, probe, game, out, derived, prompts=True):
    """PROTOTYPE's keyboard and mouse layout (THEORDER_PROTO_CONTROLS: the launcher's changes,
    JSON) for the runtime, and the button prompts: THEORDER_PROMPTS=keyboard shows the keys in
    the game's texts and hints (files made from the game's own, scripts/proto_prompts.py),
    gamepad keeps the PS4 buttons, auto (default) follows the device last played with (or, the first
    time, keyboard when no gamepad is connected)."""
    import proto_controls
    full = proto_controls.layout(get_env(env, 'PROTO_CONTROLS', ''))
    if not get_env(env, 'KEY_MAP'):
        env['THEORDER_KEY_MAP'] = proto_controls.key_map_spec(full)
        derived.append('THEORDER_KEY_MAP')
    # The runtime notes the device last played with (src/runtime_pad.c note_device).
    last_input = out / 'last_input.txt'
    env['THEORDER_LAST_INPUT'] = str(last_input)
    mode = get_env(env, 'PROMPTS', 'auto')
    if mode not in ('keyboard', 'gamepad'):
        try:
            mode = last_input.read_text(encoding='utf-8').strip()
        except OSError:
            mode = ''
        if mode not in ('keyboard', 'gamepad'):
            mode = 'gamepad' if gamepad_connected(probe) else 'keyboard'
    print(f'Button prompts: {"keyboard and mouse" if mode == "keyboard" else "controller"}', flush=True)
    if mode != 'keyboard' or not prompts:
        return
    import proto_prompts
    try:
        root = proto_prompts.generate(game, out / 'prompts', full, log=lambda m: print(m, flush=True))
    except (OSError, ValueError, ImportError) as error:
        print(f'Prompts: keyboard prompts unavailable ({error})', flush=True)
        return
    if root:
        env['THEORDER_FILE_OVERRIDES'] = str(Path(root).resolve())
        derived.append('THEORDER_FILE_OVERRIDES')


def prototype_menu(env, game, out, derived):
    """PROTOTYPE: the port's settings in the game's own Options > Video and Controller screens and a
    Display screen (files made from the game's own, scripts/proto_menu.py). THEORDER_GAME_MENU=0
    keeps the game's screens."""
    if get_env(env, 'GAME_MENU', '1') == '0':
        return
    import proto_menu
    roots = [r for r in env.get('THEORDER_FILE_OVERRIDES', '').split(';') if r]
    menu_root = str((out / 'menu').resolve())
    others = [r for r in roots if r != menu_root]
    import graphics_options
    language = resolve_language(None, env, env['THEORDER_CONFIG'], graphics_options)
    try:
        root = proto_menu.generate(game, out / 'menu', log=lambda m: print(m, flush=True), base_roots=others,
                                   language=language)
    except (OSError, ValueError, ImportError) as error:
        print(f'Menu: the game\'s options screens stay ({error})', flush=True)
        return
    # The Display screen (loose game files): Display mode and V-Sync move there from Video.
    env['THEORDER_GAME_MENU_DISPLAY'] = '1' if root and proto_menu.made_display(root) else '0'
    if 'THEORDER_GAME_MENU_DISPLAY' not in derived:
        derived.append('THEORDER_GAME_MENU_DISPLAY')
    if root:
        # First: an archive's patch here includes the prompts' patch of it.
        env['THEORDER_FILE_OVERRIDES'] = ';'.join([menu_root, *others])
        if 'THEORDER_FILE_OVERRIDES' not in derived:
            derived.append('THEORDER_FILE_OVERRIDES')


# The Discord application whose name ("PROTOTYPE") the Discord status shows, with its "logo" image
# (theorder.ini discord_app_id overrides it; discord=0 turns the status off).
DISCORD_APP_ID = '1564496546521808986'


def prototype_discord(env, game, out, derived):
    """PROTOTYPE: the Discord status (src/runtime_discord.c): its application and the story
    missions' names in the game's language (scripts/proto_presence.py)."""
    import graphics_options
    ini = graphics_options.read_ini(env['THEORDER_CONFIG'])
    app = ini.get('discord_app_id', '').strip() or DISCORD_APP_ID
    if ini.get('discord', '1') == '0' or get_env(env, 'DISCORD', '1') == '0':
        print('Discord: status off (the setting)', flush=True)
        return
    if not app:
        print('Discord: no application id (run.py DISCORD_APP_ID or theorder.ini discord_app_id)', flush=True)
        return
    env['THEORDER_DISCORD_APP_ID'] = app
    derived.append('THEORDER_DISCORD_APP_ID')
    try:
        import proto_presence
        language = resolve_language(None, env, env['THEORDER_CONFIG'], graphics_options)
        names = proto_presence.generate(game, out / 'discord_missions.txt', language,
                                        log=lambda m: print(m, flush=True))
    except (OSError, ValueError, ImportError) as error:
        print(f'Discord: no mission names ({error})', flush=True)
        names = None
    if names:
        env['THEORDER_DISCORD_MISSIONS'] = str(names)
        derived.append('THEORDER_DISCORD_MISSIONS')


def find_probe_executable():
    """Finds the probe executable (theorder-probe)."""
    return find_executable('theorder-probe' + EXE_SUFFIX)


def main():
    args = sys.argv[1:]
    if len(args) >= 2 and args[0] == '--after':
        wait_for(int(args[1]))
        args = args[2:]
    cli_lang = None
    cli_costume = None
    translated_args = []
    i = 0
    while i < len(args):
        if args[i] in ('--startlevel', '--level') and i + 1 < len(args):
            translated_args.extend(['--guest-arg', '-startlevel', '--guest-arg', args[i + 1]])
            i += 2
        elif args[i] in ('--startcheckpoint', '--checkpoint') and i + 1 < len(args):
            translated_args.extend(['--guest-arg', '-startcheckpoint', '--guest-arg', args[i + 1]])
            i += 2
        elif args[i] in ('--no-cloth', '--disablecloth'):
            translated_args.extend(['--guest-arg', '-disablecloth'])
            i += 1
        elif args[i] in ('--no-cam-springs', '--nocamsprings'):
            translated_args.extend(['--guest-arg', '-nocamsprings'])
            i += 1
        elif args[i] in ('--no-cam-additive', '--nocamadditive'):
            translated_args.extend(['--guest-arg', '-nocamadditive'])
            i += 1
        elif args[i] == '--msaa' and i + 1 < len(args):
            translated_args.extend(['--guest-arg', '-msaa', '--guest-arg', args[i + 1]])
            i += 2
        elif args[i] == '--frontend':
            translated_args.extend(['--guest-arg', '-frontend'])
            i += 1
        elif args[i] == '--perf':
            translated_args.extend(['--guest-arg', '-disablecloth', '--guest-arg', '-nocamsprings', '--guest-arg', '-nocamadditive', '--guest-arg', '-msaa', '--guest-arg', '1'])
            i += 1
        elif args[i] in ('--language', '--lang', '-l') and i + 1 < len(args):
            cli_lang = args[i + 1]
            i += 2
        elif args[i] in ('--costume', '--dlc-costume') and i + 1 < len(args):
            cli_costume = args[i + 1]
            i += 2
        else:
            translated_args.append(args[i])
            i += 1
    args = translated_args
    # As run.sh: relative paths (THEORDER_GAME_DIR, etc.) are from the port.
    os.chdir(PORT)
    env = os.environ
    data = Path(get_env(env, 'DATA_DIR', PORT))
    out = data / 'out'
    out.mkdir(parents=True, exist_ok=True)
    config_default = data / 'theorder.ini'
    # The settings of releases up to 0.2.0 (gow3.ini) move to theorder.ini once.
    if not config_default.exists() and (data / 'gow3.ini').is_file():
        try:
            (data / 'gow3.ini').replace(config_default)
        except OSError:
            config_default = data / 'gow3.ini'
    env.setdefault('THEORDER_CONFIG', get_env(env, 'CONFIG', str(config_default)))
    # Values this script derived on the previous start ("Apply and restart" inherits them) are
    # derived again from the new options.
    for key in env.pop('THEORDER_DERIVED', '').split(','):
        env.pop(key, None)
    derived = []
    sys.path.insert(0, str(PORT / 'scripts'))
    import graphics_options
    graphics, reverted = graphics_options.prepare(env['THEORDER_CONFIG'], out / 'graphics_state.json')
    if reverted:
        print('Graphics: the last restart never reached the game; previous options restored', flush=True)
    env.update(graphics_options.environment(graphics))
    # PROTOTYPE's render resolution and frame rate: the launcher writes them to theorder.ini at
    # every start and the game's Display screen changes them there ("Apply: restart now" starts
    # this script again with the launcher's old environment): the file wins.
    ini = graphics_options.read_ini(env['THEORDER_CONFIG'])
    if ini.get('proto_res'):
        env['THEORDER_PROTO_RES'] = ini['proto_res']
    if ini.get('proto_fps60') in ('0', '1'):
        env['THEORDER_FPS60'] = ini['proto_fps60']
    # THEORDER_SAFE_GPU (tests, PROTOTYPE 2 bring-up): 1 = the GPU layer's optimizations, early
    # readbacks and async shaders off; settings = only the readbacks and async shaders off.
    # THEORDER_GPU_TOGGLES=<mask>: these optimizations off (bits: src/runtime_memory.c and
    # gpu/shim/port_toggles.h).
    safe_gpu = get_env(env, 'SAFE_GPU')
    toggle_mask = get_env(env, 'GPU_TOGGLES') or ('4468274514573098427' if safe_gpu == '1' else '')
    if safe_gpu in ('1', 'settings'):
        env['THEORDER_DEFERRED_READBACK'] = '0'
        env['THEORDER_STALE_READBACK'] = '0'
        env['THEORDER_ASYNC_SHADERS'] = '0'
        print('GPU test: deferred readbacks and async shaders off', flush=True)
    if toggle_mask:
        toggles = out / 'gpu_test_toggles.txt'
        toggles.write_text(toggle_mask + '\n', encoding='utf-8')
        env['THEORDER_TOGGLE_FILE'] = str(toggles)
        print(f'GPU test: optimizations off (mask {toggle_mask})', flush=True)
    if not get_env(env, 'FSR411_DIR') and not (PORT / 'fsr4_411').is_dir() and (data / 'fsr4_411').is_dir():
        env['THEORDER_FSR411_DIR'] = str(data / 'fsr4_411')

    probe_env = get_env(env, 'PROBE')
    probe = Path(probe_env) if probe_env else find_probe_executable()
    if not probe or not probe.is_file():
        fail(f'theorder-probe{EXE_SUFFIX} / theorder-probe{EXE_SUFFIX} not found: build it with build.sh or use a packaged build.')
    # Development builds run from the MSYS2 tree: its CLANG64 DLLs (SDL3, FFmpeg, ...).
    if WINDOWS and not (probe.parent / 'SDL3.dll').is_file():
        clang64 = Path(env.get('MSYS2_ROOT', r'C:\msys64')) / 'clang64' / 'bin'
        if clang64.is_dir():
            env['PATH'] = f'{clang64}{os.pathsep}{env.get("PATH", "")}'
    # Instruction sets this CPU lacks (MOVBE, BMI1, LZCNT): the ISA rewrite covers them; exit 20
    # is a set nothing covers.
    features = subprocess.run([str(probe), '--cpu-features'], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                              text=True, creationflags=no_console())
    if features.returncode == 20:
        print(features.stdout, end='', flush=True)
        sys.exit(20)
    missing = parse_cpu_features(features.stdout)

    candidates = [
        get_env(env, 'GAME_DIR'),
        PORT.parent / 'CUSA06545',
        PORT.parent / 'CUSA00035',
    ]
    game = None
    for c in candidates:
        if c and (Path(c) / 'eboot.bin').is_file():
            game = Path(c)
            break
    if not game:
        game = Path(get_env(env, 'GAME_DIR', PORT.parent / 'CUSA00035'))
    if not (game / 'eboot.bin').is_file():
        fail(f'No eboot.bin in {game} (set THEORDER_GAME_DIR).')
    update_view = None
    patch_dir = get_env(env, 'PATCH_DIR')
    if patch_dir:
        import game_update
        try:
            update_view = game_update.create_view(game, patch_dir, out)
        except (ValueError, OSError) as error:
            fail(str(error))
        game = update_view
        print(f'Game and update combined from {patch_dir}', flush=True)
    original_game = game.resolve()
    game = Path(run_script('mods.py', game, '--out', out,
                           '--mods-dir', get_env(env, 'MODS_DIR', str(data / 'mods')),
                           '--config', get_env(env, 'MODS_CONFIG', str(data / 'mods.json')),
                           '--enabled', get_env(env, 'MODS_ENABLED', '1'), capture=True).strip())
    mod_view = game if game.resolve() != original_game else None
    # THEORDER_EBOOT: the executable to run (PROTOTYPE 2 is prototype2_ps4f.elf beside eboot.bin).
    # Each executable is prepared in its own folder, so switching games does not redo the other's.
    exe = get_env(env, 'EBOOT', 'eboot.bin')
    if not (game / exe).is_file():
        fail(f'No {exe} in {game}.')
    env['THEORDER_EBOOT'] = exe
    image_out = out if exe == 'eboot.bin' else out / Path(exe).stem
    image_out.mkdir(parents=True, exist_ok=True)
    next_exe = None
    try:
        run_script('prepare.py', game, '--out', image_out)
        run_script('link_libc.py', game, '--out', image_out)
        run_script('link_modules.py', game, '--out', image_out)
        # A failed rewrite prints its reason and the game starts without the table.
        isa_args = []
        if isa_rewrite_wanted(missing, env):
            code, output = run_script('isa_rewrite.py', image_out / 'boot-linked.bin', '--out', image_out,
                                      capture=True, check=False)
            print(output, end='', flush=True)
            if code == 0:
                isa_args = ['--isa-rewrite', str(image_out / 'isa-rewrite.bin')]
        run_script('content_profile.py', game, '--out', image_out, '--sku', get_env(env, 'CONTENT_SKU', 'full'))

        import patches
        from patches import game_profile_for, patch_requirements, patch_status, selected_patches
        profile = game_profile_for(game)
        is_prototype = bool(profile) and profile[3].startswith('PROTOTYPE')
        is_prototype2 = is_prototype and exe != 'eboot.bin'
        # The built-in patches of this executable (PROTOTYPE 2 has a file of its own).
        patch_xml = patches.patch_file(profile, exe)
        # PROTOTYPE 2's launcher settings: THEORDER_PROTO2_RES and THEORDER_PROTO2_FPS60 (PROTOTYPE:
        # THEORDER_PROTO_RES and THEORDER_FPS60), so a start from the bundle menu gets its own.
        res_key, fps_key = ('PROTO2_RES', 'PROTO2_FPS60') if is_prototype2 else ('PROTO_RES', 'FPS60')
        if is_prototype2:
            print('PROTOTYPE 2 (experimental): keyboard button icons are PROTOTYPE 1 only', flush=True)
            # Addresses in PROTOTYPE 1's image (a start from its bundle menu still has them).
            env.pop('THEORDER_FOV_SITES', None)
            # The GPU layer's draw preparation on worker threads reads each draw's resource tables
            # ahead of the GPU thread; PROTOTYPE 2 fills its vertex tables later, and the early
            # copies made geometry explode. Off for it (4096 + 8192) unless a test sets its own.
            if not get_env(env, 'GPU_TOGGLES') and get_env(env, 'SAFE_GPU') != '1':
                toggles = out / 'gpu_test_toggles.txt'
                toggles.write_text('12288\n', encoding='utf-8')
                env['THEORDER_TOGGLE_FILE'] = str(toggles)
                derived.append('THEORDER_TOGGLE_FILE')
        engine_fps, fps_patched = 60, False
        # The window shows the game's icon (BMP: the format SDL loads without extra libraries); on
        # Linux the launcher's own icon (the AppImage ships the one chosen when it was built) and
        # an application id the desktop entry matches, so the shell shows the same icon.
        icon_file = game / 'sce_sys' / 'icon0.png'
        if not WINDOWS:
            env.setdefault('SDL_APP_ID', 'theorder')
            if (PORT / 'launcher' / 'theorder.png').is_file():
                icon_file = PORT / 'launcher' / 'theorder.png'
        try:
            from PIL import Image
            with Image.open(icon_file) as icon:
                icon.convert('RGBA').resize((64, 64), Image.LANCZOS).save(out / 'window_icon.bmp')
            env.setdefault('THEORDER_WINDOW_ICON', str(out / 'window_icon.bmp'))
        except (ImportError, OSError):
            pass
        # The temporal upscalers and motion vectors are not calibrated for this game yet: they stay
        # off unless the launcher's experimental switch is on; linear image readbacks fix textures.
        if is_prototype:
            # PROTOTYPE: FSR 3.1 / 4 / 4.1 on its scene (in-game menu, theorder.ini): Native AA
            # or a quality preset; off until chosen.
            env['THEORDER_GAME'] = 'prototype'
            if 'upscaler' not in graphics_options.read_ini(env['THEORDER_CONFIG']):
                graphics_options.write_keys(env['THEORDER_CONFIG'], {'upscaler': 'off'})
            prototype_controls(env, probe, game, out, derived, prompts=not is_prototype2)
            if not is_prototype2:
                prototype_menu(env, game, out, derived)
                prototype_discord(env, game, out, derived)
        elif get_env(env, 'EXPERIMENTAL_UPSCALER') != '1':
            env.setdefault('THEORDER_UPSCALER', 'none')
        env.setdefault('THEORDER_READBACK_LINEAR', '1')
        patches_val = get_env(env, 'PATCHES', '')
        patches_only = get_env(env, 'PATCHES_ONLY') == '1'
        if is_prototype:
            # The render resolution and 60 FPS patches come from the settings below, never from the
            # list: a restart from the game's Display screen inherits the previous start's list,
            # whose old resolution would otherwise stay (or come twice).
            patches_val = ';'.join(n for n in (n.strip() for n in patches_val.split(';'))
                                   if n and not n.startswith('Render Resolution - ')
                                   and n not in ('Prototype 60 FPS', 'Prototype 2 60 FPS'))
        if profile and patch_xml:
            names = selected_patches(patch_xml, profile[2], patches_val, patches_only)
            names = graphics_options.patch_names(names, graphics)
            if is_prototype:  # The Order's engine frame rate patches: PROTOTYPE has its own
                names = [n for n in names if not n.startswith(graphics_options.FPS_PREFIX)]
            if profile[3] == 'The Order 1886':
                for auto_patch in ('Unlock All Languages', 'Unlock The Armoury Menu'):
                    if auto_patch not in names:
                        names.append(auto_patch)
            # PROTOTYPE: THEORDER_FPS60=1 (launcher, run-60fps.bat) turns on the 60 FPS patch.
            fps_patch = 'Prototype 2 60 FPS' if is_prototype2 else 'Prototype 60 FPS'
            if is_prototype and get_env(env, fps_key) == '1' and fps_patch not in names:
                names.append(fps_patch)
            # THEORDER_PROTO_RES=<w>x<h>: the game's render resolution (launcher setting).
            proto_res = get_env(env, res_key, '') if is_prototype else ''
            if proto_res and proto_res != '1920x1080':
                names.append(f'Render Resolution - {proto_res}')
            if is_prototype and not is_prototype2 and get_env(env, 'PROTO_TENDRIL_FIX', '1') == '1' and \
                    'Fix White Consume Tendrils' not in names:
                names.append('Fix White Consume Tendrils')
            names = graphics_options.restrict_to_available(names, patches.available_patches(patch_xml, profile[2]))
            env['THEORDER_PATCHES'] = ';'.join(names)
            env['THEORDER_PATCHES_ONLY'] = '1'
        (image_out / 'patch_status.json').unlink(missing_ok=True)
        run_script('patches.py', '--out', image_out, '--extra', get_env(env, 'PATCHES', ''), '--game-dir', game,
                   '--patches-dir', get_env(env, 'PATCHES_DIR', str(data / 'patches')),
                   '--patches-config', get_env(env, 'PATCHES_CONFIG', str(data / 'patches.json')))
        if profile and patch_xml:
            # Only the patches found in this eboot.bin count: a frame rate patch that was skipped
            # leaves the engine at 60 FPS, and the notes' memory and VBlank needs follow the same list.
            status = patch_status(image_out) or {}
            applied = status.get('applied', [])
            # The in-game menu's field of view writes these constants (probe.c set_fov_sites).
            env['THEORDER_FOV_SITES'] = ' '.join(hex(site) for site in status.get('fov_sites', []))
            engine_fps = graphics_options.applied_engine_fps(applied)
            fps_patched = any(n.startswith(f'{graphics_options.FPS_PREFIX} - ') for n in applied)
            dmem, vblank = patch_requirements(patch_xml, applied, profile[2])
            # The Order presents at 30 Hz (flip rate 1) and steps 33.3 ms per frame. Its 60 FPS patch
            # halves the step, so the presentation has to follow (every vblank, at most 60 FPS) or
            # the game runs in slow motion. Derived like the values below, so a restart back to 30 FPS
            # drops them (flip rate 0 without the patch doubles the game's speed).
            if profile[3] == 'The Order 1886' and f'{graphics_options.FPS_PREFIX} - 60 FPS' in applied:
                for key, value in (('FLIP_RATE', '0'), ('FPS_LIMIT', '60')):
                    if not get_env(env, key):
                        env[f'THEORDER_{key}'] = value
                        derived.append(f'THEORDER_{key}')
                print('The Order 60 FPS patch: presenting every vblank, at most 60 FPS', flush=True)
            for key, value in (('DMEM_MB', dmem), ('VBLANK_HZ', vblank)):
                if value and not get_env(env, key):
                    env[f'THEORDER_{key}'] = str(value)
                    derived.append(f'THEORDER_{key}')
        if not get_env(env, 'VBLANK_HZ'):
            env['THEORDER_VBLANK_HZ'] = '60'
            derived.append('THEORDER_VBLANK_HZ')
        # PROTOTYPE paces itself on the PS4's 60 Hz flips: without a cap the present thread
        # let it run at 70-120 FPS (menus and loading screens too fast).
        if is_prototype and not get_env(env, 'FPS_LIMIT'):
            env['THEORDER_FPS_LIMIT'] = '60'
            derived.append('THEORDER_FPS_LIMIT')
        # Without the frame rate patch the engine steps 1/60 s per frame: the present thread must
        # not flip faster, or the game speeds up.
        env['THEORDER_ENGINE_FPS'] = str(engine_fps)
        derived.append('THEORDER_ENGINE_FPS')
        # The Order runs at half its engine setting: 30 FPS at 60, 60 with the frame rate patch.
        if is_prototype:
            print(f"Game frame rate: {60 if get_env(env, fps_key) == '1' else 30} FPS", flush=True)
        else:
            print(f'Game frame rate: {engine_fps // 2} FPS'
                  + ('' if fps_patched or graphics['engine_fps'] == '60' else
                     ' (the frame rate patch was not found in this eboot.bin)'), flush=True)
        env['THEORDER_DERIVED'] = ','.join(derived)
        # The guest memory pool is committed in full at start: a resolution patch above 1080p
        # can need more than 16 GB of RAM, and the game then closes when the page file cannot grow.
        import guest_memory
        if warning := guest_memory.memory_warning(get_env(env, 'DMEM_MB'), guest_memory.physical_memory_mb(),
                                                  get_env(env, 'STREAM_MEMORY', 'cached')):
            print(warning, flush=True)

        # The in-game restart starts this script again once this process is gone.
        # GPU caches (shaders, pipelines) beside the saves; read before main(), so set here.
        user_dir = Path(get_env(env, 'USER_DIR', str(data / 'user'))).resolve()
        user_dir.mkdir(parents=True, exist_ok=True)
        env.setdefault('THEORDER_GPU_USER_DIR', str(user_dir))
        # Shader mods (docs/SHADER_MODS.md): mods/shaders and mods/<name>/shaders.
        env.setdefault('THEORDER_SHADER_MODS_DIR',
                       str(Path(get_env(env, 'MODS_DIR', str(data / 'mods'))).resolve()))
        from save_backup import backup
        try:
            if made := backup(user_dir):
                print(f'Saves backed up to {made}', flush=True)
        except OSError as error:
            print(f'Save backup failed, starting anyway: {error}', flush=True)
        this = ['--run'] if getattr(sys, 'frozen', False) else [str(Path(__file__).resolve())]
        # Windows: the probe starts the new launch and ends, so it waits for this process. Linux:
        # the probe replaces itself with the new launch while this process still waits for it.
        after = ['--after', str(os.getpid())] if WINDOWS else []
        restart = [sys.executable, *this, *after, *args]
        restart_cmd = subprocess.list2cmdline(restart) if WINDOWS else shlex.join(restart)
        env['THEORDER_RESTART_COMMAND'] = restart_cmd
        # Resolve language & pass guest arguments
        ps4_lang = resolve_language(cli_lang, env, env['THEORDER_CONFIG'], graphics_options)
        engine_lang = PS4_TO_ENGINE_LANG.get(ps4_lang, ps4_lang)
        env['THEORDER_LANGUAGE'] = str(ps4_lang)
        print(f'Language: PS4 id {ps4_lang} -> engine id {engine_lang}', flush=True)

        guest_lang_args = []
        if is_prototype:
            pass  # The Order's engine arguments mean nothing to PROTOTYPE.
        elif '-language' not in args:
            guest_lang_args.extend(['--guest-arg', '-language', '--guest-arg', str(engine_lang)])
        if not is_prototype and '-enablesubtitles' not in args:
            guest_lang_args.extend(['--guest-arg', '-enablesubtitles'])

        timeout_val = get_env(env, 'TIMEOUT', '0')
        command = [*launch_prefix(env), str(probe), str(image_out / 'boot-linked.bin'), '--content-profile', str(image_out / 'content.bin'),
                   '--patches', str(image_out / 'patches.bin'), '--app0', str(game),
                   '--user', str(user_dir),
                   '--timeout', timeout_val, *isa_args, *guest_lang_args, *args]
        (PORT / 'out' / 'loadexec.txt').unlink(missing_ok=True)
        # No stdin: an inherited pipe (shells such as Git Bash) cost the game its console output.
        code = subprocess.run(command, stdin=subprocess.DEVNULL, creationflags=no_console()).returncode
        if code == 42 and is_prototype:
            # sceSystemServiceLoadExec: one game started the other (the bundle menu PROTOTYPE 2, and
            # PROTOTYPE 2 the menu again); the runtime wrote the requested path (runtime_proto.c).
            try:
                requested = (PORT / 'out' / 'loadexec.txt').read_text(encoding='utf-8', errors='replace').strip()
            except OSError:
                requested = ''
            name = requested.replace('\\', '/').rsplit('/', 1)[-1]
            if name and (game / name).is_file() and get_env(env, 'NO_LOADEXEC') != '1':
                print(f'The game started {name}', flush=True)
                next_exe = name
            else:
                print(f'The game asked to start "{requested}", which is not in the game folder', flush=True)
        if not next_exe:
            return code
    finally:
        if mod_view:
            shutil.rmtree(mod_view, ignore_errors=True)
        if update_view:
            game_update.remove_view(update_view)
    env['THEORDER_EBOOT'] = next_exe
    return main()


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
