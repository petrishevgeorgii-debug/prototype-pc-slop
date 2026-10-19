# PROTOTYPE port — technical notes

First public release: v0.4.0, October 19, 2026 ([release notes](releases/v0.4.0.md)).

Game: PROTOTYPE BIOHAZARD BUNDLE, CUSA06545, APP_VER 01.00. `eboot.bin` is PROTOTYPE (with the
bundle menu); `prototype2_ps4f.elf` is PROTOTYPE 2, started from the menu with
`sceSystemServiceLoadExec`. Engine: Radical's Pure3D (`.p3d`, `.rcf` data; Scaleform GFx UI; Bink
video).

Reference hashes (sha256) of the untouched executables:

| File | sha256 |
|---|---|
| `eboot.bin` | `664269bbadb9681d9e69db4306ae788435c67799d0867aeb635ce18137311473` |
| `prototype2_ps4f.elf` | `da64c2ce9b323de0ad903d0fb2afb842a6f41c19e1da400177d53375ae2f1df5` |

## Changes to The Order port's runtime

| What | Where | Why |
|---|---|---|
| Fibers (`_sceFiberInitializeImpl`, Run, Switch, ReturnToThread, Finalize) | `src/runtime_proto.c` | The save system runs on a fiber. Each fiber gets a 1 MiB host stack (Windows fibers with the whole stack committed, ucontext on Linux) instead of the 16 KiB the game provides: runtime code called from the fiber overflowed it under shadPS4. |
| POSIX semaphores (`sem_*`) | `src/runtime_proto.c` | Imported through libScePosix. |
| PlayGo language mask / to-do list, trophy icons, local-time conversion, module list, LoadExec | `src/runtime_proto.c` | Imports The Order never used. LoadExec ends the game with exit code 42 (PROTOTYPE 2 is not prepared yet). |
| `sceKernelReadTsc` / `GetTscFrequency` at 1,593,749,508 Hz | `src/runtime_kernel.c` | The game hardcodes the PS4 TSC rate for frame timing; a nanosecond counter made it run at 63% speed. |
| Import names | `src/import_names.inc` | NID → symbol table for this game's imports (names from shadPS4's aerolib). |
| Present cap 60 FPS | `run.py` (`THEORDER_FPS_LIMIT`) | The game paces itself on 60 Hz flips; without a cap menus ran at up to 120 FPS. |
| Quieter logs | `runtime_sema.c`, `runtime_pad.c`, `video_out.cpp` | The game creates thousands of semaphores and sets the flip rate every frame. |
| FSR 1 setting | `gpu/shim/port_settings.cpp` | Re-enabled as a launcher setting. |
| Save/load dialog | `gpu/shim/port_overlay.cpp` | Redesigned list (picture, title, location, date). |
| Selectable executable | `scripts/*.py` (`THEORDER_EBOOT`) | Groundwork for PROTOTYPE 2. |

## Patches (`patches/Prototype-Orbis.xml`)

Addresses are eboot virtual addresses; every line also carries the original bytes as its
pattern, so nothing is written to a different build.

- **Fix White Consume Tendrils** (on by default) — `0xc9b550`: the 16-byte fill pattern of the
  dummy vertex stream, `0xDEADBEEF` ×4 → zeros. The GPU layer reads past that stream where the PS4
  returns zero.
- **Prototype 60 FPS** — `0x227fb5`: the flip pacing block sets the flip rate every frame (30 Hz
  while a video plays, judged by the FMV queue count at `0x122f358`, else 60 Hz); `0x3ff64e` + cave
  at `0x5080`: Scaleform `GFxMovieRoot::Advance` gets half the frame time when no video plays, so
  menus and HUD animations keep their speed.
- **Render Resolution - WxH** (experimental) — `0x227b50`: the display device size (stored as
  `0x780`/`0x438` right after `sceVideoOutGetResolutionStatus`, which the game ignores), and
  `0xbe820`: the Pure3D display init parameters. The game settings block at `0x12361e0` must not
  be changed (a first version that did made loading a save run out of memory).

## Frame structure (for temporal upscaling)

From a RenderDoc capture of a gameplay frame: shadow cascades in a 2048×2048 D32S8 atlas; a depth
pre-pass into a 1920×1080 D32S8 target (vertex shader `0x10e04a6a`, one 4×4 matrix per draw at
constant offset 0); linear depth R32F; SSAO/lighting RGBA8 targets; the main colour pass into an
RGBA8 target sharing the scene depth (material shaders, e.g. `0xf1958b0b`, carry the object's
world-view-projection and world matrices); bloom chain at 960×540 and 480×270; composite and
Scaleform UI into the RGBA8 sRGB display buffer. The scene colour is LDR, so FSR 3.1/4 run on it
before post-processing.

How the port hooks FSR in (`THEORDER_GAME=prototype`, set by `run.py`):

- **Scene targets** (`vk_rasterizer.cpp`): a draw into a single RGBA8/RGBA16F colour target with a
  same-size, non-square D32S8 depth is a scene draw; that depth is the scene depth (the UI's
  depth pairs with the sRGB display buffer and is ignored). Draws using the scene depth get
  the jitter, and only while FSR runs.
- **Camera** (`vk_camera_motion.cpp`, `OnObjectConstants`): material vertex constants hold
  World (floats 0–15) and WVP (16–31). VP = WVP·World⁻¹ is accepted when two draws agree, then split
  into the view matrix and the projection (scale x/y, depth = a + b/z; near ≈ 0.3, far ≈ 7500).
- **When**: after 8+ scene draws into one colour target, the first draw that targets neither
  it nor the scene depth (the depth-of-field or bloom pass) starts post-processing; FSR runs
  right before it and writes into the scene colour (`upscale_merge.comp`, `SCENE_LDR`).
- **Presets**: from the first scene draw until FSR, passes render into reduced proxies
  (`vk_scene_resolution.cpp`), with the scene size taken from the game's targets (its patched
  render resolution); FSR rebuilds the full size. Quality 1/1.5, Balanced 1/1.7, Performance
  1/2, Ultra Performance 1/3.

