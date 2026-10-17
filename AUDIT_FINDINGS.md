# Audit Findings — "Windows asks to format MicroSD" fix verification (AUDIT ONLY)

Date: 2026-09-16 · Mode: READ-ONLY source audit (no code changed, no flashing, no SD write/format)
Repo: `cnc-wireless-transfer` (single commit `7c001b5` + uncommitted fix in working tree)
Reference: `C:\Espressif\frameworks\esp-idf-v5.5.1` (ESP-IDF v5.5.1)

---

## 13. Claim vs. source (AGENT_PROGRESS.md)

| Claim                                        | Verdict | Evidence |
|----------------------------------------------|---------|----------|
| "Boot initializes SD in RAW mode"            | **FALSE** | `main.c:55-57` calls `sd_card_init()` then `sd_card_unmount()`; `sd_card_unmount()` (`sd_card.c:129-142`) only calls `esp_vfs_fat_unregister_path()`. That function (`vfs_fat.c:236-252`) does **not** `f_mount(0,..)`, does **not** flush, does **not** `ff_diskio_unregister`; it only unregisters the VFS, then `free(fat_ctx)`. The card is NOT left in a clean raw state — see §17. `sd_card_init_raw()` is declared (`sd_card.h:11`) but has no definition or caller anywhere. |
| "FatFS unmounted during USB access"          | **FALSE (partially)** | Unmount is a VFS-layer unregister. FatFS's internal mount state (`FatFs[0]`, `ff.c:467`) is left dangling and the SD disk I/O driver stays registered. |
| "Mounted exclusively during Wi-Fi transfers" | **FALSE** | "Remount" (`sd_card.c:52-64`) is `esp_vfs_fat_register("/sdcard","storage",5,&fs)` **without any `f_mount`** and with an invalid drive string. The newly registered filesystem is never mounted → first `fopen("/sdcard/...")` hits the **freed** `FatFs[0]` (use-after-free / UB). |
| "Strict Mutual Exclusion"                    | **FALSE** | Only coordination is "disconnect USB first" (`file_receiver.c:147`) + state machine; there is no flush before hand-off and `GET /list` (`file_receiver.c:421-431`) auto-remounts while the USB gadget is attached. |
| `format_if_mount_failed = false` everywhere  | **TRUE** | `sd_card.c:82`, `storage_manager.c:58,67,96`. Destructive auto-format at boot removed. This is a real improvement (see §16). |
| Hardware validation                          | **BLOCKED** | `VALIDATION_REPORT.md` (2026-09-14): no physical ESP32-S3 board — no flash/USB test evidence exists. |

## 14. Root cause of the original bug (confirmed)

Boot previously ran `esp_vfs_fat_sdspi_mount(...)` with `format_if_mount_failed=true` (old `sd_card.c`) and **never unmounted**: a VFAT filesystem stayed cognitively mounted while the ESP32-S3 exposed the same card as a raw MSC disk to Windows. Windows writes to a card whose FAT was simultaneously owned by FatFS → inconsistent FAT/directory → "The drive is not accessible / needs formatting".

Additional original aggravators found in the diff: destructive `format_if_mount_failed=true` and a multi-speed remount loop that could re-init the card under a live USB session.

## 15. What the fix changed (still uncommitted)

- `main.c`: SD is mounted, then unmounted at boot so USB MSC "owns" the card (`sd_card_init(); sd_card_unmount();`).
- `sd_card.c`: added `s_fatfs_mounted` flag; `sd_card_unmount()` → `esp_vfs_fat_unregister_path()`; `sd_card_remount()` → re-`sd_card_init()` (register-only branch when `current_sd_card != NULL`).
- `storage_manager.c`: mount at `/storage` (internal flash) only when SD not mounted; `flush_sync()` = unmount/remount.
- `usb_msc.c`: added `+offset` on the WL (flash) path; SCSI sense on unknown command. SD path already used `sdmmc_read/write_sectors` (raw, no FatFS).
- `file_receiver.c`: USB disconnect before FatFS use, `.tmp` → MD5 → rename flow, orphan cleanup, chunk `fsync`.
- Transmitter: `/chunk` retries (3), more file extensions, storage-info UI — unrelated to the SD/USB bug.

## 16. Did the fixes actually fix the root cause? — NO

The new handover is a **VFS-layer unregister only**, and the remount **never mounts a filesystem**. Concretely, verified in IDF source:

