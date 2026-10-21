#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A map of the game's code, for the native game layer (docs/NATIVE.md).

Reads the decrypted eboot (out/eboot.elf, written by prepare.py) and writes out/game_map/:

  classes.txt    every C++ class with run-time type information: its bases, its virtual function
                 tables and their functions
  functions.csv  every function found (prologues and call targets) with the name the map can give
                 it: a virtual function of a class, a constructor/destructor (it stores a vtable),
                 or the source file its assertions name
  sources.txt    the engine source files the eboot names, with the functions that name them
  symbols.txt    "name address" lines for Ghidra (ImportSymbolsScript.py) or other tools
  summary.txt    counts

The map is made from your own copy of the game and stays in out/ (it is never shipped).

  python scripts/game_map.py [--elf out/eboot.elf] [--out out/game_map]
"""
import argparse
import bisect
import collections
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from prepare import nid  # noqa: E402  (the PS4's import name hash)


def load(elf):
    """(image, code range, relocation targets {slot: value}, import slots {slot: symbol name})."""
    phoff, = struct.unpack_from('<Q', elf, 0x20)
    phentsize, phnum = struct.unpack_from('<HH', elf, 0x36)
    ph = []
    for i in range(phnum):
        kind, flags, offset, vaddr, _, filesz, memsz, _ = struct.unpack_from('<IIQQQQQQ', elf, phoff + i * phentsize)
        ph.append(dict(type=kind, flags=flags, offset=offset, vaddr=vaddr, filesz=filesz, memsz=memsz))
    loads = [p for p in ph if p['type'] in (1, 0x61000010)]
    image = bytearray(max(p['vaddr'] + p['memsz'] for p in loads))
    for p in loads:
        image[p['vaddr']:p['vaddr'] + p['filesz']] = elf[p['offset']:p['offset'] + p['filesz']]
    code = next(p for p in ph if p['type'] == 1 and p['flags'] & 1)
    dp = next(p for p in ph if p['type'] == 2)
    tags = {}
    for pos in range(dp['offset'], dp['offset'] + dp['filesz'], 16):
        tag, value = struct.unpack_from('<QQ', elf, pos)
        if tag == 0:
            break
        tags[tag] = value
    lib = next(p for p in ph if p['type'] == 0x61000000)
    blob = elf[lib['offset']:lib['offset'] + lib['filesz']]
    strings = blob[tags[0x61000035]:tags[0x61000035] + tags[0x61000037]]
    syms = blob[tags[0x61000039]:tags[0x61000039] + tags[0x6100003f]]
    symbols = []
    for pos in range(0, len(syms), 24):
        name, info, _, section, value, _ = struct.unpack_from('<IBBHQQ', syms, pos)
        symbols.append((strings[name:strings.index(0, name)].decode('ascii', 'replace'), section, value))
    relocs, imports = {}, {}
    for offset_tag, size_tag in ((0x61000029, 0x6100002d), (0x6100002f, 0x61000031)):
        table = blob[tags[offset_tag]:tags[offset_tag] + tags[size_tag]]
        for pos in range(0, len(table), 24):
            target, info, addend = struct.unpack_from('<QQq', table, pos)
            kind, sym = info & 0xffffffff, info >> 32
            if kind == 8:
                relocs[target] = addend
            elif kind in (1, 6, 7):
                name, section, value = symbols[sym]
                if section:
                    relocs[target] = value + addend
                else:
                    imports[target] = name
    for target, value in relocs.items():
        if target + 8 <= len(image):
            struct.pack_into('<Q', image, target, value & 0xffffffffffffffff)
    return bytes(image), (code['vaddr'], code['vaddr'] + code['filesz']), relocs, imports


def demangle(names):
    tool = shutil.which('llvm-cxxfilt') or shutil.which('c++filt')
    if not tool or not names:
        return {n: n for n in names}
    out = subprocess.run([tool], input='\n'.join('_Z' + n for n in names), capture_output=True,
                         text=True).stdout.splitlines()
    return {n: (d if not d.startswith('_Z') else n) for n, d in zip(names, out)}


def cstring(image, at, limit=512):
    end = image.find(b'\0', at, at + limit)
    return image[at:end].decode('latin1') if end > at else ''


def main():
    root = Path(__file__).resolve().parent.parent
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--elf', type=Path, default=root / 'out' / 'eboot.elf')
    p.add_argument('--out', type=Path, default=root / 'out' / 'game_map')
    a = p.parse_args()
    if not a.elf.is_file():
        sys.exit(f'{a.elf} not found: start the game once (run.py prepares it)')
    image, (code_lo, code_hi), relocs, imports = load(a.elf.read_bytes())
    text = image[code_lo:code_hi]
    in_code = lambda v: code_lo <= v < code_hi  # noqa: E731

    # ---- functions: frame-pointer prologues and direct call targets
    starts = set()
    for m in re.finditer(rb'\x55\x48\x89\xe5', text):
        starts.add(code_lo + m.start())
    calls = collections.Counter()
    for m in re.finditer(rb'\xe8', text):
        i = m.start()
        if i + 5 > len(text):
            break
        t = code_lo + i + 5 + struct.unpack_from('<i', text, i + 1)[0]
        if in_code(t):
            calls[t] += 1
    starts |= {t for t, n in calls.items() if n >= 2 or t in starts}
    starts |= {v for v in relocs.values() if in_code(v)}
    names = {}
    notes = collections.defaultdict(list)
    # Names found by hand (game/p1/known_functions.txt) come first.
    known_file = root / 'game' / 'p1' / 'known_functions.txt'
    if known_file.is_file():
        for line in known_file.read_text(encoding='utf-8').splitlines():
            parts = line.split('#')[0].split()
            if len(parts) == 2:
                names[int(parts[0], 16)] = parts[1]
                notes[int(parts[0], 16)].append('known')
                starts.add(int(parts[0], 16))
    functions = sorted(starts)

    def owner(addr):
        i = bisect.bisect_right(functions, addr) - 1
        return functions[i] if i >= 0 else None

    # ---- run-time type information (Itanium C++ ABI)
    abi = {nid('_ZTVN10__cxxabiv117__class_type_infoE'): 'class',
           nid('_ZTVN10__cxxabiv120__si_class_type_infoE'): 'single',
           nid('_ZTVN10__cxxabiv121__vmi_class_type_infoE'): 'multiple'}
    typeinfos = {}  # typeinfo address -> mangled name
    for slot, kind_name in imports.items():
        kind = abi.get(kind_name.split('#')[0])
        if not kind:
            continue
        name_ptr = relocs.get(slot + 8)
        if name_ptr is None:
            continue
        name = cstring(image, name_ptr)
        if re.fullmatch(r'[A-Za-z0-9_]+', name or ''):
            typeinfos[slot] = (name, kind)
    pretty = demangle(sorted({n for n, _ in typeinfos.values()}))
    bases = {}
    for ti, (name, kind) in typeinfos.items():
        if kind == 'single' and relocs.get(ti + 16) in typeinfos:
            bases[ti] = [relocs[ti + 16]]
        elif kind == 'multiple':
            count, = struct.unpack_from('<I', image, ti + 20)
            bases[ti] = [relocs[ti + 24 + 16 * k] for k in range(min(count, 64))
                         if relocs.get(ti + 24 + 16 * k) in typeinfos]
    vtables = collections.defaultdict(list)  # typeinfo -> [(vtable address, offset to top, [functions])]
    by_value = collections.defaultdict(list)
    for slot, value in relocs.items():
        by_value[value].append(slot)
    for ti in typeinfos:
        for slot in sorted(by_value.get(ti, ())):
            top, = struct.unpack_from('<q', image, slot - 8)
            if not -0x100000 < top <= 0:
                continue
            funcs, at = [], slot + 8
            while relocs.get(at) is not None and in_code(relocs[at]):
                funcs.append(relocs[at])
                at += 8
            if funcs or top == 0:
                vtables[ti].append((slot + 8, -top, funcs))
    for ti, tables in vtables.items():
        cls = pretty[typeinfos[ti][0]]
        for vt, top, funcs in tables:
            for k, f in enumerate(funcs):
                if f not in names:
                    names[f] = f'{cls}::vf{k}' + (f'@{top:#x}' if top else '')
                notes[f].append(f'{cls} vtable{"+" + hex(top) if top else ""} slot {k}')
    # Constructors and destructors store their class's vtable: lea reg,[rip+vtable]; mov [obj],reg.
    vt_class = {vt: pretty[typeinfos[ti][0]] for ti, tables in vtables.items() for vt, _, _ in tables}
    for m in re.finditer(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]', text):
        i = m.start()
        t = code_lo + i + 7 + struct.unpack_from('<i', text, i + 3)[0]
        cls = vt_class.get(t) or vt_class.get(t + 16)
        if cls:
            f = owner(code_lo + i)
            if f is not None and f not in names:
                names[f] = f'{cls}::{cls.split("::")[-1]}_or_dtor'
            if f is not None:
                notes[f].append(f'stores {cls} vtable')

    # ---- engine source files named by assertions
    sources = collections.defaultdict(set)
    for m in re.finditer(rb'[A-Za-z]:\\[^\0]{3,200}?\.(?:cpp|h|inl)\0', image):
        path = m.group()[:-1].decode('latin1')
        short = path.split('\\code\\')[-1] if '\\code\\' in path else path
        sources[short].add(m.start())
    string_at = {addr: short for short, addrs in sources.items() for addr in addrs}
    users = collections.defaultdict(set)
    for m in re.finditer(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]', text):
        i = m.start()
        t = code_lo + i + 7 + struct.unpack_from('<i', text, i + 3)[0]
        if t in string_at:
            f = owner(code_lo + i)
            if f is not None:
                users[string_at[t]].add(f)
                notes[f].append(f'asserts in {string_at[t]}')
                if f not in names:
                    stem = re.sub(r'\W', '_', Path(string_at[t].replace('\\', '/')).stem)
                    names[f] = f'{stem}_{f:x}'

    # ---- output
    a.out.mkdir(parents=True, exist_ok=True)
    with (a.out / 'classes.txt').open('w', encoding='utf-8') as f:
        for ti, (name, kind) in sorted(typeinfos.items(), key=lambda kv: pretty[kv[1][0]]):
            line = pretty[name]
            if bases.get(ti):
                line += ' : ' + ', '.join(pretty[typeinfos[b][0]] for b in bases[ti])
            f.write(line + f'    [typeinfo {ti:#x}]\n')
            for vt, top, funcs in vtables.get(ti, ()):
                f.write(f'    vtable {vt:#x}' + (f' (base at +{top:#x})' if top else '') + f', {len(funcs)} functions\n')
                for k, fn in enumerate(funcs):
                    f.write(f'        {k:3d}  {fn:#x}\n')
    with (a.out / 'functions.csv').open('w', encoding='utf-8') as f:
        f.write('address,name,notes\n')
        for fn in functions:
            f.write(f'{fn:#x},{names.get(fn, "")},"{"; ".join(sorted(set(notes.get(fn, ()))))[:300]}"\n')
    with (a.out / 'sources.txt').open('w', encoding='utf-8') as f:
        for short in sorted(sources):
            f.write(short + '\n')
            for fn in sorted(users.get(short, ())):
                f.write(f'    {fn:#x}  {names.get(fn, "")}\n')
    with (a.out / 'symbols.txt').open('w', encoding='utf-8') as f:
        for fn, name in sorted(names.items()):
            f.write(f'{re.sub(r"[^A-Za-z0-9_:@.~<>,]", "_", name)} {fn:x}\n')
    named = sum(1 for fn in functions if fn in names)
    summary = (f'functions: {len(functions)} ({named} named)\n'
               f'classes with type information: {len(typeinfos)}\n'
               f'virtual function tables: {sum(len(v) for v in vtables.values())}\n'
               f'virtual functions: {len({fn for v in vtables.values() for _, _, fs in v for fn in fs})}\n'
               f'source files named: {len(sources)}\n')
    (a.out / 'summary.txt').write_text(summary, encoding='utf-8')
    print(summary + f'Written to {a.out}')


if __name__ == '__main__':
    main()
