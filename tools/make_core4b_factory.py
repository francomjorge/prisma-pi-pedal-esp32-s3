#!/usr/bin/env python3
from pathlib import Path

FLASH_SIZE = 16 * 1024 * 1024
IMAGE_A_OFFSET = 0x620000
IMAGE_B_OFFSET = 0x720000
IMAGE_BANK_SIZE = 0x100000
VIDEO_A_OFFSET = 0x820000
VIDEO_B_OFFSET = 0xB20000
VIDEO_BANK_SIZE = 0x300000
STORAGE_OFFSET = 0xE20000
STORAGE_SIZE = 0x1E0000

PARTS = [
    (0x000000, Path('build/bootloader/bootloader.bin'), 'bootloader'),
    (0x008000, Path('build/partition_table/partition-table.bin'), 'partition table'),
    (0x020000, Path('build/prisma_pi_pedal_core4b.bin'), 'app'),
    (STORAGE_OFFSET, Path('build/storage.bin'), 'storage'),
]

OUT_DIR = Path('dist')
OUT = OUT_DIR / 'PRISMA-PI-PEDAL-CORE4B-16MB.bin'

image = bytearray(b'\xff') * FLASH_SIZE
for offset, path, name in PARTS:
    if not path.is_file():
        raise SystemExit(f'missing {name}: {path}')
    data = path.read_bytes()
    if name == 'storage' and len(data) != STORAGE_SIZE:
        raise SystemExit(f'storage size mismatch: {len(data)} != {STORAGE_SIZE}')
    end = offset + len(data)
    if end > FLASH_SIZE:
        raise SystemExit(f'{name} exceeds flash: 0x{end:x}')
    if name == 'app' and end > IMAGE_A_OFFSET:
        raise SystemExit(f'app overlaps IMAGE A: app end 0x{end:x}')
    image[offset:end] = data
    print(f'{name:16s} 0x{offset:06x}-0x{end-1:06x} {len(data)} bytes')

for offset, size, name in (
    (IMAGE_A_OFFSET, IMAGE_BANK_SIZE, 'image_a'),
    (IMAGE_B_OFFSET, IMAGE_BANK_SIZE, 'image_b'),
    (VIDEO_A_OFFSET, VIDEO_BANK_SIZE, 'video_a'),
    (VIDEO_B_OFFSET, VIDEO_BANK_SIZE, 'video_b'),
):
    end = offset + size
    if any(b != 0xFF for b in image[offset:end]):
        raise SystemExit(f'{name} is not erased in factory image')

OUT_DIR.mkdir(parents=True, exist_ok=True)
OUT.write_bytes(image)
if OUT.stat().st_size != FLASH_SIZE:
    raise SystemExit('factory size mismatch')
print(f'factory {OUT} {OUT.stat().st_size} bytes')
