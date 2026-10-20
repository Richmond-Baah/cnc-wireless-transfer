# CNC Wireless Transfer (commercial product)
ESP32-S3 boards: 1 transmitter, up to 16 receivers. Transmitter: SoftAP "CNC_WIRELESS" 192.168.4.1, web UI + auth, forwards uploads. Receiver: Wi-Fi client (DHCP), USB mass-storage drive for CNC/laser/printer, SD over SPI (fallback: internal flash).
- Receiver ID "rx-"+last 3 MAC bytes; heartbeat POST <gateway>/rx/heartbeat every 10 s (receiver/main/pairing.c).
- Transmitter rx_registry: paired list (id, name, key) in NVS, pending list in RAM, online = heartbeat < 30 s.
- UI selects a machine; every receiver proxy takes X-Rx-Id (or ?rx=) and sends that receiver's X-Auth-Key.
- AP creds: change bumps config_version; receivers pull GET /rx/config; transmitter restarts after they fetch (max 30 s).

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
- Pairing: receiver /pair only while unpaired; once paired every receiver API needs X-Auth-Key; receiver "/" is info only.
- Pairing reset only by holding BOOT 10 s (or transmitter /unpair with the key); never automatic.
- One upload session at a time across all receivers (409 for another receiver).
- Status/cards come from the heartbeat cache only; request handlers never call receiver /status.
- Recovery: receiver alternates saved/factory AP creds every 10 failed connects; transmitter /recovery = factory AP for 5 min, then reverts.

## Open items
- Unpaired receiver API is open; key travels in plain HTTP (WPA2 AP only). /debug shows AP password.
- Compile out /debug for production.
- ".." rejected anywhere in name (too strict; only whole-name matters).
- Busy-guard threshold untuned (polling machines trigger it).
- Commercial: own USB VID/PID (not SanDisk), per-device credentials, signed OTA, certified module.
- Transmitter chip has 4 MB flash + 2 MB PSRAM; sdkconfig set to 2 MB flash. Fix before OTA.

## Working rules
- Minimal changes. Cite file:line. Build both boards, commit. Short replies.
- Nothing is hardware-tested unless the user says so.
