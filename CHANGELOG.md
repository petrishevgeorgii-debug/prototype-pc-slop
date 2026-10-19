# Changelog

All releases of the PROTOTYPE native PC port. Each version's full notes are in
[docs/releases](docs/releases) and on the [Releases page](https://github.com/petrishevgeorgii-debug/prototype-pc-slop/releases).

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

0.5.1 was prepared but not published; its fixes are in 0.5.2.

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
