# PROTOTYPE's Pure3D files

PROTOTYPE (PS4, CUSA06545) keeps its world, characters, effects and interface in Pure3D files
(`p1/art/**/*.p3d.rz`). This is what has been worked out by reading them; `tools/formats/pure3d.py`
implements it. Everything is little-endian.

## Containers

- **`.rz`**: `"RZ"`, 6 zero bytes, the unpacked size (u64), then one zlib stream.
- **`.p3d`**: a tree of chunks. Each chunk is:
  - id (u32);
  - data size (u32, including this 12-byte header);
  - total size (u32, including the chunk's children);
  - its data;
  - its children.

  The file is chunk `0xff443350` (`"P3D\xff"`) with only a header.
- **Strings**: one length byte, then that many bytes, zero-padded.

## The city

`art/locations/manhattan/` holds the city:

| File | Contents |
|---|---|
| `manhattan_Cell_<n>.p3d.rz` | The detailed city, streamed in by cell. Many are empty (33 bytes). |
| `manhattan_Cell_<n>_ft.p3d.rz` | Each cell's game objects. |
| `midgeo.p3d.rz` | The whole city, low detail: one mesh per cell, plus the distant surroundings (about 100,000 triangles). |
| `textures.p3d.rz` | Textures shared by the cells. |
| `props.p3d.rz`, `heightMapExterior.p3d.rz` | Props and the height map. |

Positions are in metres, world space, y up. The island is about 2 x 3.7 km.

## Chunks

| Id | Name | Data |
|---|---|---|
| `00010000` | Mesh | name, ...; children MeshStats, PrimGroup x n, BoundingBox, BoundingSphere, RenderStatus, MeshLod |
| `00010020` | PrimGroup | u32 version, shader name, u32 primitive type (0: triangle list), u32 vertex flags, u32 vertex count, u32 index count; children VertexFormat, VertexBuffer, IndexBuffer, BoundingBox, BoundingSphere |
| `00010014` | VertexFormat | u16 1, u16 2, u32 2, u32 buffer size, u32 element bytes (17 per element). Then per element: u32 name hash, u32 packed format (7: 4 x u8, else 0), u32 offset, u16 stride, u16 components, u8 kind. |
| `00010012` | VertexBuffer | u16 1, u16 2, u32 2, u32 size, the vertices |
| `00010013` | IndexBuffer | u16 1, u16 2, u32 0, u32 size, the indices (u16) |
| `00010003` / `00010004` | BoundingBox / BoundingSphere | 2 x float3 / float3 + radius |
| `00011015` | Shader | name (the hash the PrimGroups use), u16 0, u16 1, effect name (e.g. `zCBV2_ao_building_noglass`), u32 parameter count; children ShaderTexture `00011016` (parameter, texture name), ShaderFloat `00011017`, ShaderVector `00011019`, ShaderColour `0001101a`, ShaderInt `00011021`, ShaderTextureSet `00011020`, ShaderStates `0001101c` |
| `00019000` | Texture | name, ...; child Image `00019001`, then ImageData `00019002` and ImageSource `00019003` (the artist's path) |
| `00019002` | ImageData | u32 size, then a **GNF**: `"GNF "`, u32 header size (0xf8), u8 version 2, u8 count 1, ..., then the GPU texture descriptor (T#, 8 words) and, after the header, the texels in the GPU's tiled layout |
| `00023000` | Skeleton | name, joints `00023001` |
| `00123000` | CompositeDrawable | props made of parts, with their placement |
| `00124000` / `00010029` | Locator / MeshTransform | positions and matrices |
| `07020000` | PhysicsObject | ravenphysics shapes and materials (`07016xxx`, `07021xxx`) |
| `07f00000` | GameObject | `name(1):instance`, attributes `07f00001` (`META`, then named values: `VectorAttribute`...) |
| `00017005`..`0001700e` | Flares | light flares (FlareGroup `00017006`) |

### Placed objects (`*_ft` files)

The cells' `_ft` files place the city's objects as GameObjects (`07f00000`). Each chunk holds
the full name (`proto::CloudSpawner(1):Cell_100airconditionerOS001_1`), the short name and the
type. Its GameObjectAttributes child (`07f00001`) holds:

- u32 size;
- `"META"`;
- a position (3 floats, world space);
- a rotation quaternion (4 floats, x y z w);
- what the type keeps.

Cell 100 has these types:

| Type | Count | What it is |
|---|---|---|
| `proto::CloudSpawner` | 1087 | Props. The data names the prop (`airconditionerOS001`, `Collectible004`), then a scale. |
| `proto::ParkedCarSpawner` | 73 | Parked cars. |
| `proto::LightDecalInfo` | 68 | Light decals (a layout of their own). |
| `proto::WebNodeLocator` | 4 | |
| `proto::AmbientExclusionVolume` | 2 | |

A prop is in `props.p3d.rz` under its name, in three chunks:

- a CompositeDrawable (`00123000`);
- a Skeleton (`00023000`), whose joints place the parts;
- meshes `<name>InitialShape` and `<name>DamagedShape`.

`pure3d.py objects <cell>_ft.p3d.rz` lists the placed objects.

### Vertex elements

Names are hashes; consecutive values are consecutive sets.

| Hash | Element | Data |
|---|---|---|
| `2c929929` | position | float4 (xyz, plus a per-vertex value in w) |
| `00364509` / `0a` / `0b` | uv0 / uv1 / uv2 | float2 |
| `c206bce7` | normal | float3 |
| `a4176245` | tangent | float4 |
| `3898fc04` / `05` | colour0 / colour1 | 4 x u8 (packed format 7) |

### Textures (GNF)

Read from the T#:

| Field | Location |
|---|---|
| Data format | word 1, bits 20-25 |
| Width - 1 | word 2, bits 0-13 |
| Height - 1 | word 2, bits 14-27 |
| Tile mode | word 3, bits 20-24 |
| Pitch - 1 | word 4, bits 13-26 |

The city's textures seen so far are all:

- **Format:** BC1 (35) or BC3 (37).
- **Tile mode:** 13 (Thin1DThin). Each 8 x 8 block tile holds its blocks in the order x0 y0 x1 y1 x2 y2, and the tiles go in rows across the pitch.

Other tile modes (2D macro tiling) are still to do.

## The light rig (`manhattan_tod.p3d.rz`)

The time-of-day file is game objects (0x07f00000) like the cells' `_ft` files, but the plain
`GameObject(1):name` ones have named attributes after `META`: a u32 count, then for each a
u32-length name, a u32-length type and the value.

- `transform` (`MatrixAttribute`): u16 1, the position (3 floats) and rotation (quaternion
  x y z w).
- Lights (`engine::LightBehaviour`): 16 bytes after the behaviour's name, the colour (3 floats)
  and the intensity (1.1 for the sun, 0 for the others). The light shines along its local +z.
  - `directional_sun`: towards (0.58, 0.82, -0.03), colour (0.92, 0.91, 0.77) × 1.1.
  - `ambientLight1` (0.29, 0.39, 0.55); `fillLight1`, `fillLight2` from above, blue-grey;
    `shadowLight`, almost straight down.
- Districts (`proto::DistrictBehaviour`, with an `engine::trigger::OBBVolume`): their centre is
  the transform's position; their name and the vehicles they spawn follow.
- `proto::TimeOfDayVarGroup` objects (`default[0]`, `Times Square`, `Infected`...) hold each
  light's keyframes over the day; not read yet.

## Skeletons and composite drawables (vehicles)

- **Skeleton (0x23000):** name, u32, u32 joint count; children SkeletonJoint (0x23001): name,
  u32 parent index, then a 4 × 4 matrix relative to the parent (row vectors: the translation is
  the last row). The first joint is the root.
- **CompositeDrawable (0x123000):** u32 0, name, the skeleton's name, u32 count; children
  (0x123001): two u32 0, the mesh's name, u32 (translucent), u32 joint index.
- A vehicle file has the whole vehicle (named after it), `_crushed`, `_med_lod` and `_low_lod`
  forms, each with its own skeleton.
- `<name>_tod.p3d.rz` is big-endian ("ATEM"): `proto::ShaderPalette` lists the paint colours
  (`color_variation`: r g b and a fourth value).

## Tools

```
python tools/formats/pure3d.py tree manhattan_Cell_100.p3d.rz        # the chunk tree
python tools/formats/pure3d.py meshes manhattan_Cell_100.p3d.rz      # meshes and vertex layouts
python tools/formats/render_preview.py city.png midgeo.p3d.rz        # quick shaded picture
python tools/formats/render_textured.py cell.png manhattan_Cell_100.p3d.rz textures.p3d.rz
python tools/formats/export_obj.py out manhattan_Cell_100.p3d.rz textures.p3d.rz  # OBJ + PNG for Blender
```

What these make comes from your own copy of the game. Keep it to yourself: none of it belongs in
the repository or a release.

