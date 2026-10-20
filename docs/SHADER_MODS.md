# Shader mods

The port runs PROTOTYPE's own shaders: it recompiles each one from the PS4's GPU code to SPIR-V
(Vulkan) when the game first uses it. A **shader mod** replaces that SPIR-V with your own,
written in GLSL, so you can change how anything in the game is drawn: its lighting, materials,
colours, fog, water, effects. The game files are never changed and the port ships none of the
game's shaders: you export them from your own copy.

## Find the shader that draws something

1. In game, press **Insert** (or R3 + L2) and go to **SHADERS** (Tab / E).
2. **SHADER** lists the pixel shaders the last frame drew, most draws first, by their ID (for
   example `a71ff1ce`), with a name next to the ones the port knows (RIVER, DEPTH OF FIELD,
   AMBIENT OCCLUSION, POST FX...). Left / Right picks one.
3. **HIDE THIS SHADER** leaves its draws out: whatever disappears from the picture is what it
   draws. **SHOW ALL** brings everything back (so does Backspace / Triangle on this page).

Large world surfaces (streets, walls, buildings) are usually near the top: they take many draws.

## Export it for editing

**EXPORT FOR EDITING** writes the picked shader into `mods\shaders` in the port folder:

- `<id>_p<version>.frag`: the shader as GLSL, one file per version. The port makes a version
  for each combination of texture types and states the game uses the shader with; edit the
  version(s) you need (usually all of them the same way).
- `_originals\<id>_p<version>.spv`: the untouched SPIR-V, for reference (not loaded).

A version appears in the export once the game has used it (the shader cache in `user\cache`
holds them). An existing `.frag` is never overwritten: delete it to export it again.

Exporting needs **spirv-cross**, compiling needs **glslangValidator**. The release has both in
`bin`; a source build finds them in MSYS2 (`pacman -S mingw-w64-clang-x86_64-spirv-cross
mingw-w64-clang-x86_64-glslang`). `THEORDER_SPIRV_CROSS` and `THEORDER_GLSLANG` can point to
other copies.

## Edit and test

Open the `.frag` in any text editor. It is the game's shader as the port translated it, so the
names are generated (`_123`, `v4`), but the structure is the game's: textures sampled, maths,
and at the end the colour written to the output declared at the top, for example

```glsl
layout(location = 0) out vec4 _out0;
...
void main()
{
    ...
    _out0 = vec4(_1234, _1235, _1236, _1237);
}
```

A first test: make the surface red by adding a line at the end of `main()`:

```glsl
    _out0.rgb = mix(_out0.rgb, vec3(1.0, 0.0, 0.0), 0.5);
```

Save the file and **restart the game**. At start the port compiles every changed source into
`mods\shaders\.compiled` and uses it in place of the original when that shader is built. The
SHADERS page shows `MOD` next to modded shaders and **SHADER MODS** shows what happened (how
many mods loaded, or the first compile error; the full compiler output is
`.compiled\<file>.log`). `last_run.log` lists every mod that was applied.

To go back to the original, delete or rename the file (a name starting with `_` or `.` is
ignored).

## Files

| File | Applies to |
|---|---|
| `mods\shaders\<id>_p<version>.frag` (`.vert`, `.comp`, ...) | that version of the shader |
| `mods\shaders\<id>.frag` | every version: only safe when they all have the same inputs (compare the exported ones) |
| `mods\shaders\<id>_p<version>.spv` | a precompiled mod |
| `mods\<name>\shaders\...` | the same, grouped as a named mod; `mods\shaders` wins over named mods |

## Sharing

The edited files contain the game's shader code (decompiled), so share them with care; a mod is
best shared as the changes to make (or a small script that applies them to an export).

## Limits (so far)

- Changes apply at the next start, not live.
- Only the shader's own inputs exist: a mod cannot add textures or render passes yet (planned:
  extra material textures and effect slots, see the roadmap in README.md).
- A few shaders may not decompile to GLSL that compiles again (spirv-cross); the status line says
  so. Fine floating-point modes of the original are lost in GLSL; results can differ in the last
  bits.
- Version numbers come from your own shader cache: they can differ on another PC.
