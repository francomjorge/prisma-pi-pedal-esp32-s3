#!/usr/bin/env python3
from pathlib import Path

FLASH_SIZE = 16 * 1024 * 1024
APP_OFFSET = 0x020000
APP_LIMIT = 0x320000

ERASED_RANGES = [
    (0x320000, 0x300000, 'video_out'),
    (0x620000, 0x100000, 'image_a'),
    (0x720000, 0x100000, 'image_b'),
    (0x820000, 0x300000, 'video_a'),
    (0xB20000, 0x300000, 'video_b'),
    (0xE20000, 0x100000, 'image_out'),
    (0xF20000, 0x020000, 'preview_img'),
    (0xF40000, 0x0C0000, 'preview_vid'),
]

PARTS = [
    (0x000000, Path('build/bootloader/bootloader.bin'), 'bootloader'),
    (0x008000, Path('build/partition_table/partition-table.bin'), 'partition table'),
    (APP_OFFSET, Path('build/prisma_pi_pedal_core5a.bin'), 'app'),
]

OUT_DIR = Path('dist')
OUT = OUT_DIR / 'PRISMA-PI-CORE5A-FACTORY-16MB.bin'

image = bytearray(b'\xff') * FLASH_SIZE
for offset, path, name in PARTS:
    if not path.is_file():
        raise SystemExit(f'missing {name}: {path}')
    data = path.read_bytes()
    end = offset + len(data)
    if end > FLASH_SIZE:
        raise SystemExit(f'{name} exceeds flash: 0x{end:x}')
    if name == 'app' and end > APP_LIMIT:
        raise SystemExit(f'app exceeds 3 MB factory partition: end=0x{end:x}')
    image[offset:end] = data

for offset, size, name in ERASED_RANGES:
    end = offset + size
    if end > FLASH_SIZE:
        raise SystemExit(f'{name} exceeds flash')
    if any(b != 0xFF for b in image[offset:end]):
        raise SystemExit(f'{name} is not erased in factory image')

OUT_DIR.mkdir(parents=True, exist_ok=True)
OUT.write_bytes(image)
if OUT.stat().st_size != FLASH_SIZE:
    raise SystemExit('factory size mismatch')
print(f'factory {OUT} {OUT.stat().st_size} bytes')
