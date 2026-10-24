# ESP32-S3 CC3200 Transplant Bring-up (Experimental)

Date: 2026-10-24
Branch: esp32-s3-cc3200-transplant

## Changelog

- Added transplant board pin mapping in esp32/main/board.c:
  - Peripheral power GPIO15 (HIGH=on)
  - Peripheral reset GPIO16 (HIGH=released)
  - SD/NFC power GPIO21 (LOW=on)
  - Ear buttons GPIO7/8
  - NFC SPI GPIO2/38/39 with CS GPIO1 and IRQ GPIO13
- Switched SD host wiring to 1-bit SDMMC in esp32/main/sd.c:
  - CLK GPIO41, CMD GPIO42, DAT0 GPIO47
- Updated LED mapping in esp32/main/led.c to GPIO17/18/40.
- Added standalone 16 MB partition table in esp32/partitions.csv.
- Updated ESP32 image slot assumptions for larger app partitions:
  - esp32/main/slots.c SLOT_SIZE set to 0x360000
  - tools/esp32img.py SLOT set to 0x360000
  - tools/esp32img.py flash-size check set to 16 MB code
- Enabled PSRAM defaults in esp32/sdkconfig.defaults and logged runtime PSRAM status in esp32/main/main.c.
- Added fresh-NVS recovery in esp32/main/nvsfile.c:
  - Erase+reinit when no free pages or NVS version mismatch.
- Added serial remote control path in esp32/main/main.c to run without NFC:
  - Commands: status, play <uid16>, pause, continue, stop, next, prev, vol <1..15>, setup, help
- Made NFC reader startup non-fatal in src/tag.c:
  - Reader failures now back off/retry and do not trigger rollback checks.
- Added certificate storage fallback for standalone boards in esp32/main/flashfs.c:
  - /cert/ca.der, /cert/client.der, /cert/private.der stored in NVS keys when factory assets are absent.

## Build Status

- ESP-IDF v5.4.2 installed locally at C:/esp/v5.4.2/esp-idf.
- Firmware builds successfully with target esp32s3.
- Produced artifacts:
  - esp32/build/toniebox-cfw-esp32.bin
  - esp32/build/bootloader/bootloader.bin
  - esp32/build/partition_table/partition-table.bin
- Image validation passes:
  - python ../tools/esp32img.py check build/toniebox-cfw-esp32.bin

## Runtime/Hardware Status

Working in software build flow:

- Compile and image validation on IDF 5.4.2.
- Standalone 16 MB partition layout generated.
- Tagless control path available over serial command input.

Not yet validated on target hardware in this session:

- Flash/write on COM10.
- Boot logs from target device.
- I2C codec init (TLV320DAC3100 @ 0x18) and accelerometer (MMA8451Q @ 0x1D) on this exact unit.
- SD read/write behavior on the live card with existing content.
- TeddyCloud end-to-end playback and remote network control handshake.

Blocker observed:

- COM10 not currently available to tooling in-session (port missing or in use), so chip detect and flash were not completed.

## TeddyCloud and Certificate Provisioning Notes

For TLS cloud connectivity, the firmware expects:

- /cert/ca.der
- /cert/client.der
- /cert/private.der

On this transplant firmware, these can be provisioned via the setup page certificate upload endpoints. If assets partition content from original ESP firmware is absent, certificate data falls back to NVS-backed storage keys and is still usable by TLS code.

## Safety Constraints Maintained

- No secure boot or flash-encryption actions were added.
- No eFuse programming actions were added.
- No writes to non-target devices were performed.
