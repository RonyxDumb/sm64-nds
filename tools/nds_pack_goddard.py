#!/usr/bin/env python3
"""Pack original Goddard integer keyframes into lossless 16-frame blocks."""
import ast
import re
import struct
import sys
from pathlib import Path

ARRAY = re.compile(r"static s16 (\w+)\[\]\[(\d+)\]\s*=\s*(\{.*?\});", re.S)


def pack_frames(rows):
    width = len(rows[0])
    blocks = []
    for start in range(0, len(rows), 16):
        block = rows[start:start + 16]
        axes, widths = [], []
        for axis in range(width):
            deltas = [b[axis] - a[axis] for a, b in zip(block, block[1:])]
            bits = 0
            while any(d < -(1 << (bits - 1)) or d >= (1 << (bits - 1))
                      for d in deltas) if bits else any(deltas):
                bits += 1
            widths.append(bits)
            data = bytearray((len(deltas) * bits + 7) // 8)
            for i, delta in enumerate(deltas):
                value = delta & ((1 << bits) - 1)
                for bit in range(bits):
                    data[(i * bits + bit) // 8] |= ((value >> bit) & 1) << ((i * bits + bit) % 8)
            axes.append(data)
        blocks.append(struct.pack('<' + 'h' * width, *block[0]) + bytes(widths) + b''.join(axes))
    offset = 8 + 4 * (len(blocks) + 1)
    offsets = [offset]
    for block in blocks:
        offset += len(block)
        offsets.append(offset)
    return (struct.pack('<IHH', len(rows), width, len(blocks)) +
            struct.pack('<' + 'I' * len(offsets), *offsets) + b''.join(blocks) + bytes(3))


def convert(source):
    counts = {}
    def replace(match):
        name, width = match[1], int(match[2])
        rows = ast.literal_eval(match[3].replace('{', '[').replace('}', ']'))
        if not rows or any(len(r) != width or any(not -32768 <= v <= 32767 for v in r) for r in rows):
            raise ValueError(f'invalid integer keyframes: {name}')
        counts[name] = len(rows)
        data = pack_frames(rows)
        lines = [', '.join(str(v) for v in data[i:i + 24]) for i in range(0, len(data), 24)]
        return 'static const unsigned char ' + name + '[] = {\n    ' + ',\n    '.join(lines) + '\n};'
    result = ARRAY.sub(replace, source)
    for name, count in counts.items():
        pattern = r'\{\s*ARRAY_COUNT\(' + re.escape(name) + r'\),\s*(GD_ANIM_\w+),\s*' + re.escape(name) + r'\s*\}'
        result, n = re.subn(pattern, lambda m: '{ ' + str(count) + ', (0x80 | ' + m[1] + '), (void *)' + name + ' }', result)
        if n != 1:
            raise ValueError(f'expected one animation descriptor for {name}, found {n}')
    return result


if __name__ == '__main__':
    target = Path(sys.argv[2])
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(convert(Path(sys.argv[1]).read_text()))
