# Changelog

All releases of the PROTOTYPE native PC port. Each version's full notes are in
[docs/releases](docs/releases) and on the [Releases page](https://github.com/petrishevgeorgii-debug/prototype-pc-slop/releases).

## Unreleased

### Fixed
- **The mouse could not choose a power on the power wheel** (hold Tab): the wheel selects with the right stick, but the mouse turns PROTOTYPE's camera directly, so it never reached the stick. While the power wheel is held, the mouse now points the right stick (a short move towards a power chooses it, and it stays chosen until you move again or let go).

## 1.0.4 — October 26, 2026 ([notes](docs/releases/v1.0.4.md))

### Added
- **A Display screen in the game's own options**, in the pause menu (a fourth category after Video, Audio and Controller) and in the main menu's Options (a fourth button): **Resolution** (1280×720 to 3840×2160), **Frame rate** (30 or 60 FPS), **Display mode**, **V-Sync**, **Frame counter** and **Apply resolution**.
  - The resolution and the frame rate are patched into the game at start: they apply on the next start, or at once with Apply resolution > **Restart now** (the game starts again; unsaved progress is lost). They are saved in `theorder.ini` (`proto_res`, `proto_fps60`), which `run.py` and the launcher now read: a change in the game shows in the launcher too.
  - **Resolution also sizes the window:** windowed, the window takes the resolution's size (made to fit the screen with its title bar), and exclusive fullscreen switches the display to the closest mode; both follow a change at once. Borderless stays at the desktop's size. (The window was always 1920×1080, so on a 1080p screen windowed looked like borderless and a new resolution showed no difference.)
  - Every change made in the game's Options screens is logged (`Menu: Display > RESOLUTION set to 1600x900`), and the window's mode and size at start (`Window: Windowed, 1600x900`).
  - Display mode and V-Sync moved there from Options > Video, which keeps Brightness, Upscaling, Upscaling quality and Sharpening.
  - The screens are the PC version's Game options screens (`PROTO_PAUSE_GAME`, `PROTO_GAMEOPTION`), which the game still registers but has no movies for: `scripts/proto_menu.py` makes them from the Controller screens' movies, in place of two movies the PS4 game never shows (the developers' `protodebug.gfx` and `protodevmenu.gfx`, which `protomain.gfx` now gives the two screens), and adds the category and the button to the other options screens. Loose files and `p1\*.rcf` archives alike (in archives the movies are found by what they hold).
- DualSense and other gamepads:
  - **Light bar:** the colours the game sets reach a DualSense or DualShock 4, as on the PS4 (`scePadSetLightBar`, `scePadResetLightBar`; they were ignored).
  - **Vibration** runs as long as the game asks, as on the PS4 (it stopped after a second), with a strength setting from off to 200%.
  - **Vibration works with PS4 profiles that have it switched off.** The game's own rumble switch (`Proto_Profile_GamepadRumbleEnabled`, the PS3's Rumble row, hidden on the PS4) can be off in a PS4 profile, and then the game never vibrates. The port keeps it on (`game/p1/rumble.cpp`); the port's Vibration setting decides how strong, off included. The Vibration row of Options > Controller also sets the game's switch, so the profile saves it on.
  - **Action vibration:** the port adds vibration where the game has none (`game/p1/rumble.cpp`, from the start and end of the game's own moves, their `fight::Action` vtables):
    - **Consuming:** a rumble that swells while the tendrils take the victim in and fades when they let go.
    - **Whipfist:** a crack when it lashes out, a tug when it catches, a pull while it drags you, a light snap back.
    - **Wall running:** a thump onto the wall, a step rhythm while you run, a push on wall jumps, a light buzz while sliding down.
    - **Powers and air moves:** devastators build up, ground spikes erupt, air dash, air attacks, the dive's landing, the shield going up and blocking.
    - **Combat:** grabs, throws, hits you land, hits you take, heavy steps; landings from a height, harder the longer the fall (only the player's moves: the action's owner is compared with the player's).
    - **Explosions and impacts:** every camera shake the game asks for (the CameraManager's), as strong as the shake and its distance.
    - Effects run on eight channels that add up (the stronger motor wins), so a wall run's rhythm goes on under a hit. **Action vibration** (Options > Controller, the launcher's and the port menu's CONTROLLER page; `extra_vibration` in `theorder.ini`) turns them off; the game's own vibration stays.
  - **Menu vibration:** a short tap when you press select, back or switch tabs in the game's menus (start screen, main menu, pause menu) and when you scroll the main menu (New game, Continue, Options...), or also when you move between items in every menu. It follows the menus' own sounds (`snd_PlayPatchOverlapped`, through the PlaceLuaCall hand-over). Off, Presses (default) or All.
  - Settings: PROTOTYPE's own **Options > Controller** screen (main menu and pause menu) has **Vibration**, **Light bar**, **Menu vibration** and **Action vibration** after Target toggle, as the game's own rows; also the launcher's new CONTROLLER page and the port menu's new CONTROLLER page (all live); `theorder.ini` keys `vibration`, `light_bar`, `menu_vibration`.
  - `scripts/proto_menu.py` makes the Controller screens too (`protocontrolleroption.gfx`, `protopausecontroller.gfx`). The port's menu rows come in pages (`port_Count(page)`..., game database names `PortSetting<page>_<row>`).

### Fixed
- **Restart now** kept the previous start's resolution: the restarted `run.py` inherited the previous patch list (`THEORDER_PATCHES`), so its old Render Resolution patch stayed (twice in the log), going back to 1920×1080 never worked, and the 60 FPS patch could not be turned off. The resolution and 60 FPS patches now come only from the settings.
- The log no longer says "Frame Rate Patch - 60 FPS is not available" for PROTOTYPE (The Order's engine frame rate setting; PROTOTYPE's 60 FPS patch is its own).

## 1.0.3 — October 26, 2026 ([notes](docs/releases/v1.0.3.md))

### Fixed
- Options > Video showed only Brightness with dumps that keep p1 in archives (`p1\*.rcf`, straight from the PS4 package). `scripts/proto_menu.py` now finds the Video screen's movies in the archives (plain, RZ or zlib-compressed) and writes a patch per archive. An archive the keyboard prompts also patch gets one patch with both, read first.

## 1.0.2 — October 26, 2026 ([notes](docs/releases/v1.0.2.md))

### Added
- The port's settings in PROTOTYPE's own **Options > Video** screen, in the main menu and the pause menu: Upscaling, Upscaling quality, Sharpening, Display mode and V-Sync, as the game's own rows under Brightness (the screen has room for five). They apply at once and are saved to `theorder.ini`.
  - The screen is made from the player's own menu files at start (`scripts/proto_menu.py`) and read in place of the game's: the game folder is not changed. `THEORDER_GAME_MENU=0` keeps the game's own screen.
  - The screen's calls reach the port through the game's own way of calling native code from its menus, `PlaceLuaCall` (`game/p1/options_menu.cpp`, [docs/NATIVE.md](docs/NATIVE.md)).
- File overrides (`THEORDER_FILE_OVERRIDES`) can come from several folders (separated by `;`).

## 1.0.1 — October 25, 2026 ([notes](docs/releases/v1.0.1.md))

### Changed
- The launcher is redesigned from scratch: a Blackwatch terminal look (dossier pages, monospace type, scanlines, warning stripes). Every setting, the controls, the folder check and Prepare / Restore / Clean up are still there.

### Removed
- PROTOTYPE's shadow options: shadow resolution 4096 and shadow distance 2×, 3×, 4× and Whole city (launcher GAME page), and the port menu's Image → SHADOW DISTANCE. They were very experimental and are scrapped.
- Water reflections (screen-space reflections on the river, off by default): the launcher's Graphics switch and the port menu's Image → WATER REFLECTIONS. They looked too poor to offer.
- The launcher's Manhattan Engine button. `manhattan.exe` still starts on its own.

### Added
- The native game layer ([docs/NATIVE.md](docs/NATIVE.md)) replaces parts of the game with C++ written for the port. Each part is installed only where the original code is found unchanged, and `THEORDER_NATIVE=0` turns the layer off.
- The Manhattan Engine ([docs/ENGINE.md](docs/ENGINE.md)) is the start of a PC engine that runs PROTOTYPE from your own copy's data, as the P.T. PC port does. Milestone 0 is the city's data read without the game: `tools/formats` reads PROTOTYPE's Pure3D files and untiles their textures ([docs/formats/pure3d.md](docs/formats/pure3d.md)).
  - The tools can draw the whole of Manhattan and a textured city block, and export cells to OBJ for Blender.
  - Milestone 1: `manhattan.exe` flies through PROTOTYPE's city in its own Vulkan renderer. It shows the low-detail city, with the detailed cells and their textures streamed in around the camera.
  - Milestone 2, the look: the city's own light rig from `manhattan_tod` (the sun, the sky light and two fill lights), the sun's shadows in four cascades, normal maps, the vertex tint the game colours its buildings with, cut-out leaves and fences, blended decals (the hive infection), a sky, haze and tone mapping. Light cones and particles are left out for now. N and B fly to the city's 14 districts; L turns shadows off; [ and ] change the exposure.
  - Parked cars: the cells' parking places (`proto::ParkedCarSpawner`) get the vehicles their district spawns, by the district's weights, from `art/packages/vehicles`. Each car is put together from its skeleton's parts (body, doors, wheels) and painted with a colour from its own palette (`<car>_tod`). Car windows are glass that reflects the sky.
  - Fixed: the low-detail city's traffic strips covered the streets of the detailed cells.
  - On foot (F): walk, sprint at 22 m/s and jump through the city with collision against its buildings, cars and props. Sprinting into a wall runs up it, Alex's way, and over the top. `--walk-test seconds` checks it without a screen.
  - Alex: the game's own model (body, head, hoodie, arms), skeletons and animations, read from `art/alex/alex.p3d.rz` (634 clips). On foot the camera is behind him; he stands, walks, runs, sprints, rises, falls and runs up walls with the game's clips, blended into each other, and vaults cars instead of running up them. V switches to first person; `--anim name` plays one clip.
  - More of Alex's moves, all the game's clips: the jump's take-off, a double jump (Space in the air: a flip), leaning into long jumps, landings by how hard he lands (soft, running on, heavy with a moment's hold, a dive roll when sprinting), stopping from a run or a sprint, and coming over the top of a wall.
  - Charged jump (hold Space on the ground: up to about 20 m), glide (hold Space after the double jump), dash (E, once in the air), and a dive pose when falling fast; the game's clips for each.
- `scripts/game_map.py` writes a map of the game's code to `out/game_map`. The map lists the 11,600 classes with their virtual functions, 73,000 functions with names where possible, and the engine's source files. It also writes a Ghidra symbol list.

## 0.7.0 — October 20, 2026 ([notes](docs/releases/v0.7.0.md))

### Added
- Graphics modding: shader mods and an in-game shader browser. See [docs/SHADER_MODS.md](docs/SHADER_MODS.md).
  - Shader mods are GLSL files in `mods/shaders`, compiled at start, that replace the game's shaders.
  - The browser is the port menu's SHADERS page. It lists the last frame's pixel shaders, with names for the known passes.
  - It can hide a shader to show what it draws, and export it for editing.
  - The release ships glslangValidator and spirv-cross for this.
- PROTOTYPE, **very experimental** (launcher, GAME page, off by default):
  - **Shadow resolution 4096:** the sun's shadow map is 4096 × 4096 instead of 2048 × 2048, for sharper shadows. It uses 64 MB more video memory.
  - **Shadow distance 2×, 3× or 4×:** shadows reach that many times as far, and are softer up close.
  - **Shadow distance Whole city:** near shadows stay as sharp as before. Far ones are meant to reach about 1.5 km (PS4: about 120 m).
- Water reflections on the river: screen-space reflections, experimental and off by default. They are on the launcher's Graphics page and in the port menu.
- Patch files can grow a memory size on top of another patch (`add32` lines) and add to the direct memory pool ("Adds N MB to DMEM").

### Fixed
- PROTOTYPE: black water. The GPU layer dropped the river's reflection cube map, because its image descriptor lacked the array bit the GPU layer expected. The cube map's coordinates were also converted twice.

### Known issues
- The shadow options are very experimental. They have only been tried briefly on one PC. Shadows can break, float or flicker, and Whole city costs frame rate. Set them back to 2048 / PS4 if anything looks wrong.
- Distant buildings may still cast no shadow with Whole city: their far-away models may not be shadow casters.

## 0.6.0 — October 20, 2026 ([notes](docs/releases/v0.6.0.md))

### Added
- PROTOTYPE 2 (experimental): starts from the launcher or the bundle menu (LoadExec) and plays. Fixes needed for it:
  - the PSN toolkit thread's lost start-up signal (black screen);
  - buffers larger than the GPU buffer cache's arenas, now bound up to the arena end (crash after the Activision logo);
  - the GPU layer's draw preparation on worker threads, now off for PROTOTYPE 2, plus stale vertex buffer descriptors read as empty streams (exploding geometry);
  - 101 more system imports (PSN offline, dialogs, RFC 3339 dates and others).
- PROTOTYPE 2 patches in their own file (`patches/Prototype2-Orbis.xml`): render resolution 1280 × 720 to 3840 × 2160 (above 1080p with more video memory) and 60 FPS.
- Launcher redesign:
  - big game tabs across the top, each in its own colours and key art;
  - a GAME page with each game's own resolution, frame rate and fixes;
  - graphics, display and controls shared by both games.
- Mouse look drives PROTOTYPE's on-foot camera directly: no dead zone, curve or turn-speed limit.
- The hang report also runs on its own when a compressed game file stays half-read for a minute.

### Changed
- Semaphores each have their own lock instead of one shared by every thread.

### Known issues
- PROTOTYPE 2:
  - no keyboard and mouse button icons;
  - FSR upscaling does not work;
  - the mouse camera goes through stick emulation;
  - 4K is untested.

## 0.5.3 — October 20, 2026 ([notes](docs/releases/v0.5.3.md))

### Fixed
- Endless loading screens, a black pause menu and soft locks after consuming. The game's "job done" signal lowers a counter and then wakes the waiting thread in a second step. A new job set up in between got the late wake-up, so the file unpacker read an empty result and looped forever. The port now runs each of these signal steps whole (in memory; game files unchanged).

### Added
- The F10 hang report shows the game's call stacks, which thread waits on which semaphore, and the open files with how far each was read.

## 0.5.2 — October 19, 2026 ([notes](docs/releases/v0.5.2.md))

### Fixed
- Picked-up objects (cars, people) were invisible at render resolutions above 1080p instead of half-transparent. The game's video memory pool for its screen images was sized for 1080p, and the blur image for held objects no longer fitted. The 1440p, 1800p and 4K patches now enlarge it: 640, 1000 and 1440 MiB instead of 360 MiB. 1800p and 4K reserve 1 GiB more memory at start.
- Endless loading screens (after cutscenes, loading a save, the pause menu): a fiber could be resumed by another thread before its state was saved.
- File reads into memory the GPU watches could fail or stop short, and the game retried them forever. They now complete through a host buffer.
- Keyboard and mouse button icons now also appear with dumps that keep the game's files in archives (`p1\*.rcf`).
- Reads of the same archive from several threads no longer move each other's file position.

### Added
- The F10 hang report lists the game's fibers and the semaphores threads wait on.
- Frame captures record every draw's render state (developer option).

0.5.1 was prepared but not published; its fixes are in 0.5.2. 0.5.2 was published as a tag only; its notes are part of 0.5.3.

## 0.5.0 — October 19, 2026 ([notes](docs/releases/v0.5.0.md))

### Added
- A PC keyboard and mouse layout made for PROTOTYPE's actions (WASD, mouse camera, Space jump, Shift sprint, mouse buttons attack and special, E grab, Q target, Tab power wheel, F disguise menu, 1–4 powers and disguise, Esc pause, J Web of Intrigue).
- CONTROLS page in the launcher: two keys per action (any key, mouse button or wheel), mouse look, sensitivity, reset.
- Keyboard and mouse button icons in game. The hints and tutorials show your keys instead of the PS4 buttons: Keyboard & mouse, PlayStation, or Automatic (what you played with last).
- F10 writes a hang report to the log.

## 0.4.0 — October 19, 2026 ([notes](docs/releases/v0.4.0.md))

First public release.

- The PS4 game's own code running natively on the PC, with the shadPS4 video core on Vulkan.
- Correct game speed, working saves, consume tendrils, steady frame pacing.
- 60 FPS gameplay (videos and menus keep their speed).
- Render resolution from 720p to 4K (above 1080p experimental).
- FSR 3.1 and FSR 4 anti-aliasing and upscaling, FSR 1, RCAS sharpening, 16× anisotropic filtering.
- An in-game menu (English / Russian) and a launcher with settings, save backups and the log.
