# CNC Wireless Transfer (commercial product)
Two ESP32-S3 boards. Transmitter: SoftAP "CNC_WIRELESS" 192.168.4.1, web UI + auth, forwards uploads. Receiver: Wi-Fi client 192.168.4.150, USB mass-storage drive for CNC/laser/printer, SD over SPI (fallback: internal flash).

## Build / flash
- ESP-IDF 5.5.1. `idf.py build` in receiver/ and transmitter/ (or build_all.ps1).
- Receiver flash: hold BOOT, tap RST, then `idf.py -p COMx flash`; press RST after. Logs on UART0 (GPIO43/44), not USB.
- Transmitter: `idf.py -p COMx flash monitor` (SPIFFS UI flashed automatically).
- "ninja: Access is denied" = OneDrive file locking; retry.

## Invariants (do not break)
- While USB owns storage, receiver FS stays unmounted. Transfer: USB off → mount → write .tmp → MD5 → rename → unmount → USB on.
- Never auto-format. Mount failure → CARD_ERROR. Format/Wipe need typed FORMAT/WIPE, checked server-side.
- Every error path: delete .tmp before unmount, release storage, reconnect USB.
- Chunks: indexed; dup = ack+discard, out-of-order = 409.
- Busy guard: /prepare, /clear and /delete → 423 if host read <10 s ago, unless X-Force: 1.
- Space: need size + max(64 KB, 1% of drive); UI blocks Start, /prepare re-checks after mount (same-name file still counts as used) → 507 before writing, release, USB on.
- /delete {"files":[...]}: one mount cycle, root files only (upload name rules), missing = skipped+reported, then snapshot + USB on.
- Filenames: reject / \ : control chars, empty/dots-only.
- Upload is browser-driven chunking: /upload_begin, /upload_chunk, /upload_finish, /upload_cancel.

## Open items
- Receiver API + its / page have no auth.
- Compile out /debug for production.
- ".." rejected anywhere in name (too strict; only whole-name matters).
- Busy-guard threshold untuned (polling machines trigger it).
- Commercial: own USB VID/PID (not SanDisk), per-device credentials, signed OTA, certified module.
- Transmitter chip has 4 MB flash + 2 MB PSRAM; sdkconfig set to 2 MB flash. Fix before OTA.

## Working rules
- Minimal changes. Cite file:line. Build both boards, commit. Short replies.
- Nothing is hardware-tested unless the user says so.
