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

## 3./7. Measurements (fill in from the on-device speed test, Settings > System)

Each test logs a `PERF` line on the transmitter (and on the receiver for SD) UART.

| Leg | 2 MB before | 2 MB after | 50 MB before | 50 MB after |
|---|---|---|---|---|
| Phone/PC → TX (discard) | | | | |
| TX → RX (RAM, discard) | | | | |
| RX SD write (local) | | | | |
| Full path | | | | |
