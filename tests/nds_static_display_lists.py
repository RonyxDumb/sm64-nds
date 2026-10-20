"""Validate static list compilation, including original interior entry points."""
import argparse
import importlib.util
import random
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('nds_compile', ROOT / 'tools/nds_compile_display_lists.py')
compiler = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compiler)

def tri(rng, double):
    values = [rng.randrange(16) for _ in range(6 if double else 3)]
    w0 = (6 if double else 5) << 24
    w0 |= sum(value * 2 << shift for value, shift in zip(values[:3], (16, 8, 0)))
    w1 = sum(value * 2 << shift for value, shift in zip(values[3:], (16, 8, 0)))
    return struct.pack('<II', w0, w1)

def run(data, base, start=0, initial=()):
    offset = start
    batch = list(initial)
    batches = []
    def flush():
        nonlocal batch
        if batch:
            batches.append(tuple(batch))
            batch = []
    for _ in range(10000):
        w0, w1 = struct.unpack_from('<II', data, offset)
        opcode = w0 >> 24
        if (opcode not in (5, 6, compiler.OPCODE) and batch) or len(batch) > 90:
            flush()
        if opcode in (5, 6):
            batch.extend(compiler.indices(w0, w1))
        elif opcode == compiler.OPCODE:
            pos = w1 - base
            for _ in range(w0 & 65535):
                if len(batch) > 90:
                    flush()
                count = data[pos]
                pos += 1
                assert count in (3, 6)
                batch.extend(data[pos:pos+count])
                pos += count
            offset += ((w0 & 65535) - 1) * 8
        elif opcode == 0xDF:
            return batches
        offset += 8
    raise AssertionError('List did not terminate')

def synthetic():
    header=(ROOT/'src/nds/nds_gfx_commands.h').read_text()
    assert f'#define NDS_GFX_TRI_BATCH 0x{compiler.OPCODE:X}' in header
    assert f'#define NDS_GFX_TRI_COMMANDS {compiler.MAX_COMMANDS}' in header
    rng = random.Random(6432)
    for _ in range(100):
        data = bytearray()
        for i in range(150):
            data.extend(tri(rng, bool(rng.randrange(2))))
            if rng.randrange(20) == 0:
                data.extend(struct.pack('<II', 0xD7000000, 1))
        data.extend(struct.pack('<II', 0xDF000000, 0))
        data = bytes(data)
        compiled, stats = compiler.compile_runs(data, 0x04000000, [(0,len(data))], budget=10000)
        assert stats['runs'] > 0
        for entry in range(0, len(data), 8):
            for count in (0, 3, 87, 90, 93, 96):
                initial = [i % 16 for i in range(count)]
                assert run(data, 0x04000000, entry, initial) == run(compiled, 0x04000000, entry, initial)
        again, _ = compiler.compile_runs(compiled, 0x04000000, [(0,len(data))], budget=10000)
        assert again == compiled
        limited, stats = compiler.compile_runs(data, 0x04000000, [(0,len(data))], budget=20)
        assert len(limited)-len(data) <= 20
        assert run(limited, 0x04000000) == run(data, 0x04000000)
    assert compiler.compile_runs(b'12345678',0,[])[0] == b'12345678'
    print('Static compiler batching, budgets and interior entry points: PASS')

def original_binary(elf):
    shoff=struct.unpack_from('<I',elf,32)[0]
    count=struct.unpack_from('<H',elf,48)[0]
    sections=[struct.unpack_from('<10I',elf,shoff+i*40) for i in range(count)]
    sections=[s for s in sections if s[1]==1 and s[2]&2 and s[5]]
    base=min(s[3] for s in sections)
    data=bytearray(max(s[3]+s[5] for s in sections)-base)
    for s in sections:
        data[s[3]-base:s[3]-base+s[5]]=elf[s[4]:s[4]+s[5]]
    return bytes(data)

def built(directory):
    names=compiler.gfx_names()
    files=runs=commands=extra=0
    for elf_path in sorted(directory.rglob('*.elf')):
        binary=elf_path.with_suffix('.bin')
        if not binary.exists() or not any(p in ('actors','levels','bin') for p in elf_path.relative_to(directory).parts[:-1]):
            continue
        elf=elf_path.read_bytes()
        base,arrays=compiler.elf_arrays(elf,names)
        original=original_binary(elf)
        actual=binary.read_bytes()
        expected,stats=compiler.compile_runs(original,base,arrays)
        assert actual==expected, binary
        prefix=bytearray(actual[:len(original)])
        for offset in range(0,len(original)-7,8):
            if prefix[offset:offset+8] != original[offset:offset+8]:
                w0,w1=struct.unpack_from('<II',prefix,offset)
                assert w0>>24==compiler.OPCODE
                n=w0&65535
                assert 3<=n<=compiler.MAX_COMMANDS
                packet=bytearray()
                for j in range(n):
                    a,b=struct.unpack_from('<II',original,offset+j*8)
                    assert a>>24 in (5,6)
                    decoded=compiler.indices(a,b)
                    packet.extend((len(decoded),*decoded))
                assert actual[w1-base:w1-base+len(packet)]==packet
                prefix[offset:offset+8]=original[offset:offset+8]
        assert bytes(prefix)==original
        assert stats['extra_bytes']<=min(4096,len(original)//16)
        files+=1;runs+=stats['runs'];commands+=stats['commands'];extra+=stats['extra_bytes']
    print(f'Built overlays: PASS ({files} files, {runs} compiled runs, {commands} original triangle commands, {extra} extra bytes in ROM)')

if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--built',type=Path)
    args=parser.parse_args()
    synthetic()
    if args.built:
        built(args.built)
