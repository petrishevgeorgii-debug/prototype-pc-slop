PROTOTYPE - native PC port (unofficial)            https://github.com/petrishevgeorgii-debug/prototype-pc-slop
==========================================================================================

A native Windows port of PROTOTYPE (and, experimental, PROTOTYPE 2) from the PS4
"PROTOTYPE BIOHAZARD BUNDLE" (CUSA06545).
This package holds NO game files. You need your own legally obtained, decrypted dump of
CUSA06545 (version 01.00): the folder with eboot.bin, prototype2_ps4f.elf, sce_sys,
sce_module, p1 and p2.
Your game folder is never changed: every fix is applied in memory.

Needs: Windows 10/11 64-bit, a Vulkan 1.3 graphics card with an up-to-date driver.
Tested on AMD Radeon. NVIDIA and Intel are untested: please report how it runs.

PLAYING
  1. Unpack this folder anywhere (a path without special characters is safest).
  2. Start Prototype.exe, choose your game folder, pick PROTOTYPE or PROTOTYPE 2 at the top,
     press PLAY.
     The first start prepares the game (about a minute); later starts are quick.

KEYBOARD AND MOUSE
  WASD move, mouse camera, Space jump, Left Shift sprint/glide, left click attack, right click
  special attack/consume, E grab/throw, Q target lock, Tab power wheel, F disguise menu,
  1/2/3 powers on/off, 4 or Z disguise, middle click or V reset camera, Esc pause,
  J Web of Intrigue;
  menus: arrows, Enter, Backspace. The launcher's CONTROLS page changes the keys, the mouse
  sensitivity and the button icons (your keys or the PlayStation buttons in the game's hints;
  made from your game files at start, the game folder is not changed).

IN GAME
  Insert (keyboard) or R3 + L2 (controller) opens the port menu:
    UPSCALING  FSR 3.1 / FSR 4 (FSR 4 needs a Radeon RX 9000), Native AA or a quality preset
    IMAGE      FSR 1, sharpening
    DISPLAY    window mode, V-Sync, frame counter
    FSR TESTS  for problem reports (jitter switches, motion vectors view, saving FSR images)
    SHADERS    the shader browser for graphics mods (docs\SHADER_MODS.md)
  Tab / Q / E (or Square, L1 / R1) switches sections; Backspace (Triangle) resets a section.

  The launcher's GAME page has each game's own render resolution and 30/60 FPS. FSR 1,
  sharpening, texture filtering, display mode, V-Sync and the controls are shared.

WHAT'S NEW IN 0.7.0 (all versions: CHANGELOG.md)
  - Graphics modding: your own GLSL shaders in mods\shaders replace the game's; the port
    menu's SHADERS page shows what each shader draws and exports it (docs\SHADER_MODS.md).
  - PROTOTYPE shadows, VERY EXPERIMENTAL (launcher GAME page, off by default): 4096 shadow
    map, shadow distance 2x to 4x, or Whole city. If shadows break, flicker or the game
    crashes on load, set them back to 2048 / PS4.
  - Fixed: black river water in PROTOTYPE.

SAVES AND LOGS
  Saves:   user\savedata (backed up at every start to user\savedata_backups)
  Log:     last_run.log (send it with any problem report)

KNOWN ISSUES
  - If a loading screen still never ends: press F10 while it hangs and send last_run.log
    and out\hang_sample.txt.
  - PROTOTYPE 2 is experimental: no keyboard and mouse button icons yet (PlayStation buttons
    are shown), FSR upscaling does not work in it yet, its mouse camera goes through the right
    stick, and 4K is untested.
  - Render resolution above 1080p is experimental.
  - The shadow options are very experimental: shadows can break, float or flicker, and
    Whole city costs frame rate.
  - FSR 4.1 needs a driver feature AMD's Windows driver does not have yet; it stays hidden.

PROTOTYPE is a trademark of its owners. This is an unofficial fan project, not affiliated with
or endorsed by Activision, Radical Entertainment or Sony Interactive Entertainment.
License: GPL-2.0-or-later (LICENSE). Credits and third-party licenses: CREDITS.md, licenses\.
The FSR 4 shaders (fsr4_shaders) are AMD FidelityFX SDK, MIT (fsr4_shaders\LICENSE-FSR4-v07.txt).
