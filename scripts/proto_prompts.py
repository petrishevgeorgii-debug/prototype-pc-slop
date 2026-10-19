#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Keyboard and mouse button prompts for PROTOTYPE, made from the player's own game files.

The game shows its PS4 buttons two ways:
  * text: the text bible (p1/art/hud/fe_textbible.p3d.rz, textbible_<language>.p3d.rz) writes a
    symbol per button ("НАЖМИТЕ ±" = press Circle) that its font draws as the button. Each symbol
    becomes the bound key in brackets, "[E]", as the PC version's own texts write keys.
  * the hints above people and objects (art/hud/buttonhint.gfx): textures in fe_ingame.p3d.rz.
    The button pictures are replaced with key caps drawn here (BC3, the PS4's 1D tiling, stored
    upside down like the originals): same size, so nothing else in the package moves.

The results go to <out>/<game path in lower case>; the runtime reads them instead of the game's
files (THEORDER_FILE_OVERRIDES, src/runtime_file.c). The game folder is only read, and nothing
written here is part of a release.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import struct
import sys
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent))
import proto_controls  # noqa: E402

HUD = Path('p1') / 'art' / 'hud'
TEXT_FILES = ('fe_textbible.p3d.rz', 'textbible_english.p3d.rz', 'textbible_french.p3d.rz',
              'textbible_italian.p3d.rz', 'textbible_spanish.p3d.rz')
HINT_FILE = 'fe_ingame.p3d.rz'
VERSION = 1  # of this script's output (part of the cache key)

# buttonhint.gfx: the textures of its PA..PDL frames (A = Cross, B = Circle, X = Square,
# Y = Triangle, the PS3-era names) in its two sets, and the d-pad picture of the small set.
HINT_TEXTURES = {
    'buttonhint__I16.dds': 'cross', 'buttonhint__I18.dds': 'circle', 'buttonhint__I1A.dds': 'square',
    'buttonhint__I1C.dds': 'triangle', 'buttonhint__I1E.dds': 'l1', 'buttonhint__I20.dds': 'r1',
    'buttonhint__I22.dds': 'l2', 'buttonhint__I24.dds': 'r2', 'buttonhint__I26.dds': 'up',
    'buttonhint__I28.dds': 'right', 'buttonhint__I2A.dds': 'down', 'buttonhint__I2C.dds': 'left',
    'buttonhint__I2F.dds': 'cross', 'buttonhint__I31.dds': 'circle', 'buttonhint__I33.dds': 'square',
    'buttonhint__I35.dds': 'triangle', 'buttonhint__I37.dds': 'l1', 'buttonhint__I39.dds': 'r1',
    'buttonhint__I3B.dds': 'l2', 'buttonhint__I3D.dds': 'r2', 'buttonhint__I3F.dds': 'dpad',
}


# --- Radical's RZ (one zlib stream) and Pure3D chunks -------------------------------------------

def read_rz(path):
    data = Path(path).read_bytes()
    if data[:2] != b'RZ':
        raise ValueError(f'{path}: not an RZ file')
    size = struct.unpack_from('<I', data, 8)[0]
    out = zlib.decompress(data[16:])
    if len(out) != size:
        raise ValueError(f'{path}: {len(out)} bytes unpacked, header says {size}')
    return out


def write_rz(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + '.tmp')
    tmp.write_bytes(b'RZ\0\0' + struct.pack('<III', 0, len(data), 0) + zlib.compress(data, 6))
    tmp.replace(path)


def chunks(data, start, end):
    """(id, offset, header size, total size) of the chunks in data[start:end]."""
    pos = start
    while pos + 12 <= end:
        cid, header, total = struct.unpack_from('<III', data, pos)
        if total < 12 or pos + total > end:
            break
        yield cid, pos, header, total
        pos += total


def pstring(data, pos):
    n = data[pos]
    return data[pos + 1:pos + 1 + n].split(b'\0')[0].decode('latin-1'), pos + 1 + n


# --- Text -------------------------------------------------------------------------------------

TEXT_BIBLE, TEXT_DATA = 0x18202, 0x18201


