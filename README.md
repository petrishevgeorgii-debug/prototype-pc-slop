# PROTOTYPE — native PC port

A native Windows port of **PROTOTYPE** from the PS4 *PROTOTYPE BIOHAZARD BUNDLE* (CUSA06545).
The game's own PS4 code runs directly on the PC's x86-64 CPU; a small runtime replaces the PS4
system libraries and the [shadPS4](https://github.com/shadps4-emu/shadPS4) video core turns the
PS4 GPU commands into Vulkan. It is built on
[The Order: 1886 port](https://github.com/mateussmeneses/theorder1886-port) (see [Credits](#credits)).

> **First release: October 21, 2026.** Until then this is a work in progress; build it from
> source if you want to try it early.
>
> **Tested on AMD Radeon only. NVIDIA GPUs are untested** — reports (with `last_run.log`) are
> welcome in Issues.

**This repository contains no game files.** You need your own legally obtained, decrypted dump of
CUSA06545. Nothing in your game folder is ever modified: every fix is applied in memory.

## Status

| | |
|---|---|
| PROTOTYPE | Playable: tested through the early missions (saving, loading, cutscenes, videos) |
| PROTOTYPE 2 | Not yet (the bundle menu's choice ends the game with a message) |
| Platform | Windows 10/11, x86-64, Vulkan 1.3 GPU |
| GPUs | AMD Radeon: tested. NVIDIA: untested. Intel: untested |
| Release | October 21, 2026 |

Fixes built into the port:

- **Game speed** — the game assumes the PS4's fixed 1.59 GHz timestamp counter; the port's counter ticks at exactly that rate.
- **Saving** — game fibers run on their own 1 MiB stacks (the game gives its save fiber 16 KiB).
- **White consume tendrils** — the dummy vertex stream is zero-filled, as the PS4 GPU reads it.
- **60 FPS** (optional) — gameplay at 60; videos and menu/UI animation keep their normal speed.
- **Frame pacing** — presentation capped at the PS4's 60 Hz.

## Playing

1. Build the port (below), or use a release package.
2. Start **Prototype.exe**, choose your game folder (the one with `eboot.bin`, `sce_sys`,
   `sce_module`, `p1`, `p2`) and press **PLAY**.

The launcher holds the settings: render resolution (experimental), FSR 1 upscaling and
sharpening, anisotropic filtering, 30/60 FPS, display mode, V-Sync, frame counter.

In game, **Insert** (keyboard) or **R3 + L2** (controller) opens the port menu, drawn like the
game's own options screens:

| | Keyboard | Controller |
|---|---|---|
| Choose option | Up / Down | D-pad up / down |
| Change value | Left / Right, Enter | D-pad left / right, Cross |
| Next / previous section | Tab, E / Q | Square, R1 / L1 |
| Defaults of the section | Backspace | Triangle |
| Close | Esc | Circle |

Sections: **Upscaling** (FSR 3.1, FSR 4 and FSR 4.1 temporal anti-aliasing at the render
resolution — experimental; FSR 4/4.1 need a Radeon RX 9000 GPU), **Image** (FSR 1, sharpening),
**Display**. Choices are saved to `theorder.ini`.

Saves are kept in `user\savedata` in the port folder and backed up at every start to
`user\savedata_backups` (the launcher has buttons for both). The log of the last run is
`last_run.log`.

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
   git clone --recursive https://github.com/Glebos222/prototype-pc-port.git
   cd prototype-pc-port
   setup.bat
   ```

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
