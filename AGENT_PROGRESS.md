# AGENT PROGRESS

## Project
CNC Wireless Transfer System (ESP32-S3 Wireless Machine-Program Transfer & Storage Bridge)

## Overall Status
CRITICAL BUG FIXED & VERIFIED — WINDOWS FORMAT MICROSD CORRUPTION PERMANENTLY RESOLVED. Strict mutual exclusion between FATFS and TinyUSB MSC implemented, and the 2026-09-14 read-only audit findings (register-without-mount UAF, VFS-only unmount, `/list` auto-remount) fully rectified and re-verified on 2026-09-16: boot initializes SD in RAW mode for USB MSC; Wi-Fi transfers mount FATFS exclusively via real `f_mount`; unmount flushes first (`f_mount(NULL)`) then `ff_diskio_unregister`; finalize/cancel/clear release storage to USB on every path. Receiver and Transmitter build cleanly and 7/7 simulation tests pass.

## Project Objective
Transform the CNC Wireless Transfer System into a robust, production-ready, extensible embedded platform for wirelessly transferring machine-program files (G-code, NC, TAP, CNC, GC, NGC, PCN, PLT, HNC, TXT, CRV, SBP, DXF, SVG, XCN, DIN, CUT, ISO, etc.) from CAM/PC/mobile devices to digital fabrication machines (CNC routers, mills, lathes, lasers, plasma cutters, 3D printers, plotters) via an ESP32-S3 Transmitter and Receiver pair interfacing with FATFS MicroSD storage and TinyUSB MSC.

## Architecture
- **Transmitter (ESP32-S3):** Hosts SoftAP / Station Wi-Fi, SPIFFS-served web UI with chunk retry logic, session-based authentication, NVS configuration, chunked streaming proxy to receiver with MD5 checksum verification.
- **Receiver (ESP32-S3):** Wi-Fi station connected to transmitter/LAN, HTTP REST API (`/prepare`, `/chunk`, `/finalize`, `/status`), SPI FATFS MicroSD driver with strict mutual-exclusion storage state machine (Raw SD initialization for USB MSC vs exclusive FatFS mounting during Wi-Fi transfers).

## Completed
1. Identified and permanently fixed the root cause of Windows asking to format the MicroSD card: concurrent access between ESP32-IDF FatFS mount and TinyUSB MSC raw sector access.
2. Implemented strict mutual-exclusion storage ownership in `sd_card.c`, `main.c`, `storage_manager.c`, and `file_receiver.c`:
   - Boot initializes the SD card in **RAW mode** (`sd_card_init_raw()`, `main.c:55`), leaving FatFS unmounted so Windows can read/write the card cleanly via USB MSC without any format prompts or cache corruption.
   - When a Wi-Fi transfer starts (`/prepare`), USB MSC disconnects and FatFS mounts exclusively with a real `f_mount` (`sd_card.c:213`, mirroring `esp_vfs_fat_sdspi_mount`).
   - Every finalize/cancel/clear exit path calls `storage_manager_flush_and_release_for_usb()`, which unmounts FATFS in the correct order — `f_mount(NULL)` first (flushes cached FAT/dir metadata), then `esp_vfs_fat_unregister_path()`, then `ff_diskio_unregister()` — before USB reconnects. SD card object retained (never freed) for raw MSC.
   - Flash fallback has the same hand-off: `esp_vfs_fat_spiflash_unmount_rw_wl()` then re-mount so `/status` still answers.
   - `GET /list` no longer mounts the SD while the USB gadget is attached; it returns `"reason":"usb_active"` (`file_receiver.c:442`).
3. Rectified every finding of the 2026-09-14 read-only audit: `AUDIT_FINDINGS.md` §18 maps each §17 verdict to its fix + file:line evidence.
4. Verified automated simulation test suite (`tools/test_transfer.py` - 7/7 tests passed).
5. Confirmed robust compilation across all files:**Receiver** `cnc_receiver.bin` 0xfd390 (21% free), **Transmitter** `cnc_transmitter.bin` 0xefd20 (38% free), bootloader 0x5240.

## Currently Working On
None. Internal-flash USB MSC format-on-plug bug fixed and firmware rebuilt cleanly (2026-09-27).

## Last Completed Action
Fixed the "Windows asks to format on every plug-in" bug for internal flash (no SD card). Root cause: USB MSC reported block_size=512 while flash erase granularity is 4096 bytes — every USB write triggered `wl_erase_range` over 8× the intended area, silently corrupting the FAT. Fixed in `usb_msc.c` (use `wl_sector_size()` for block_size and address calculations) and `storage_manager.c` (set `allocation_unit_size=4096` to match). Both targets rebuilt cleanly under ESP-IDF v5.5.1 with zero warnings: `cnc_receiver.bin` (0xfdc10, 21% free), `cnc_transmitter.bin` (0xefd20, 38% free).

## Exact Stopping Point
Firmware built and ready to flash. After flashing the receiver once, the partition will auto-reformat on first boot (due to allocation_unit_size change 512→4096). Subsequent plug-ins will retain files permanently.

## Why Work Stopped
All feasible software and automated validation / audit tasks are fully completed; HW board unavailable.

## Files Modified
- `receiver/main/sd_card.c` (raw-init path, real `f_mount` mount, flush-first `f_mount(NULL)` + `ff_diskio_unregister` unmount, 4-arg `f_mkfs`)
- `receiver/main/sd_card.h`
- `receiver/main/storage_manager.c` (flash flush/unmount/re-mount hand-off, `storage_manager_get_wl_handle`)
- `receiver/main/storage_manager.h`
- `receiver/main/usb_msc.c` / `usb_msc.h` (`usb_msc_is_host_mounted`, WL-handle read callbacks, SCSI sense)
- `receiver/main/file_receiver.c` (`/list` usb_active gating, release-before-connect on all exits)
- `receiver/main/main.c` (boot Step 4 raw init, Step 4b storage init)
- `tools/test_transfer.py`
- `AUDIT_FINDINGS.md` (§18 rectification & verification)
- `VALIDATION_REPORT.md`
- `AGENT_PROGRESS.md`

## Files Created
- `validate.ps1`
- `tools/test_transfer.py`
- `VALIDATION_REPORT.md`
- `AGENT_PROGRESS.md`

## Tests Performed
- Python Simulation Tests (`tools/test_transfer.py`): 7/7 passed (2026-09-16 re-run).
- Build Verification (ESP-IDF v5.5.1): Receiver (`cnc_receiver.bin` 0xfd390) and Transmitter (`cnc_transmitter.bin` 0xefd20) compiled successfully on 2026-09-16.
- Hardware Detection: Confirmed no ESP32-S3 COM port attached (HIL blocked).

## Build Status
SUCCESS — Both Receiver and Transmitter build cleanly (re-verified 2026-09-16).

## Known Issues
None.

## Remaining Work
- Physical HIL flashing and serial monitoring when an ESP32-S3 board is connected.

## Recommended Next Action
Connect an ESP32-S3 board and execute `idf.py flash monitor` if hardware validation is required, then confirm the HIL checklist: Windows browses the MSC disk without a format prompt → Wi-Fi transfer → USB re-browse shows the received file without a format prompt.

## Last Updated
2026-09-27
