# The native game layer

PROTOTYPE's own PS4 code runs directly on the PC's CPU (the PS4 is x86-64 too). The native game
layer goes a step further: it **replaces parts of the game with C++ written for the port**, one
piece at a time, while everything else keeps running as before. The game stays playable at every
step, and each native part can be checked against the original code it replaces.

GTA III's re3 was built this way, and the layer can grow towards what the P.T. PC port did. That
port replaced the whole game with an engine of its own, which reads only the game's assets. Each
subsystem moved to native code here is one less part of the original. The renderer comes first:
once the port knows what every draw is (a building, a character, the sun's shadow), its own
lighting, materials (PBR) and ray tracing become possible.

## How it works

- `game/native_registry.h`: a replacement names the original code it takes over and the exact
  bytes that code has in the game build it was written for (PROTOTYPE, CUSA06545 01.00). The
  loader installs it only when those bytes are found unchanged. Another build of the game, or a
  patch that changed the same code, leaves the original running, and `last_run.log` says so.
  There are three kinds:
  - `Function`: the whole function jumps to the native one.
  - `CallSite`: one call goes to the native function. That function can do its own work and
    call the original (`callee`).
  - `Custom`: anything else, such as an operand redirected to native data.
- `game/guest.h`: the game's globals, object fields and functions from C++. Addresses are the
  eboot's virtual addresses, and the game's calling convention is `GUEST_ABI` (System V, the
  PS4's, also on Windows).
- `game/native.cpp`: installs every registered replacement (from `src/probe.c`, after the patches
  and before the game starts) and logs each one:

  ```
  Native: <name> [<group>] at 0x...
  Native: N replacements installed, 0 not
  ```

- `THEORDER_NATIVE=0` turns the whole layer off, to compare with the original. To leave out a
  group or a single replacement, list it with a minus sign: `THEORDER_NATIVE=-<group>`.
- `tests/test_native.cpp` installs the layer into a stand-in of the eboot that holds only the
  bytes it checks, and runs the native code against stand-in game objects (`bash build.sh --test`
  on Linux).

## The map of the game

```
python scripts/game_map.py
```

It reads the decrypted eboot (`out/eboot.elf`, written at the first start) and writes `out/game_map/`:

| File | Contents |
|---|---|
| `classes.txt` | The game's C++ classes from their run-time type information (about 11,600), with their bases, their virtual function tables and the functions in them. |
| `functions.csv` | Every function found (about 73,000), named where the map can: hand-identified ones from `game/p1/known_functions.txt`, a class's virtual function, a constructor or destructor (it stores a class's vtable), or a function whose assertions name its source file. |
| `sources.txt` | The engine's source files that the eboot names (`shadowrendermanager.cpp` and others), with the functions in them. |
| `symbols.txt` | `name address` lines for Ghidra (`ImportSymbolsScript.py`) or other tools. |

The map comes from your own copy of the game and stays in `out/`: it is not part of the
repository or the release. `game/p1/known_functions.txt` holds the names worked out by hand;
please add to it when you identify a function.

## What is native so far

### The game's menus call the port (`game/p1/options_menu.cpp`)

PROTOTYPE's menus are Scaleform movies (`p1/art/hud/*.gfx`). Their ActionScript calls the game
through `ExternalInterface.call("PlaceLuaCall", function, ...)` (`commonregisters.DoCallBack`):
the movie's ExternalInterface handler (`0x1caf0`) hands `PlaceLuaCall` on to `0x1cd90` with a tail
call, which runs the Lua function of that name. That hand-over now goes through native code
first. It answers:

- `port_Count(page)`, `port_Label(page, row)`, `port_Choices(page, row)`,
  `port_Choice(page, row, index)`: the port's settings rows of a page, 0 Video and 1 Controller
  (`gpu/shim/port_game_menu.cpp`, `TheOrderGameMenu` in `gpu/theordergpu.h`);
- `db_GetInt`, `db_SetInt` and `fe_RestoreGameDBDefault` for the game database names
  `PortSetting<page>_<row>`: the settings themselves.

Everything else goes to the game's Lua as before. `scripts/proto_menu.py` makes the screens that
ask for these: the Video screens (`protovideooption.gfx`, `protopausevideo.gfx`: after Brightness,
a loop over the port's rows instead of the PC version's rows the PS4 hides) and the Controller
screens (`protocontrolleroption.gfx`, `protopausecontroller.gfx`: after Target toggle, instead of
the PS3's Rumble row and the PC's mouse and keyboard rows), and gives `protooptionsoverlay.gfx`
the toggles they use (in `VideoOptionsOverlay` and `ControllerOptionsOverlay`). It also makes a
Display screen for page 2: the PC version's Game options screens (`PROTO_PAUSE_GAME`,
`PROTO_GAMEOPTION`, still registered by the game, without movies) get movies made from the
Controller screens' own, in place of the developers' pause debug screen and menu
(`protodebug.gfx`, `protodevmenu.gfx`: archives cannot get new files), which `protomain.gfx`
gives them; the pause options screens get a fourth category (`mCategoryNames`,
`GetCategoryIndexFromScreenName`, `OnCategoryChange`) and the main menu's Options a fourth button
(`protooptions.gfx`'s own `GotoGame`), its screens' lists too. The files are made from the player's own when
the game starts and read in place of the game's (`THEORDER_FILE_OVERRIDES`).

`scripts/proto_menu.py` can be run on its own (`python scripts/proto_menu.py <game> <out>`); its
patched movies can be checked with any SWF disassembler (they are uncompressed GFX 8, SWF 8).

### The rumble switch stays on (`game/p1/rumble.cpp`)

The game vibrates the controller only while the profile's `Proto_Profile_GamepadRumbleEnabled`
is on: the PS3's Rumble row of Options > Controller, hidden on the PS4, and off in some PS4
profiles (then `scePadSetVibration` is never called). The game database's listener (`0x5685a0`)
hands a change of it (also on loading a profile) to `0x568510(on, player)` with a tail call; that
hand-over now always says on. How strong the vibration is, off included, is the port's Vibration
setting (`src/runtime_pad.c`). The Controller screen's Vibration row is also linked to the
switch (`TheOrderGameMenu::linked`): it sets it with the game's own `db_SetInt`, so the profile
saves it on.

### Vibration on the player's moves, on camera shakes and in the menus

The game vibrates for few things itself. Every move is a `proto::fightitems::motion::*Action`
whose vtable slot 6 starts it and slot 7 ends it; `game/p1/rumble.cpp` points those slots of
about thirty moves (consuming, the whipfist, wall running, devastators, the shield, air moves,
grabs, throws, hits, falls...) to native functions that start an effect and call the game's own
(each slot is checked to hold the game's function first). Moves anyone can make vibrate only for
the player: the action's owner handle (at an offset per class) is compared with the player's
(`0x13491c0`, `Native::IsPlayer`). Camera shakes: the CameraManager (pointer at `0x135c5f0`)
applies a requested shake at `0x70c2ed` (`call 0x103e90` per part of it); the call now also
vibrates by the shake's strength (`[manager+0x94]`, its level times its nearness).

Effects (`Native::Effect`, `runtime_pad_effect` in `src/runtime_pad.c`) are taps, swells or
rhythms on eight channels whose motor levels are added to the game's by taking the larger, scaled
by the Vibration setting; one with an id lasts until the same id ends it (a wall run's steps until
the wall run ends). The Action vibration setting turns them off. The menus play their sounds
through PlaceLuaCall (`snd_PlayPatchOverlapped`, protomain's soundmanager): `options_menu.cpp`
taps the controller for the select, back and tab sounds and for scrolling the main menu, and for
the other menus' move sounds with Menu vibration set to All.

### The mouse in the menus (`game/p1/menu_mouse.cpp`)

The menus are Scaleform GFx 2.x movies; the PC version fed them the mouse and the movies still
react to it, but the PS4 game never does. `GFxMovieRoot`'s vtable (`0x1065d30`) slot 31 is
`Advance` (`0x3ff610`): it now first calls `SetMouseCursorCount(1)` (slot 46, once per movie: GFx
ignores mice beyond the count), sends a `SetFocus` event (`HandleEvent`, slot 36, type 8: an
unfocused movie drops the others) while the movie's focus byte (`+0xc91`) is clear, then
`NotifyMouseState(x, y, buttons, 0)` (slot 38) with the place in the rect the movie is drawn in
(its viewport at `+0x64`: buffer width and height, then the rect's left, top, width, height;
GFx maps the rect's pixels to the stage. The PS4 game's buffer says 1280x720 while the rect is the
render resolution, so the buffer's size kept the mouse in the top left of the screen) and a `MouseWheel` event (type 4, delta at `+0x10`) per notch. The window's side
(`gpu/shim/port_pointer.cpp`) maps the mouse onto the game's picture with the presenter's
letterbox. The same hook reads the screen on top of protomain's
`_global.managers.screen.arrScreenStack` with the movie's `GetVariable` (slot 13, `0x404630`):
`IGHUD` while playing, any other is a menu; and shows the PC cursor (`_root.mMouseCursor`) at
`_root._xmouse`/`_ymouse` with `SetVariable` (slot 12, `0x403ee0`). (A frame script in protomain
asking the port stopped reaching it once the game had been played.) The right button, while the menus are up, is the pad's Circle
(back; `TheOrderMouse::menu_back`, `src/runtime_pad.c`). The main menu's buttons (`protoButton` in
`protocontrols.gfx`) are pointed at by their label alone, a text box 64 px tall for 29 px between
buttons; the one above is on top, so its box took the next one's text. `scripts/proto_menu.py`
sets the labels' AutoSize flag, so their boxes fit the text.

A left click (any mode, `theordergpu_pointer_clicks`) skips the movie playing, checked in the same
`Advance` hook (it runs on the thread of the game's Lua, which the menus call into): when
`fmv_IsPlaying` (`0x19890`) would say yes (`0x122f3b8` set up, `0x122f358` playing, `0x122f3c0`
loaded) and it is not skipped yet (`0x122f3c8`), the port calls the game's own `fmv_Skip`
(`0x19840`), the Lua function its scripts call to skip.

## The trainer

`game/p1/trainer.cpp`, from protomain's `Advance` (the same hook as the menu mouse, on the thread
that runs the game's Lua). Opening: F1 or the touchpad held (`gpu/shim/port_trainer.cpp`,
`src/runtime_pad.c`); from the HUD (`IGHUD`) the port clicks the touchpad's right side (the PS4 game's pause menu,
which opens on its map; Options is the Web of Intrigue, which it leaves first), waits for the pause menu (the stack holds the
screens' names: `PAUSEMENU`; the trainer is `DEBUG`, the map `MAP`) and
`Invoke`s `_root.PortTrainerPush` (GFxMovie slot 18, `0x4055e0`), which `scripts/proto_menu.py`
defines in protomain as `managers.screen.PushScreen(PROTO_PAUSE_DEBUG)` (over the map too, the
pause menu's first screen; taking a pause screen's place emptied the stack and backing out froze
the game); leaving it for the pause
menu presses Options (the pause menu's `ExitPause` on the PS4). The screen's rows are page
3 of `TheOrderGameMenu`; they act as they change, and leaving the screen never asks to save or
saves the profile (its `PromptLeaveScreen` and `SaveSettings` are cut short: saving the profile
from the pause menu hung the game).

The rows run Lua through the game's script manager (`0xca650` returns it; `+0x18` is the
`lua_State`): `PushFunction(name)` `0xd1ad0` (a global function), `PushString` `0xd1a80`,
`Call(nargs)` `0xd1800` (`lua_pcall` with one result; errors go to `print`), `lua_type(-1)`
`0xd1870`, `GetFloat(L, float*)` `0xd1960`, `gettop`/`settop` `0xd1b20`/`0xd1b30`. A chunk is
`loadstring(code)` called, then called itself. The player is the scripts' global `PLAYER_1`.

Critical mass: Alex's health behaviour (`proto::PrototypeHealthBehaviour`, vtable `0x1092230`)
handles a SetHealth message (type `0x14`, kind 0, value at `+0x10`) in `0x6bc570`: health above
the normal top (`+0x1c`) is only added scaled by `criticalMassHealthGainFactor` (`+0x23c`), and
the result is capped at the top plus critical mass (`+0x1dc`). The trainer sends
`go_SetHealth(PLAYER_1, 98765)`, a marker its replacement of that vtable slot turns into the
health setter (`0x1258c0`) with the cap itself.

## The frame's last submit (`game/p1/frame_submit.cpp`)

`ProtoMidgroundZPass` kicks the frame so far (the shadow maps and the depth pre-pass) to the GPU
with Gnmx's `submit()`, which leaves the context's submission start at the frame's start, so the
frame's `submitAndFlip` sent them again and the GPU drew them twice every frame (about 1870 of
the city's 5100 draws; the PS4 too). The kick (`ps4Context`'s vtable slot 70, `0x236810`) is
replaced by one that calls it and then moves the submission start to where it ended (Gnmx's own
step for a full command buffer, `0x921101`). Group `framesubmit`. Details in
[RENDERER.md](RENDERER.md).

## The shadow distance (`game/p1/shadows.cpp`)

Options > Video > **SHADOW DISTANCE**: GAME (about 120 m), 250, 500 or 1000 m, live (the row is
`THEORDER_VIDEO_SHADOW_DISTANCE`, saved as `shadow_distance` in `theorder.ini`).

`pure3d::Shadow` (built by `0x2c5e20`) is the sun's cascaded shadow: cascade count `+0x300`,
extents `+0x318` (cascade `i` ends at `0.5 x unit x extent[i]` along the view, clipped to the
shadow box), unit divisor `+0x32c` (40) and scale `+0x330` (1): `unit = 2 x range x scale /
divisor` = 10 m; range `+0x338`, the box being +- range around the camera, copied each frame from
the global `0x1218930` (200, `rm_SetGlobalShadowRangeHack`). The sun light gives it 4 cascades
with extents 1, 2, 4, 24: they end at 5, 10, 20 and 120 m. `Shadow::ComputeCascades`
(`0x2c9590`) compares the range with 200 (`0x2c95db`, the constant at `0xc9f818`) and uses a fixed
debug box for any other value.

Both of the renderer's calls to `Shadow::Update` (`0x330dca`, `0x344eb3`) come to native code
first. With a distance set, the two nearest cascades stay the game's (the same detail around
Alex), the other two end a constant factor further each, the last at the distance (250 m: 5, 10,
50, 250; 1000 m: 5, 10, 100, 1000), the range becomes 1.1 x the distance with the divisor keeping
the unit at 10 m, and the range check reads a native value that follows the range. GAME puts the
game's values back. The casters come with it: the shadow pass culls the scene once per cascade
against that cascade's planes (`0x349160`: `0x2b02c0` for each), so a longer cascade draws more
of the city into the shadow map. The render map logs the shadow pass's draws every 5 s to see it.

An earlier try (0.7.0) was removed in 1.0.0 (it did not show a difference in play). It spread all
four cascades from 5 m, which blurs the nearest shadows, and it kept the game's values in four
fixed slots: if the game makes a new sun shadow with each scene, a fifth one finds no free slot
and stays as the game set it. Now the slots are reused.

## Next

1. **Renderer front end.** Map Pure3D's draw path: passes (`pure3d::Pass`, `ShadowPass`,
   `ProtoShadowPass`), drawables, materials (shaders and their parameters) and lights. Then
   follow each draw from the game's object to the GPU command, so the port knows what every draw
   is.
2. **Native lighting and shadows** on the port's own GPU passes: per-material changes such as
   PBR, ray-traced shadows (Vulkan ray queries) for the city.
3. **Assets.** The game's own loaders for its files (`.p3d`, textures, meshes), found through the
   map, then native loaders. From there the original's systems can be replaced one by one, as far
   as it goes.
