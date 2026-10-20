# PROTOTYPE port — technical notes

First public release: v0.4.0, October 19, 2026 ([release notes](releases/v0.4.0.md)). Current: v0.6.0 ([notes](releases/v0.6.0.md), [changelog](../CHANGELOG.md)); v0.6.0 added PROTOTYPE 2 (experimental); v0.5.0 added keyboard and mouse ([notes](releases/v0.5.0.md)).

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
| PlayGo language mask / to-do list, trophy icons, local-time conversion, module list, LoadExec | `src/runtime_proto.c` | Imports The Order never used. LoadExec ends the game with exit code 42 and writes the executable to `out/loadexec.txt`; `run.py` then starts that one. |
| `sceKernelReadTsc` / `GetTscFrequency` at 1,593,749,508 Hz | `src/runtime_kernel.c` | The game hardcodes the PS4 TSC rate for frame timing; a nanosecond counter made it run at 63% speed. |
| Import names | `src/import_names.inc` | NID → symbol table for this game's imports (names from shadPS4's aerolib). |
| Present cap 60 FPS | `run.py` (`THEORDER_FPS_LIMIT`) | The game paces itself on 60 Hz flips; without a cap menus ran at up to 120 FPS. |
| Quieter logs | `runtime_sema.c`, `runtime_pad.c`, `video_out.cpp` | The game creates thousands of semaphores and sets the flip rate every frame. |
| FSR 1 setting | `gpu/shim/port_settings.cpp` | Re-enabled as a launcher setting. |
| Save/load dialog | `gpu/shim/port_overlay.cpp` | Redesigned list (picture, title, location, date). |
| Selectable executable | `scripts/*.py` (`THEORDER_EBOOT`) | PROTOTYPE 2 is prepared in `out/prototype2_ps4f/`. |

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
  Above 1080p the patches also enlarge the video memory for render targets: the "Render" heap's
  Garlic part (`0x56b036`, 360 MiB at 1080p) becomes 640 / 1000 / 1440 MiB at 1440p / 1800p / 4K,
  and the Garlic direct memory block (`0x419c` allocation, `0x41b8` mapping, `0x427b` "PS4 core"
  heap; 1724 MiB) grows by as much. Without it the held-object fade's blur target failed to
  allocate (texture address 0) and picked-up objects vanished instead of turning half-transparent.
- **Shadow Resolution - 4096** (experimental; launcher GAME page) — the sun's cascaded shadow
  map. Pure3D's shadow render manager (`shadowrendermanager.cpp`) keeps all cascades in one D32S8
  depth texture, two 1024-texel tiles wide and `ceil(cascades / 2)` tiles high (2048 × 2048 for the
  PS4's four), sized by quality level: 0 = 2048 wide (the PS4's), 1 = 1024, 2 = 512, 3 = 1024 square.
  The patch makes level 0 4096 wide with 2048-texel rows in the constructor (`0x2cac20`), the
  setters (`0x2cad08`, `0x2cb1f3`, `0x2cb72e`, `0x2cb7f2`) and the size query (`0x2cb4ff`). The
  cascade viewports are derived from the texture size (half its height per tile), except one
  case capped at 1024 texels (`0xc36444`, a constant only `0x2c6009` reads), raised to 2048. The `shadowMapHeap` (one map plus 4 MiB,
  `0x2cae27` and `0x2caf93`) grows from 24 to 84 MiB, so the "Render" heap's Garlic part and the
  Garlic direct memory block get 64 MiB more. Those are `add32` lines: patches.py adds them on
  top of a resolution patch's values, and the note's "Adds 64 MB to DMEM" adds to its pool.
  (`rm_SetShadowResolution` is registered but its handler is an empty function in this build.)
- **Shadow Distance - 2x / 3x / 4x** (experimental; launcher GAME page) — the sun's shadow volume
  is a box of ± the global shadow range around the camera (`0x1218930`, 200.0; the variable
  `rm_SetGlobalShadowRangeHack` sets), and the cascade distances are `2 × range × scale / 40`
  (10 m) times each light's `shadowCascadeExtent` values. The patch sets the range to 400 / 600 /
  800 and the 200.0 the setup compares it with (`0xc9f818`, read only at `0x2c95db`; any other
  range selects a fixed debug box around (0, 0, -300)), so the box and every cascade grow alike.
- **Shadow Distance - City** (experimental) — cascade `i` ends at `0.5 × unit × extent[i]` along the
  horizontal view direction (clipped to the box), `unit = 2 × range / 40` (the `/ 40` is the shadow's
  default at `0x2c5ecd`). The sun light hands its shadow 4 cascades (`numShadowCascadesPS3`, at
  `0xb05ca`) with extents 1 / 2 / 4 / 24 (`0xb05d8` .. `0xb0614`; index 3 reads extent 5), i.e. 5,
  10, 20 and 120 m. The patch sets the range to 2000 (box ± 2 km), the divisor to 400 (unit 10 as
  before), forces 4 cascades and loads the extents 1.6 / 8 / 40 / 300 from read-only constants:
  about 8, 40, 200 and 1500 m.

