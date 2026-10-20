# Changelog

All releases of the PROTOTYPE native PC port. Each version's full notes are in
[docs/releases](docs/releases) and on the [Releases page](https://github.com/petrishevgeorgii-debug/prototype-pc-slop/releases).

## Unreleased

### Added
- Graphics modding: shader mods (`mods/shaders`, GLSL, compiled at start) and an in-game shader browser (port menu, SHADERS) that lists the last frame's pixel shaders, with names for the known passes, hides one to show what it draws and exports it for editing. See [docs/SHADER_MODS.md](docs/SHADER_MODS.md).
- PROTOTYPE: shadow resolution 4096 (launcher, GAME page; experimental). The sun's shadow map is 4096 × 4096 instead of 2048 × 2048, for sharper shadows, using 64 MB more video memory.
- Water reflections on the river (screen-space; off by default, in the launcher's Graphics page and the in-game menu).

### Fixed
- PROTOTYPE: black water. The river's reflection cube map was dropped (its image descriptor lacked the array bit the GPU layer expected), and its coordinates were converted twice.

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
