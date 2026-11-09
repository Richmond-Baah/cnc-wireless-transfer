# FileXfer PCB — Stage 1: requirements + pin map

Status: draft for approval. Nothing here is hardware-tested.
Branch `pcb` (from `main` @ 1edb6e8). `main` is 36 commits behind `test-combined`; the pin assignments are identical on both (checked with `git grep`).

Legend: **[V]** verified against the cited datasheet/guide; **[F]** taken from firmware source; **[U]** unverified — must be checked before it is relied on.

Sources used:
- DS-M: ESP32-S3-MINI-1 & MINI-1U Datasheet v1.7 (documentation.espressif.com)
- DS-C: ESP32-S3 Series Datasheet v2.2
- HDG: ESP32-S3 Hardware Design Guidelines (docs.espressif.com, "latest"), Schematic Checklist + PCB Layout Design

## 1. Current GPIO use (firmware)

| Function | GPIO | Board | Source |
|---|---|---|---|
| BOOT button (10 s hold), internal pull-up | 0 | TX+RX | components/boot_button/boot_button.c:6 |
| WS2812 RGB LED (led_strip, RMT, 1 LED) | 48 | TX+RX | receiver/main/led.c:9, transmitter/main/led.c:8 |
| SD SPI MISO | 13 | RX | receiver/main/sd_card.c:23 |
| SD SPI MOSI | 11 | RX | receiver/main/sd_card.c:24 |
| SD SPI CLK | 12 | RX | receiver/main/sd_card.c:25 |
| SD SPI CS | 10 | RX | receiver/main/sd_card.c:26 |
| SD power switch | none (-1) | RX | receiver/main/sd_card.c (PIN_NUM_PWR, test-combined:28) |
| USB D- / D+ (TinyUSB MSC; USB-Serial-JTAG turned off by register writes) | 19 / 20 | RX | receiver/main/usb_msc.c:391 |
| USB D- / D+ (USB-Serial-JTAG secondary console) | 19 / 20 | TX | transmitter/sdkconfig:1302 |
| UART0 console 115200 (U0TXD/U0RXD) | 43 / 44 | TX+RX | receiver/sdkconfig:1304, transmitter/sdkconfig:1305 |

Related settings:
- SD on SPI3 using `SDSPI_HOST_DEFAULT()` (receiver/main/sd_card.c, test-combined:119). This is 20 MHz by default in ESP-IDF. [F; the 20 MHz default value comes from IDF, not checked in the IDF headers]
- USB config descriptor declares bMaxPower 500 mA (receiver/main/usb_msc.c:81).
- Wi-Fi TX power cap is 34 qdBm (8.5 dBm) on both boards (receiver/sdkconfig:606, transmitter/sdkconfig:605).
- Brownout detector is set to level 7 on both boards (sdkconfig:1127). The level-7 voltage for the ESP32-S3 is not published in DS-C. The about-2.80 V figure found is for the ESP32 Kconfig. [U]
- The board ID is not on the PCB: separate apps (cnc_transmitter / cnc_receiver) set the role.

## 2. Datasheet facts the design relies on

