# FileXfer PCB — Stage 2: part shortlist + BOM cost

Status: draft for approval. Prices and stock were read from LCSC product pages and the JLC class from jlcpcb.com/partdetail on **2026-11-09**; they change daily.
Legend: **[V]** checked in the datasheet or drawing named; **[L]** from the LCSC/JLC listing only; **[U]** unverified.

## 0. Decisions from Stage 1 (user, 2026-11-09)
- Module ESP32-S3-MINI-1-N8. MINI-1U-N8 is a footprint-compatible build option (U.FL), with no other board changes.
- SD power switch on GPIO2 (soft-start), card detect on GPIO21 (optional fit), board ID by ADC divider on GPIO1, TC2030-NL, SD socket top side only.
- Case is a soft target: inner 18.54 W × 7.3 H × 40.0 L (TX) / 64.3 L (RX); total ≤ about 9 mm including the case.

## 1. Brownout thresholds (ESP32-S3)
- The ESP32-S3 Series Datasheet v2.2 has **no brownout table** (searched for "brown"/BOD).
- ESP-IDF v5.5.1 `components/esp_hw_support/power_supply/port/esp32s3/Kconfig.power`: level 7 = **2.44 V** (the default and our setting), 6 = 2.56, 5 = 2.67, 4 = 2.84, 3 = 2.98, 2 = 3.19, 1 = 3.30 V. The file says "the voltage levels here are estimates" and that they vary per chip. [V-IDF, estimate]
- **So every brownout logged today means the 3V3 rail fell below about 2.44 V**, a very deep sag. That points at the supply path (cable/adapter, Super Mini LDO, little bulk capacitance), not a marginal threshold. This supports fixing it in hardware.

## 2. Shortlist

