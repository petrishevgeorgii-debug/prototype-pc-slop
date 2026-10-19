PROTOTYPE - native PC port (unofficial)            https://github.com/petrishevgeorgii-debug/prototype-pc-slop
==========================================================================================

A native Windows port of PROTOTYPE from the PS4 "PROTOTYPE BIOHAZARD BUNDLE" (CUSA06545).
This package holds NO game files. You need your own legally obtained, decrypted dump of
CUSA06545 (version 01.00): the folder with eboot.bin, sce_sys, sce_module, p1 and p2.
Your game folder is never changed: every fix is applied in memory.

Needs: Windows 10/11 64-bit, a Vulkan 1.3 graphics card with an up-to-date driver.
Tested on AMD Radeon. NVIDIA and Intel are untested: please report how it runs.

PLAYING
  1. Unpack this folder anywhere (a path without special characters is safest).
  2. Start Prototype.exe, choose your game folder, press PLAY.
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
  Tab / Q / E (or Square, L1 / R1) switches sections; Backspace (Triangle) resets a section.

  The launcher has the render resolution, FSR 1, sharpening, texture filtering, 30/60 FPS,
  display mode, V-Sync and the controls.

SAVES AND LOGS
  Saves:   user\savedata (backed up at every start to user\savedata_backups)
  Log:     last_run.log (send it with any problem report)

KNOWN ISSUES
  - A loading screen can rarely never end: press F10 while it hangs and send last_run.log
    and out\hang_sample.txt.
  - PROTOTYPE 2 (from the bundle menu) is not supported yet: choosing it ends the game.
  - Render resolution above 1080p is experimental.
  - FSR 4.1 needs a driver feature AMD's Windows driver does not have yet; it stays hidden.

PROTOTYPE is a trademark of its owners. This is an unofficial fan project, not affiliated with
or endorsed by Activision, Radical Entertainment or Sony Interactive Entertainment.
License: GPL-2.0-or-later (LICENSE). Credits and third-party licenses: CREDITS.md, licenses\.
The FSR 4 shaders (fsr4_shaders) are AMD FidelityFX SDK, MIT (fsr4_shaders\LICENSE-FSR4-v07.txt).
