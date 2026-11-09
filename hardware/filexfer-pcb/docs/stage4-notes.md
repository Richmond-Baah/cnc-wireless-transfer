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

## 4. Placement (rev A, not routed) — `tools/gen_pcb.py` → `filexfer-pcb.kicad_pcb`
Frame: y from the plug board edge toward the antenna; x from the centreline, **+x = right, viewed from the top with the plug pointing away** (card slot is on the left, −x).
- Board **18.3 × 64.2 mm**, 2 layers, 1.6 mm; module antenna overhangs the far end by 5.1 mm (total 69.3); plug protrudes 14.86 mm past the front edge.
- All assembled parts on top; bottom = 10 test pads only (copper, not in BOM/CPL). J2 TC2030 is copper only.
- Order: plug (0–6.8) → USB ESD / LDO / soft-start / LED (6.8–20) → buttons + TC2030 (20–27.2) → SD socket (27.2–44.4) → SD passives, SD switch, board ID (44.5–48.4) → module (48.4–64.2) + antenna.
- **Notch reserve** x ±6.5, y 0–3.9: KiCad rule area on both layers (no tracks, vias, pour); only J1's own NPTH holes and leg slots are inside. F1/D1 sit beside it (|x| ≥ 6.78). Note: the leg slots are at x ±5.70 (pads to ±6.5), so a notch can be at most about 9.8 mm wide unless the legs are inside it [U, depends on the STEP].
- Module strips (1.15 mm between module courtyard and board edge): C7 10 µF 0402 + C8 by the 3V3 pin; R8 (TXD), R4/C9 (EN RC) on the other side.
- Rules (JLC 2-layer): 0.127 track/space min, default net class 0.2/0.15, via 0.6/0.3, copper–edge 0.25, hole–hole 0.5.
- DRC: 0 courtyard/clearance/edge errors; remaining = unrouted (163), 4 module silk outside the board edge (antenna overhang, clipped by the fab).
- Height (top / bottom of board): plug +1.90/−2.60 or +2.60/−1.90 [U face], module 2.4, SD 2.0, SOT-223 1.8, TS-1088 1.6; bottom: plug only. Stack ≤ 6.8 mm vs 7.3 inner.

### Case features (new positions; the old Super Mini case does not apply)
| Feature | x (mm) | y from plug board edge | Notes |
|---|---|---|---|
| LED (WS2812B-2020) | +3.6 | 8.2 | top, 0.85 mm tall |
| BOOT button SW1 | +5.25 | 23.6 | TS-1088-AR02016, 1.6 mm actuator top [U] |
| RESET button SW2 | −5.25 | 23.6 | same |
| TC2030 (J2) | 0 | 23.6 | case open to use |
| microSD card slot | left edge (−x) | body 28.4–43.2, card about 30.3–41.3 | socket front flush with the board edge; side-wall slot needed |
| Antenna | — | 64.2–69.3 | plastic only around it |
- Case inner length grows from 64.3 to about 70 mm. Dropping the TC2030 (UART/EN/IO0/3V3/GND remain as bottom test pads) would save about 3.5 mm.
- Placeholder 3D models: XKB plug and SD socket are boxes at datasheet dimensions (`tools/gen_fp.py`), not vendor geometry.