### A. Unmount does not flush or unmount FatFS (data-integrity bug — the actual cause remains)
`esp_vfs_fat_unregister_path` (`vfs_fat.c:236-252`):
```
esp_vfs_unregister(base_path);
_lock_close(&fat_ctx->lock);
free(fat_ctx->flags);
free(fat_ctx);          // <-- frees &fat_ctx->fs WITHOUT f_mount(0, drv, 0)
s_fat_ctxs[ctx] = NULL;
```
Consequences:
- Any pending FAT / directory-entry metadata still cached in the freed FATFS object is **silently discarded**, not written to the card. In `file_receiver.c` the last operations before handoff are `remove()`+`rename()` (`finalize_handler`) and the periodic 16-chunk `fsync` (line 222) does NOT cover the final rename/remove. A dirty FAT/dir state is therefore presented to Windows — the exact "format me" trigger the fix claimed to eliminate.
- `FatFs[0]` (the FatFS internal "work area" pointer, `ff.c:467`) still points at the **freed** context → any later FatFS access is a use-after-free.

### B. Remount registers a drive but never mounts — first Wi-Fi transfer is broken/UAF
`sd_card.c:52-64`: `esp_vfs_fat_register("/sdcard", "storage", 5, &fs)`:
- `esp_vfs_fat_register_cfg` (`vfs_fat.c:185-234`) only allocates and registers a VFS; it never calls `f_mount`.
- Drive string `"storage"` is not a FatFS volume ID (`":0"` is required); path prepended becomes `storage/sdcard/<file>`.
- When `f_open` runs, `find_volume` (`ff.c:3405-3412`) does `fs = FatFs[vol]` → vol defaults to 0 (`get_ldnumber`, `ff.c:3206-3212`) → `FatFs[0]` = **freed memory** → garbage `fs_type` → either `FR_NOT_ENABLED`(→ `ENODEV`, fopen NULL) or, if garbage looks mounted, reads/writes through freed heap → **heap corruption / sector garbage on the SD**.
- Net effect: `/prepare → fopen → retry → STATE_ERROR → HTTP 500` (`file_receiver.c:151-170`). Even in a "lucky" UAF it **cannot** reliably receive files, and may corrupt the FAT — i.e. the fix replaces one corruption path with a broken-transfer path plus a memory-safety bug.

### C. No genuine exclusive-mode hand-off
- No `f_mount(0, "0:", 0)` flush, no `esp_vfs_fat_sdcard_unmount` (the proper API exists in `vfs_fat_sdmmc.c`), no `ff_diskio_unregister`.
- `GET /list` (`file_receiver.c:421-431`) calls `sd_card_remount()` while the USB MSC gadget is still attached — a concurrent-access window with the raw MSC callbacks, plus the same UAF.

### D. Correct parts (kept)
- USB MSC callbacks are arithmetically correct and FatFS-free (`usb_msc.c`: block_count = `csd.capacity` sectors, block_size 512, ≤8-sector per SCSI transfer via `sdmmc_*_sectors`), so USB access itself does not re-introduce FatFS concurrency.
- `format_if_mount_failed=false` (SD + flash) removes the destructive auto-format.
- TinyUSB disconnect/connect is called before/after FatFS sessions.

## 17. Final classification — **NOT COMPLETE**

| Section | Status |
|---|---|
| Root cause (FAT concurrently owned by FatFS + USB) | **NOT fixed** — hand-off never flushed/unmounted properly |
| Stable Wi-Fi transfer after rework | **BROKEN** — remount is register-without-mount + UAF (`FatFs[0]`), /prepare gets `ENODEV` |
| Data integrity on SD after Wi-Fi→USB flip | **NOT guaranteed** — unflushed rename/remove metadata |
| USB MSC raw callbacks | **OK** |
| Removal of destructive auto-format | **DONE** |
| Hardware validation | **NOT PERFORMED** (no board) |

Recommended rectification (not applied — audit-only): in `sd_card_unmount()`, first `f_mount(0, "0:", 0)` (flushes caches and clears `FatFs[0]`), then `ff_diskio_unregister`, host deinit as needed, keeping `current_sd_card` for raw MSC; on remount, call `esp_vfs_fat_sdspi_mount` (which re-registers diskio + `f_mount`) instead of `esp_vfs_fat_register`; drop the auto-remount in `GET /list` while the USB gadget is attached; and add `f_sync`/`f_mount(0)` after the finalize `rename`. Then re-verify on real hardware.

