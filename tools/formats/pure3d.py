#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""PROTOTYPE's Pure3D files (.p3d, .p3d.rz): reading them without the game's code.

Format notes: docs/formats/pure3d.md. The Manhattan Engine (docs/ENGINE.md) loads the same data.

  python tools/formats/pure3d.py tree  <file.p3d[.rz]> [--depth N]   chunk tree
  python tools/formats/pure3d.py stats <file>...                      chunk types and counts
  python tools/formats/pure3d.py meshes <file>                        meshes, their parts, sizes
"""
import argparse
import collections
import struct
import sys
import zlib
from pathlib import Path

# Chunk ids seen in PROTOTYPE's files, named after the Pure3D layout they follow (the same
# family as earlier Radical games) and what their contents show. Unnamed ones print as hex.
CHUNK_NAMES = {
    0xff443350: 'File',
    0x00010000: 'Mesh',
    0x00010003: 'BoundingBox',
    0x00010004: 'BoundingSphere',
    0x00010012: 'VertexBuffer',
    0x00010013: 'IndexBuffer',
    0x00010014: 'VertexFormat',
    0x00010017: 'RenderStatus',
    0x00010020: 'PrimGroup',
    0x00010021: 'MeshStats',
    0x00010022: 'MeshBounds',
    0x00010029: 'MeshTransform',
    0x00011015: 'Shader',
    0x00011016: 'ShaderTexture',
    0x00011017: 'ShaderFloat',
    0x00011019: 'ShaderVector',
    0x0001101a: 'ShaderColour',
    0x0001101c: 'ShaderStates',
    0x0001101e: 'ShaderTexturePath',
    0x00011020: 'ShaderTextureSet',
    0x00011021: 'ShaderInt',
    0x00017005: 'FlareBoard',
    0x00017006: 'FlareGroup',
    0x00019000: 'Texture',
    0x00019001: 'Image',
    0x00019002: 'ImageData',
    0x00019003: 'ImageSource',
    0x00023000: 'Skeleton',
    0x00023001: 'SkeletonJoint',
    0x00122000: 'MeshLod',
    0x00123000: 'CompositeDrawable',
    0x00123001: 'CompositeDrawableProp',
    0x00123002: 'CompositeDrawablePropRef',
    0x00124000: 'Locator',
    0x07020000: 'PhysicsObject',
    0x07f00000: 'GameObject',
    0x07f00001: 'GameObjectAttributes',
    0x0900000a: 'ShaderRef',
    0x20000701: 'Playback',
    0x20000702: 'PlaybackData',
}


def name_of(cid):
    return CHUNK_NAMES.get(cid, f'{cid:08x}')


def read(path):
    """A Pure3D file's bytes: .rz files are "RZ", 6 bytes, the size (u64) and a zlib stream."""
    data = Path(path).read_bytes()
    if data[:2] == b'RZ':
        size, = struct.unpack_from('<Q', data, 8)
        data = zlib.decompress(data[16:])
        if len(data) != size:
            raise ValueError(f'{path}: {len(data)} bytes unpacked, header says {size}')
    if data[:4] and struct.unpack_from('<I', data, 0)[0] != 0xff443350:
        raise ValueError(f'{path}: not a Pure3D file')
    return data


class Chunk:
    """id, its own data (after the 12-byte header) and its children."""
    __slots__ = ('id', 'offset', 'data', 'children')

    def __init__(self, cid, offset, data, children):
        self.id, self.offset, self.data, self.children = cid, offset, data, children

    @property
    def name(self):
        return name_of(self.id)

    def find(self, cid):
        return [c for c in self.children if c.id == cid]

    def first(self, cid):
        return next((c for c in self.children if c.id == cid), None)

    def walk(self, depth=0):
        yield depth, self
        for c in self.children:
            yield from c.walk(depth + 1)


def parse(data, offset=0, end=None):
    """The chunks between offset and end: each is id u32, data size u32 (with the header),
    total size u32 (with its children), its data, then its children."""
    end = len(data) if end is None else end
    out = []
    while offset + 12 <= end:
        cid, dsize, tsize = struct.unpack_from('<III', data, offset)
        if dsize < 12 or tsize < dsize or offset + tsize > end:
            raise ValueError(f'bad chunk {cid:08x} at {offset:#x}')
        out.append(Chunk(cid, offset, data[offset + 12:offset + dsize],
                         parse(data, offset + dsize, offset + tsize)))
        offset += tsize
    return out


def load(path):
    return Chunk(0xff443350, 0, b'', parse(read(path), 12))


