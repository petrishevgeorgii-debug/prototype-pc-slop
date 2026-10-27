# The renderer (PROTOTYPE)

What the port knows about how PROTOTYPE draws a frame, mapped from the PS4 executable (its C++
type information names the classes) and checked in the running game. The plan this serves: a
native renderer, one step at a time, without the game ever drawing through anything but its own
code until a step replaces it.

1. **Map the draw path** (this document): the passes, the primitives, the shaders, down to the
   GPU command each draw becomes. *In progress.*
2. Tag every GPU draw with what it is (pass, primitive type, shader, material) as the port runs
   it, so the GPU side can treat world geometry, characters, particles, the HUD differently.
3. Replace parts of the frame with the port's own Vulkan passes (lighting, shadows), the game
   still deciding what is drawn.
4. Then ray tracing: the scene's geometry is known (2), so it can go into acceleration structures.

## How a frame is drawn

Pure3D (Radical's engine) draws a frame as a tree of **passes** (`pure3d::Pass`). Each pass's
`Render(this, RenderParams*)` is its vtable slot 5 (`RootPass`'s does a little and goes on to
0x2cdf30, presumably its children). The render schemes (`pure3d::ProtoRenderScheme`,
`StandardRenderScheme`...) are likely what builds the tree: not checked yet.

Passes draw **primitives** (`pure3d::Primitive`: `Mesh`, `Skin`, `GpuSkin`, `BillboardGroup`,
`Sprite`, `Decal`, `ParticleSystem`, and the game's `proto::HUD`, `BloodSplatter`,
`BulletTracer`, `LaserSight`), each with a **shader** (`pure3d::Shader`, a material: a
`ps4Shader` program and its parameters).

Everything ends in **`pure3d::ps4Context`** (Pure3D's PS4 device, `pure3d\pddi\src\ps4`), whose
draw functions (vtable slots 4 and 5, 0x234900 and 0x234db0) set the state and write the draw
packets into the current **Gnmx context**: its pointer is at **0x1277938**, and it starts with
the `DrawCommandBuffer`:

| offset | field |
|---|---|
| +0x00 | start |
| +0x08 | end |
| +0x10 | write pointer |
| +0x18 | reserve callback `bool(buffer, dwords, user)`, called when the room left is too small |
| +0x20 | its user data |
| +0x28 | predication (bit 0, or-ed into packet headers) |

The packets are written by the game's statically linked Gnm (0x969f20 writes `DRAW_INDEX_2`,
0x96a040 `DRAW_INDEX_AUTO`; more draw packet writers follow it up to about 0x96a300).
`ps4Context` has debug markers of its own (vtable slots 68 and 69, 0x2367f0 and 0x236800: push
and pop), which the passes use for their parts ("Shadows", "Fast Z", "World", "Front End"...):
the names under the pass classes in the map below.

## The render map (step 1, running)

`game/p1/render_map.cpp` wraps every pass class's `Render` (below): a debug marker NOP with the
class's name before it and one after (the packets `sceGnmInsertPushMarker` and
`sceGnmInsertPopMarker` write), in the game's own command buffer. On the GPU side
(`gpu/shim/port_render_map.cpp`, from `liverpool.cpp`) the draws between them are counted, and a
frame's passes are logged in order with their draws and the pixel shaders those used (by the
hash shader mods are named after, `mods\<hash>_p0.frag`; the four most used), at frames 300,
1200, 2400, then every 3600:

    Render map: frame 3600: 92 passes, 5098 draws (0 outside them)
      RootPass > ProtoShadowPass > Shadows                    688  (3 shaders: ...)
      ...

`THEORDER_RENDER_MAP=1` turns it on (test builds of the launcher do). With
`THEORDER_VK_MARKERS=1` too, the passes are named scopes in a RenderDoc capture.

### The pass classes

