#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path

FLASH_SIZE = 16 * 1024 * 1024
OFFSETS = {
    "bootloader": 0x000000,
    "partition_table": 0x008000,
    "app": 0x020000,
}
APP_LIMIT = 0x600000


def overlay(image: bytearray, offset: int, payload: bytes, name: str) -> None:
    end = offset + len(payload)
    if end > len(image):
        raise SystemExit(f"{name}: end 0x{end:X} exceeds 16 MB flash")
    image[offset:end] = payload
    print(f"{name:16s} 0x{offset:06X}..0x{end - 1:06X}  {len(payload)} bytes")


def main() -> None:
    parser = argparse.ArgumentParser(description="Build deterministic 16 MB PRISMA-PI PEDAL factory image")
    parser.add_argument("--build", default="build", help="ESP-IDF build directory")
    parser.add_argument("--out", default=None, help="Output factory .bin")
    args = parser.parse_args()

    build = Path(args.build)
    bootloader = build / "bootloader" / "bootloader.bin"
    partition_table = build / "partition_table" / "partition-table.bin"
    app = build / "prisma_pi_pedal_core.bin"
    out = Path(args.out) if args.out else build / "PRISMA-PI-PEDAL-factory-16MB.bin"

    for path in (bootloader, partition_table, app):
        if not path.is_file():
            raise SystemExit(f"missing build artifact: {path}")

    if app.stat().st_size > APP_LIMIT:
        raise SystemExit(f"app is {app.stat().st_size} bytes; limit is {APP_LIMIT} bytes")

    image = bytearray(b"\xFF") * FLASH_SIZE
    overlay(image, OFFSETS["bootloader"], bootloader.read_bytes(), "bootloader")
    overlay(image, OFFSETS["partition_table"], partition_table.read_bytes(), "partition_table")
    overlay(image, OFFSETS["app"], app.read_bytes(), "app")

    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(image)
    print(f"factory          {out}  {out.stat().st_size} bytes")
    if out.stat().st_size != FLASH_SIZE:
        raise SystemExit("factory image size mismatch")


if __name__ == "__main__":
    main()
