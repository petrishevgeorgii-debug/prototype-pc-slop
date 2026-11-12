#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The launcher's pictures and names, made from the player's own game files (nothing ships).

  * key art: the bundle's sce_sys/pic0.png, its PROTOTYPE half (Alex Mercer) and its PROTOTYPE 2
    half (James Heller);
  * PROTOTYPE's mission pictures: the save screen's (protolistsaveimages.gfx's slotImage frames,
    one per mission, their textures in p1/art/hud/fe_permanent.p3d.rz);
  * PROTOTYPE 2's story pictures: p2/art/hud/story_*_01.p3d.rz;
  * the mission and district names in the game's language (its text bible).

Everything is written once to <out> (each picture as JPEG for the web window and BMP for the
native one, the window icon as icon.bmp, and names.json) and reused while the game files stay
the same. Dumps that keep the files in archives get no pictures: the launcher shows none.
"""
import hashlib
import io
import json
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import proto_prompts  # noqa: E402  (RZ, P3D chunks, GNF)

VERSION = 2
P1_HUD = Path('p1') / 'art' / 'hud'
P2_HUD = Path('p2') / 'art' / 'hud'
FORMATS = {35: (b'DXT1', 8), 36: (b'DXT3', 16), 37: (b'DXT5', 16)}  # GNF data format: DDS, block bytes


def _texture_chunks(data):
    """[(texture name, GNF bytes)] of a P3D package (PROTOTYPE's and PROTOTYPE 2's layouts)."""
    out = []

    def walk(start, end, name):
        for cid, pos, header, total in proto_prompts.chunks(data, start, end):
            if cid == 0x19000:
                name = proto_prompts.pstring(data, pos + 12)[0]
            if cid == 0x19002:
                out.append((name, data[pos + 16:pos + total]))
            elif header < total and cid in (0x19000, 0x19001, 0x19006):
                walk(pos + header, pos + total, name)
    walk(12, len(data), None)
    return out


def decode(gnf):
    """A GNF texture (BC1/2/3, the PS4's 1D thin tiling, stored upside down) as an RGB image."""
    from PIL import Image
    if gnf[:4] != b'GNF ':
        raise ValueError('not a GNF texture')
    fmt, width, height, tiling, header = proto_prompts.gnf_info(gnf)
    if fmt not in FORMATS or tiling != 13 or width % 32 or height % 32:
        raise ValueError(f'texture format {fmt}/{tiling} not handled')
    fourcc, size = FORMATS[fmt]
    bw, bh = width // 4, height // 4
    tiled = gnf[header:header + bw * bh * size]
    linear = bytearray(len(tiled))
    tiles_x = bw // 8
    for ty in range(bh // 8):
        for tx in range(tiles_x):
            base = (ty * tiles_x + tx) * 64
            for i in range(64):
                x = (i & 1) | ((i >> 2) & 1) << 1 | ((i >> 4) & 1) << 2
                y = ((i >> 1) & 1) | ((i >> 3) & 1) << 1 | ((i >> 5) & 1) << 2
                dst = ((ty * 8 + y) * bw + tx * 8 + x) * size
                linear[dst:dst + size] = tiled[(base + i) * size:(base + i + 1) * size]
    dds = struct.pack('<4sIIIIIII44xIII20xI16x', b'DDS ', 124, 0x1007, height, width, 0, 0, 1, 32, 4,
                      struct.unpack('<I', fourcc)[0], 0x1000) + bytes(linear)
    with Image.open(io.BytesIO(dds)) as image:
        return image.convert('RGB').transpose(Image.FLIP_TOP_BOTTOM)


def mission_frames(gfx):
    """{mission id: texture name} of protolistsaveimages.gfx: its slotImage's frames are labelled
    with the missions' ids; each places a shape filled with one picture (shape id = picture id + 1)."""
    import proto_menu
    pictures, frames = {}, {}
    start = proto_menu.movie_start(gfx)
    for code, _, body, end in proto_menu.tags(gfx, start, len(gfx)):
        if code == 1001:  # GFx external image: id, ..., file name
            pid = struct.unpack_from('<H', gfx, body)[0]
            length = gfx[body + 9]
            pictures[pid] = gfx[body + 10:body + 10 + length].decode('latin-1')
        elif code == 39 and struct.unpack_from('<H', gfx, body + 2)[0] > 30:  # the slotImage sprite
            label = placed = None
            for c, _, b, e in proto_menu.tags(gfx, body + 4, end):
                if c == 43:
                    label = gfx[b:e].split(b'\0')[0].decode('latin-1')
                elif c == 26 and gfx[b] & 2:
                    placed = struct.unpack_from('<H', gfx, b + 3)[0]
                elif c == 1 and label and placed:
                    frames[label] = placed - 1
    return {label: pictures[pid] for label, pid in frames.items() if pid in pictures and label != 'none'}


def _jpeg(image, path, quality=82):
    """The picture as JPEG (the web window) and BMP (the native one: SDL reads BMP itself)."""
    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(path, 'JPEG', quality=quality, optimize=True, progressive=True)
    image.convert('RGB').save(path.with_suffix('.bmp'), 'BMP')


def _fit(image, size):
    from PIL import ImageOps
    return ImageOps.fit(image, size, method=3, centering=(0.5, 0.45))


def _content_box(image):
    """PROTOTYPE 2's story pictures sit on black with a torn lower edge: the picture's box."""
    gray = image.convert('L').point(lambda v: 255 if v > 14 else 0)
    w, h = image.size
    rows = [y for y in range(0, h, 2) if gray.crop((0, y, w, y + 1)).histogram()[255] > w * 0.6]
    cols = [x for x in range(0, w, 2) if gray.crop((x, 0, x + 1, h)).histogram()[255] > h * 0.4]
    if not rows or not cols:
        return 0, 0, w, h
    return min(cols) + 4, min(rows) + 4, max(cols) - 4, max(rows) - int(h * 0.06)


def _key(game):
    files = [Path(game) / 'sce_sys' / 'pic0.png', Path(game) / 'sce_sys' / 'icon0.png',
             Path(game) / P1_HUD / 'fe_permanent.p3d.rz',
             Path(game) / P1_HUD / 'protolistsaveimages.gfx', Path(game) / P1_HUD / 'textbible_english.p3d.rz',
             *sorted((Path(game) / P2_HUD).glob('story_*_01.p3d.rz'))]
    stats = [(str(p.name), p.stat().st_size, int(p.stat().st_mtime)) for p in files if p.is_file()]
    return hashlib.sha256(json.dumps({'v': VERSION, 'game': str(game), 'files': stats}).encode()).hexdigest()


def generate(game, out, log=print):
    """Writes the pictures and names into `out` (cached); returns the names.json contents."""
    out = Path(out)
    stamp = out / 'names.json'
    key = _key(game)
    try:
        cached = json.loads(stamp.read_text(encoding='utf-8'))
        if cached.get('key') == key:
            return cached
    except (OSError, ValueError):
        pass
    from PIL import Image, ImageStat
    result = {'key': key, 'art': [], 'p1': [], 'p2': [], 'missions': {}, 'districts': []}
    game = Path(game)
    pic = game / 'sce_sys' / 'pic0.png'
    if pic.is_file():
        try:
            with Image.open(pic) as art:
                art = art.convert('RGB')
                w, h = art.size
                for name, box in (('alex', (0, 0, w // 2, h)), ('heller', (w // 2, 0, w, h))):
                    _jpeg(art.crop(box).resize((720, 810), Image.LANCZOS), out / f'{name}.jpg', 80)
                    result['art'].append(name)
        except OSError as error:
            log(f'launcher art: {pic.name}: {error}')
    icon = game / 'sce_sys' / 'icon0.png'
    if icon.is_file():
        try:
            with Image.open(icon) as image:
                out.mkdir(parents=True, exist_ok=True)
                image.convert('RGB').resize((64, 64), Image.LANCZOS).save(out / 'icon.bmp', 'BMP')
        except OSError as error:
            log(f'launcher art: {icon.name}: {error}')
    gfx, package = game / P1_HUD / 'protolistsaveimages.gfx', game / P1_HUD / 'fe_permanent.p3d.rz'
    if gfx.is_file() and package.is_file():
        try:
            frames = mission_frames(gfx.read_bytes())
            textures = dict(_texture_chunks(proto_prompts.read_rz(package)))
            for mission, texture in sorted(frames.items()):
                if texture in textures and texture != 'protolistsaveimages__I1.dds':  # I1: the [PROTOTYPE] card
                    _jpeg(_fit(decode(textures[texture]), (384, 216)), out / 'p1' / f'{mission}.jpg')
                    result['p1'].append(mission)
        except (OSError, ValueError, struct.error, IndexError) as error:
            log(f'launcher art: PROTOTYPE mission pictures: {error}')
    for path in sorted((game / P2_HUD).glob('story_*_01.p3d.rz')):
        try:
            for _, gnf in _texture_chunks(proto_prompts.read_rz(path))[:1]:
                image = decode(gnf)
                name = path.name[len('story_'):-len('_01.p3d.rz')]
                picture = _fit(image.crop(_content_box(image)), (384, 216))
                if ImageStat.Stat(picture.convert('L')).mean[0] > 170:
                    continue  # a white flash (the final boss's card): no picture for a camera
                _jpeg(picture, out / 'p2' / f'{name}.jpg')
                result['p2'].append(name)
        except (OSError, ValueError, struct.error, IndexError) as error:
            log(f'launcher art: {path.name}: {error}')
    try:
        import proto_presence
        result['missions'] = proto_presence.mission_names(game, 1)
        for _, data in proto_presence.loose_tables(game, 1):
            table = proto_presence.strings(data)
            result['districts'] = [table[k] for k in (f'META_DISTRICT_{i}' for i in range(1, 30)) if table.get(k)]
            break
    except (OSError, ValueError, struct.error, ImportError) as error:
        log(f'launcher art: names: {error}')
    out.mkdir(parents=True, exist_ok=True)
    stamp.write_text(json.dumps(result, ensure_ascii=False), encoding='utf-8')
    return result


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('game', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    r = generate(args.game, args.out)
    print(f"art {r['art']}, {len(r['p1'])} PROTOTYPE pictures, {len(r['p2'])} PROTOTYPE 2 pictures, "
          f"{len(r['missions'])} mission names, {len(r['districts'])} districts")