## PROTOTYPE 2 (`prototype2_ps4f.elf`, experimental)

Runtime fixes, installed only when the bytes match this executable (`src/probe.c`):
- **PSN toolkit start-up** — the main thread locks the toolkit event, starts `Toolkit::NP` and
  waits. Offline, the thread signalled at once without taking the lock, so on Windows the signal
  could come before the wait (black screen). The thread entry (`0xc32740`) now goes through
  `p2_np_entry`, which takes and releases the lock first.
- **Draw preparation off** — the GPU layer's worker threads read each draw's V# tables ahead of
  the GPU thread; PROTOTYPE 2 fills its vertex tables later (exploding geometry). `run.py` sets
  the GPU toggles 4096 + 8192 for it.

Patches (`patches/Prototype2-Orbis.xml`, `AppElf="prototype2_ps4f.elf"`):
- **Prototype 2 60 FPS** — `0x27bf05`: the flip rate set once at start, 1 → 0.
- **Render Resolution - WxH** (experimental) — `0x27be83` display device size, `0x112f32`
  Pure3D display init, `0xd4e6b0` / `0xd4e6c0` the render-size getters (1920 / 1080). Above
  1080p also the Garlic direct block (`0x24642c` allocation, `0x246448` mapping, `0x246510`
  "PS4 core" heap) and the render heap's Garlic part (`0x594ec7`, 683 MiB) grow by 512 / 1024 /
  1536 MiB. The game settings defaults at `0x114360` are not changed.

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


## Keyboard, mouse and button prompts

The game's actions sit on fixed PS4 buttons; its text bible names a symbol per button
(`BJU_SYMBOL` ◊ jump on Cross, `BAT_SYMBOL` £ attack on Square, `BSP_SYMBOL` ³ special on
Triangle, `BGR_SYMBOL` ± grab on Circle, `BL1` ¹ power wheel, `BR1` ¥ disguise menu, `BL2` »
target, `BR2` ¼ sprint, `BR3` § camera reset, `BSE` · upgrades, `DPU/DPR/DPD/DPL` ½ ¯ ¾ ¬ the
quick slots, `ALE`/`ARI` µ ¶ the sticks), which its font draws as the button.

- `scripts/proto_controls.py`: the actions, their PS4 targets and the default keys. The launcher's
  Controls page saves the changes; `run.py` sends the whole layout as `THEORDER_KEY_MAP` with a
  leading `!` (`src/runtime_pad.c`: mouse buttons, wheel notches and stick directions are key map
  sources and targets there).
- `scripts/proto_prompts.py`: from the player's `p1/art/hud/fe_textbible.p3d.rz` and
  `textbible_<language>.p3d.rz` (RZ: a 16-byte header and one zlib stream around Pure3D chunks;
  text bible chunk 0x18202 holds the keys and start/end+1 offsets, chunk 0x18201 the UTF-8 text)
  every symbol becomes `[KEY]` (Russian key names when the language's text is Cyrillic; the
  `NEXT_`/`PREVIOUS_` menu hints show the arrows). In `fe_ingame.p3d.rz` the `buttonhint.gfx`
  pictures (GNF textures, BC3, 1D thin tiling: 8×8 blocks per tile in Morton order, stored upside
  down) are replaced in place with key caps of the same size. The files go to `out/prompts`, which
  the runtime reads instead of the game's (`THEORDER_FILE_OVERRIDES`, `src/runtime_file.c`).
- Dumps straight from the PS4 package keep p1 in Radical's archives (`art.rcf`, ...: "ATG CORE
  CEMENT LIBRARY", directory of {u32 hash, u32 offset, u32 length} at the u32 offset 0x24). The
  members are found by their contents (a text bible, the HUD package with `buttonhint__I16.dds`);
  the patched ones are added past the archive's end and their entries moved there, as
  `out/prompts/p1/<archive>.patch` (G3PATCH1), which the runtime lays over the archive.
- The prompts are chosen at start (`THEORDER_PROMPTS`: auto, keyboard, gamepad); the game reads
  its texts once, so switching needs a restart. Auto follows `out/last_input.txt`, which the
  runtime rewrites when the player switches device (`THEORDER_LAST_INPUT`).
- `BSE_SYMBOL` (·) stands for two buttons: in play the touchpad opens the pause menu and Options
  the Web of Intrigue, so the Web of Intrigue's texts (`WOI_*`) get the Options key.
