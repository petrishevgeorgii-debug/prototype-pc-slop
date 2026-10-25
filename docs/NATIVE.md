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

Nothing yet. The first part, the sun's shadow distance in the port menu, was removed: it was
too experimental to keep. The layer, its checks and its test stay, ready for the renderer work
below.

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
