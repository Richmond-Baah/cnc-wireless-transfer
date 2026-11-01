# FileXfer (FILEXFER; formerly CNC Wireless Transfer, commercial product)
ESP32-S3 boards: 1 transmitter, up to 16 receivers. Transmitter: SoftAP "FILEXFER-<last 4 MAC hex>" 192.168.4.1 (factory/recovery SSID "CNC_WIRELESS"), http://filexfer.local (alias cnc-transfer.local), web UI + auth, forwards uploads. Receiver: Wi-Fi client (DHCP), USB mass-storage drive for CNC/laser/printer, SD over SPI (only storage; no internal-flash fallback).
- Receiver ID "rx-"+last 3 MAC bytes; heartbeat POST <gateway>/rx/heartbeat every 10 s (receiver/main/pairing.c).
- Transmitter rx_registry: paired list (id, name, key) in NVS, pending list in RAM, online = heartbeat < 30 s.
- UI selects a machine; every receiver proxy takes X-Rx-Id (or ?rx=) and sends that receiver's X-Auth-Key.
- Add machine (ESP-NOW, components/pair_proto): UI opens 120 s window; unpaired off-Wi-Fi receiver bursts PAIR_REQ ch 1-13 every ~3 s; approve -> PAIR_CFG (X25519+AES-GCM: AP creds, key, tx id, name); key is pre-registered on tx and provisional on rx until a heartbeat confirms (3 rejects -> rx drops it).
- Pairing file: UI "Download pairing file" -> pair.cfg {v,ssid,pass,key,tx_id,name,expiry}; key pre-registered 24 h (NVS, clock from browser); receiver reads+deletes /pair.cfg at boot before USB, pairs provisionally; expiry enforced by transmitter only (receiver has no clock).
- AP creds: change bumps config_version; receivers pull GET /rx/config; transmitter restarts after they fetch (max 30 s).
- Web UI (transmitter/web, one page: login + app views): all text in strings.js (t(key)), theme tokens in style.css (light/dark/system, saved in localStorage), no CDN/web fonts, messages inline (no alert/confirm/prompt); tools/bundle_ui.py inlines it into one gzipped HTML embedded in the app, served at /ui/<crc> (cached 1 year); API responses no-store.

## Build / flash
- Both boards 4 MB flash. partitions.csv: nvs 0x9000 (tx 0x5000, rx 0x6000, offsets never move), otadata (tx 0xE000, rx 0x3D0000), ota_0 0x10000 + ota_1 0x1F0000 (0x1E0000 each), coredump 64 KB (tx 0x3D0000, rx 0x3E0000). Layout changes need a USB flash.
- PROJECT_VER = `git describe --tags` without "v" (fallback 0.0.0-dev), refreshed on commit/checkout.
- Transmitter OTA (Settings > System > Update firmware): browser reads the app desc (project/version), POST /fw/begin {size} -> /fw/chunk (X-Offset, <=32 KB, sequential, dup acked) -> /fw/finish (esp_ota_end, set boot, restart) | /fw/cancel; upload worker. Server checks ESP32-S3, project cnc_transmitter, version newer (fw_version_cmp: x.y.z then commits after tag). Not during a file transfer (and uploads refused during OTA). Rollback on: main.c marks valid only if wifi_init + web_server_start return ESP_OK, else rolls back; crash before that -> bootloader reverts.
- Receiver OTA, streamed (nothing stored on tx): UI picks a cnc_receiver .bin, "Update <machine>" -> tx /rxfw/begin|chunk|finish|cancel?rx= relays each request to rx /fw/* (key, paired only, 16 KB chunks, rx worker). rx refuses /fw/begin while busy (host read <10 s, no force) or RECEIVING; /prepare refused during an update. One update or transfer at a time across tx + all receivers. rx restarts 1 s after /fw/finish; marks valid on the first accepted heartbeat, rolls back if none within 300 s (fw_update_rollback_timer). Shared code: components/fw_update.
- Signed apps, no eFuses: CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT + RSA-3072 (V2) + SIGNED_ON_UPDATE; no secure boot / flash encryption. esp_ota_end checks the signature against the key in the RUNNING app's signature block (so the USB-flashed app must be signed with the same key); ESP32-S3 bootloader cannot verify at boot. CMakeLists signs build/<proj>.bin in place with $FILEXFER_SIGNING_KEY (outside repo; *.pem/*.key ignored); without it: CMake warning, unsigned build (IDF's "App built but not signed" echo prints either way). Unsigned rejected before upload (UI: last 4 KB must start E7 02) and on device (size % 4096, sig sector magic before that chunk is written); wrong key -> esp_ota_end fails, slot never activated. Key: `espsecure.py generate_signing_key --version 2 --scheme rsa3072 <path outside repo>`; losing it = USB reflash of every unit.
- Core dump to flash (ELF). GET /coredump (tx, authed), GET /receiver_coredump?rx= (tx proxies rx /coredump, key, paired only); UI Settings > System > Crash report. Decode: `idf.py coredump-info -c <file>`.
- ESP-IDF 5.5.1. `idf.py build` in receiver/ and transmitter/ (or build_all.ps1).
- Receiver flash: hold BOOT, tap RST, then `idf.py -p COMx flash`; press RST after. Logs on UART0 (GPIO43/44), not USB.
- Transmitter: `idf.py -p COMx flash monitor` (SPIFFS UI flashed automatically).
- "ninja: Access is denied" = OneDrive file locking; retry.

## Invariants (do not break)
- While USB owns storage, receiver FS stays unmounted. Transfer: USB off → mount → write .tmp → MD5 → rename → unmount → USB on.
- SD card is the only storage. No card at boot → CARD_ERROR (storage_type "None", UI "No SD card") until restarted with one; SD failure → 503/CARD_ERROR.
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
- BOOT (GPIO0 pinhole, components/boot_button): debounced, fires once after a 10 s hold while running; a press held at task start is ignored; shorter holds do nothing. LED: magenta fast blink while held, solid magenta 2 s on fire. Receiver: clear pairing. Transmitter: admin login back to factory default only (network + machines unchanged), logged.
- Factory reset (UI Settings > System, POST /factory_reset {"confirm":"RESET","remove_machines"}): AP-only, AP FILEXFER-XXXX + factory password, no router, factory admin login; keeps machines (they pull the new AP creds before the restart) unless remove_machines unpairs all. The only path that changes the AP SSID of a set-up unit.
- One upload session at a time across all receivers (409 for another receiver).
- Status/cards come from the heartbeat cache only; request handlers never call receiver /status.
- Recovery: receiver alternates saved/factory AP creds every 10 failed connects; transmitter /recovery = factory AP for 5 min, then reverts.

## Open items
- Unpaired receiver API is open; key travels in plain HTTP (WPA2 AP only). /debug shows AP password.
- Compile out /debug for production.
- ".." rejected anywhere in name (too strict; only whole-name matters).
- Busy-guard threshold untuned (polling machines trigger it).
- Commercial: own USB VID/PID (not SanDisk), per-device credentials, certified module. OTA images are signed but bootloader/app can still be replaced over USB (no secure boot).

## Working rules
- Minimal changes. Cite file:line. Build both boards, commit. Short replies.
- Nothing is hardware-tested unless the user says so.
- Updates must never change AP SSID on units already set up.
