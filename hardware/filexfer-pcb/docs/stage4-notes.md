# FileXfer PCB — Stage 4 notes

Legend: **[V]** checked in the named datasheet/drawing; **[U]** unverified.

## 0. Stage 3 approval changes (user, 2026-11-09)
1. **LED buffer → SN74LV1T125DBVR** (LCSC C473338, JLC Extended, 86k in stock, $0.13 @50 / $0.12 @150) [V TI SCLS745D]:
   - VCC 1.6–5.5 V recommended (AHCT: 4.5–5.5 V), so VBUS sag to 4.0 V stays in spec.
   - VIH max: 2.03 V at VCC 4.5–5.0 V, 2.11 V at 5.5 V, 1.48 V at 3.6 V (4.0 V not tabulated, lies between) → ESP32 VOH ≥ 2.64 V gives ≥ 0.5 V margin over 4.0–5.5 V. VIL 0.8 V. Inputs 5 V tolerant.
   - VOH ≥ VCC − 0.1 V at 20 µA → meets WS2812B VIH 0.7·VDD (same +5V rail).
   - Pinout OE̅ 1, A 2, GND 3, Y 4, VCC 5 = 74LVC1G125 symbol. Cost +about $0.08 vs AHCT.
2. **TPS22917 current** [V TI SLVSDW8B §6.3/6.5]: IMAX 2 A continuous, IPLS 2.5 A (< 300 µs, 2 %). Module peak 355 mA + SD about 200 mA = 0.56 A → 3.6× margin (5.6× on the module alone). RON ≤ 140 mΩ (VIN 3.6 V, −40…85 °C) → ≤ 78 mV drop, 44 mW at 0.56 A.
3. **SD pull-ups 10 kΩ** on CMD, DAT0–3 [V ESP-IDF v5.5.1 "SD Pull-up Requirements"]: already in the schematic (R18–R22). CLK has no pull-up (not required).
   **Card-detect polarity:** the TF PUSH drawing (Rev A/0) defines pins 1–8 only; pad "Cd" has no description or switch diagram [U]. A single Cd pad means the switch closes to the shell (GND). The circuit (10k pull-up on GPIO21, switch to GND) works for both NO and NC; firmware sense is set after a bench test.
4. **XKB only**: Molex 48037 alternate dropped (stage2 row 6a struck).

## 1. Module footprint vs DS v1.7 Fig 11-1 [V]
| Item | Fig 11-1 | KiCad `RF_Module:ESP32-S2-MINI-1` | Espressif `ESP32-S3-MINI-1` (kicad-libraries) |
|---|---|---|---|
| Signal pads | 60 × 0.4 × 0.8, 0.85 pitch, 11.9 span | same | same |
| Corner pads | 4 × 0.8 × 0.8, 14.0 c-c | same | same |
| EPAD | 3 × 3 of 1.2, 1.65 pitch (4.5 span), pin-1 chamfer | same (no chamfer) | same (chamfered) |
| Pad numbers/positions | — | identical to Espressif (all 65 names, 0 position diffs) | — |
| Module outline | 15.4 × 20.5 | **15.4 × 20.0** (antenna end 0.5 mm short) | 15.4 × 20.5 |
| Antenna keep-out | (HDG) | 45.4 × 19.5 mm, extends 14.7 mm past the module tip | 15.4 × 5.1 mm (antenna area), all layers |
- Symbol pin numbers vs DS pin table: 40 named rows match; only aliases differ (IO19/IO20 = USB_D−/D+).
- **Decision: use Espressif's footprint** (`filexfer:ESP32-S3-MINI-1`, CC-BY-SA 4.0 with design exception). Pads are the same as stock; outline and keep-out are correct for this module. MINI-1U shares all 73 pads [V].
- 3D model (Espressif STEP, 6.8 MB) path `${KIPRJMOD}/3dmodels/ESP32-S3-MINI-1.STEP`; added with the 3D render.

## 2. Antenna placement [V ESP HW Design Guidelines, ESP32-S3, "Positioning a Module on a Base Board"]
- Preferred: antenna **outside** the base board, feed point at the board edge. Otherwise cut the board away on both sides of and below the antenna (clearance ≥ 15 mm to the side, max 2 mm / max 1 mm overhang, 6 mm deep per HDG figure); dense GND vias near the antenna.
- Housing: "a clearance of at least 15 mm is recommended in all directions" → not possible in a 20.9 mm case; only plastic near the antenna, test range (end-product test required by Espressif anyway).
- **Plan: PCB ends at the antenna boundary** (module overhangs 5.1 mm, antenna over nothing). All module pads stay on the board. Board-edge GND stitching along the end.