| class | vtable | Render |
|---|---|---|
| `engine::render::FrontEndPass` | `0x104ab60` | `0xc36f0` |
| `engine::render::LetterboxRenderPass` | `0x10c5220` | `0x9d0ac0` |
| `pure3d::BloomColourAdjustPass` | `0x1060e20` | `0x325bf0` |
| `pure3d::BloomCompositePass` | `0x1060e80` | `0x3261b0` |
| `pure3d::BloomPass` | `0x1060dc0` | `0x325610` |
| `pure3d::BlurPass` | `0x1060f40` | `0x3268c0` |
| `pure3d::CapturePass` | `0x1060fc0` | `0x328010` |
| `pure3d::ColourMatrixPass` | `0x1061050` | `0x329120` |
| `pure3d::CopyPass` | `0x10610d0` | `0x3293c0` |
| `pure3d::Debug2DPass` | `0x1061150` | `0x329770` |
| `pure3d::Debug3DPass` | `0x10611d0` | `0x329ce0` |
| `pure3d::DebugPass` | `0x1062e70` | `0x3540d0` |
| `pure3d::DepthOfFieldPass` | `0x10612b0` | `0x32b880` |
| `pure3d::FastZPass` | `0x10618d0` | `0x334860` |
| `pure3d::LetterBoxPass` | `0x1062f20` | `0x354640` |
| `pure3d::OldWireframePass` | `0x1061770` | `0x333760` |
| `pure3d::OpaquePass` | `0x10613e0` | `0x32eee0` |
| `pure3d::OrthoNormalPass` | `0x1062fa0` | `0x354700` |
| `pure3d::PrimColourPass` | `0x1063020` | `0x3549c0` |
| `pure3d::ProtoAlphaTestZPass` | `0x10628f0` | `0x349a90` |
| `pure3d::ProtoBloodPass` | `0x1061b50` | `0x3384c0` |
| `pure3d::ProtoBloomColourAdjustPass` | `0x1061c30` | `0x3397e0` |
| `pure3d::ProtoBloomCompositePass` | `0x1061c90` | `0x339de0` |
| `pure3d::ProtoBloomPass` | `0x1061bd0` | `0x3391a0` |
| `pure3d::ProtoCapturePass` | `0x1061d50` | `0x33a110` |
| `pure3d::ProtoColourMatrixPass` | `0x1061df0` | `0x33b5f0` |
| `pure3d::ProtoDecalPass` | `0x1061e70` | `0x33bab0` |
| `pure3d::ProtoFBPass` | `0x1061ef0` | `0x33ccf0` |
| `pure3d::ProtoFastZPass` | `0x1062890` | `0x349300` |
| `pure3d::ProtoFogPass` | `0x1061ff0` | `0x33e2f0` |
| `pure3d::ProtoHUDPass` | `0x1062070` | `0x33e8e0` |
| `pure3d::ProtoLightDecalPass` | `0x10620f0` | `0x33ecf0` |
| `pure3d::ProtoLitRootPass` | `0x10622f0` | `0x3427e0` |
| `pure3d::ProtoMidgroundPass` | `0x1062170` | `0x33f300` |
| `pure3d::ProtoMidgroundZPass` | `0x10627d0` | `0x348bd0` |
| `pure3d::ProtoMotherPass` | `0x10621f0` | `0x3401e0` |
| `pure3d::ProtoOpaquePass` | `0x1062270` | `0x3405b0` |
| `pure3d::ProtoProperFadeBlurPass` | `0x1062c10` | `0x350e90` |
| `pure3d::ProtoProperFadePass` | `0x1062c90` | `0x351800` |
| `pure3d::ProtoRadialBlurPass` | `0x1062470` | `0x343e60` |
| `pure3d::ProtoSSAOApplyPass` | `0x1062d10` | `0x352460` |
| `pure3d::ProtoSSAOExtractionPass` | `0x1062d90` | `0x352a30` |
| `pure3d::ProtoShadowPass` | `0x10624f0` | `0x344cd0` |
| `pure3d::ProtoSlowZPass` | `0x1062830` | `0x348f00` |
| `pure3d::ProtoThermalPass` | `0x1062570` | `0x3468f0` |
| `pure3d::ProtoToneMapPass` | `0x1061f50` | `0x33d580` |
| `pure3d::ProtoTranslucentPass` | `0x1062650` | `0x347390` |
| `pure3d::ProtoTranslucentPassBase` | `0x10625f0` | `0x347390` |
| `pure3d::ProtoWireframePass` | `0x10626f0` | `0x347b80` |
| `pure3d::ProtoZPass` | `0x1062770` | `0x348560` |
| `pure3d::RootPass` | `0x1061460` | `0x330380` |
| `pure3d::ShadowDebugPass` | `0x10614e0` | `0x330550` |
| `pure3d::ShadowPass` | `0x1061560` | `0x330ca0` |
| `pure3d::SlowZPass` | `0x1061870` | `0x334690` |
| `pure3d::ToneMapPass` | `0x10630a0` | `0x3555c0` |
| `pure3d::TranslucentPass` | `0x10615e0` | `0x332310` |
| `pure3d::ViewPass` | `0x1061660` | `0x332a00` |
| `pure3d::WireframeOverlayPass` | `0x1061710` | `0x333610` |
| `pure3d::ZPass` | `0x1061810` | `0x333fc0` |

