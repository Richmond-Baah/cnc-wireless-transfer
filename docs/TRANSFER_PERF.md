# Transfer throughput (phone/PC → TX → RX → SD)

## 1. Data path today (main @ 1edb6e8)

| Leg | Transport | Unit | Ack / flow | Buffering | Code |
|---|---|---|---|---|---|
| Browser: MD5 | spark-md5 over the whole file, 2 MB slices, before anything is sent | — | — | — | transmitter/web/app.js:1226 |
| Browser → TX | HTTP POST /upload_chunk over Wi-Fi (TX SoftAP) | 32 KB (CHUNK_SIZE) | stop-and-wait: next chunk only after the TX reply, which comes after the RX ack | none in flight | app.js:1120, 1272-1277 |
| TX receive | httpd (upload worker task), `read_full_body` into `s_chunk_buf` | 32 KB | whole chunk read before forwarding (store-and-forward) | 32 KB | web_server.c upload_chunk_handler |
| TX → RX | esp_http_client POST /chunk, keep-alive socket reused | 32 KB + X-Chunk-Index | waits for RX 200 (after SD write) | — | file_transfer.c:193-244 |
| RX receive | httpd, `httpd_req_recv` into 16 KB `chunk_buf` | whatever TCP returns (often 1.4-5.7 KB) | dup = ack, out-of-order = 409 | 16 KB max | file_receiver.c chunk_post_handler |
| RX → SD | `fwrite` per recv piece, `fflush`+`fsync` every 16 recv pieces, MD5 per piece | 1.4-16 KB writes | — | stdio + FatFS sector cache | file_receiver.c chunk_post_handler |
| SD | SDSPI (SPI2, CLK 12 / MOSI 11 / MISO 13 / CS 10), default 20 MHz | — | — | — | sd_card.c |

- **No ESP-NOW for data.** ESP-NOW is only used to pair receivers (components/pair_proto). File data is HTTP over TCP over Wi-Fi.
- **Radio:** one 2.4 GHz channel. The phone and every receiver are stations of the TX SoftAP, so each byte crosses the air twice (phone → TX, TX → RX) on the same channel. Today the two legs never overlap: everything is serialised per 32 KB chunk.
- **TCP:** both boards use `CONFIG_LWIP_TCP_WND_DEFAULT = 5760` and `TCP_SND_BUF = 5760` (4 × MSS), with a Wi-Fi BA window of 6. Throughput per TCP leg is limited to about window / RTT.
- **CPU:** TX runs at 160 MHz, RX at 240 MHz. Both have Wi-Fi TX power capped at 8.5 dBm (`CONFIG_CNC_WIFI_MAX_TX_POWER = 34`), which lowers the PHY rate at distance.
- **Per-chunk cost:** browser → TX transfer, then TX → RX transfer, then up to 16 small `fwrite` + MD5 calls and sometimes an `fsync` (FAT/dir update), then two HTTP replies. No stage overlaps with another.

## 3./7. Measurements (not run yet: needs the hardware)

Flash **013e997** (speed test, original data path) for "before" and **45ee39c** (all changes) for "after". Then open Settings > System > Speed test, pick a machine, run it at 2 MB and at 50 MB, and copy the rows here. Each leg also logs on the transmitter UART:
`PERF browser->TX`, `PERF TX->RX`, `PERF SD write` (also on the receiver UART), plus `XFER ... summary` on both boards for the full transfers.

| Leg | 2 MB before | 2 MB after | 50 MB before | 50 MB after |
|---|---|---|---|---|
| Phone/PC → TX (discard) | | | | |
| TX → RX (RAM, discard) | | | | |
| RX SD write (local) | | | | |
| Full path, buffered relay | | | | |
| Full path, streaming relay | n/a | | n/a | |

## 4./5. Changes (unmeasured)

| Commit | Change | Why |
|---|---|---|
| 4032953 | RX reads the whole 32 KB chunk, then one `fwrite` + one MD5; no `fsync` per chunk; a short body writes nothing and can be retried | was one `fwrite` per TCP read (1.4-16 KB) plus `fsync` (FAT + dir update) every 16 reads |
| 4032953 | TX `X-Relay: stream`: chunk pieces go to the RX as they arrive from the browser | overlaps the two legs. Default since 2026-11-07 (`RELAY_STREAM_DEFAULT`, file_transfer.c): 2 MB speed test, one run via router, stream 286 KB/s vs buffered 231 KB/s |
| bad0752 | TCP window and send buffer 5760 → 32768 on both boards; Wi-Fi RX buffers and BA windows ↑; TX CPU 160 → 240 MHz | a 5.7 KB window caps each TCP leg at about window / RTT |

- **Step 4, receive the whole file first:** not possible. The TX has 2 MB of flash for the app and no filesystem for data, so it cannot hold a 50-900 MB file. Streaming vs store-and-forward is therefore compared per 32 KB chunk.
- **Step 5, ESP-NOW changes:** not applicable, because ESP-NOW does not carry file data. The "buffer at least 32 KB before `f_write`" part is done (4032953).
- **Not changed:**
  - SD SPI clock (`SDSPI_HOST_DEFAULT`, 20 MHz).
  - Wi-Fi TX power (8.5 dBm, capped on purpose to avoid brownouts).
  - Browser MD5 pass before upload. For 900 MB on a phone this pass alone can take minutes.

## 8. Proposal (not implemented): faster data path

The receivers already join the TX SoftAP and data already flows as HTTP over TCP, so "RX joins TX AP + TCP" is the current design and would gain nothing on its own. Remaining limits and options:

| Option | Expected gain | Effort | Risks |
|---|---|---|---|
| Browser uploads straight to the RX (TX issues a short-lived per-transfer token; RX accepts /chunk with it, CORS) | Removes TX store-and-forward and its TCP/CPU hop. Air time is unchanged, because in infrastructure mode the AP still relays every frame. Estimate +10-30 % over the buffered relay | medium: RX upload API with tokens, CORS, UI target switch, recovery paths | security (token reuse, open RX API), phones dropping the AP connection during long uploads, two code paths |
| SD over SDMMC 1/4-bit instead of SPI | SD ceiling from about 1-2 MB/s (SPI 20 MHz, practical FatFS) to about 5-10 MB/s | hardware: new pins/board revision, sd_card.c rewrite | board redesign, card compatibility |
| SPI 20 → 40 MHz | up to about +50 % SD ceiling, if the wiring allows | small (one line) | signal integrity on the current board: test with many cards |
| Wi-Fi TX power above 8.5 dBm on both boards | higher PHY rate and fewer retries at distance | small (Kconfig) | brownout on weak supplies (the reason for the cap) |

**Ceilings (estimates, to confirm with the speed test):** one 2.4 GHz channel carries each byte twice, so end-to-end is at most about half of one leg's TCP rate. ESP32-S3 TCP over 20 MHz HT is a few MB/s at best at short range. SD over SPI is about 1-2 MB/s practical. The SD card or the shared channel, whichever is lower, sets the upper bound.
