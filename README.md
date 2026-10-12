# PRISMA-PI PEDAL — ESP32-S3 CORE

Common factory/core firmware for the PRISMA-PI single-effect pedal family.

Hardware target: Waveshare ESP32-S3-Touch-LCD-1.54 (SKU 33869), ESP32-S3R8, 16 MB flash, 8 MB PSRAM, ST7789 240x240 display, CST816S touch.

## Scope

This repository is the reusable infrastructure layer. Product/effect logic (POSTER first) lives in a separate repository and must not be coupled into the core.

Order of work:

1. Factory/core infrastructure.
2. Safe media storage and validation.
3. Wi-Fi provisioning, discovery, Admin/API and streaming services.
4. Freeze a reusable factory baseline.
5. Build the POSTER pedal as source -> POSTER -> output.

## Fixed decisions

- ESP-IDF 5.5.1.
- No OTA partitions and no OTA update path.
- Full 16 MB factory image flashed at `0x000000`.
- Application starts at `0x020000`.
- GPIO2 is the battery/power hold line.
- Media is stored transactionally in A/B raw flash slots:
  - IMAGE_A / IMAGE_B: 1 MB each.
  - VIDEO_A / VIDEO_B: 3 MB each.
- Slot activation is metadata-driven and requires header + length + CRC32 validation before switching active media.

## Infrastructure milestones

### CORE-1 — hardware + factory + flash map

- boot/power hold
- ST7789 display
- CST816S touch + swipe primitive
- 16 MB non-OTA partition table
- A/B media slot discovery/validation
- deterministic factory image generation

### CORE-2 — network services

- Wi-Fi STA
- provisioning AP + scan + DHCP
- persistent credentials/config
- mDNS + UDP discovery
- stable Device ID

### CORE-3 — management plane

- authenticated Admin
- common REST API
- media upload into inactive A/B slot
- CRC/validation/atomic activation/recovery
- image/video streaming endpoints

### CORE-4 — factory freeze

- integration test on hardware
- power-cycle/recovery tests
- produce final 16 MB factory binary
- tag reusable core baseline

POSTER is implemented only after CORE-4 is stable.
