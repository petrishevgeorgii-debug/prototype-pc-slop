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
  - the textures in other tile modes (one font in the city's files).

**Milestone 2: the look.** The first build is in.

- The light is the city's own: `manhattan_tod.p3d.rz` holds the game's light rig, read as it is:
  - the sun (`directional_sun`: its direction from its rotation, colour and intensity);
  - the sky's light (`ambientLight1`) and two fill lights from above (`fillLight1`, `fillLight2`).
  The engine makes the sun stronger and the others weaker than the game does, because it lights
  in linear colour with real shadows.
- The sun casts shadows: four cascades (to 18, 60, 200 and 650 m) in one 4096 × 4096 depth map,
  each moved in whole texels so the edges stay still, filtered over four taps. Cut-out leaves
  cast cut-out shadows.
- The surfaces use the shader's textures:
  - the colour map, and its alpha to cut out leaves, fences and signs (effects with `1a` or
    `alpha` in their name);
  - the normal map (red and green are x and y), with the vertices' tangents;
  - the vertex tint: the effects whose name starts with `zCBV` ("colour by vertex") darken and
    colour each building by its vertices' first colour;
  - decals (`env_decal*`: the hive infection on the ground and walls) blended over what is
    under them.
- Left out until the engine draws them properly: light cones (`env_light_decal_spot`),
  particles (`fx_*`), lit signs, the low-detail city's window lights and traffic.
- A sky (from the haze at the horizon to the ambient light's colour overhead, with the sun),
  haze that thins with height, and ACES tone mapping.
- The districts (`proto::DistrictBehaviour` objects, from Times Square to Harlem) are read too:
  N and B fly to the next and previous one, `--district Times_Square` starts there, and the
  window's title shows where the camera is.
- More controls: L turns the shadows off and on; [ and ] change the exposure (`--exposure`).
- Not yet: the grime textures (with the second UVs and the tint's alpha), the vertices' second
  colour (light baked near the street), reflections and glass, the time of day's keyframes
  (`TimeOfDayVarGroup`, in `default[0]`).

**Parked cars.** In.

- Each cell's `_ft` file has its parking places (`proto::ParkedCarSpawner`, all named
  `generic_taxi`: the name is a stand-in). The engine gives each one a vehicle from the list of
  the district it is in (each district names its vehicles with weights, e.g. University_Poor:
  sedanOS003, sedanOS001, suvOS001, carOS005...). Buses, tankers and emergency vehicles are not
  parked. The choice comes from the place's name, so it is the same every time.
- A vehicle (`art/packages/vehicles/<name>/<name>.p3d.rz`) is a CompositeDrawable: meshes
  (body, doors, wheels) on the joints of a Skeleton. The engine puts them together at the
  skeleton's rest pose; the driver and the headlights' glow are left out.
- Paint: `env_vehicle_colorvar` paints the body where its `color_variation_mask` is black. The
  colours are the vehicle's own palette (`<name>_tod.p3d.rz`, `proto::ShaderPalette`; that file
  is big-endian), one per car.
- Glass (`env_vehicle_glass_fade`): dark, see-through and reflecting the sky, more at grazing
  angles.

## Next milestones

1. **The rest of the city's data:**
   - instances (CompositeDrawable, locators, `_ft` game objects), which place props, flares
     and decals; props with joints of their own;
   - the shader effects' parameters, the height map, the time of day's keyframes.
2. **More of the look**: grime, glass and reflections, the night's lights, the hive's glow;
   ray-traced shadows and the like belong here, without the PS4's rendering in the way.
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
