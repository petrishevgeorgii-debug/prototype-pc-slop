#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Xbox button prompts for PROTOTYPE, made from the player's own game files.

The PS4 game draws its buttons two ways (scripts/proto_prompts.py has the keyboard version):
  * in its texts, as symbols its fonts draw as the PS4 buttons ("PRESS ±" is Circle). The PS4
    files still hold the PC version's font, advfont.gfx (PrototypeBodyPC), whose same symbols are
    the Xbox buttons: X, Y, B, LB, RB, LT, RT, RS, Back, Start, the d-pad and the sticks. Their
    shapes replace the PS4 ones in the game's fonts (fonts_latin.gfx, gfxfontlib.gfx), scaled to
    them. The PC font has no A (Cross is ◊ on PS4 only): it is made from B's ring and a bold A.
    In the Web of Intrigue's texts the touchpad symbol means Options, so there it becomes the
    Start symbol (¸).
  * the hints above people and objects (buttonhint.gfx's pictures in fe_ingame.p3d.rz): on the
    face buttons the PS4 symbol is painted out and the Xbox letter drawn in its colour; on the
    shoulder buttons the engraved L1, R1, L2, R2 become LB, RB, LT, RT.

The results go to <out>/<game path in lower case>, read instead of the game's files
(THEORDER_FILE_OVERRIDES), or a patch per archive for dumps that keep p1 in Radical's archives.
The game folder is only read, and nothing written here is part of a release.
"""
import hashlib
import json
from pathlib import Path
import struct
import sys
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent))
import proto_menu  # noqa: E402  (SWF tags)
import proto_prompts  # noqa: E402  (text bible, hint textures, archives)

VERSION = 1  # of this script's output (part of the cache key)
HUD = proto_prompts.HUD
FONT_FILES = ('fonts_latin.gfx', 'gfxfontlib.gfx')
PC_FONT = 'advfont.gfx'
DEFINE_FONT3 = 75

# The PS4 symbols drawn as buttons, the same symbol in the PC font (◊, Cross, is made: see above).
SYMBOLS = '£³±¹¥»¼§·¸½¯¾¬´µ¶'
START = '¸'  # the PC font's Start (the PS4 font's OPTIONS)


# --- Glyph shapes (SWF SHAPE records) ------------------------------------------------------------
# A glyph is a list of records with absolute coordinates: ['style', flags, fill0, fill1, line,
# (x, y) or None] (a move when flags & 1), ['l', x, y] (a line), ['q', cx, cy, x, y] (a curve).

class _Bits:
    def __init__(self, data, pos):
        self.data, self.pos, self.bit = data, pos, 0

    def u(self, n):
        v = 0
        for _ in range(n):
            v = (v << 1) | ((self.data[self.pos] >> (7 - self.bit)) & 1)
            self.bit += 1
            if self.bit == 8:
                self.bit, self.pos = 0, self.pos + 1
        return v

    def s(self, n):
        v = self.u(n)
        return v - (1 << n) if n and v & (1 << (n - 1)) else v


class _Writer:
    def __init__(self):
        self.out, self.acc, self.n = bytearray(), 0, 0

    def u(self, v, n):
        for i in range(n - 1, -1, -1):
            self.acc = (self.acc << 1) | ((v >> i) & 1)
            self.n += 1
            if self.n == 8:
                self.out.append(self.acc)
                self.acc = self.n = 0

    def s(self, v, n):
        self.u(v & ((1 << n) - 1), n)

    def done(self):
        if self.n:
            self.u(0, 8 - self.n)
        return bytes(self.out)


def parse_shape(data, pos=0):
    """(fill bits, line bits, records) of the glyph SHAPE at data[pos]."""
    b = _Bits(data, pos)
    nf, nl = b.u(4), b.u(4)
    x = y = 0
    recs = []
    while True:
        if b.u(1) == 0:
            flags = b.u(5)
            if flags == 0:
                break
            move = f0 = f1 = line = None
            if flags & 1:
                n = b.u(5)
                x, y = b.s(n), b.s(n)
                move = (x, y)
            if flags & 2:
                f0 = b.u(nf)
            if flags & 4:
                f1 = b.u(nf)
            if flags & 8:
                line = b.u(nl)
            if flags & 16:
                raise ValueError('a glyph with its own styles')
            recs.append(['style', flags, f0, f1, line, move])
        elif b.u(1):
            n = b.u(4) + 2
            if b.u(1):
                x += b.s(n)
                y += b.s(n)
            elif b.u(1):
                y += b.s(n)
            else:
                x += b.s(n)
            recs.append(['l', x, y])
        else:
            n = b.u(4) + 2
            cx = x + b.s(n)
            cy = y + b.s(n)
            x = cx + b.s(n)
            y = cy + b.s(n)
            recs.append(['q', cx, cy, x, y])
    return nf, nl, recs


def _bits(*values):
    n = 1
    for v in values:
        while not -(1 << (n - 1)) <= v < (1 << (n - 1)):
            n += 1
    return n


def encode_shape(nf, nl, recs):
    w = _Writer()
    w.u(nf, 4)
    w.u(nl, 4)
    x = y = 0
    for r in recs:
        if r[0] == 'style':
            _, flags, f0, f1, line, move = r
            w.u(0, 1)
            w.u(flags, 5)
            if flags & 1:
                x, y = move
                n = _bits(x, y)
                w.u(n, 5)
                w.s(x, n)
                w.s(y, n)
            if flags & 2:
                w.u(f0, nf)
            if flags & 4:
                w.u(f1, nf)
            if flags & 8:
                w.u(line, nl)
        elif r[0] == 'l':
            dx, dy = r[1] - x, r[2] - y
            if not dx and not dy:
                continue
            n = max(_bits(dx, dy), 2)
            w.u(3, 2)
            w.u(n - 2, 4)
            if dx and dy:
                w.u(1, 1)
                w.s(dx, n)
                w.s(dy, n)
            elif dy:
                w.u(1, 2)
                w.s(dy, n)
            else:
                w.u(0, 2)
                w.s(dx, n)
            x, y = r[1], r[2]
        else:
            _, cx, cy, ex, ey = r
            deltas = (cx - x, cy - y, ex - cx, ey - cy)
            n = max(_bits(*deltas), 2)
            w.u(2, 2)
            w.u(n - 2, 4)
            for v in deltas:
                w.s(v, n)
            x, y = ex, ey
    w.u(0, 6)
    return w.done()


def subpaths(recs):
    out = []
    for r in recs:
        if r[0] == 'style' and r[5] is not None or not out:
            out.append([r])
        else:
            out[-1].append(r)
    return out


def _points(recs):
    for r in recs:
        if r[0] == 'style':
            if r[5]:
                yield r[5]
        elif r[0] == 'l':
            yield r[1], r[2]
        else:
            yield r[1], r[2]
            yield r[3], r[4]


def bbox(recs):
    p = list(_points(recs))
    xs, ys = [a for a, _ in p], [b for _, b in p]
    return min(xs), min(ys), max(xs), max(ys)


def transform(recs, f):
    out = []
    for r in recs:
        if r[0] == 'style':
            out.append(['style', *r[1:5], f(*r[5]) if r[5] else None])
        elif r[0] == 'l':
            out.append(['l', *f(r[1], r[2])])
        else:
            out.append(['q', *f(r[1], r[2]), *f(r[3], r[4])])
    return out


def area(sp):
    p = list(_points(sp))
    return sum(p[i][0] * p[(i + 1) % len(p)][1] - p[(i + 1) % len(p)][0] * p[i][1] for i in range(len(p))) / 2


def reverse(sp):
    """A closed subpath of straight lines the other way round."""
    pts = [sp[0][5]] + [(r[1], r[2]) for r in sp[1:]]
    pts.reverse()
    return [sp[0][:5] + [pts[0]]] + [['l', x, y] for x, y in pts[1:]]


# --- DefineFont3 ---------------------------------------------------------------------------------

def read_font(data, body, end):
    fid, flags, lang, name_len = struct.unpack_from('<HBBB', data, body)
    name = data[body + 5:body + 5 + name_len]
    n = struct.unpack_from('<H', data, body + 5 + name_len)[0]
    table = body + 7 + name_len
    wide = bool(flags & 0x08)
    size = 4 if wide else 2
    offsets = list(struct.unpack_from(f'<{n}{"I" if wide else "H"}', data, table))
    codes_at = struct.unpack_from('<' + ('I' if wide else 'H'), data, table + size * n)[0]
    ends = offsets[1:] + [codes_at]
    return {'id': fid, 'flags': flags, 'lang': lang, 'name': name, 'n': n,
            'shapes': [data[table + offsets[i]:table + ends[i]] for i in range(n)],
            'codes': list(struct.unpack_from(f'<{n}H', data, table + codes_at)),
            'rest': data[table + codes_at + 2 * n:end]}  # the layout: ascent... advances, bounds, kerning


def write_font(f):
    n = f['n']
    shapes = sum(len(s) for s in f['shapes'])
    wide = bool(f['flags'] & 0x08) or 2 * (n + 1) + shapes > 0xffff
    flags = f['flags'] | 0x08 if wide else f['flags'] & ~0x08
    offsets, at = [], (4 if wide else 2) * (n + 1)
    for s in f['shapes']:
        offsets.append(at)
        at += len(s)
    return (struct.pack('<HBBB', f['id'], flags, f['lang'], len(f['name'])) + f['name'] +
            struct.pack('<H', n) + struct.pack(f'<{n + 1}{"I" if wide else "H"}', *offsets, at) +
            b''.join(f['shapes']) + struct.pack(f'<{n}H', *f['codes']) + f['rest'])


def fonts_in(data):
    start = proto_menu.movie_start(data)
    for code, header, body, end in proto_menu.tags(data, start, len(data)):
        if code == DEFINE_FONT3:
            yield header, body, end, read_font(data, body, end)


def _advances(f):
    return list(struct.unpack_from(f'<{f["n"]}h', f['rest'], 6))


def _rect(data, pos):
    """(xmin, xmax, ymin, ymax) and the end of the RECT at data[pos]."""
    b = _Bits(data, pos)
    n = b.u(5)
    values = tuple(b.s(n) for _ in range(4))
    return values, b.pos + (1 if b.bit else 0)


def _encode_rect(values):
    w = _Writer()
    n = _bits(*values)
    w.u(n, 5)
    for v in values:
        w.s(v, n)
    return w.done()


def _set_layout(f, changes):
    """The layout's advance and bounds of glyphs {index: (advance, (x0, y0, x1, y1))}."""
    n, rest = f['n'], f['rest']
    head = bytearray(rest[:6 + 2 * n])
    pos, bounds = 6 + 2 * n, []
    for _ in range(n):
        start = pos
        _, pos = _rect(rest, pos)
        bounds.append(rest[start:pos])
    for i, (advance, (x0, y0, x1, y1)) in changes.items():
        struct.pack_into('<h', head, 6 + 2 * i, advance)
        if _rect(bounds[i], 0)[0] != (0, 0, 0, 0):  # most are left empty, as the exporter does
            bounds[i] = _encode_rect((x0, x1, y0, y1))
    f['rest'] = bytes(head) + b''.join(bounds) + rest[pos:]


# DejaVu Sans Bold's A (Bitstream Vera licence, as gpu/third_party/fonts/LICENSE-DejaVu.txt), y down:
# the PC font's letters in rings are heavy, its own A is not.
BOLD_A = [['style', 3, 1, None, None, (1094, -272)], ['l', 492, -272], ['l', 397, 0], ['l', 10, 0],
          ['l', 563, -1493], ['l', 1022, -1493], ['l', 1575, 0], ['l', 1188, 0], ['l', 1094, -272],
          ['style', 1, None, None, None, (588, -549)], ['l', 997, -549], ['l', 793, -1143], ['l', 588, -549]]


def pc_glyphs(advfont):
    """{symbol: (fill bits, line bits, records)} and {symbol: advance} of the PC font's Xbox
    buttons (and its letters), with the A button made."""
    fonts = [f for _, _, _, f in fonts_in(advfont)]
    if not fonts or not all(ord(c) in fonts[0]['codes'] for c in SYMBOLS + 'ABLRST'):
        raise ValueError('advfont.gfx is not the PC font expected')
    f = fonts[0]
    advances = _advances(f)
    glyphs, widths = {}, {}
    for i, c in enumerate(f['codes']):
        glyphs[chr(c)] = parse_shape(f['shapes'][i])
        widths[chr(c)] = advances[i]
    nf, nl, b_button = glyphs['±']
    ring, letter = subpaths(b_button)[:2], subpaths(b_button)[2:]
    lx0, ly0, lx1, ly1 = bbox([r for sp in letter for r in sp])
    a = BOLD_A
    if (area(subpaths(a)[0]) > 0) != (area(letter[0]) > 0):
        a = [r for sp in subpaths(a) for r in reverse(sp)]
    ax0, ay0, ax1, ay1 = bbox(a)
    k = (ly1 - ly0) / (ay1 - ay0)
    cx, cy = (lx0 + lx1) / 2, (ly0 + ly1) / 2
    a = transform(a, lambda x, y: (round(cx + (x - (ax0 + ax1) / 2) * k), round(cy + (y - (ay0 + ay1) / 2) * k)))
    glyphs['◊'] = (nf, nl, [r for sp in ring for r in sp] + a)
    widths['◊'] = widths['±']
    return glyphs, widths


def patch_fonts(data, pc, log=lambda m: None):
    """Movie `data` with every PS4 button glyph of its fonts drawn as the Xbox one; returns
    (data, glyphs replaced)."""
    glyphs, widths = pc
    out, last, replaced = bytearray(), 0, 0
    for header, body, end, f in list(fonts_in(data)):
        index = {chr(c): i for i, c in enumerate(f['codes'])}
        if '±' not in index or 'A' not in index:
            continue
        cap = bbox(parse_shape(f['shapes'][index['A']])[2])
        circle = parse_shape(f['shapes'][index['±']])[2]
        if not circle:
            continue
        cb = bbox(circle)
        if cb[3] - cb[1] < 0.9 * (cap[3] - cap[1]):
            continue  # here ± is the character, not the Circle button
        sb = bbox(glyphs['±'][2])
        k = (cb[3] - cb[1]) / (sb[3] - sb[1])
        dy = (cb[1] + cb[3]) / 2 - (sb[1] + sb[3]) / 2 * k
        layout = {}
        done = []
        for symbol in '◊' + SYMBOLS:
            if symbol not in index:
                continue
            i = index[symbol]
            old = parse_shape(f['shapes'][i])[2]
            if not old or bbox(old)[3] - bbox(old)[1] < 0.9 * (cap[3] - cap[1]):
                continue  # a character in this font
            nf, nl, recs = glyphs[symbol]
            new = transform(recs, lambda x, y: (round(x * k), round(y * k + dy)))
            f['shapes'][i] = encode_shape(nf, nl, new)
            if f['flags'] & 0x80:  # the layout: the PC glyph's own advance, the new bounds
                layout[i] = (round(widths[symbol] * k), bbox(new))
            done.append(symbol)
        if done:
            if layout:
                _set_layout(f, layout)
            out += data[last:header] + proto_menu.tag(DEFINE_FONT3, write_font(f))
            last = end
            replaced += len(done)
            log(f'Prompts: {f["name"].rstrip(bytes(1)).decode("latin-1")}: {len(done)} buttons')
    if not replaced:
        return data, 0
    out += data[last:]
    return proto_menu.with_length(bytes(out)), replaced


# --- Hint pictures -------------------------------------------------------------------------------

COLORS = {'A': (96, 186, 56), 'B': (232, 62, 52), 'X': (52, 132, 232), 'Y': (246, 190, 24)}
FACE = {'cross': ('A', '◊'), 'circle': ('B', '±'), 'square': ('X', '£'), 'triangle': ('Y', '³')}
SHOULDER = {'l1': 'LB', 'r1': 'RB', 'l2': 'LT', 'r2': 'RT'}
# Where the engraved L1, R1, L2, R2 are on the game's pictures (256 x 128).
LABELS = {'buttonhint__I1E.dds': (106, 19, 151, 57), 'buttonhint__I20.dds': (106, 19, 151, 57),
          'buttonhint__I22.dds': (106, 43, 152, 79), 'buttonhint__I24.dds': (106, 43, 152, 79),
          'buttonhint__I37.dds': (106, 13, 151, 44), 'buttonhint__I39.dds': (106, 13, 151, 44),
          'buttonhint__I3B.dds': (106, 36, 152, 72), 'buttonhint__I3D.dds': (106, 36, 152, 72)}


def _inpaint(rgb, mask, rounds=60):
    """The mask's pixels filled from their surroundings (repeated blurs)."""
    from PIL import Image, ImageChops, ImageFilter
    keep = ImageChops.invert(mask)
    for i in range(rounds):
        rgb = Image.composite(rgb, rgb.filter(ImageFilter.GaussianBlur(6 if i < rounds // 2 else 2)), keep)
    return rgb


def _mask(size, recs, box, scale=4):
    """recs filled (even-odd) into `box`, as an L mask of `size`."""
    from PIL import Image, ImageChops, ImageDraw
    W, H = size[0] * scale, size[1] * scale
    x0, y0, x1, y1 = bbox(recs)
    bx0, by0, bx1, by1 = (v * scale for v in box)
    k = min((bx1 - bx0) / (x1 - x0), (by1 - by0) / (y1 - y0))
    ox, oy = (bx0 + bx1) / 2 - (x0 + x1) / 2 * k, (by0 + by1) / 2 - (y0 + y1) / 2 * k
    mask = Image.new('1', (W, H), 0)
    for sp in subpaths(recs):
        poly, lx, ly = [], 0, 0
        for r in sp:
            if r[0] == 'style':
                lx, ly = r[5]
                poly.append((lx, ly))
            elif r[0] == 'l':
                lx, ly = r[1], r[2]
                poly.append((lx, ly))
            else:
                _, cx, cy, x, y = r
                for t in range(1, 11):
                    t /= 10
                    poly.append(((1 - t) ** 2 * lx + 2 * (1 - t) * t * cx + t * t * x,
                                 (1 - t) ** 2 * ly + 2 * (1 - t) * t * cy + t * t * y))
                lx, ly = x, y
        if len(poly) > 2:
            m = Image.new('1', (W, H), 0)
            ImageDraw.Draw(m).polygon([(px * k + ox, py * k + oy) for px, py in poly], fill=1)
            mask = ImageChops.logical_xor(mask, m)
    return mask.convert('L').resize(size, Image.LANCZOS)


def face_button(image, button, pc):
    """A PS4 face button picture with the Xbox letter in place of its symbol, or None."""
    from PIL import Image, ImageChops, ImageFilter
    rgba = image.convert('RGBA')
    hsv = rgba.convert('RGB').convert('HSV')
    symbol = ImageChops.multiply(hsv.getchannel('S').point(lambda a: 255 if a > 90 else 0),
                                 hsv.getchannel('V').point(lambda a: 255 if a > 60 else 0))
    box = symbol.getbbox()
    if not box:
        return None
    rgb = _inpaint(rgba.convert('RGB'), symbol.filter(ImageFilter.MaxFilter(5)))
    letter, glyph = FACE[button]
    shape = [r for sp in subpaths(pc[0][glyph][2])[2:] for r in sp]  # the letter, not its ring
    x0, y0, x1, y1 = box
    side = max(x1 - x0, y1 - y0) * 0.82
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    m = _mask(rgba.size, shape, (cx - side / 2, cy - side / 2, cx + side / 2, cy + side / 2))
    out = Image.merge('RGBA', (*rgb.split(), rgba.getchannel('A')))
    out.paste((10, 10, 10, 255), (1, 2), m.filter(ImageFilter.GaussianBlur(1.5)).point(lambda a: int(a * 0.8)))
    out.paste(COLORS[letter] + (255,), (0, 0), m)
    return out


def shoulder_button(image, button, pc, name):
    """A PS4 bumper or trigger picture engraved LB, RB, LT or RT, or None."""
    from PIL import Image, ImageChops, ImageDraw
    if name not in LABELS or image.size != (256, 128):
        return None
    rgba = image.convert('RGBA')
    x0, y0, x1, y1 = LABELS[name]
    region = Image.new('L', rgba.size, 0)
    ImageDraw.Draw(region).rectangle((x0, y0, x1, y1), fill=255)
    rgb = _inpaint(rgba.convert('RGB'), region)
    grain = Image.effect_noise((x1 - x0 + 1, y1 - y0 + 1), 9).convert('RGB')  # the surface's grain
    rgb.paste(ImageChops.add(rgb.crop((x0, y0, x1 + 1, y1 + 1)), grain, 1, -128), (x0, y0))
    glyphs, widths = pc
    text, x = [], 0
    for ch in SHOULDER[button]:
        text += transform(glyphs[ch][2], lambda px, py, x=x: (px + x, py))
        x += widths[ch]
    tx0, ty0, tx1, ty1 = bbox(text)
    h = (y1 - y0) - 8
    w = h * (tx1 - tx0) / (ty1 - ty0)
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    m = _mask(rgba.size, text, (cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2))
    out = Image.merge('RGBA', (*rgb.split(), rgba.getchannel('A')))
    out.paste((120, 120, 120, 255), (0, 1), m.point(lambda a: int(a * 0.55)))  # the engraving's lit edge
    out.paste((18, 18, 18, 255), (0, 0), m)
    return out


def decode_picture(gnf):
    """The RGBA picture of a hint texture (BC3, 1D thin tiling, stored upside down)."""
    import io
    from PIL import Image
    _, width, height, _, header = proto_prompts.gnf_info(gnf)
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
        return im.convert('RGBA').transpose(Image.FLIP_TOP_BOTTOM)


def patch_hints(data, pc):
    """fe_ingame with the hint pictures of the face and shoulder buttons made Xbox; returns
    (data, replaced). The d-pad's stay: the Xbox one is the same cross."""
    from PIL import Image
    out = bytearray(data)
    found = []

    def walk(start, end, name):
        for cid, pos, header, total in proto_prompts.chunks(data, start, end):
            if cid == 0x19000:
                name = proto_prompts.pstring(data, pos + 12)[0]
            if cid == 0x19002 and name in proto_prompts.HINT_TEXTURES:
                found.append((name, pos, total))
            elif header < total and cid in (0x19000, 0x19001):
                walk(pos + header, pos + total, name)

    walk(12, len(data), None)
    replaced = 0
    for name, pos, total in found:
        button = proto_prompts.HINT_TEXTURES[name]
        gnf = data[pos + 16:pos + total]
        if gnf[:4] != b'GNF ' or button not in FACE and button not in SHOULDER:
            continue
        fmt, width, height, tiling, header = proto_prompts.gnf_info(gnf)
        if fmt != 37 or tiling != 13 or width % 32 or height % 32:
            continue
        picture = decode_picture(gnf)
        new = face_button(picture, button, pc) if button in FACE else shoulder_button(picture, button, pc, name)
        if new is None:
            continue
        blocks = proto_prompts.tile_bc(proto_prompts.encode_bc3(new.transpose(Image.FLIP_TOP_BOTTOM)),
                                       width // 4, height // 4, 16)
        at = pos + 16 + header
        if at + len(blocks) <= pos + total:
            out[at:at + len(blocks)] = blocks
            replaced += 1
    return bytes(out), replaced


# --- Archives ------------------------------------------------------------------------------------

def _archive_members(path):
    """[(entry offset, kind, member)] of what is changed in archive `path`: 'text', 'hints',
    'fonts' and 'pc' (advfont.gfx)."""
    size = path.stat().st_size
    found = []
    with open(path, 'rb') as f:
        for entry_at, _, offset, length in proto_prompts.rcf_entries(f):
            if length < 64 or offset + length > size:
                continue
            f.seek(offset)
            head = f.read(min(length, 2048))
            kind = proto_prompts.classify(head)
            if kind is None and head[:3] == b'GFX':
                f.seek(offset)
                member = f.read(length)
                if b'PrototypeBodyPC' in member:
                    found.append((entry_at, 'pc', member))
                elif b'PrototypeBodyPS4' in member or b'PrototypeTitlePS4' in member:
                    found.append((entry_at, 'fonts', member))
                continue
            if kind:
                f.seek(offset)
                found.append((entry_at, kind, f.read(length)))
    if not any(kind == 'hints' for _, kind, _ in found):
        for i, (entry_at, kind, member) in enumerate(found):
            if kind == 'hud':
                data = zlib.decompress(member[16:]) if member.startswith(b'RZ\0\0') else member
                if b'buttonhint__I16.dds' in data:
                    found[i] = (entry_at, 'hints', member)
                    break
    return [item for item in found if item[1] != 'hud']


# --- Driver --------------------------------------------------------------------------------------

def generate(game, out, log=print):
    """Writes the Xbox prompts' overrides into `out` (cached); returns the override root or None."""
    hud = Path(game) / HUD
    loose = [hud / n for n in (*proto_prompts.TEXT_FILES, proto_prompts.HINT_FILE, *FONT_FILES, PC_FONT)
             if (hud / n).is_file()]
    archives = sorted(p for p in (Path(game) / 'p1').glob('*.rcf') if p.is_file())
    if not loose and not archives:
        log('Prompts: no p1/art/hud files nor p1/*.rcf in the game folder; PS4 buttons stay')
        return None
    key = hashlib.sha256(json.dumps({
        'v': VERSION, 'base': proto_prompts.VERSION,
        'files': [(s.name, s.stat().st_size, int(s.stat().st_mtime)) for s in (*loose, *archives)]},
        sort_keys=True).encode()).hexdigest()
    out = Path(out)
    stamp = out / 'prompts.json'
    try:
        if json.loads(stamp.read_text(encoding='utf-8')).get('key') == key:
            return out
    except (OSError, ValueError):
        pass
    counts = {'texts': 0, 'hints': 0, 'glyphs': 0}

    def change(kind, data, pc):
        if kind == 'text':
            data, n = proto_prompts.patch_text_bible(data, None, xbox=True)
            counts['texts'] += n
        elif kind == 'hints':
            data, n = patch_hints(data, pc)
            counts['hints'] += n
        elif kind == 'fonts':
            data, n = patch_fonts(data, pc, log)
            counts['glyphs'] += n
        return data

    target_dir = out / str(HUD).replace('\\', '/').lower()
    pc = None
    if (hud / PC_FONT).is_file():
        try:
            pc = pc_glyphs((hud / PC_FONT).read_bytes())
        except (ValueError, struct.error, IndexError) as error:
            log(f'Prompts: {PC_FONT}: {error}')
    for source in loose:
        if source.name == PC_FONT:
            continue
        target = target_dir / source.name.lower()
        try:
            if source.suffix == '.gfx':
                if pc is None:
                    continue
                data = change('fonts', source.read_bytes(), pc)
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
            else:
                kind = 'hints' if source.name == proto_prompts.HINT_FILE else 'text'
                if kind == 'hints' and pc is None:
                    continue
                proto_prompts.write_rz(target, change(kind, proto_prompts.read_rz(source), pc))
        except (ValueError, OSError, zlib.error, struct.error, IndexError) as error:
            log(f'Prompts: {source.name} left as it is ({error})')
            target.unlink(missing_ok=True)
    for archive in archives:
        target = out / 'p1' / (archive.name.lower() + '.patch')
        try:
            members = _archive_members(archive)
            for _, kind, member in members:
                if kind == 'pc' and pc is None:
                    pc = pc_glyphs(member)
            changed = []
            for entry_at, kind, member in members:
                if kind == 'pc' or kind in ('hints', 'fonts') and pc is None:
                    continue
                packed = member.startswith(b'RZ\0\0')
                data = zlib.decompress(member[16:]) if packed else member
                new = change(kind, data, pc)
                if new != data:
                    if packed:
                        new = b'RZ\0\0' + struct.pack('<III', 0, len(new), 0) + zlib.compress(new, 6)
                    changed.append((entry_at, new))
            if changed and proto_prompts.write_patch(archive, target, changed, log):
                log(f'Prompts: {archive.name}: Xbox buttons')
            else:
                target.unlink(missing_ok=True)
        except (ValueError, OSError, zlib.error, struct.error, IndexError) as error:
            log(f'Prompts: {archive.name} left as it is ({error})')
            target.unlink(missing_ok=True)
    if pc is None:
        log('Prompts: the PC font (advfont.gfx) is not in the game files; the PS4 buttons stay')
    stamp.write_text(json.dumps({'key': key}), encoding='utf-8')
    log(f'Prompts: Xbox ({counts["glyphs"]} button glyphs, {counts["hints"]} hint pictures, '
        f'{counts["texts"]} texts)')
    return out


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('game', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    root = generate(args.game, args.out, log=lambda m: print(m, flush=True))
    sys.exit(0 if root else 3)
