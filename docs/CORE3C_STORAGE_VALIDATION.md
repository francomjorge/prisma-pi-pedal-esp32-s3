# CORE-3C Storage Validation

Goal: validate IMAGE/VIDEO persistence and recovery before any streaming or processing work.

Rules:
- Active media bank is immutable during replacement.
- Replacement writes the inactive bank only.
- The inactive bank is staging/rollback storage; it is not a frame-by-frame processing buffer.
- Runtime processing should use RAM/PSRAM chunks and read the active flash bank.
- A new bank is committed only after a full flash readback CRC matches the streamed write CRC.
- On boot, metadata candidates are evaluated newest-first. A valid metadata record whose payload CRC fails is rejected; the older committed record may be used as rollback.
- Logical DELETE remains authoritative and persists across reboot.

Validation sequence:
1. Upload IMAGE #1 -> A, VERIFY, reboot, VERIFY.
2. Upload IMAGE #2 -> B, VERIFY, reboot, VERIFY.
3. Upload IMAGE #3 -> A, VERIFY.
4. Interrupt an IMAGE upload and confirm prior active bank remains selected and verifies.
5. DELETE IMAGE, reboot, confirm empty.
6. Repeat equivalent sequence for VIDEO.
7. Confirm legacy SPIFFS files remain readable and unchanged.