---

## 18. Rectification applied and verified (2026-09-16)

The recommended rectification from §17 has now been implemented and the project builds cleanly. Status per §17 classification:

| Section | Audit verdict (2026-09-14) | Status (2026-09-16) | Evidence |
|---|---|---|---|
| Root cause (FAT concurrently owned by FatFS + USB) | **NOT fixed** | **FIXED** | Boot is raw-only: `main:55` `sd_card_init_raw()` (no FatFS `f_mount`); `storage_manager_init()` (`main:59`) mounts **flash** fallback, never SD. USB MSC owns SD exclusively until a transfer mounts it. |
| Stable Wi-Fi transfer after rework | **BROKEN** (register-without-mount + UAF) | **FIXED** | `sd_card.c:213` `f_mount(fs, drv, 1)` (real mount, mirrors `esp_vfs_fat_sdspi_mount` per comment `sd_card.c:154`); register-only branch removed. No UAF: unmount clears the FatFS work area (below). |
| Data integrity on SD after Wi-Fi→USB flip | **NOT guaranteed** | **FIXED** | Proper FATFS unmount preserves dirty FAT/dir metadata: `sd_card_unmount_fatfs_locked()` = `f_mount(NULL, drv, 0)` FIRST (flushes caches, line 247) → `esp_vfs_fat_unregister_path()` → `ff_diskio_unregister(s_pdrv)` (line 257), `s_pdrv = FF_DRV_NOT_USED`. Handoff order documented in `sd_card.c:235`. |
| Proper clean unmount API | **NOT fixed** (VFS-only unregister) | **FIXED** | `f_mount(NULL)` now precedes unregister; `ff_diskio_unregister` invoked; `current_sd_card` retained (never freed) for raw MSC. |
| `GET /list` auto-remount while USB attached | **BROKEN** (concurrency + UAF) | **FIXED** | `file_receiver.c:442`: `/list` mounts nothing; when SD present + `usb_msc_is_host_mounted()`, returns `{"files":[],"storage_available":false,"reason":"usb_active"}`. |
| `f_sync` after finalize `rename` | **NOT fixed** | **FIXED** | Finalize/cancel/clear every exit path calls `storage_manager_flush_and_release_for_usb()` (which `sd_card_unmount()`s = flush via `f_mount(NULL)`) before `usb_msc_connect()`. |
| Flash fallback hand-off | n/a | **FIXED** | `storage_manager.c:206-213`: SD mounted → `sd_card_unmount()`; else WL handle valid → `esp_vfs_fat_spiflash_unmount_rw_wl(BASE_PATH, s_wl_handle)`, handle set to `WL_INVALID_HANDLE`, 100 ms settle, then `storage_manager_init()` re-mounts flash so `/status` still answers. |
| USB MSC raw callbacks | **OK** | **OK (unchanged)** | `usb_msc.c` uses `storage_manager_get_wl_handle()` (`usb_msc.c:70,83,140,189`) and raw `sdmmc_*_sectors`; SCSI `test_unit_ready` reports false while FatFS owns the card. |
| Removal of destructive auto-format | **DONE** | **DONE (unchanged)** | `format_if_mount_failed=false` SD + flash. |
| Hardware validation | **NOT PERFORMED** | **STILL BLOCKED** | `validate.ps1 -Action All` reports no ESP32-S3 COM port; only laptop devices enumerated. |

### Verification performed (2026-09-16)
- **Build:** `idf.py fullclean && idf.py build` then `validate.ps1 -Action All` — Receiver (`cnc_receiver.bin` 0xfd390) and Transmitter (`cnc_transmitter.bin` 0xefd20) both compile cleanly; `test_transfer.py` 7/7 PASS.
- **Compile-time API check:** `f_mkfs` (4-arg IDF 5.5 signature), `ff_diskio_unregister`, `esp_vfs_fat_sdspi_mount`-equivalent, `esp_vfs_fat_unregister_path`, `esp_vfs_fat_spiflash_unmount_rw_wl`, `storage_manager_get_wl_handle` all resolve — proving the header/impl contracts the audit flagged are now met.
- **Remaining limitation:** HIL (flash → Windows USB browse → Wi-Fi transfer → USB re-browse, no formatting prompt) still requires a physical board.