def prompt_texts(full, russian, menu=False):
    """{symbol: replacement} for one language; `menu`: the menu keys (the arrows on the d-pad)."""
    def label(target):
        keys = proto_controls.keys_for(full, target, menu)
        return proto_controls.key_label(keys[0], russian) if keys else None

    result = {}
    for symbol, target in proto_controls.SYMBOLS.items():
        if target == 'lstick':
            keys = [proto_controls.keys_for(full, f'lstick_{d}')[:1] for d in ('up', 'left', 'down', 'right')]
            text = '/'.join(k[0] for k in keys if k)
            if [k[0] for k in keys if k] == ['W', 'A', 'S', 'D']:
                text = 'WASD'
        elif target == 'rstick':
            text = 'МЫШЬ' if russian else 'MOUSE'
        elif target == 'dpad':
            keys = [label(d) for d in ('up', 'right', 'down', 'left')]
            if None in keys:
                continue
            text = '1-4' if keys == ['1', '2', '3', '4'] else '/'.join(keys)
        else:
            text = label(target)
        if text:
            result[symbol] = f'[{text}]'
    return result


def patch_text_bible(data, full):
    """The text bible package with every button symbol replaced; returns (data, replaced)."""
    out = bytearray(data[:12])
    replaced = 0
    for cid, pos, header, total in chunks(data, 12, len(data)):
        chunk = data[pos:pos + total]
        if cid != TEXT_BIBLE:
            out += chunk
            continue
        p = 12
        _, p = pstring(chunk, p)
        _, count = struct.unpack_from('<II', chunk, p)
        p += 8
        for _ in range(count):
            p = pstring(chunk, p)[1]
        starts = struct.unpack_from(f'<{count}I', chunk, p)
        offsets_at = p
        p += 8 * count
        if p != header:
            raise ValueError('unexpected text bible layout')
        dcid, dpos, dheader, dtotal = next(chunks(chunk, header, total))
        if dcid != TEXT_DATA:
            raise ValueError('text bible without its text')
        q = dpos + 12
        _, q = pstring(chunk, q)
        _, size = struct.unpack_from('<II', chunk, q)
        text = chunk[q + 8:q + 8 + size]
        bounds = list(starts) + [size]
        strings = [text[bounds[i]:bounds[i + 1]].decode('utf-8', 'replace') for i in range(count)]
        russian = sum('Ѐ' <= ch <= 'ӿ' for s in strings[:400] for ch in s) > 200
        table = prompt_texts(full, russian)
        menu_table = prompt_texts(full, russian, menu=True)
        keys = []
        p = 12
        _, p = pstring(chunk, p)
        p += 8
        for _ in range(count):
            key, p = pstring(chunk, p)
            keys.append(key)
        new_strings = []
        for key, s in zip(keys, strings):
            n = s
            # Menu hints ("NEXT ENTRY ¾") show the menu keys.
            menu = key.startswith(('NEXT_', 'PREVIOUS_'))
            for symbol, replacement in (menu_table if menu else table).items():
                if symbol in n:
                    n = n.replace(symbol, replacement)
            replaced += n != s
            new_strings.append(n.encode('utf-8'))
        new_text = b''.join(new_strings)
        new_starts, at = [], 0
        for s in new_strings:
            new_starts.append(at)
            at += len(s)
        new_ends = new_starts[1:] + [len(new_text)]
        new_ends = [e + 1 for e in new_ends]  # the game's end offsets count one past (as the originals)
        head = bytearray(chunk[:header])
        struct.pack_into(f'<{count}I', head, offsets_at, *new_starts)
        struct.pack_into(f'<{count}I', head, offsets_at + 4 * count, *new_ends)
        data_head = bytearray(chunk[dpos:q + 8])
        struct.pack_into('<I', data_head, len(data_head) - 4, len(new_text))
        new_data = data_head + new_text + chunk[q + 8 + size:dpos + dtotal]
        struct.pack_into('<II', new_data, 4, len(new_data), len(new_data))
        rest = chunk[dpos + dtotal:]
        new_chunk = head + new_data + rest
        struct.pack_into('<I', new_chunk, 8, len(new_chunk))
        out += new_chunk
    struct.pack_into('<I', out, 8, len(out))
    return bytes(out), replaced