| Fact | Value | Src |
|---|---|---|
| MINI-1 variants | -N8 (8 MB quad flash, no PSRAM), -N4R2 (4 MB quad flash + 2 MB quad PSRAM); –40…85 °C. H4R2 variants have been removed. | DS-M Table 1-1, rev history [V] |
| Size | MINI-1 15.4 × 20.5 × 2.4 mm; MINI-1U 15.4 × 15.4 × 2.4 mm | DS-M Table 1-1 [V] |
| MINI-1U footprint compatibility | Same pin diagram, "but the latter has no antenna keepout zone". Both land patterns are 60 × (0.4 × 0.8) pads + 4 × (0.8 × 0.8) thermal pads. **MINI-1U fits the MINI-1 footprint; it is 5.1 mm shorter and needs a U.FL antenna.** | DS-M Fig 3-1 note, Fig 11-1/11-2 [V] |
| Exposed GPIO | 0–21, 26, 33–48 (TXD0=43, RXD0=44); 3V3 = pin 3; EN = pin 45 | DS-M Table 3-1 [V] |
| GPIO26 | Wired to the PSRAM on -N4R2, so not usable | DS-M Table 3-1 note b [V] |
| GPIO33–37 | Used only with octal flash/PSRAM; MINI-1 has quad only, so they are free | HDG Flash/PSRAM + DS-M [V] |
| GPIO47/48 | SPICLK_P/N_DIFF; follow VDD_SPI, which is 3.3 V when GPIO45 = 0 at reset (the default) | HDG VDD_SPI table [V] |
| Strapping pins | GPIO0 weak pull-up (1 = SPI boot), GPIO3 floating (JTAG source, only with an eFuse), GPIO45 weak pull-down (0 = VDD_SPI 3.3 V), GPIO46 weak pull-down | DS-C Table 3-1 [V] |
| Supply | 3.0–3.6 V; supply ≥ 500 mA; ESD diode + ≥ 10 µF at the main power entry | DS-M Table 6-2, HDG Power Supply [V] |
| Peak current | 355 mA at 802.11b 20.5 dBm; 286 mA at HT20 MCS7 17.5 dBm; RX 95 mA | DS-M Table 6-4 [V] |
| EN | RC 10 kΩ / 1 µF; never floating; tSTBL, tRST ≥ 50 µs | DS-M §9, HDG [V] |
| GPIO0 | External pull-up recommended; no large capacitor (risk of download mode) | HDG Strapping [V] |
| USB | GPIO19 = D-, GPIO20 = D+; reserve 22/33 Ω series + caps to GND (DNP); 90 Ω ±10 % differential | HDG USB [V] |
| SDMMC | One host; any GPIO through the GPIO matrix; pull-ups on CMD/DAT needed (**no value given**); reserve series resistors; 50 Ω, ±50 mil match to CLK, ≤ 2000 mil total | HDG SDIO [V]; pull-up value [U] (common 10 kΩ) |
| UART0 | 499 Ω series on U0TXD near the module | HDG UART [V] |
| Antenna | Antenna outside the base board, or feed at the edge with the board cut away on both sides and below; ≥ 15 mm clearance inside the housing | HDG Layout for Modules [V]; keep-out mm in a figure only (read from the KiCad footprint in Stage 4) [U] |
| USB VBUS capacitance | ≤ 10 µF across VBUS at attach, else inrush limiting | Microchip MCP2222 doc, TI ISOUSB211 DS, USB-IF Device Capacitance ECN (secondary) [V-secondary] |
| USB 100 mA before configuration | Standard USB 2.0 rule; no readable source found | [U] |
| Super Mini LDO / bulk capacitance | No schematic found; the LDO part and current are unknown | [U] |

## 3. Requirements (draft)

### 3.1 One board, two roles
- Same PCB and BOM for TX and RX. On TX, do not fit (DNP) the SD socket, its pull-ups and the optional SD power FET.
- Board ID: resistor divider on **GPIO1 (ADC1_CH0)** + 0.1 µF (HDG ADC note). About 8 levels encode role (TX/RX) + board rev. NVS can override it. Firmware reads it at boot and refuses to run the wrong app (logged). Alternative: two digital straps on GPIO4/5 (simpler but fewer codes).

### 3.2 Module
- **ESP32-S3-MINI-1-N8** is preferred: 8 MB flash gives OTA headroom, no PSRAM is used today (no CONFIG_SPIRAM), and GPIO26 stays free. -N4R2 stays an option.
- MINI-1U option on the same footprint (see §2) with a U.FL antenna, for metal CNC cabinets. The board outline would then be shorter, or the antenna area left empty.
- Module at the far end from the USB plug. Antenna area flush with or overhanging the board edge; no copper on any layer under the antenna area.

### 3.3 Power (fixes the brownouts)
- VBUS → polyfuse (≥ 750 mA hold, Stage 2) → TVS on VBUS → LDO 3.3 V **≥ 600 mA** (margin over 355 mA peak + SD writes, about 100 mA [U, card dependent] + LED), low dropout at VBUS ≈ 4.4 V [U, spec minimum to confirm].
- Capacitance: **≤ 10 µF on VBUS** (USB attach limit). On 3V3: ≥ 10 µF at the module pin (HDG) + 22–47 µF bulk behind the LDO; the LDO's current limit/soft-start bounds inrush (confirm in Stage 2 from the LDO datasheet).
- Goal: support 19.5 dBm TX power (the highest firmware level) without brownout. Hardware is necessary but not sufficient; the cap stays in firmware until bench-tested.
- Risk [U]: RX draws Wi-Fi current possibly before enumeration (USB 100 mA unconfigured rule). Hardware cannot fix this; noted for firmware.

### 3.4 USB-A male plug on the board edge
- Through-hole/SMT USB-A male plug with a shell (not a gold-finger PCB plug: wear). D+/D-: 22 Ω series (HDG allows 22/33), low-capacitance ESD array (USBLC6-2 class, Stage 2), cap-to-GND footprints DNP. 90 Ω pair, GND pour, no vias.
- Optional (DNP) external pull-up footprint on D+ (HDG power-up note); value [U].

### 3.5 microSD (RX)
- Push-push socket, **SDMMC 4-bit**, keeping today's pins so SPI mode still works on the same wires:

