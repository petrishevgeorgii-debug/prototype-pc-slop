#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""PROTOTYPE's story mission names for the Discord status (src/runtime_discord.c).

The game names its story missions in its text bible (p1/art/hud/textbible_<language>.p3d.rz, or
inside the p1/*.rcf archives of PS4 package dumps): "<episode>_<mission>_MISSION_NAME", e.g.
1_2_MISSION_NAME, for the mission the game database calls e01m02. This writes them, in the game's
language, as "<id>\\t<name>" lines (names in title case, as a status reads them), cached by the
game files they came from.

    python scripts/proto_presence.py <game> <out file> [--language <PS4 language id>]
"""
import argparse
import hashlib
import json
import re
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import proto_prompts  # noqa: E402

VERSION = 1
LANGUAGE_FILES = {1: 'english', 2: 'french', 3: 'spanish', 4: 'german', 5: 'italian'}
# What each language's text bible calls "YES": tells apart the text bibles of an archive.
YES = {1: 'YES', 2: 'OUI', 3: 'SÍ', 4: 'JA', 5: 'SÌ', 8: 'ДА'}
MISSION_KEY = re.compile(r'^(\d+)_(\d+)_MISSION_NAME$')


def strings(package):
    """{key: text} of a text bible package (uncompressed P3D)."""
    out = {}
    for cid, pos, header, total in proto_prompts.chunks(package, 12, len(package)):
        if cid != proto_prompts.TEXT_BIBLE:
            continue
        chunk = package[pos:pos + total]
        p = 12
        _, p = proto_prompts.pstring(chunk, p)
        _, count = struct.unpack_from('<II', chunk, p)
        p += 8
        keys = []
        for _ in range(count):
            key, p = proto_prompts.pstring(chunk, p)
            keys.append(key)
        starts = struct.unpack_from(f'<{count}I', chunk, p)
        dcid, dpos, _, _ = next(proto_prompts.chunks(chunk, header, total))
        if dcid != proto_prompts.TEXT_DATA:
            continue
        q = dpos + 12
        _, q = proto_prompts.pstring(chunk, q)
        _, size = struct.unpack_from('<II', chunk, q)
        text = chunk[q + 8:q + 8 + size]
        bounds = list(starts) + [size]
        for i, key in enumerate(keys):
            out[key] = text[bounds[i]:bounds[i + 1]].split(b'\0')[0].decode('utf-8', 'replace')
    return out


def title(name):
    """A mission name as a status shows it: "MEMORY IN DEATH" -> "Memory In Death" (Cyrillic:
    only the first letter capital, as Russian titles are written)."""
    name = ' '.join(name.split()).strip('!').strip()  # one is "!!!...!!!" (a placeholder left in)
    if any('Ѐ' <= ch <= 'ӿ' for ch in name):
        return name[:1].upper() + name[1:].lower()
    return ' '.join(w[:1].upper() + w[1:].lower() for w in name.split(' '))


def names(table):
    """{"e01m02": name} from a text bible's strings."""
    out = {}
    for key, text in table.items():
        m = MISSION_KEY.match(key)
        if m and text.strip():
            out[f'e{int(m[1]):02d}m{int(m[2]):02d}'] = title(text)
    return out


def loose_tables(game, language):
    hud = Path(game) / proto_prompts.HUD
    wanted = LANGUAGE_FILES.get(language, 'english')
    for lang in dict.fromkeys((wanted, 'english')):
        path = hud / f'textbible_{lang}.p3d.rz'
        if path.is_file():
            yield path, proto_prompts.read_rz(path)


def archive_tables(game):
    for archive in sorted(p for p in (Path(game) / 'p1').glob('*.rcf') if p.is_file()):
        size = archive.stat().st_size
        with open(archive, 'rb') as f:
            for _, _, offset, length in proto_prompts.rcf_entries(f):
                if length < 64 or offset + length > size:
                    continue
                f.seek(offset)
                if proto_prompts.classify(f.read(min(length, 2048))) != 'text':
                    continue
                f.seek(offset)
                member = f.read(length)
                yield archive, zlib.decompress(member[16:]) if member.startswith(b'RZ\0\0') else member


def mission_names(game, language):
    """{id: name} in the game's language (English when that language's text is not found)."""
    tables = [strings(data) for _, data in loose_tables(game, language)]
    tables = [t for t in tables if names(t)]
    if tables:
        return names(tables[0])
    found = [t for t in (strings(data) for _, data in archive_tables(game)) if names(t)]
    for want in (YES.get(language), 'YES', None):
        for table in found:
            if want is None or table.get('YES', '').strip().upper() == want:
                return names(table)
    return {}


def generate(game, out_file, language=1, log=print):
    """Writes the mission names file (cached); returns its path, or None without names."""
    hud = Path(game) / proto_prompts.HUD
    sources = sorted([*hud.glob('textbible_*.p3d.rz'), *(Path(game) / 'p1').glob('*.rcf')])
    key = hashlib.sha256(json.dumps({'v': VERSION, 'language': language, 'files': [
        (s.name, s.stat().st_size, int(s.stat().st_mtime)) for s in sources if s.is_file()]}).encode()).hexdigest()
    out_file = Path(out_file)
    stamp = out_file.with_suffix('.json')
    try:
        if json.loads(stamp.read_text(encoding='utf-8')).get('key') == key and out_file.is_file():
            return out_file
    except (OSError, ValueError):
        pass
    try:
        found = mission_names(game, language)
    except (OSError, ValueError, zlib.error, struct.error, StopIteration) as error:
        log(f'Discord: no mission names ({error})')
        return None
    if not found:
        log('Discord: no mission names in the game\'s text')
        return None
    out_file.parent.mkdir(parents=True, exist_ok=True)
    out_file.write_text(''.join(f'{k}\t{v}\n' for k, v in sorted(found.items())), encoding='utf-8')
    stamp.write_text(json.dumps({'key': key}), encoding='utf-8')
    return out_file


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('game', type=Path)
    parser.add_argument('out', type=Path)
    parser.add_argument('--language', type=int, default=1)
    args = parser.parse_args()
    path = generate(args.game, args.out, args.language)
    if path:
        print(path.read_text(encoding='utf-8'), end='')
    return 0 if path else 1


if __name__ == '__main__':
    sys.exit(main())
