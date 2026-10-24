#!/usr/bin/env python3
"""Check every original keyframe against the compiled DS decoder."""
import ast
import ctypes
import importlib.util
import random
import struct
import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('packer', root / 'tools/nds_pack_goddard.py')
packer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packer)

with tempfile.TemporaryDirectory() as temp:
    lib = Path(temp) / 'codec.so'
    subprocess.run(['cc', '-shared', '-fPIC', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(root / 'src/nds/nds_goddard_anim.c'), '-o', str(lib)], check=True)
    decode = ctypes.CDLL(str(lib)).nds_goddard_frame
    decode.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.POINTER(ctypes.c_int16)]
    total = raw_bytes = packed_bytes = 0
    def check(rows):
        global total, raw_bytes, packed_bytes
        flat = [v for row in rows for v in row]
        packed = packer.pack_frames(rows)
        source = ctypes.create_string_buffer(packed)
        out = (ctypes.c_int16 * len(rows[0]))()
        for frame, expected in enumerate(rows):
            assert decode(source, frame, out) == len(expected)
            assert list(out) == expected
        total += len(rows)
        raw_bytes += len(flat) * 2
        packed_bytes += len(packed)

    for path in (root / 'src/goddard/dynlists').glob('anim*.c'):
        for match in packer.ARRAY.finditer(path.read_text()):
            check(ast.literal_eval(match[3].replace('{', '[').replace('}', ']')))
    print(f'Original Goddard: {total} exact frames; {raw_bytes} -> {packed_bytes} bytes')
    rng = random.Random(0)
    for width in (3, 6, 9):
        for count in (1, 2, 15, 16, 17, 31, 32, 33, 127):
            check([[rng.randint(-32768, 32767) for _ in range(width)] for _ in range(count)])
            check([[32767 if i % 2 else -32768] * width for i in range(count)])
            check([[0] * width for _ in range(count)])
    print('Signed deltas, constant axes and block boundaries: PASS')