## 3. XKB U217 seating
- **3D model not obtained**: EasyEDA API 403, XKB site unreachable, LCSC STEP needs login. [U]
- EasyEDA footprint for C319178 (LCSC image SVG, board-outline + silk layers) [V as cross-check]:
  - Pads/holes/slots match the XKB drawing (7.00 span, 4.50 holes, 11.40 slots, slot 1.60 × 4.10 pad). EasyEDA pads are 2.50 long (vendor 2.00: kept vendor, 1.60–3.60 from the hole row).
  - **Board edge 2.92 mm from the hole row** (shell side); shell front 17.78 mm → **14.86 mm overhang**. Now on Dwgs.User in the footprint.
- Drawing side view: board-top plane 1.90 mm from one shell face, 2.60 from the other → shell straddles the board; shell rear (front − 18.75) is about 1 mm behind the hole row, so about 3.9 mm of shell is over the board. Whether the lower half of the shell is cut away there (legs only) or needs a board notch is **not determinable from the drawing** [U].
- Ask: a STEP from LCSC (logged in) into `hardware/case/`, or a sample to measure.

## 4. Placement (rev A) — `tools/gen_pcb.py` → `filexfer-pcb.kicad_pcb`
Frame: y from the plug board edge toward the antenna; x from the centreline, **+x = right, viewed from the top with the plug pointing away** (card slot is on the left, −x).
- Board **18.3 × 62.75 mm**, 2 layers, 1.6 mm; module antenna overhangs the far end by 5.1 mm (total 67.85); plug protrudes 14.86 mm past the front edge.
- All assembled parts on top; bottom = 10 test pads only (copper, not in BOM/CPL), the production pogo-jig target. TC2030 dropped (user, routing go-ahead).
- Order: plug (0–6.8) → USB ESD / LDO / soft-start / LED (6.8–20) → buttons (20–23.7) → SD socket (23.75–39.7) → SD resistor columns + SD switch (41–46.9) → module (47.35–62.75) + antenna. Board-ID divider and EN/UART parts sit in the strips beside the module (§5).
- **Notch reserve** x ±6.5, y 0–3.9: KiCad rule area on both layers (no tracks, vias, pour); only J1's own NPTH holes and leg slots are inside. F1/D1 sit beside it (|x| ≥ 6.78). Note: the leg slots are at x ±5.70 (pads to ±6.5), so a notch can be at most about 9.8 mm wide unless the legs are inside it [U, depends on the STEP].
- Module strips (1.15 mm between module courtyard and board edge): C7 10 µF 0402 + C8 by the 3V3 pin; R8 (TXD), R4/C9 (EN RC) on the other side.
- Rules (JLC 2-layer): 0.127 track/space min, default net class 0.2/0.15, via 0.6/0.3, copper–edge 0.25, hole–hole 0.5.
- Height (top / bottom of board): plug +1.90/−2.60 or +2.60/−1.90 [U face], module 2.4, SD 2.0, SOT-223 1.8, TS-1088 1.6; bottom: plug only. Stack ≤ 6.8 mm vs 7.3 inner.

### Case features (new positions; the old Super Mini case does not apply)
| Feature | x (mm) | y from plug board edge | Notes |
|---|---|---|---|
| LED (WS2812B-2020) | +3.6 | 8.2 | top, 0.85 mm tall |
| BOOT button SW1 | +5.5 | 21.9 | TS-1088-AR02016, 1.6 mm actuator top [U] |
| RESET button SW2 | −5.5 | 21.9 | same |
| microSD socket J3 | −9.15 … +5.35 (slot faces −x, flush with the left edge) | body 23.75–39.7; card about 25.7–36.7 | **sealed inside, no case slot.** Socket 2.0 mm tall (height list above). Retaining rib: case wall/rib at the left edge, y ≈ 26–36, ≥0.3 mm from the card face, so a push-push eject cannot move the card out. Keep 0.5 mm clear above the socket. |
| Antenna | — | 62.75–67.85 | plastic only around it |
- Case inner length about 68.5 mm (board 67.85 + clearance).
- Placeholder 3D models: XKB plug and SD socket are boxes at datasheet dimensions (`tools/gen_fp.py`), not vendor geometry.

## 5. Routing (rev A) — `tools/route.py` (run after `gen_pcb.py`)
Freerouting could not be downloaded (blocked as external code), so routing is a script:
- **Fixed geometry** for the critical and congested parts: USB pair; SD bus fan; SD switch group; front↔module trunks under the SD socket; terminations at the module strips; LED nets; GND vias for boxed-in pads.
- **A\* maze router** (0.1 mm grid, two layers, real clearances) for everything else; it also joins islands.
- **GND:** fanout vias, stitching (module EPAD ×4, a 1 mm row inside the antenna edge, board edges, 2 mm grid), pours on both layers (0.2 mm clearance), plus a via in every pour island.

**DRC (KiCad 10, JLC rules, schematic parity on): 0 errors, 0 unconnected, 0 parity issues.** 9 warnings left:
- 4 module silkscreen lines past the board edge (antenna overhang; the fab clips them).
- 5 GND pour necks 0.08–0.10 mm. They are at the J1 shell pads, which sit inside the notch reserve where pour is not allowed, and at two vias. Each item is also connected by other paths. Cosmetic.

