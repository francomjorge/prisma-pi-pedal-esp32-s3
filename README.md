# PRISMA-PI PEDAL — ESP32-S3 CORE

Reusable infrastructure/factory baseline for the PRISMA-PI single-effect pedal family.

## Baseline policy

`main` starts from a hardware-validated golden firmware. The golden source is pinned by commit and rebuilt without functional changes before any infrastructure refactor is accepted.

Current golden source:

- Repository: `francomjorge/prisma-pi-lite-waveshare-esp32-s3-touch-lcd-1-54`
- Commit: `0fb5c8dc39c562c27afdd2ba3a2bc863506899ea`
- ESP-IDF: `v5.5.1`
- Target: `esp32s3`
- Flash: 16 MB

The previous experimental CORE-1 implementation that failed to boot on hardware is preserved in branch `archive/core1-failed` and must not be used as a factory baseline.

## Development sequence

1. BASELINE-0: rebuild the exact known-good firmware from the pinned source commit and validate it on hardware.
2. CORE-1A: change only the partition/storage layout.
3. CORE-1B: IMAGE A/B transactional storage.
4. CORE-1C: VIDEO A/B transactional storage.
5. CORE-2: Wi-Fi STA + provisioning AP + discovery.
6. CORE-3: authenticated Admin/API + media upload/activation + streaming.
7. CORE-4: freeze reusable factory infrastructure.
8. Product repository (`Prisma-Pi-poster`): add the single POSTER effect.