## A frame, mapped (b9, the open city, standing still)

The render map's frame 3600 (5098 draws; passes without draws of their own left out unless
they hold others). Indented names are children; `Proto...Pass` are pass classes, the rest the
game's own markers inside them.

    RootPass
      ProtoShadowPass > Shadows                         688   the shadow maps
      LetterBoxPass
        ViewPass > Visibility
        ViewPass > ProtoLitRootPass > ProtoCapturePass   1
          ProtoZPass                                          the depth pre-pass
            ProtoFastZPass > Fast Z                     928
            ProtoSlowZPass > Slow Z                     126
            ProtoAlphaTestZPass > Alpha Test Z            0
            ProtoMidgroundZPass > Midground Z           127   then the kick (below)
          ProtoSSAOExtractionPass > SSAO Extract > Linearize Z, SSAO Calc, Blur X, Blur Y (1 each)
          ProtoDecalPass > Decal                         17
          ProtoLightDecalPass > Light decal               0
          ProtoOpaquePass > Opaque
            Unlit 28, World 224, Objects 792, Road > World 10, Sky > Unlit 1
          ProtoMidgroundPass > Midground > Unlit        127
          ProtoSSAOApplyPass > SSAO Apply                 1
          ProtoBloodPass > Blood                          0
          ProtoTranslucentPass > Translucent 49, Particles 3, Mission markers 0
          ProtoProperFadePass
        ViewPass                                              post-processing
          ProtoRadialBlurPass > Radial blur               1
          DepthOfFieldPass > Depth of Field > Generate Bluriness, Downsample, Blur framebuffer 2, Composite
          ProtoFBPass > FB Effects > ProtoBloomPass > Bloom (Colour Adjust, Blur 2), Draw distortion objects, DrawQuad
          ProtoHUDPass > HUD                              8
        LetterboxRenderPass
        FrontEndPass > Front End                         81   the Scaleform UI

A forward renderer: shadow maps, a depth pre-pass (which the CPU waits on: it is
kicked to the GPU early), SSAO from the depth, then the opaque world and objects lit in one go,
the midground (distant city), translucent, then post-processing on the frame buffer. Opaque
objects are the most draws (792 here), the shadow maps and the depth pre-pass together 1869.

### The kicked depth pre-pass ran twice