| # | Function | Part (MPN) | LCSC # | JLC | Key spec (source) | Price @10 / @100 (USD) | Stock | Why |
|---|---|---|---|---|---|---|---|---|
| 1 | MCU module | Espressif **ESP32-S3-MINI-1-N8** | C2913206 | Ext | 8 MB quad flash, no PSRAM, PCB antenna, 15.4×20.5×2.4 mm, –40…85 °C [V DS-M v1.7] | 4.208 / 3.395 | 11 357 | Chosen in Stage 1 |
| 1U | MCU module (option) | ESP32-S3-MINI-1U-N8 | C2980299 | Ext | Same pads, 15.4×15.4 mm, U.FL [V DS-M] | 4.303 / 3.624 | 534 | Metal cabinets; low stock |
| 2 | 3.3 V LDO | ST **LDL1117S33R** (SOT-223) | C435835 | Ext | VIN 2.6–18 V (abs 20 V), 1.2 A, dropout 350 typ / 600 max mV @1.2 A, I_SC 1.5–2 A, θJA 120 °C/W, ceramic Cout OK [V ST DocID030319 Rev 3] | 0.391 / 0.288 | 60 535 | 1.2 A ≫ 355 mA peak; 18 V input survives bad chargers; in stock |
| 2a | LDO (alt.) | Diodes AP7361C-33Y5-13 (SOT-89-5) | C460397 | Ext | 1 A, VIN 2.2–6 V, dropout 340 mV typ @1 A, limit 1.1–1.5 A, θJA 150 °C/W [V Diodes DS37274 Rev 5-2] | 0.474 / 0.358 | **4** | Lower dropout, but no stock |
| 3 | USB ESD | ST **USBLC6-2SC6** | C7519 | Ext | D+/D-/VBUS, C(I/O–GND) 2.5 typ / 3.5 max pF [V ST Doc 11265 Rev 5] | 0.164 / 0.125 | 28 495 | Standard USB 2.0 FS ESD array |
| 4 | VBUS surge TVS | Leiditech **SMF5.0A** (SOD-123FL) | C2759874 | Ext | 5 V standoff, 9.2 V clamp, 21.7 A Ipp [L] | 0.073 / 0.059 | 4 890 | Bulk surge on VBUS; USBLC6 covers ESD only. Datasheet [U] |
| 5 | Polyfuse | BHFUSE **BSMD1206-075-6V** | C883127 | Ext | 1206, 6 V, 750 mA hold [L]; R and trip current [U] | 0.048 / 0.037 | 15 440 | Hold ≥ 600 mA LDO load. Datasheet not read: confirm R1max (the voltage drop matters) |
| 6 | USB-A **male** plug | XKB **U217-041N-4BV81** | C319178 | Ext | USB2.0 plug, shell 12.00 × 4.50 mm, 18.75 mm long, 4 SMD pins + 2 shell slots, 1.5 A, 3000 cycles [V XKB drawing A1, 2024-10-18] | 0.106 / 0.084 | 9 725 | Male, in stock, assembly-compatible. JLC notes it needs a fixture |
| 6a | ~~USB-A plug (alt.)~~ **dropped 2026-11-09: XKB only** | Molex **48037-2100** | C136453 | Ext | USB A plug, SMT, recommended PCB 1.60 mm, 12.00 × 4.50 mm shell [V Molex SD-48037-003] | 0.837 / 0.646 | **34** | Premium second source |
| 7 | microSD push-push | SHOU HAN **TF PUSH** | C393941 | Ext | Push-in/push-out, **CD pad**, 14.75 × 14.50 mm, height 1.80 min / 2.00 max [V SHOU HAN TF PUSH Rev A/0] | 0.067 / 0.055 | 245 530 | Push-push + card detect, huge stock |
| 7a | microSD (alt.) | XKB XKTF-015-N | C381082 | Ext | Push in/out, 14.75 × 14.50, 1.80–2.00 H, 9 terminals [V XKB drawing] | 0.147 / 0.121 | 28 880 | Same outline/pitch: second source [U footprint identical, check in Stage 3] |
| 8 | SD power switch | TI **TPS22917DBVR** (SOT-23-6) | C2681320 | Ext | 1–5.5 V, 2 A, adjustable slew via CT (CT = 1000 pF → 4 ms at 5 V), ON VIH 1 V, ON pull-down 750 kΩ [V TI SLVSDW8B] | 0.373 / 0.285 | 2 290 | Soft-start inrush limit, as requested |
| 9 | RGB LED | Worldsemi **WS2812B-2020-V6** | C52917434 | Ext | VDD +3.3…+5.3 V (**maximum-ratings table**), VIH 0.55·VDD, 12 mA/ch [V Worldsemi DS] | 0.092 / 0.074 | 860 655 | Same protocol as today on GPIO48 |
| 10 | BOOT, RESET buttons | XUNPU **TS-1088-AR02016** ×2 | C720477 | **Basic** | 4 × 3 mm SMD, 160 gf [L]; height/actuation [U] | 0.053 / 0.043 | 467 780 | Basic part (no loading fee) |
| 11 | Passives | Samsung/UNI-ROYAL 0402/0603/0805 | e.g. C1525, C52923, C19702, C25744, C17168 | Basic | 100 nF, 1 µF, 10 µF, 10 k, 0 Ω, … | about 0.15 / 0.10 per board | ≫ | Exact values picked in Stage 3 (C45783 22 µF and C25092 22 Ω showed stock 0: substitute) |
| — | TC2030-NL, pogo pads | footprint only | — | — | — | 0 | — | — |

### Rejected after reading datasheets/drawings
- SHOU HAN AM 180 (C2681564): the drawing says "该产品只能手工焊接" ("this product can only be hand-soldered"), so not for SMT assembly.
- Ckmtw U-USBAM04P-F000/F001 (C386738, C2895042): the drawings say **USB-A/F 母头 (female)**, even though LCSC lists them as a plug.
- Kinghelm KH-USB180-AM-4P (C2828094): male, on top of the board, about 6.1 mm tall [drawing]; 70 in stock.
- Molex 104031-0811 (C585350): the drawing title is "MICROSD CARD SOCKET **PUSH-PULL** 1.42H", **not push-push**, despite the LCSC listing.
- Hirose DM3AT-SF-PEJM5 (C114218): push-push, 1.68 mm, N.O. card-detect switch [V Hirose catalogue], but LCSC stock is 20 and it costs $0.91 @100. Kept only as a premium fallback.
- AP2112K-3.3 (C51118): 600 mA, and its datasheet table gives VDROP 1000 typ / 1300 max mV @600 mA (the feature list says 250 mV). Too little margin from a sagging VBUS.
- TLV75733P: VIN max 5.5 V, too close to VBUS/charger overshoot.
- SN74AHCT1G125 level shifter: not needed while the LED runs on 3V3.

