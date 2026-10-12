# CORE-4C Local Video Processing Smoke Test

Purpose: validate frame-by-frame local video processing on ESP32-S3 without freezing the final product video container.

Test input only:
- active VIDEO A/B payload contains concatenated baseline JPEG frames
- firmware loads VIDEO from flash into PSRAM and indexes JPEG SOI/EOI frame boundaries
- each requested frame is decoded by ESP32-S3, posterized in RGB565, and returned as BMP
- browser only schedules/displays frames; all pixel processing is local to the appliance

This concatenated-JPEG payload is a smoke-test transport, not the final product video format.
