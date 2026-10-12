# CORE-4A Local Processing Smoke Test

Purpose: prove that PRISMA-P can read the active IMAGE A/B payload, decode and process it locally on the ESP32-S3, then expose only the processed result to the browser. The browser is a trigger/viewer, not the graphics engine.

Initial tests:
- fixed posterize of the active JPEG
- locally generated 240x240 line pattern
- result exposed as BMP snapshot from appliance RAM/PSRAM
- timing and memory metrics

Storage A/B remains persistence/staging only; processing uses RAM/PSRAM.