# --- Key caps ---------------------------------------------------------------------------------

def find_font():
    port = Path(__file__).resolve().parent.parent
    for candidate in (port / 'fonts' / 'Tektur-SemiCondensed.ttf',
                      port / 'gpu' / 'third_party' / 'fonts' / 'Tektur-SemiCondensed.ttf'):
        if candidate.is_file():
            return candidate
    return None


def draw_mouse(image, box, which, color, accent, width):
    """A mouse in `box` (on `image`) with the pressed part in the accent colour: left, right,
    middle (the wheel), wheel_up, wheel_down, x1, x2 (side buttons)."""
    from PIL import Image, ImageDraw
    x0, y0, x1, y1 = box
    h = y1 - y0
    w = h * 0.64
    cx = (x0 + x1) / 2
    body = (cx - w / 2, y0, cx + w / 2, y1)
    mask = Image.new('L', image.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle(body, radius=w * 0.48, fill=255)
    layer = Image.new('RGBA', image.size, (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)
    split = y0 + h * 0.44
    if which in ('left', 'right'):
        lo, hi = (body[0], cx) if which == 'left' else (cx, body[2])
        draw.rectangle((lo, y0, hi, split), fill=accent)
    image.paste(layer, (0, 0), Image.composite(layer.getchannel('A'), Image.new('L', image.size, 0), mask))
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle(body, radius=w * 0.48, outline=color, width=width)
    draw.line((body[0], split, body[2], split), fill=color, width=width)
    draw.line((cx, y0, cx, split), fill=color, width=width)
    wheel = (cx - w * 0.11, y0 + h * 0.1, cx + w * 0.11, split - h * 0.1)
    pressed = which in ('middle', 'wheel_up', 'wheel_down')
    draw.rounded_rectangle(wheel, radius=w * 0.1, fill=accent if pressed else (22, 20, 20, 255),
                           outline=color, width=max(1, width // 2))
    if which in ('wheel_up', 'wheel_down'):
        up = which == 'wheel_up'
        ax, ay, d = body[2] + w * 0.28, y0 + h * (0.2 if up else 0.34), w * 0.2
        draw.polygon([(ax - d, ay + (d if up else 0)), (ax + d, ay + (d if up else 0)),
                      (ax, ay + (0 if up else d))], fill=accent)
    if which in ('x1', 'x2'):
        top = split + h * (0.06 if which == 'x2' else 0.24)
        draw.rounded_rectangle((body[0] - w * 0.12, top, body[0] + w * 0.04, top + h * 0.15),
                               radius=w * 0.05, fill=accent)


MOUSE_ICON = {'Mouse Left': 'left', 'Mouse Right': 'right', 'Mouse Middle': 'middle',
              'Mouse X1': 'x1', 'Mouse X2': 'x2', 'Wheel Up': 'wheel_up', 'Wheel Down': 'wheel_down'}


def key_cap(size, box, key, label, font_path):
    """An RGBA image of `size` with a key cap filling `box`: the key's name, or a mouse."""
    from PIL import Image, ImageDraw, ImageFont
    scale = 4
    W, H = size[0] * scale, size[1] * scale
    image = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    x0, y0, x1, y1 = (v * scale for v in box)
    if key in MOUSE_ICON:  # a square cap: the mouse is tall
        side = min(x1 - x0, y1 - y0)
        cx = (x0 + x1) / 2
        x0, x1 = cx - side / 2, cx + side / 2
    bw, bh = x1 - x0, y1 - y0
    color, fill, shade, accent = (240, 232, 214, 255), (22, 20, 20, 235), (90, 30, 26, 255), (214, 64, 44, 255)
    line = max(2, int(min(bw, bh) * 0.06))
    radius = min(bw, bh) * 0.2
    draw.rounded_rectangle((x0, y0 + line, x1, y1), radius=radius, fill=shade)  # the key's side
    draw.rounded_rectangle((x0, y0, x1, y1 - line * 1.5), radius=radius, fill=fill, outline=color, width=line)
    inner = (x0 + line * 2.5, y0 + line * 2, x1 - line * 2.5, y1 - line * 3.5)
    if key in MOUSE_ICON:
        draw_mouse(image, inner, MOUSE_ICON[key], color, accent, max(2, int(line * 0.8)))
        return image.resize(size, Image.LANCZOS)
    tw, th = inner[2] - inner[0], inner[3] - inner[1]
    px = int(th * (0.78 if len(label) <= 2 else 0.6))
    font = None
    while px > 8:
        try:
            font = ImageFont.truetype(str(font_path), px) if font_path else ImageFont.load_default(size=px)
        except (OSError, TypeError):
            font = ImageFont.load_default()
            break
        l, t, r, b = draw.textbbox((0, 0), label, font=font)
        if r - l <= tw and b - t <= th:
            break
        px = int(px * 0.9)
    l, t, r, b = draw.textbbox((0, 0), label, font=font)
    draw.text(((inner[0] + inner[2] - (r - l)) / 2 - l, (inner[1] + inner[3] - (b - t)) / 2 - t),
              label, font=font, fill=color)
    return image.resize(size, Image.LANCZOS)


def tile_bc(blocks, bw, bh, block_bytes):
    """Linear BC blocks to the PS4's 1D thin tiling (8x8 blocks per tile, Morton order)."""
    out = bytearray(len(blocks))
    tiles_x = bw // 8
    for ty in range(bh // 8):
        for tx in range(tiles_x):
            base = (ty * tiles_x + tx) * 64
            for i in range(64):
                x = (i & 1) | ((i >> 2) & 1) << 1 | ((i >> 4) & 1) << 2
                y = ((i >> 1) & 1) | ((i >> 3) & 1) << 1 | ((i >> 5) & 1) << 2
                src = ((ty * 8 + y) * bw + tx * 8 + x) * block_bytes
                dst = (base + i) * block_bytes
                out[dst:dst + block_bytes] = blocks[src:src + block_bytes]
    return bytes(out)


def encode_bc3(image):
    """BC3 blocks (linear) of an RGBA image, through Pillow's DDS writer."""
    buffer = io.BytesIO()
    image.save(buffer, format='DDS', pixel_format='DXT5')
    raw = buffer.getvalue()
    expected = (image.width // 4) * (image.height // 4) * 16
    return raw[len(raw) - expected:]


def gnf_info(gnf):
    words = struct.unpack_from('<8I', gnf, 16)
    fmt = (words[1] >> 20) & 0x3f
    width, height = (words[2] & 0x3fff) + 1, ((words[2] >> 14) & 0x3fff) + 1
    tiling = (words[3] >> 20) & 0x1f
    header = struct.unpack_from('<I', gnf, 4)[0] + 8
    return fmt, width, height, tiling, header


def original_box(gnf, width, height, header):
    """Where the original picture is (its alpha's bounds), so the key cap takes its place."""
    from PIL import Image
    fmt, _, _, tiling, _ = gnf_info(gnf)
    bw, bh = width // 4, height // 4
    tiled = gnf[header:header + bw * bh * 16]
    linear = bytearray(len(tiled))
    tiles_x = bw // 8
    for ty in range(bh // 8):
        for tx in range(tiles_x):
            base = (ty * tiles_x + tx) * 64
            for i in range(64):
                x = (i & 1) | ((i >> 2) & 1) << 1 | ((i >> 4) & 1) << 2
                y = ((i >> 1) & 1) | ((i >> 3) & 1) << 1 | ((i >> 5) & 1) << 2
                dst = ((ty * 8 + y) * bw + tx * 8 + x) * 16
                linear[dst:dst + 16] = tiled[(base + i) * 16:(base + i + 1) * 16]
    dds = struct.pack('<4sIIIIIII44xIII20xI16x', b'DDS ', 124, 0x1007, height, width, 0, 0, 1, 32, 4,
                      struct.unpack('<I', b'DXT5')[0], 0x1000) + bytes(linear)
    with Image.open(io.BytesIO(dds)) as im:
        alpha = im.convert('RGBA').getchannel('A').point(lambda a: 255 if a > 40 else 0)
        box = alpha.getbbox()
    if not box:
        return (width * 0.1, height * 0.1, width * 0.9, height * 0.9)
    x0, y0, x1, y1 = box
    return (x0, height - y1, x1, height - y0)  # stored upside down


def patch_hints(data, full, font_path):
    """fe_ingame with the button hint pictures replaced; returns (data, replaced)."""
    from PIL import Image
    out = bytearray(data)
    replaced = 0
    found = []  # (texture name, GNF chunk offset, its total size)

    def walk(start, end, name):
        for cid, pos, header, total in chunks(data, start, end):
            if cid == 0x19000:
                name = pstring(data, pos + 12)[0]
            if cid == 0x19002 and name in HINT_TEXTURES:
                found.append((name, pos, total))
            elif header < total and cid in (0x19000, 0x19001):
                walk(pos + header, pos + total, name)

    walk(12, len(data), None)
    for name, pos, total in found:
        target = HINT_TEXTURES[name]
        gnf = data[pos + 16:pos + total]
        if gnf[:4] != b'GNF ':
            continue
        fmt, width, height, tiling, gheader = gnf_info(gnf)
        if fmt != 37 or tiling != 13 or width % 32 or height % 32:
            print(f'Prompts: {name} has an unexpected format, kept', flush=True)
            continue
        if target == 'dpad':
            keys = [proto_controls.keys_for(full, d)[:1] for d in ('up', 'right', 'down', 'left')]
            labels = [proto_controls.key_label(k[0]) for k in keys if k]
            key = None
            label = '1-4' if labels == ['1', '2', '3', '4'] else '/'.join(labels)
        else:
            keys = proto_controls.keys_for(full, target)
            if not keys:
                continue
            key = keys[0]
            label = proto_controls.key_label(key)
        box = original_box(gnf, width, height, gheader)
        image = key_cap((width, height), box, key, label, font_path).transpose(Image.FLIP_TOP_BOTTOM)
        blocks = tile_bc(encode_bc3(image), width // 4, height // 4, 16)
        at = pos + 16 + gheader
        if at + len(blocks) > pos + total:
            continue
        out[at:at + len(blocks)] = blocks
        replaced += 1
    return bytes(out), replaced


# --- Driver -----------------------------------------------------------------------------------

def generate(game, out, full, log=print):
    """Writes the overrides for layout `full` into `out` (cached); returns the override root or
    None when the game files are not the expected ones."""
    hud = Path(game) / HUD
    sources = [hud / n for n in (*TEXT_FILES, HINT_FILE) if (hud / n).is_file()]
    if not any(s.name == 'fe_textbible.p3d.rz' for s in sources):
        log('Prompts: no p1/art/hud/fe_textbible.p3d.rz in the game folder; controller prompts stay')
        return None
    font = find_font()
    key = hashlib.sha256(json.dumps({
        'v': VERSION, 'layout': full, 'font': str(font),
        'files': [(s.name, s.stat().st_size, int(s.stat().st_mtime)) for s in sources]},
        sort_keys=True).encode()).hexdigest()
    out = Path(out)
    stamp = out / 'prompts.json'
    try:
        if json.loads(stamp.read_text(encoding='utf-8')).get('key') == key:
            return out
    except (OSError, ValueError):
        pass
    target_dir = out / str(HUD).replace('\\', '/').lower()
    texts = hints = 0
    for source in sources:
        try:
            data = read_rz(source)
            if source.name == HINT_FILE:
                data, n = patch_hints(data, full, font)
                hints += n
            else:
                data, n = patch_text_bible(data, full)
                texts += n
            write_rz(target_dir / source.name.lower(), data)
        except (ValueError, OSError, zlib.error, struct.error) as error:
            log(f'Prompts: {source.name} left as it is ({error})')
            (target_dir / source.name.lower()).unlink(missing_ok=True)
    stamp.write_text(json.dumps({'key': key}), encoding='utf-8')
    log(f'Prompts: keyboard and mouse ({texts} texts, {hints} hint pictures)')
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('game', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--layout', default='', help='the saved layout (JSON), as the launcher keeps it')
    args = parser.parse_args()
    root = generate(args.game, args.out, proto_controls.layout(args.layout),
                    log=lambda m: print(m, flush=True))
    if root:
        print(root, flush=True)
    return 0 if root else 3


if __name__ == '__main__':
    sys.exit(main())