| SD signal | GPIO | SPI-mode role (current firmware) |
|---|---|---|
| CLK | 12 | CLK |
| CMD | 11 | MOSI |
| DAT0 | 13 | MISO |
| DAT3 | 10 | CS |
| DAT1 | **14** (new) | — |
| DAT2 | **9** (new) | — |
| Card detect (optional) | **21** (new) | — |
| SD VDD switch (optional P-FET) | **2** (new) | PIN_NUM_PWR |

- 10 kΩ pull-ups on CMD, DAT0–3 [U value]; 0 Ω series footprints on each line (HDG). Short, matched traces (HDG SDIO rules).
- SD power switch lets firmware power-cycle a hung card instead of needing a reboot. Card detect could later replace "no card at boot → restart".

### 3.6 Status LED
- Keep a WS2812-protocol LED on **GPIO48**, powered from **3V3**, so 3.3 V logic is valid. Many WS2812B need VDD ≥ 3.5 V and VIH = 0.7·VDD; the part must be rated at 3.3 V (Stage 2) [U].

### 3.7 Buttons, debug, test
- BOOT (GPIO0, 10 kΩ pull-up, no cap) and RESET (EN, 10 kΩ/1 µF RC) as SMD tact switches under case pinholes.
- **Field serial logs (pain point):** UART0 (499 Ω on TX) + EN + GPIO0 + 3V3 + GND on a **Tag-Connect TC2030-NL** footprint (6 pins: exactly these; no BOM cost). With a USB-UART cable it gives logs and flashing without the jig. For RX logs without opening the case, a firmware log relay over Wi-Fi is the real fix (out of scope here).
- Pogo test pads (≥ 1.0 mm, 2.54 mm grid where possible): 3V3, GND, VBUS, EN, GPIO0, U0TX, U0RX, D+, D-, board-ID.

### 3.8 Pins left alone
- GPIO3, 45, 46: unconnected (defaults give SPI boot + 3.3 V VDD_SPI).
- GPIO19/20: USB only. GPIO43/44: UART0 only. GPIO26: unused (PSRAM on N4R2).

### 3.9 Outline / mechanical
- Width **≤ 18.0 mm** (18.5 mm inner, 0.25 mm per side). Module 15.4 mm fits with 1.3 mm per side.
- Rough length: USB-A plug + SD socket (about 14–15 mm [U]) + module 20.5 mm → about 45–55 mm. Putting the SD socket on the bottom under the module (not under the antenna) shortens it but needs 2-sided assembly (cost; Stage 2).
- 2-layer, 1.6 mm (or 1.2 mm if the USB-A plug part allows), JLC rules.

## 4. Proposed new-board pin map (summary)

| GPIO | Function | TX | RX |
|---|---|---|---|
| 0 | BOOT button + 10 k pull-up | ✓ | ✓ |
| 1 | Board-ID ADC | ✓ | ✓ |
| 2 | SD VDD enable (opt.) | DNP | ✓ |
| 9 / 10 / 11 / 12 / 13 / 14 | SD D2 / D3 / CMD / CLK / D0 / D1 | DNP | ✓ |
| 19 / 20 | USB D- / D+ | ✓ | ✓ |
| 21 | SD card detect (opt.) | DNP | ✓ |
| 43 / 44 | UART0 TX / RX → TC2030 + pads | ✓ | ✓ |
| 48 | RGB LED (WS2812-type) | ✓ | ✓ |
| EN | RESET button + RC | ✓ | ✓ |

Firmware impact: SD driver moves from SDSPI to SDMMC 4-bit (pins kept); board-ID read; optional CD/power pins. All of it goes behind a per-rev board-config header (Stage 3).

## 5. Tooling

- KiCad MCP Pro v4.10.0 with KiCad 10.0.4 CLI (ERC/DRC/export through kicad-cli). The GUI/IPC is not running (no open docs). Its default tool categories are project / pcb_read / pcb_write / routing; schematic authoring needs a schematic profile [to confirm in Stage 3].
- Skills: `kicad` (review/ERC/DRC/DFM), `bom` (BOM lifecycle). **No LCSC/JLCPCB parts-search tool or MCP is installed**, so Stage 2 stock and prices come from web lookups and will be dated and flagged.

## 6. Open questions for approval

1. Case: inner thickness and length (or a drawing/STEP)? Needed for the plug position and the SD socket side.
2. Module: MINI-1-N8 (recommended) or N4R2? Also provide a MINI-1U variant?
3. Add the optional SD card-detect (GPIO21) and SD power switch (GPIO2)?
4. Board ID by ADC divider on GPIO1 (recommended) or two digital straps?
5. Tag-Connect TC2030-NL for field UART: OK?
6. SD socket top-side only (longer board, 1-sided assembly) or bottom (shorter, 2-sided)?
