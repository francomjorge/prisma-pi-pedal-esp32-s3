#!/usr/bin/env python3
from pathlib import Path
import sys

FLASH_SIZE = 16 * 1024 * 1024
PARTS = [
    (0x000000, Path('build/bootloader/bootloader.bin'), 'bootloader'),
    (0x008000, Path('build/partition_table/partition-table.bin'), 'partition table'),
    (0x020000, Path('build/prisma_pi_pedal_core1a.bin'), 'app'),
    (0x620000, Path('build/storage.bin'), 'storage'),
]
OUT = Path('build/PRISMA-PI-PEDAL-CORE1A-16MB.bin')

image = bytearray(b'\xff') * FLASH_SIZE
for offset, path, name in PARTS:
    if not path.is_file():
        raise SystemExit(f'missing {name}: {path}')
    data = path.read_bytes()
    end = offset + len(data)
    if end > FLASH_SIZE:
        raise SystemExit(f'{name} exceeds flash: 0x{end:x}')
    image[offset:end] = data
    print(f'{name:16s} 0x{offset:06x}-0x{end-1:06x} {len(data)} bytes')

OUT.write_bytes(image)
if OUT.stat().st_size != FLASH_SIZE:
    raise SystemExit('factory size mismatch')
print(f'factory          {OUT} {OUT.stat().st_size} bytes')