`ProtoMidgroundZPass`'s Render ends with `ps4Context`'s kick (vtable slot 70, 0x236810): an
end-of-pipe label (the CPU waits for it later, 0x237870) and Gnmx's `submit()` (0x91d790) of the
frame so far. But `submit()` sends the context from its submission start (+0x108) and leaves it
there; only `reset()` (0x920d50, at the frame's start) and a full command buffer (0x921101) move
it. So the frame's `submitAndFlip` (0x91db30) sent the whole frame again from its start, and the
GPU drew the shadow maps and the depth pre-pass twice every frame: in the first render map the
frame's list began with them, unclosed, and then the whole frame again with the same draw counts
(688, 928, 126, 127). On a PS4 the same.

`game/p1/frame_submit.cpp` moves the submission start to the end of what the kick sent (as Gnmx
does for a full buffer: the start of the draw buffer +0x108, of the other two +0x110 and +0x118,
the kept ranges at +0x2b8 to none), so the last submit sends the rest of the frame only: about
1870 fewer draws a frame in the city, of 5100. `THEORDER_NATIVE=-framesubmit` leaves it out.
Checked in b10: the frame's list is one tree, every pass closed, and the frame stats' draws per
frame in the city went from 3300-6150 (b9) to 1650-3550.

### The pixel shaders (b10, frame 7200)

The most used per part, by the hash shader mods are named after (`mods\<hash>_p0.frag`):

| part | draws | shaders | most used |
|---|---|---|---|
| Shadows | 919 | 6 | `e5684a4c` (792), `8aaeb600` (75) |
| Fast Z | 729 | 1 | `e5684a4c` |
| Slow Z | 91 | 6 | `8aaeb600` (60) |
| Midground Z | 26 | 2 | `e5684a4c` (22) |
| SSAO: Linearize Z, Calc, Blur X/Y | 1 each | | `1f51c54f`, `f6c8b098`, `5e160f08` |
| Decal | 95 | 4 | `3faccb42` (52), `c644fc6a` (38) |
| Opaque: Unlit | 63 | 4 | `3013c54` (60) |
| Opaque: World | 316 | 18 | `e3e8f4d` (115), `e9490c95` (92) |
| Opaque: Objects | 433 | 29 | `9c16da53` (142), `6afa801b` (37) |
| Sky | 1 | 1 | `f5131314` |
| Midground (distant city) | 26 | 3 | `68fe1d0f` (13) |
| SSAO Apply | 1 | 1 | `bb27e350` |
| Translucent | 129 | 18 | `d497baf1` (28), `819705e3` (27), `4cfd3a02` (26) |
| Particles | 3 | 1 | `f7814d18` |
| Radial blur, depth of field, bloom, distortion | 1-4 each | | `1b837996`, `d9caa112`... |
| Front End (Scaleform) | 108 | 4 | `9a490302` (75) |

`e5684a4c` is the depth-only shader (shadow maps and the depth pre-pass), `8aaeb600` its
alpha-tested version; the shader mods in the port's `mods` folder change these two, the SSAO
(`f6c8b098`) and the sky (`f5131314`). The world and objects are the lit materials: about 50
shaders between them.

## Draw tags (step 2, b11)

Every draw of a Pure3D primitive group goes through `ps4Context`'s **DrawPrimGroup** (vtable
slot 4, `0x234900`: this, a key, the primitive group, ...) or **DrawPrimGroups** (slot 5,
`0x234db0`: this, an array of groups, their count). A primitive group's shader (its material,
a `pure3d` shader object) is at its `+0xa0` (when 0, the context's default at `+0x728`).

With the render map on, `game/p1/render_map.cpp` wraps both: before each call a **draw tag** goes
into the command buffer (a NOP with payload `0x6875d7a6`: the slot, the return address into the
caller, the group's shader and the vtables of the shader and the group), after it a tag end
(`0x6875d7a7`). The GPU side gives the draws in between to that caller, and under each pass the
log lists them:

    RootPass > ... > Opaque > Objects                         433  (29 shaders: ...)
          draw 4 from 2bb4f3: 412 draws in 412 calls, 37 materials, group 105e000, shader 105c3a0 x400
          untagged: 21 draws

What b11 showed (frame 7200, 3714 draws): every 3D draw is tagged; only the full-screen passes,
the HUD and Scaleform's front end are not. But the callers are not the primitives: they are the
passes' own drawing loops, which draw sorted lists of primitive groups (Pure3D sorts the scene's
primitives into the passes' lists first):

| caller | in | draws | materials |
|---|---|---|---|
| `0x345943` (slot 5), `0x345d99` (4) | ProtoShadowPass's list drawer (`0x345500`) | 807 in 4 calls, 114 | 3, 22 |
| `0x3499a6` (5) | `ProtoFastZPass` Render | 1040 in 1 call | 1 |
| `0x348f6e` (4) | `ProtoSlowZPass` Render | 168 | 18 |
| `0x348d8c` (5), `0x348d0b` (4) | `ProtoMidgroundZPass` Render | 47 in 1 call, 10 | 1, 2 |
| `0x33be7e` (4) | `ProtoDecalPass` Render | 33 | 21 |
| `0x3434ce` (4) | the unlit list drawer (`0x3433e0`): Unlit, Sky, Midground | 124, 1, 57 | 3, 1, 14 |
| `0xaabe58` (4) | the opaque drawer (`0xaabd10`): World, Road | 507, 12 | 256+, 12 |
| `0xaac0d5` (4) | the same, Objects | 565 | 212 |
| `0x346ddc`, `0x347172` (4) | the translucent drawers (`0x346b40`, `0x347050`) | 38, 38 | 30, 33 |

So slot 5 (whole arrays in one call) is the depth-only work: the fast Z list draws 1040 groups
with one material. Everything lit goes one group at a time through slot 4, each with its own
material: about 470 materials in the opaque world and objects. A material here is a
`pure3d::ps4Shader` (vtable `0x105b920`; every group's shader is one), a `pddiShaderCommon`:
`+0x10` its program (the shader definition, refcounted, given to its constructor), `+0x18` its
parameter block (0x30 bytes), `+0x28` a serial number. Primitive groups have no vtable (their
first field is data).

What a draw is (static world, character, vehicle) is therefore not in who calls the draw; it is
in the material's program (skinned or not) and the group's vertex format. 

A material's `+0x10` is its **template** (`pure3d::ps4ShaderTemplate`, vtable `0x105b9a0`; the
material's slot 5, `0xa96990`, returns it): the kind of shader, shared by every material of that
kind. Its constructor (`0x221aa0`) keeps only its name's hash (`+0x3c0`, from `0x9397f0`:
`h = h * 31 + c` over the name, letters lowercased), so the names are matched offline against the
strings in the game's files. From b12 the tag carries that hash, and under each caller the log
counts the draws per template and per vertex shader (by hash).

The b12 log's 63 templates all match names in the game's shader file (`art/startup_shaders.p3d`)
or the executable. In the open city:

| pass (caller) | templates, by draws |
|---|---|
| Opaque > World | `zCBV2_env_color_noglass` 71, `zCBV2_env_color_glass` 60, `zCBV2_ao_building_glass_grime` 19, `zCBV2_ao_building_noglass` 18, `zCBV2_env_building_noglass` 10, `env_water` 8, `zCBV2_ao_billboard` 7, `zCBV2_env_sidewalk` 4 |
| Opaque > Objects | `env_building_hive_noglass` 136, `env_vehicle_military` 53, `env_vehicle_colorvar` 46, `char_ped` 42, `env_car_tire_no_vertex_blend` 23, `env_building_hive` 22, `env_car_tire` 22 |
| Opaque > Road | `zCBV2_env_road` |
| Unlit | `env_alphacbv`, `unlit`; Sky: `env_sky_dynamic` |
| Midground (distant city) | `env_midground_road`, `env_midground`, `env_midground_tree`, `env_midground_shoreline`, `env_midground_traffic` |
| Decal | `decal_1c`, `env_decal`, `env_decal_nm`, `env_decal_bias_nm`, `env_hive_decal` |
| Translucent | `env_vehicle_glass_fade` 43, `env_sign_alpha`, `env_haze`, `fx_cbvcolor_alpha` (mission markers) |
| Shadows, Slow Z (one group at a time) | `env_alphacbv`, `env_vehicle_colorvar`, `env_car_tire`, `1c1a`, `1c1n`, `prop_damage_2nm_vtx`, `char_ped_lod`... |

Others seen: `char_alex_cloth`, `char_alex_tendril`, `char_ped_colorvar`, `env_vehicle_lod`,
`env_debris`, `env_lit_sign`, `env_litMarquee`, `env_videoticker`, `env_videoscreen1`,
`env_bridgelights`, `env_hive_tower`, and generic ones named by their inputs (`1c`, `1cv`, `1c1n`,
`1c1s1n1a`: colour, normal, specular, alpha maps). The depth-only batches (slot 5) carry the
first group's template only, which says nothing about the rest.

So the template is what a draw is: `char_alex*` Alex, `char_*` people, `env_vehicle*` and
`env_car_tire*` vehicles, `*decal*` decals, `env_midground*` the distant city, the rest of
`env_*`/`zCBV2_*` and the generic ones the world. From b13 the port learns the names itself (it
wraps the hash call in the template's constructor, `0x21db06`, while the render map is on, and
writes each name once into the command buffer, NOP `0x6875d7a8`), names the templates in the log,
and sums each frame's draws by kind:

    Render map: frame 1200 by kind: world 978, roads 22, Alex 4, people 74, vehicles 83, decals 49,
      distant city 144, sky 1, water 5, unlit 2, depth batches 1494, the rest 115

(b13, the open city: every tagged draw's template named; "the rest" are the full-screen passes,
the HUD, Scaleform and a few translucent draws outside the tags.)

## Next: the scene's geometry (towards ray tracing)

The plan for step 3: take each solid draw's triangles **after its vertex shader**. The port
translates every vertex shader to SPIR-V already; a variant for tagged draws of the kinds that
count (world, roads, Alex, people, vehicles) would also write each vertex's position to a
storage buffer. That gives skinned characters in their pose and every object where it is, with
no need to know the game's vertex formats, skinning or world matrices; the camera's matrices
(`p3dViewProjectionMatrix` and the rest are named constants) turn clip space back into world
space. The index buffer is the draw's own. Then acceleration structures from those triangles,
rebuilt or refitted each frame, and ray-traced shadows as the first use.

### The scene capture (b14, first try)

`video_core/renderer_vulkan/vk_scene_capture.{h,cpp}`, `THEORDER_SCENE_CAPTURE=1` (test builds,
with the render map). Every 30 s from frame 900 one frame is a capture frame:

- The draw pipeline is off for it (the choice is per draw, on the command processor's thread,
  where the render map knows the draw's kind).
- A solid draw (world, roads, Alex, people, vehicles) asks the pipeline cache for its **capture
  variant** (`RequestSceneCapture`): the same pipeline with the vertex shader permutation
  `hw.vs.scene_capture`, whose epilogue (`EmitVertexCapture`, before the depth and clip-space
  conversions) stores `gl_Position` by buffer device address for every vertex of the draw's first
  instance. Its key has `motion_vectors = 2` (no new key field: the stored pipeline keys keep their
  hashes) and is not stored for later sessions. Until a variant is built (in the background) the
  draw uses its usual pipeline.
- The rasterizer gives the draw its parameters (`SceneCaptureParam`: the vertex range its indices
  reach, as the object motion does, through the push constants' `motion_param`) and keeps its
  triangles (indices rebased onto the stored positions).
- Once the GPU is done with that frame, the log sums it up per kind (vertices written, triangles
  complete, in front of the camera, in view, the range of w), and the first two captures are
  written to `scene_capture_<n>.ply` (x, y and w of the clip positions: the camera's view up to a
  scale per axis).

The primitive classes (vtables; slots 0-1 the destructors):

| class | vtable |
|---|---|
| `pure3d::Mesh` | `0x105f920` |
| `pure3d::Skin` | `0x105c1d0` |
| `pure3d::GpuSkin` | `0x105f860` |
| `pure3d::BillboardGroup` | `0x105f4a0` |
| `pure3d::Decal` / `DeformableDecal` | `0x105f680` / `0x105f6f0` |
| `pure3d::ParticleSystem` | `0x105e7c0` |
| `pure3d::Sprite` | `0x105ff50` |
| `proto::HUD` | `0x1092950` |
| `proto::BloodSplatter`, `BulletTracer`, `LaserSight` | `0x10a3ea0`, `0x10926d0`, `0x10a3fc0` |
| `engine::MotionTrail` | `0x104acd0` |

## Next

- Primitives: their draw (`Mesh` vtable 0x105f920, `Skin` 0x105c1d0, `GpuSkin` 0x105f860...)
  and the shader each uses, so draws can be tagged with what they are (step 2).
- `ps4Context`'s state calls: render targets, the shader programs (`ps4Program`), constant
  buffers (`ps4ShaderConstantBuffer`): where the camera, lights and materials reach the GPU.
