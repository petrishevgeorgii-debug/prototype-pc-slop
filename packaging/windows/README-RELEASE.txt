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
  2. Start Prototype.exe, choose your game folder, pick PROTOTYPE or PROTOTYPE 2 (the file
     tabs at the top left), press PLAY or Enter.
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

WHAT'S NEW IN 1.0.4 BETA 2 (a test build; all versions: CHANGELOG.md)
  - Options > Display (pause menu: a fourth category; main menu: a fourth Options button):
    resolution, frame rate (30/60), display mode, V-Sync, frame counter. Resolution and frame
    rate apply on the next start, or at once with Apply > Restart now (unsaved progress is
    lost). With loose game files and with p1\*.rcf archives alike.
  - DualSense and other gamepads: the game's light bar colours and vibration (it never
    vibrated: the PS4 profile's rumble switch is kept on), a vibration strength, vibration
    on consuming and in the menus. Options > Controller has Vibration, Light bar and Menu
    vibration; so do the launcher's CONTROLLER page and the port menu.
  - 1.0.3: Fixed: Options > Video showed only Brightness with dumps that keep the game's files in
    archives (p1\*.rcf). The port's rows now appear with every dump.
  - 1.0.2: PROTOTYPE's Options > Video screen (main menu and pause menu) has the port's settings
    as the game's own rows: Upscaling, Upscaling quality, Sharpening, Display mode, V-Sync.
    They apply at once. Made from your game's menu files: the game folder is not changed
    (THEORDER_GAME_MENU=0 keeps the game's own screen).
  - 1.0.1: the redesigned launcher; the experimental shadow options and water reflections
    were removed.

SAVES AND LOGS
  Saves:   user\savedata (backed up at every start to user\savedata_backups)
  Log:     last_run.log (send it with any problem report)

KNOWN ISSUES
  - PROTOTYPE 2 is experimental: no keyboard and mouse button icons yet (PlayStation buttons
    are shown), FSR upscaling does not work in it yet, its mouse camera goes through the right
    stick, and 4K is untested.
  - Render resolution above 1080p is experimental.
  - FSR 4.1 needs a driver feature AMD's Windows driver does not have yet; it stays hidden.

PROTOTYPE is a trademark of its owners. This is an unofficial fan project, not affiliated with
or endorsed by Activision, Radical Entertainment or Sony Interactive Entertainment.
License: GPL-2.0-or-later (LICENSE). Credits and third-party licenses: CREDITS.md, licenses\.
The FSR 4 shaders (fsr4_shaders) are AMD FidelityFX SDK, MIT (fsr4_shaders\LICENSE-FSR4-v07.txt).