class Reader:
    def __init__(self, data, at=0):
        self.d, self.at = data, at

    def u8(self):
        self.at += 1
        return self.d[self.at - 1]

    def u16(self):
        self.at += 2
        return struct.unpack_from('<H', self.d, self.at - 2)[0]

    def u32(self):
        self.at += 4
        return struct.unpack_from('<I', self.d, self.at - 4)[0]

    def f32(self, n=1):
        self.at += 4 * n
        v = struct.unpack_from(f'<{n}f', self.d, self.at - 4 * n)
        return v if n > 1 else v[0]

    def pstr(self):
        """A string: length byte, then that many bytes (zero-padded)."""
        n = self.u8()
        self.at += n
        return self.d[self.at - n:self.at].split(b'\0')[0].decode('latin1')


# ---- meshes

# Vertex element names by the hash the files give them (VertexFormat).
ELEMENT_NAMES = {0x2c929929: 'position', 0x00364509: 'uv0', 0x0036450a: 'uv1', 0x0036450b: 'uv2',
                 0xc206bce7: 'normal', 0xa4176245: 'tangent', 0x3898fc04: 'colour0', 0x3898fc05: 'colour1'}


def vertex_format(chunk):
    """[(name, offset, components, kind, packed)] and the stride: VertexFormat is u16 1, u16 2,
    u32 2, u32 buffer size, u32 bytes of elements that follow (17 each), then per element u32
    name hash, u32 packed format (7: 4 x u8, else 0: floats), u32 offset, u16 stride,
    u16 components, u8 kind."""
    r = Reader(chunk.data)
    r.u16(), r.u16(), r.u32()
    size, length = r.u32(), r.u32()
    elements, stride = [], 0
    for _ in range(length // 17):
        h, packed, off, stride, comps, kind = r.u32(), r.u32(), r.u32(), r.u16(), r.u16(), r.u8()
        elements.append((ELEMENT_NAMES.get(h, f'{h:08x}'), off, comps, kind, packed))
    return elements, stride, size


class Prim:
    __slots__ = ('shader', 'kind', 'vertex_count', 'index_count', 'elements', 'stride', 'vertices', 'indices')


def prim_group(chunk):
    """PrimGroup: u32 version, shader name, u32 primitive type (0 = triangle list), u32 vertex
    flags, u32 vertex count, u32 index count; children VertexFormat, VertexBuffer, IndexBuffer."""
    r = Reader(chunk.data)
    r.u32()
    p = Prim()
    p.shader = r.pstr()
    p.kind, _flags, p.vertex_count, p.index_count = r.u32(), r.u32(), r.u32(), r.u32()
    fmt, vb, ib = chunk.first(0x10014), chunk.first(0x10012), chunk.first(0x10013)
    p.elements, p.stride, _ = vertex_format(fmt) if fmt else ([], 0, 0)
    # Buffers: u16 1, u16 2, u32 kind, u32 size, then the bytes.
    p.vertices = vb.data[12:12 + struct.unpack_from('<I', vb.data, 8)[0]] if vb else b''
    if ib:
        size, = struct.unpack_from('<I', ib.data, 8)
        width = 4 if p.index_count and size == 4 * p.index_count else 2
        p.indices = struct.unpack_from(f'<{size // width}{"I" if width == 4 else "H"}', ib.data, 12)
    else:
        p.indices = ()
    return p


def positions(p):
    """The prim's vertex positions [(x, y, z)]."""
    el = next((e for e in p.elements if e[0] == 'position'), None)
    if not el or not p.stride:
        return []
    off = el[1]
    n = len(p.vertices) // p.stride
    return [struct.unpack_from('<3f', p.vertices, i * p.stride + off) for i in range(n)]


def meshes(root):
    """[(mesh name, [Prim])] of a file."""
    out = []
    for _, c in root.walk():
        if c.id == 0x10000:
            name = Reader(c.data).pstr()
            out.append((name, [prim_group(g) for g in c.find(0x10020)]))
    return out


# ---- shaders and textures

def shader(chunk):
    """Shader: name, u16 0, u16 1, the effect it uses (e.g. zCBV2_ao_building_noglass), u32
    parameter count; children ShaderTexture (parameter, texture name), ShaderFloat, ShaderInt,
    ShaderColour..."""
    r = Reader(chunk.data)
    name = r.pstr()
    r.u16(), r.u16()
    effect = r.pstr()
    textures = {}
    for c in chunk.find(0x11016):
        t = Reader(c.data)
        param = t.pstr()
        textures[param] = t.pstr()
    return name, effect, textures


# GCN texture data formats (T# word 1, bits 20-25) the files use.
GNF_BC1, GNF_BC2, GNF_BC3 = 35, 36, 37
_BCN = {GNF_BC1: (8, 1), GNF_BC2: (16, 2), GNF_BC3: (16, 3)}


def _thin_micro_tile():
    """Element order inside an 8 x 8 micro tile of a "thin" tile mode: x0 y0 x1 y1 x2 y2."""
    order = []
    for i in range(64):
        x = (i & 1) | ((i >> 1) & 2) | ((i >> 2) & 4)
        y = ((i >> 1) & 1) | ((i >> 2) & 2) | ((i >> 3) & 4)
        order.append((x, y))
    return order


_THIN = _thin_micro_tile()


def texture(chunk):
    """(name, width, height, PIL image of the top mip level) of a Texture chunk, or the reason
    it cannot be read yet. ImageData holds a GNF: u32 size, "GNF ", u32 header size, then the
    texture descriptor (T#) and, after the header, the data in the GPU's tiled layout."""
    from PIL import Image
    name = Reader(chunk.data).pstr()
    data = next((c for _, c in chunk.walk() if c.id == 0x19002), None)
    if data is None:
        return name, 0, 0, 'no image data'
    g = data.data
    at = g.find(b'GNF ')
    words = struct.unpack_from('<8I', g, at + 16)
    fmt = (words[1] >> 20) & 0x3f
    width, height = (words[2] & 0x3fff) + 1, ((words[2] >> 14) & 0x3fff) + 1
    tiling = (words[3] >> 20) & 0x1f
    pitch = ((words[4] >> 13) & 0x3fff) + 1
    header, = struct.unpack_from('<I', g, at + 4)
    pixels = g[at + 8 + header:]
    if fmt not in _BCN or tiling != 13:  # 13: Thin1DThin, 8 x 8 micro tiles in rows
        return name, width, height, f'format {fmt}, tile mode {tiling} not supported yet'
    block, bcn = _BCN[fmt]
    bw, bh = (width + 3) // 4, (height + 3) // 4
    tiles_x = ((pitch + 3) // 4 + 7) // 8
    tiles_y = (bh + 7) // 8
    out = bytearray(bw * bh * block)
    for ty in range(tiles_y):
        for tx in range(tiles_x):
            base = (ty * tiles_x + tx) * 64 * block
            if base >= len(pixels):
                break
            for k, (x, y) in enumerate(_THIN):
                bx, by = tx * 8 + x, ty * 8 + y
                if bx < bw and by < bh:
                    src = base + k * block
                    out[(by * bw + bx) * block:(by * bw + bx + 1) * block] = pixels[src:src + block]
    image = Image.frombytes('RGBA', (bw * 4, bh * 4), bytes(out), 'bcn', bcn)
    return name, width, height, image.crop((0, 0, width, height))


def textures(root):
    """{name: Texture chunk} of a file."""
    return {Reader(c.data).pstr(): c for _, c in root.walk() if c.id == 0x19000}


# ---- command line

def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)
    t = sub.add_parser('tree')
    t.add_argument('file')
    t.add_argument('--depth', type=int, default=3)
    s = sub.add_parser('stats')
    s.add_argument('files', nargs='+')
    m = sub.add_parser('meshes')
    m.add_argument('file')
    a = p.parse_args()
    if a.cmd == 'tree':
        for depth, c in load(a.file).walk():
            if 0 < depth <= a.depth:
                label = ''
                if c.data[:1] and 0 < c.data[0] < 64 and all(32 <= b < 127 for b in c.data[1:1 + min(c.data[0], 8)] if b):
                    label = Reader(c.data).pstr()
                print('  ' * (depth - 1) + f'{c.name} ({len(c.data)} bytes) {label}')
    elif a.cmd == 'stats':
        count = collections.Counter()
        for f in a.files:
            for depth, c in load(f).walk():
                if depth:
                    count[c.id] += 1
        for cid, n in count.most_common():
            print(f'{n:8d}  {cid:08x}  {name_of(cid)}')
    elif a.cmd == 'meshes':
        for name, prims in meshes(load(a.file)):
            verts = sum(len(q.vertices) // max(q.stride, 1) for q in prims)
            tris = sum(len(q.indices) // 3 for q in prims)
            print(f'{name}: {len(prims)} parts, {verts} vertices, {tris} triangles')
            for q in prims:
                print('    ' + q.shader + '  ' + ', '.join(f'{e[0]}@{e[1]}' for e in q.elements) + f'  stride {q.stride}')


if __name__ == '__main__':
    sys.exit(main())
