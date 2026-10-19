# PROTOTYPE — native PC port

A native Windows port of **PROTOTYPE** from the PS4 *PROTOTYPE BIOHAZARD BUNDLE* (CUSA06545).
The game's own PS4 code runs directly on the PC's x86-64 CPU; a small runtime replaces the PS4
system libraries and the [shadPS4](https://github.com/shadps4-emu/shadPS4) video core turns the
PS4 GPU commands into Vulkan. It is built on
[The Order: 1886 port](https://github.com/mateussmeneses/theorder1886-port) (see [Credits](#credits)).

**Version 0.4.0 — first public release, October 19, 2026.**
[Download it from Releases](https://github.com/petrishevgeorgii-debug/prototype-pc-slop/releases/latest).

**This repository and its releases contain no game files.** You need your own legally obtained,
decrypted dump of CUSA06545 (version 01.00). Nothing in your game folder is ever modified: every
fix is applied in memory, and the port checks the executables against the untouched originals.

## Status

| | |
|---|---|
| PROTOTYPE | Playable; tested through the early missions: saving, loading, cutscenes, videos, combat, consume, HUD and map |
| PROTOTYPE 2 | Not yet: choosing it in the bundle menu ends the game with a message |
| Platform | Windows 10 / 11, x86-64, a Vulkan 1.3 graphics card with a current driver |
| AMD Radeon | Tested (RX 9070 XT) |
| NVIDIA, Intel | **Untested** — reports welcome ([Issues](https://github.com/petrishevgeorgii-debug/prototype-pc-slop/issues), with `last_run.log`) |

## Features

**Fixes for running the PS4 game on a PC**

- **Game speed** — the game assumes the PS4's fixed 1.59 GHz timestamp counter; the port's counter ticks at exactly that rate.
- **Saving** — game fibers run on their own 1 MiB stacks (the game gives its save fiber 16 KiB).
- **White consume tendrils** — the dummy vertex stream is zero-filled, as the PS4 GPU reads it.
- **Frame pacing** — presentation capped at the PS4's 60 Hz.

**Improvements over the PS4 version**

- **60 FPS** (optional) — gameplay at 60; videos and menu/UI animation keep their normal speed.
- **Render resolution** — 1280 × 720 up to 3840 × 2160 (above 1080p is experimental); the HUD is drawn at the same resolution.
- **FSR 3.1 and FSR 4** temporal anti-aliasing and upscaling: *Native AA*, *Quality*, *Balanced*,
  *Performance*, *Ultra Performance*, switchable while playing. FSR 4 needs a Radeon RX 9000 GPU.
- **FSR 1** scaling to the window, **RCAS** sharpening, **anisotropic filtering** up to 16×.
- An **in-game menu drawn like the game's own options screens** (English or Russian with the game
  language), with keyboard, controller and mouse.
- **Keyboard and mouse** — a PC layout for PROTOTYPE's actions, remappable in the launcher, and
  the game's hints and tutorials show your keys instead of the PS4 buttons.
- A **launcher** (`Prototype.exe`): game folder check, settings, controls, saves and backups, the log.

## Playing

1. Download `PROTOTYPE-PC-Port-v0.4.0.zip` from [Releases](https://github.com/petrishevgeorgii-debug/prototype-pc-slop/releases/latest)
   and unpack it anywhere (or build the port, below).
2. Start **Prototype.exe**, choose your game folder (the one with `eboot.bin`, `sce_sys`,
   `sce_module`, `p1`, `p2`) and press **PLAY**. The first start prepares the game (about a
   minute); later starts are quick.

The launcher holds the render resolution, FSR 1 and sharpening, anisotropic filtering,
30 / 60 FPS, display mode, V-Sync, the frame counter and the keyboard and mouse controls.

### Keyboard and mouse

| Action | Keys | | Action | Keys |
|---|---|---|---|---|
| Move | W A S D | | Power wheel (hold) | Tab |
| Camera | Mouse | | Disguise menu (hold), vehicle weapon | F |
| Jump | Space | | Offensive / vision / defensive power | 1 / 2 / 3 |
| Sprint, glide (hold) | Left Shift | | Disguise | 4 or Z |
| Attack | Left click | | Reset camera | Middle click or V |
| Special attack, consume | Right click | | Pause, upgrades | Esc |
| Grab, throw | E | | Options button | M |
| Target lock, aim (hold) | Q | | Menus | Arrows, Enter, Backspace |

The launcher's **CONTROLS** page changes any of them (two keys per action), and sets the mouse
sensitivity and the **button icons in game**: *Keyboard & mouse*, *PlayStation*, or *Automatic*
(the device you played with last time). The keyboard prompts are made at
start from your own game files (the texts and the hint pictures) into `out\prompts`; the game
folder is not changed. A controller works at any time.

### Port menu

In game, **Insert** (keyboard) or **R3 + L2** (controller) opens the port menu:

| | Keyboard | Controller |
|---|---|---|
| Choose option | Up / Down | D-pad up / down |
| Change value | Left / Right, Enter | D-pad left / right, Cross |
| Next / previous section | Tab, E / Q | Square, R1 / L1 |
| Defaults of the section | Backspace | Triangle |
| Close | Esc | Circle |

Sections: **Upscaling** (FSR 3.1 / FSR 4, quality preset, FSR sharpening), **Image** (FSR 1,
sharpening), **Display** (window mode, V-Sync, frame counter) and **FSR tests** (switches and a
motion-vector view for problem reports). Choices are saved to `theorder.ini`.

Saves are kept in `user\savedata` in the port folder and backed up at every start to
`user\savedata_backups` (the launcher has buttons for both). The log of the last run is
`last_run.log`.

## Known issues

- PROTOTYPE 2 does not start yet.
- Render resolutions above 1080p are experimental.
- FSR 4.1 needs a Vulkan feature (`VK_VALVE_shader_mixed_float_dot_product`) that AMD's Windows
  driver does not offer yet; the menu hides it.
- Characters can ghost slightly with FSR (the port gives FSR the camera motion; animated objects
  have no motion vectors of their own yet).

## Reporting a problem

Open an [issue](https://github.com/petrishevgeorgii-debug/prototype-pc-slop/issues) with your GPU and driver,
what you did, and `last_run.log` from the port folder. For FSR problems, the menu's
**FSR tests → Save FSR images** writes three frames of FSR's inputs and output to `out\dump`.

## Roadmap

- PROTOTYPE 2
- Object motion vectors for FSR (less ghosting on characters)
- Higher shadow resolution and more draw distance
- NVIDIA and Intel testing and fixes

## Building

1. Install [MSYS2](https://www.msys2.org) to `C:\msys64`, open the **MSYS2 CLANG64** shell and
   install the toolchain:

   ```bash
   pacman -S --needed git patch \
     mingw-w64-clang-x86_64-{toolchain,cmake,ninja,pkgconf,python} \
     mingw-w64-clang-x86_64-{vulkan-headers,vulkan-loader,vulkan-memory-allocator,glslang} \
     mingw-w64-clang-x86_64-{sdl3,fmt,boost,robin-map,xxhash,ffmpeg,zydis} \
     mingw-w64-clang-x86_64-{spirv-tools,spirv-headers,spirv-cross}
   ```

2. Install a Windows [Python](https://www.python.org) 3 (for the launcher).
3. Clone with submodules and build:

   ```bat
   git clone --recursive https://github.com/petrishevgeorgii-debug/prototype-pc-slop.git
   cd prototype-pc-slop
   setup.bat
   ```

A release package is made with `bash packaging/windows/package_prototype.sh` in the MSYS2
CLANG64 shell after `setup.bat`: `dist/PROTOTYPE-PC-Port-v<version>.zip`. It refuses to package
any game file or anything made from one.

`setup.bat` builds the engine (`build.sh`, output in `out\`) and the launcher
(`Prototype.exe`, PyInstaller in its own environment `out\pyenv`), and puts a shortcut on the
desktop. Run it again after pulling updates.

Without the launcher: `python run.py` with `THEORDER_GAME_DIR` set to the game folder
(environment variables are described in `docs/upstream-theorder/README.md`).

## How it works

| Part | Where | What |
|---|---|---|
| Preparation | `scripts/prepare.py`, `link_libc.py`, `link_modules.py` | Reads the PS4 executable and the game's own `libc.prx`/`libSceFios2.prx` from your dump and links them into one native image |
| Loader | `src/probe.c` | Maps the image, binds imports, starts the game's code |
| Runtime | `src/runtime_*.c` | The PS4 system functions: memory, threads, files, audio, controller, saves, services; `runtime_proto.c` holds what PROTOTYPE needs beyond The Order (fibers, POSIX semaphores, PlayGo, local time, LoadExec) |
| GPU | `gpu/shadps4/` | The shadPS4 video core with the God of War III and The Order ports' work |
| Patches | `patches/Prototype-Orbis.xml`, `scripts/patches.py` | Applied in memory; each patch checks the original bytes first |
| Launcher | `launcher/prototype_launcher.py` | Settings, game folder checks, starts `run.py` |

More detail: [docs/PROTOTYPE.md](docs/PROTOTYPE.md).

## Credits

See [CREDITS.md](CREDITS.md). In short: this port is a fork of Mateus Meneses'
[The Order: 1886 port](https://github.com/mateussmeneses/theorder1886-port), itself built on
Felipe Prado's [God of War III port](https://github.com/FelipePrado0/gow3_pc) and deadinside28's
[bloodborne_pc](https://github.com/deadinside28/bloodborne_pc), with the
[shadPS4](https://github.com/shadps4-emu/shadPS4) video core.

PROTOTYPE is a trademark of its owners. This is an unofficial fan project, not affiliated with or
endorsed by Activision, Radical Entertainment or Sony Interactive Entertainment.

## License

GNU General Public License v2 or later — see [LICENSE](LICENSE). Third-party components keep
their own licenses (listed in [CREDITS.md](CREDITS.md)).
