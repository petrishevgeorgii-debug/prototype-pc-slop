#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exports the world geometry of Pure3D files to Wavefront OBJ with materials and PNG textures,
for Blender or any 3D viewer (no game code). Textures are looked up in all the files given, so
add the location's textures.p3d.rz.

  python tools/formats/export_obj.py out_dir file.p3d.rz [more files]

The export comes from your own copy of the game: it is for viewing, not for sharing.
"""
import argparse
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pure3d  # noqa: E402

COLOUR_PARAMS = ('color', 'colour', 'diffuse', 'color_inards', 'color_infection')


def safe(name):
    return re.sub(r'[^A-Za-z0-9_.-]', '_', name)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('out', type=Path)
    p.add_argument('files', nargs='+')
    p.add_argument('--all', action='store_true', help='also the props modelled at their own origin')
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    roots = [pure3d.load(f) for f in a.files]
    shaders, texture_chunks = {}, {}
    for root in roots:
        texture_chunks.update(pure3d.textures(root))
        for _, c in root.walk():
            if c.id == 0x11015:
                name, effect, tex = pure3d.shader(c)
                shaders[name] = (effect, tex)
    written, missing = {}, set()

    def texture_file(name):
        if name in written:
            return written[name]
        written[name] = None
        chunk = texture_chunks.get(name)
        if chunk is None:
            missing.add(name)
            return None
        _, _, _, image = pure3d.texture(chunk)
        if isinstance(image, str):
            missing.add(f'{name} ({image})')
            return None
        file = safe(Path(name).stem) + '.png'
        image.save(a.out / file)
        written[name] = file
        return file

    obj = [f'mtllib scene.mtl']
    mtl = []
    materials = set()
    base = 1
    for root in roots:
        for mesh_name, prims in pure3d.meshes(root):
            for i, prim in enumerate(prims):
                pos = pure3d.positions(prim)
                if not pos or prim.kind != 0 or not prim.indices:
                    continue
                if not a.all:
                    cx = sum(v[0] for v in pos) / len(pos)
                    cz = sum(v[2] for v in pos) / len(pos)
                    span = max(max(v[k] for v in pos) - min(v[k] for v in pos) for k in range(3))
                    if (cx * cx + cz * cz) ** 0.5 < 60 and span < 60:
                        continue
                uv = next((e for e in prim.elements if e[0] == 'uv0'), None)
                effect, tex = shaders.get(prim.shader, ('', {}))
                colour = next((tex[k] for k in COLOUR_PARAMS if k in tex), None)
                material = safe(prim.shader or 'none')
                if material not in materials:
                    materials.add(material)
                    mtl.append(f'newmtl {material}\nKd 0.8 0.8 0.8')
                    if colour and (file := texture_file(colour)):
                        mtl.append(f'map_Kd {file}')
                    mtl.append(f'# effect {effect}\n')
                obj.append(f'o {safe(mesh_name)}_{i}\nusemtl {material}')
                for v in pos:
                    obj.append(f'v {v[0]:.4f} {v[1]:.4f} {v[2]:.4f}')
                if uv:
                    for k in range(len(pos)):
                        u, w = struct.unpack_from('<2f', prim.vertices, k * prim.stride + uv[1])
                        obj.append(f'vt {u:.5f} {1 - w:.5f}')
                idx = prim.indices
                for t in range(0, len(idx) - 2, 3):
                    q = [base + idx[t + j] for j in range(3)]
                    if uv:
                        obj.append(f'f {q[0]}/{q[0]} {q[1]}/{q[1]} {q[2]}/{q[2]}')
                    else:
                        obj.append(f'f {q[0]} {q[1]} {q[2]}')
                base += len(pos)
    (a.out / 'scene.obj').write_text('\n'.join(obj) + '\n')
    (a.out / 'scene.mtl').write_text('\n'.join(mtl) + '\n')
    print(f'{a.out / "scene.obj"}: {base - 1} vertices, {len(materials)} materials, '
          f'{sum(1 for v in written.values() if v)} textures')
    if missing:
        print(f'{len(missing)} textures not found in these files (add textures.p3d.rz): ' +
              ', '.join(sorted(missing)[:8]) + (' ...' if len(missing) > 8 else ''))


if __name__ == '__main__':
    main()