ERC 0. 310 tracks, 169 vias.

### USB D+/D− (90 Ω differential)
- Geometry: 0.5 mm traces, 0.15 mm gap, 0.2 mm gap to the coplanar GND pour, 1.6 mm FR-4 (εr 4.5), no solder mask.
  - Result: **91.4 Ω** (my own 2D field solver, `tools/zdiff.py`). Mask should pull it down 2–3 Ω, so expect about 88–90 Ω [estimate; not the fab's calculator].
  - Solver check: an isolated 1.1 mm microstrip gives 158.5 Ω vs Hammerstad's ≈163 Ω (−3 %).
- Route: J1 → U1 (ESD, flow-through) on top → vias → coupled run on the **bottom under the SD socket**, referenced to the top GND pour under the socket body → vias → R1/R2 (22 Ω) on top → module pins 23/24.
  - D+/D− swap order between plug and module; one crossover at the plug-end vias.
  - Necked to 0.4 mm only at the module pins.
- Lengths: D+ 42.9 mm, D− 41.9 mm, excluding test-pad stubs and the R3 stub. The ~1 mm skew is negligible at USB full speed (12 Mbit/s).
- Stubs: TP8/TP9 (bottom, 1.45 mm) and R3 (DNP D+ pull-up, moved beside the plug-end D+ via, 1.4 mm). C1/C2 (DNP) sit on the connector side of R1/R2.

### Power
- VBUS 0.6 mm; +5V and 3V3_LDO 0.5 mm.
- +3V3 trunk 0.5 mm from the front to the module along the bottom corridor, then east under the SD resistor row and up the +x strip to pin 3 / C7 / C8.
- A 0.3 mm second +3V3 trunk, fed from TP1, runs along the −x edge to R4 and the SD switch (U6).
- SD_VDD 0.4 mm (0.3 mm inside the U6 group).

### Antenna keep-out
- The board ends at the antenna boundary. No copper, vias or parts under the antenna (it overhangs).
- GND stitching row 1 mm pitch just inside that edge.
- Bottom under the module core and top inside the pad ring are GND-only.

### Changes made for routing (schematic / placement / firmware proposal)
1. **SDMMC pins remapped** to follow the module edge order (any GPIO is allowed via the GPIO matrix — ESP-IDF SDMMC docs [V]): CLK IO13, CMD IO12, D0 IO14, D1 IO15, D2 IO10, D3 IO11, CD IO16. IO9 and IO21 are now NC.
2. **SD_PWR_EN moved IO2 → IO35** (module pin 31, beside U6). IO33–IO37 are octal-PSRAM pins only on modules with octal PSRAM; MINI-1-N8 has none (my reading of the module variants; not rechecked this stage) [U]. Don't swap to an octal-PSRAM module.
3. **SD bus layout:** resistors in columns at 1.0 mm pitch. Each column is the 10k pull-up (card side), then a via, then the 22 Ω series resistor, running straight to the module pins.
   - J3 pins escape to vias beside the socket and run in non-crossing bottom lanes. SD_VDD runs as a top lane beside J3.
   - R23 (CD pull-up) moved beside J3.
4. **U6 (TPS22917) group:**
   - QOD tied directly to VOUT (datasheet option 1, unchanged behaviour). The 0 Ω R24 is removed, and the QOD pin type is set to passive in the embedded symbol so ERC accepts the tie.
   - C14 changed to 10 µF **0402** (CL05A106MQ5NUNC, same part as C7). DC-bias derating at 3.3 V not checked [U].
5. **Board-ID divider** (R6/R7/C10) moved beside module pin 5 (IO1); TP10 is under it on the bottom. **R8/R4/C9** stay beside the −x pins.
6. TP symbols excluded from the BOM in the schematic, matching the PCB. All symbol fields are copied to the footprints.
7. `firmware/board_config.h` updated for items 1–2.

### Open / unverified
- **TPS22917 CT capacitor:** the schematic note says "CT to VIN"; recheck against the datasheet (I believe the usual connection is CT to GND) [U].
- Impedance: not checked by the fab's stack-up calculator (JLC impedance tool) [U].
- XKB plug seating and the plug notch: waiting for the STEP. Notch width is limited to about 9.8 mm by the leg slots.
- CD switch polarity; TS-1088 height; C7/C14 0402 10 µF derating.

### Rev B (to shorten the board)
- Evaluate the SD socket on the bottom, under the module.
- Evaluate solderable SD NAND (e.g. an LGA-8 part) instead of a socket. It drops the socket, card retention and CD.

Renders: `docs/img/routed-3d-top.png`, `routed-3d-bottom.png`, `routed-3d-iso.png`, `routed-copper.png` (top/bottom copper, pours shaded).
