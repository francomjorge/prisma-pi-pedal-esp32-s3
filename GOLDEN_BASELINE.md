# GOLDEN BASELINE 0

This file locks the only source accepted for the first pedal-core baseline.

```text
repository = francomjorge/prisma-pi-lite-waveshare-esp32-s3-touch-lcd-1-54
commit     = 0fb5c8dc39c562c27afdd2ba3a2bc863506899ea
esp-idf    = v5.5.1
target     = esp32s3
flash      = 16MB
```

Hardware validation: this source lineage was restored as a merged 16 MB recovery image and successfully booted the physical Waveshare ESP32-S3-Touch-LCD-1.54 appliance after the experimental CORE-1 failed.

No CORE refactor may alter boot/power/display/touch simultaneously. Changes are introduced and hardware-tested one subsystem at a time.
