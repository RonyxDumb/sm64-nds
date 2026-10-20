#!/usr/bin/env python3
"""Predecode static triangle runs in NDS overlay binaries, preserving addresses.

This is a conservative command compiler, not a complete N64-to-DS GPU compiler.
State, lighting, vertices, matrices and dynamic lists still use the renderer.
Only declared Gfx arrays with a final ENDDL are scanned. Skipped original
commands remain intact, so references to interior list entries keep working.
"""
import argparse
import re
import struct
from pathlib import Path

OPCODE = 0xD4
MAX_COMMANDS = 32
MAX_EXTRA_BYTES = 4096
ROOT = Path(__file__).resolve().parents[1]
KNOWN = {0, 1, 2, 3, 4, 5, 6, 7, 8, *range(0xD3, 0x100)}

def gfx_names(root=ROOT):
    names = set()
    for directory in ('actors', 'levels', 'bin', 'textures'):
        for path in (root / directory).rglob('*.c'):
            names.update(re.findall(r'\bGfx\s+([A-Za-z_]\w*)\s*\[', path.read_text(errors='replace')))
    return names

def elf_arrays(data, names):
    if data[:6] != b'\x7fELF\x01\x01':
        raise ValueError('Expected little-endian ELF32')
    if struct.unpack_from('<H', data, 18)[0] != 40:
        raise ValueError('Expected ARM ELF')
    shoff = struct.unpack_from('<I', data, 32)[0]
    entsize, count = struct.unpack_from('<HH', data, 46)
    if entsize != 40 or shoff + count * entsize > len(data):
        raise ValueError('Invalid ELF section table')
    sections = [struct.unpack_from('<10I', data, shoff + i * 40) for i in range(count)]
    allocated = [s for s in sections if s[1] == 1 and s[2] & 2 and s[5]]
    if not allocated:
        return 0, []
    base = min(s[3] for s in allocated)
    arrays = set()
    for section in sections:
        if section[1] != 2:
            continue
        strings = sections[section[6]]
        text = data[strings[4]:strings[4] + strings[5]]
        if section[9] != 16:
            raise ValueError('Invalid ELF symbol size')
        for off in range(section[4], section[4] + section[5], 16):
            nameoff, value, size, info, _, index = struct.unpack_from('<IIIBBH', data, off)
            if info & 15 != 1 or not (0 < index < len(sections)):
                continue
            name = text[nameoff:text.find(b'\0', nameoff)].decode('utf8')
            owner = sections[index]
            if name in names and size >= 24 and size % 8 == 0 and owner[1] == 1 and owner[2] & 2:
                if owner[3] <= value and value + size <= owner[3] + owner[5]:
                    arrays.add((value - base, size))
    return base, sorted(arrays)

def indices(w0, w1):
    values = [(w0 >> shift & 255) >> 1 for shift in (16, 8, 0)]
    if w0 >> 24 == 6:
        values += [(w1 >> shift & 255) >> 1 for shift in (16, 8, 0)]
    return values if max(values) < 16 else None

def compile_runs(original, base, arrays, budget=None):
    if not (0 < base >> 24 < 32) or base & 0xffffff:
        return original, {'runs': 0, 'commands': 0, 'extra_bytes': 0}
    if budget is None:
        budget = min(MAX_EXTRA_BYTES, len(original) // 16)
    candidates = {}
    for start, size in arrays:
        if start < 0 or start + size > len(original):
            raise ValueError('Gfx symbol outside binary')
        commands = list(struct.iter_unpack('<II', original[start:start + size]))
        if any(w0 >> 24 == OPCODE for w0, _ in commands):
            continue  # Already compiled; do not compact its preserved interior entries.
        if commands[-1][0] >> 24 != 0xDF or any(w0 >> 24 not in KNOWN for w0, _ in commands):
            continue
        i = 0
        while i < len(commands):
            begin = i
            packet = bytearray()
            while i < len(commands) and i - begin < MAX_COMMANDS and commands[i][0] >> 24 in (5, 6):
                decoded = indices(*commands[i])
                if decoded is None:
                    break
                packet.extend((len(decoded), *decoded))
                i += 1
            count = i - begin
            if count >= 3:
                candidates.setdefault(start + begin * 8, (count, bytes(packet)))
            if i == begin:
                i += 1
    result = bytearray(original)
    stats = {'runs': 0, 'commands': 0, 'extra_bytes': 0}
    # Prefer long runs. Limit added RAM per loaded segment to 4 KiB / 6.25%.
    for offset, (count, packet) in sorted(candidates.items(), key=lambda item: -(item[1][0] - 1) / len(item[1][1])):
        if len(result) - len(original) + len(packet) > budget:
            continue
        address = base + len(result)
        if address >> 24 != base >> 24:
            continue
        struct.pack_into('<II', result, offset, OPCODE << 24 | count, address)
        result.extend(packet)
        stats['runs'] += 1
        stats['commands'] += count
    stats['extra_bytes'] = len(result) - len(original)
    return bytes(result), stats

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path)
    parser.add_argument('binary', type=Path)
    args = parser.parse_args()
    base, arrays = elf_arrays(args.elf.read_bytes(), gfx_names())
    original = args.binary.read_bytes()
    compiled, stats = compile_runs(original, base, arrays)
    if compiled != original:
        temporary = args.binary.with_suffix(args.binary.suffix + '.tmp')
        temporary.write_bytes(compiled)
        temporary.replace(args.binary)
    print(f"NDS static triangles: {args.binary}: {stats['runs']} runs, "
          f"{stats['commands']} commands, +{stats['extra_bytes']} bytes")

if __name__ == '__main__':
    main()
