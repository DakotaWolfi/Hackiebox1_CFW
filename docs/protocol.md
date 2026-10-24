# Server protocol of the custom firmware

The custom firmware (CFW, CC3200 and ESP32 box) makes the same requests as the original firmware. A server that knows nothing of it serves it like an original box. This page lists only what the CFW adds; the byte layouts are in the server code (`cfw_format.c`, with C tests) and its box simulator (`tests/sim/cfw.py`).

| What | Way | Content |
| --- | --- | --- |
| Detection | header `X-CFW-Build: <build>` on every request | the CFW's build number (seconds since 1970, unsigned 32-bit). User-Agent and `cv` stay those of the box's original firmware, so the vendor's cloud sees an original box. Without the header the request comes from the original firmware, which must never get a CFW image. |
| Hello | field 101 (varint) in the freshness-check answer, or log-host command 63 | "this server supports the CFW"; the original firmware skips the unknown field |
| Hello answer | log record group 22, function 0x7E01 | build, capability bits (which features the box has), result of the last update, rolled-back build |
| Settings files | field 100 in the freshness-check answer | 8 UID bytes + one small file (`TBS1`, up to 256 bytes): profile of a tag, event tag, or box settings (UID 0). The whole answer stays within 1024 bytes. |
| Changes on the box | log record 0x7E02 | the box changed a profile or the box settings itself (by an event tag) |
| Request a check | log-host command 6 | when files are due, so they arrive without waiting for the next check |
| Remote control | commands 64 to 73 on the log connection, frames up to 320 bytes | pause, continue, stop, chapter, volume, play a UID, status, update check, apply settings; only to boxes that report the capability (the original firmware closes the connection on frames over 15 bytes) |
| Command result, status | log records 0x7E04, 0x7E05 | |
| Battery report | log record 0x7E06 | voltage, charging station, load |
| Firmware update | item 3 of the usual update check (`/v1/ota/3`) | CC3200: slot image of the SD bootloader with the marker `CFW1` in its build block; ESP32: ESP-IDF app image with the project name `toniebox-cfw-esp32`, followed by its SHA-256 as hex text. The box takes only its own firmware and decides nothing else: which build it gets is the server's choice. After a trial start the next hello reports whether the image was kept or rolled back. |
| ESP32 first installation | item 3 to a box with the original ESP firmware | the box installs any app image it is offered, so the server delivers the CFW once; the server may fill in its host and port in the image's 80-byte block `CFWSRV01` |
| ESP32 back to the original | item 3 with an image of project `toniebox-esp32-eu` | the CFW installs it and restarts into it |
| Setup page | `GET /v1/ota/7?cv=<page version>`, only after a hello | the setup page on the box's card (Wi-Fi, server, certificates); 304 when unchanged |

The ESP32 CFW asks for items 3 and 7 only after a hello, so a server without CFW support never sees them.

A release is one uncompressed zip with both images and the setup page (`cc3200/ng-cfw.bin`, `esp32/toniebox-cfw-esp32.bin`, `setup.htm`); a server can take it as it is.
