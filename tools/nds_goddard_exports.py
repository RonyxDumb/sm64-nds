#!/usr/bin/env python3
"""Root the module's undefined ARM symbols in the resident executable."""
import subprocess
import sys
from pathlib import Path

names = sorted({line.split()[-1] for line in subprocess.check_output(
    [sys.argv[1], '-u', sys.argv[2]], text=True).splitlines() if line.split() and line.split()[0] == 'U'})
lines = ['.section .rodata.goddard_exports,"a"', '.align 2',
         '.global gNdsGoddardExports', 'gNdsGoddardExports:']
for i, name in enumerate(names):
    lines.append(f'.word .Lgd_name_{i}, {name}')
lines.append('.word 0, 0')
for i, name in enumerate(names):
    lines.extend([f'.Lgd_name_{i}:', f'.asciz "{name}"'])
output = Path(sys.argv[3])
content = '\n'.join(lines) + '\n'
if not output.exists() or output.read_text() != content:
    output.write_text(content)
print(f'Goddard DS imports: {len(names)}')