## 3. Design notes from the datasheets
- **LDO choice / thermal:** RX in a transfer runs at about 0.2–0.3 A average [U, estimate] → (5.0–3.3) × 0.3 ≈ 0.5 W → ΔT ≈ 60 °C at θJA 120. That is acceptable (TJ max 125 °C) up to about 50 °C ambient; copper pour under the tab is required. A buck converter would run cooler but adds noise near the antenna; not proposed.
- **Headroom:** VBUS 4.4 V (worst case) − polyfuse drop [U] − 0.6 V max dropout ≥ 3.3 V needs a polyfuse drop ≤ about 0.4 V at peak. Confirm R1max in Stage 3.
- **USB inrush:** ≤ 10 µF on VBUS. The 3V3 bulk (about 22–47 µF) charges through the LDO current limit (1.5–2 A) → about 50–150 µC. **This may exceed the USB 50 µC inrush figure** [U]. Options for Stage 3: a smaller 3V3 bulk (22 µF) or a slow-start on the LDO input. It is a compliance risk, not a functional one.
- **SD power switch default:** the ON pin has an internal 750 kΩ pull-down, so the SD card would be **off at reset** and the current firmware would see no card. Proposal: a 10 k **pull-up on ON → card powered by default**; firmware pulls GPIO2 low to power-cycle the card. This keeps today's firmware working on the new board (SPI mode on the same pins).
- **LED at 3.3 V:** 3.3 V is the minimum of the rated range. VIH = 1.8 V, so the data line is solid. Blue/white brightness may drop at 3.3 V [U, bench].
- **USB-A plug height:** shell 4.50 mm + PCB 1.6 mm ≈ 6.1 mm < 7.3 mm inner. The exact plug height above the PCB and the overhang beyond the edge are set from the 3D model in Stage 4 [U].
- **One PCB vs case lengths:** the RX stack (plug on board + LDO/USB area + SD 14.5 mm + module 20.5 mm) gives a board of about 45–50 mm, which is more than the TX case's 40 mm inner length. **With one shared PCB the TX case must grow to the RX length** (or the TX gets a shorter board variant, which breaks "one PCB"). Decision needed.

## 4. Rough BOM cost (parts only, LCSC prices above)

| Build | @10 | @100 |
|---|---|---|
| TX (SD socket, load switch, SD passives DNP) | **≈ $5.34** | **≈ $4.25** |
| RX (all fitted) | **≈ $5.81** | **≈ $4.62** |
| MINI-1U build | + $0.10 / + $0.23, plus an external antenna + U.FL pigtail (not priced) | |

The module is 72–80 % of the cost. Not included [U, quote JLC at Stage 4]: PCB (2-layer, about 50 × 18 mm), assembly setup + stencil, a **per-unique-Extended-part loading fee** (about 10 extended parts → a fixed charge per order, which dominates at qty 10), and the fixture/THT fee for the USB plug's shell legs. LCSC minimum order quantities raise the @10 figures slightly.

## 5. Firmware backlog (from Stages 1–2)
1. Stream RX logs over Wi-Fi to TX (field logs without opening the case).
2. Hold off or ramp Wi-Fi TX until USB is configured (USB 100 mA pre-configuration limit) [U spec text not read].
3. SDMMC 4-bit driver via the GPIO matrix. Rev A pins (stage 4 remap): CLK IO13, CMD IO12, D0 IO14, D1 IO15, D2 IO10, D3 IO11 (`firmware/board_config.h`).
4. Board ID by ADC on GPIO1; refuse the wrong app.
5. SD power-cycle on a card error via SD_PWR_EN **IO35**; card detect on **IO16** (instead of "restart with card"); polarity set after the bench test.
6. Re-test TX power above 8.5 dBm on the new board (brownout count).
7. **TX firmware ignores BOARD_ID.** The transmitter build doesn't read IO1 yet. Add `board_id_read()` at TX boot (R7 22k → 0.60 V; RX 100k → 1.65 V) so a TX image on RX hardware, or the reverse, is logged and refused (added 2026-11-10).

## 6. Questions for approval
1. **One PCB → TX case grows to the RX length (about 50 mm board)?** Or allow a short TX variant?
2. LDL1117S33R (in stock, 18 V input) over AP7361C (lower dropout, no stock): OK?
3. XKB U217 plug (cheap, in stock, needs a JLC fixture) with Molex 48037 as the alternate: OK?
4. SD switch default **on** via pull-up (backward-compatible): OK?
