# The Manhattan Engine

A new engine for PROTOTYPE, written for the PC, that runs the game from your own copy's data:
the way the P.T. PC port's Hallway Engine runs P.T. It does not use the game's code. It reads the
game's files (world, characters, textures, sounds, scripts) and does what the game does with
them, with its own renderer.

Today's port runs the PS4 game's own code, and the native game layer (docs/NATIVE.md) replaces
parts of it. That remains the way to play the whole game while the engine grows. Each subsystem
written for the engine can also replace the same part of the running game through the native
layer.

## Where it is

**Milestone 0: the city's data, read without the game.** Done.

- `tools/formats/pure3d.py` reads PROTOTYPE's Pure3D files: the `.rz` containers, the chunk
  tree, meshes (vertex layouts, vertices, indices), shaders and their textures. It also reads
  textures, by decoding the PS4's GNF descriptor and untiling the city's BC1/BC3 textures.
  Format notes: [formats/pure3d.md](formats/pure3d.md).
- `render_preview.py` and `render_textured.py` draw it with no game code and no GPU: the whole
  of Manhattan from `midgeo.p3d.rz`, and a detailed cell (Columbia University, with the hive
  infection) with its textures.
- `export_obj.py` writes a cell as OBJ and PNG textures for Blender.

**Milestone 1: fly through the city.** The first build is in.

- `engine/` is the engine itself, C++20 with Vulkan and SDL3. The port's build makes it as
  `out/manhattan.exe`, and the launcher starts it from **Manhattan Engine** at its bottom.
- It shows:
  - the whole city at low detail;
  - the detailed cells around the camera, streamed in on a worker thread and swapped for their
    low-detail mesh;
  - the textures, uploaded as the BC textures they are, with all their mip levels;
  - the props the cells place (air conditioners, benches, street lights...) from `props.p3d.rz`,
    at their positions and rotations;
  - a first lighting model (sun, sky light, fog) in place of the game's.
- Controls:
  - hold the right mouse button (or press Tab) to look around;
  - W A S D to fly, Space and C to go up and down;
  - Shift to go faster, the mouse wheel to change the speed;
  - P to print the camera position, Esc to quit.
- `manhattan <game folder> --screenshot file.png [--at x,y,z --look yaw,pitch]` draws one view
  and saves it, for checking without a screen. Messages also go to `manhattan.log`.
- Not yet:
  - parked cars and light decals, and props whose parts have joints of their own (each prop is
    drawn as one piece);
  - the textures in other tile modes;
  - the game's own shaders and lighting.

## Next milestones

1. **The rest of the city's data:**
   - instances (CompositeDrawable, locators, `_ft` game objects), which place props, flares
     and decals;
   - the 2D-tiled textures;
   - the shader effects' parameters, the height map, the time-of-day settings (`*_tod`).
2. **The look**: physically based materials from the game's colour, normal and grime textures,
   the sun and sky from the time-of-day files, shadows. Ray-traced shadows and the like belong
   here, without the PS4's rendering in the way.
3. **Characters**: skeletons, skinned meshes and animations (`art/alex`, `art/packages/pedestrians`).
4. **Playable**:
   - Alex on foot in the city: movement, parkour and camera, rebuilt from the game's behaviour
     and checked against the running game;
   - then crowds and vehicles, combat and powers, missions (Lua, `p1/scripts`), the interface
     (Scaleform `.gfx`) and audio.

The native layer's map of the game (`scripts/game_map.py`) and the running port are the
reference: the original's behaviour can be measured, and its code read, at every step.

## Rules

- No game data in the repository or the releases: everything is read from your own copy at run
  time, as now.
- Each milestone ships on its own when it is useful: a viewer, then a city to walk around, then
  more of the game.
