#!/usr/bin/env python3
"""Load RV32 little-endian ELF PT_LOAD segments into the simulated 256 KiB RAM."""
import struct
import sys
from pathlib import Path
data = Path(sys.argv[1]).read_bytes()
assert data[:6] == b'\x7fELF\x01\x01'
assert struct.unpack_from('<H',data,18)[0] == 243, 'not RISC-V'
phoff = struct.unpack_from('<I',data,28)[0]
entsize, count = struct.unpack_from('<HH',data,42)
ram = bytearray(256*1024)
for i in range(count):
    kind, off, addr, _, size, memsize, _, _ = struct.unpack_from('<8I',data,phoff+i*entsize)
    if kind == 1:
        assert addr+memsize <= 128*1024, 'firmware overlaps reserved stack/mailbox'
        ram[addr:addr+size] = data[off:off+size]
Path(sys.argv[2]).write_text(''.join(f'{w[0]:08x}\n' for w in struct.iter_unpack('<I',ram)))
print(f'RV32 ELF loaded: {len(data)} bytes; generated {len(ram)//1024} KiB RAM image')
