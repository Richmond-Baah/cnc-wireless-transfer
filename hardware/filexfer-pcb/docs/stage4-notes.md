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
- All assembled parts on top; bottom = 10 test pads only (copper, not in BOM/CPL), the production pogo-jig target. TP2 (GND) at (−5.2, 8.4) and TP5 (BOOT) at (3.6, 21.5) since the review. TC2030 dropped (user, routing go-ahead).
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
2. **SD_PWR_EN moved IO2 → IO35** (module pin 31, beside U6). **Module is ESP32-S3-MINI-1-N8 only (IO35 used; no PSRAM variants)**, noted on U4 in the schematic/BOM. IO33–IO37 are octal-PSRAM pins on PSRAM variants. LCSC lists C2913206 as ESP32-S3FN8 (no PSRAM). The MINI-1U-N8 alternate is also N8.
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
- **TPS22917 CT capacitor:** CT to VIN is correct. TI SLVSDW8B pin table: "Connect capacitor from this pin to VIN" [V]. Recommended operating conditions: VCT, timing-capacitor voltage rating, 7 V. C13 (1 nF) and C5 (2.2 nF) are 50 V X7R, so OK [V]. Rise time ≈ 1.6 µs/pF at 3.6 V → C13 ≈ 1.6 ms, C5 ≈ 3.5 ms.
- Impedance: not checked by the fab's stack-up calculator (JLC impedance tool) [U].
- XKB plug seating and the plug notch: waiting for the STEP. Notch width is limited to about 9.8 mm by the leg slots.
- CD switch polarity; TS-1088 height; C7/C14 0402 10 µF derating.

### Rev B (to shorten the board)
- Evaluate the SD socket on the bottom, under the module.
- Evaluate solderable SD NAND (e.g. an LGA-8 part) instead of a socket. It drops the socket, card retention and CD.

Renders: `docs/img/routed-3d-top.png`, `routed-3d-bottom.png`, `routed-3d-iso.png`, `routed-copper.png` (top/bottom copper, pours shaded).

## 6. Pre-fab design review (kicad skill pass, 2026-11-10)
Analyzers run: schematic, PCB (`--full`, with schematic), cross-analysis. KiCad DRC (schematic parity on) and ERC.
**Not run:**
- SPICE: no simulator installed.
- EMC: no emc skill installed.
- Thermal and lifecycle scripts: crash on a missing module in the skill. Done by hand below.
- Gerber analysis: no gerbers yet.
- Datasheet sync: no `datasheets/` folder. Pin-level checks rely on the [V] items in stages 2–4.

**Fixed in this pass**
1. **Vias in SMD pads (~45, from my router).** Same-net SMD pads now block vias (`route.py blocked()`).
   - New scripted escapes: U3.6, U1.2 (between the pin rows), U3.2.
   - **TP5 (BOOT) moved** to the bottom at (3.6, 21.5), under SW1.1, with a scripted BOOT front link.
   - Left as-is: the scripted via between C2/C15 overlaps both pads' annulus. Its hole is outside C15; C2 is DNP.
   - Module EPAD vias sit in the gaps between the thermal pads.
2. **Tombstoning:** 0402 GND pads now keep thermal spokes. Only pads KiCad flags as starved go solid: C11.2, R7.2, R9.2 (`fix_starved`, using kicad-cli DRC). Each of these also has a fanout track.
3. **BOM:** every part now has an MPN and LCSC number (passives table in `gen_sch.py`). All in stock on JLC 2026-11-10.
   - Extended passives: C5 2.2 nF (C1531), R8 499 Ω (C4125).
   - TX variant: R7 22k = C25768.
   - Lowest-stock parts: TPS22917 C2681320 (2.2k), ESP32-S3-MINI-1-N8 (22.7k).

**DRC after the fixes:** 0 errors, 0 unconnected, 0 parity. Warnings: 4 module silk lines past the edge (antenna), 3 pour necks at the J1 shell. ERC 0.

**Findings and decisions (updated 2026-11-10, design archived)**
- **Single board, no panel.** **Order-time: request JLC edge rails with mouse-bites, not V-cut** (parts 0.07–0.22 mm from the long edges, C6 near the edge); the rails carry the fiducials. The board has no fiducials of its own.
- **LDO U2 tab copper (added).** 3V3_LDO spreader on both layers (`route.py LDO_CU`, priority-1 zones, solid pad connection):
  - Top: 21.8 mm² around the tab. Bottom: 18.4 mm² under it.
  - 4 thermal vias in a row beside the tab, at y 10.4 (not in the pad).
  - Other nets are kept out of the region. TP2 moved to (−5.2, 8.4).
  - The top spreader stops at x −7.6, leaving a GND corridor along the −x edge (without it the front GND pour was cut off).
- **LDO temperature (estimate [U]):** LDL1117 datasheet θJA = 120 °C/W (board not specified); operating Tj max 125 °C [V DS Table 3].
  - With the spreader I estimate ≈ 90–100 °C/W.
  - 0.43 W average → +39–43 °C → Tj ≈ 80–83 °C in a 40 °C cabinet.
  - 0.6 W sustained worst case → Tj ≈ 95–100 °C.
  - Before the spreader: ≈ 92 / 112 °C. Check on the bench (thermocouple on the tab).
- **SD socket C393941 = push-push [V].** SHOU HAN drawing TF PUSH Rev A (2020/07/07, page 1 of the LCSC PDF) shows the "PUSH IN" / "PUSH OUT" card positions and the latch cam. The JLC attribute "No Push-Push" is a listing error (LCSC's own title says "Connector and Ejector Push-Push"). Keep the case retaining rib. Same drawing: card-detect pad "Cd", height 1.80 min / 2.00 max.
- **0402 10 µF X5R 6.3 V (C7, C14):** DC-bias loss at 3.3 V not checked [U].
- **SD_CLK_C reference:** 80 % (bottom lanes cross top tracks under J3). Short net at ≤25 MHz; accepted.
- **Analyzer false positives:**
  - VM-001: LV1T125 is the level shifter; VIH 2.03 V max was checked in §0.
  - UC-001/002: VBUS protection (TVS D1, C3) sits after F1 on +5V by design.
  - J3 courtyard over the edge: the socket is flush with the edge.
  - U4 over the edge: antenna overhang.

## 7. Fab files (rev A, single board) — `tools/gen_fab.py` → `fab/rev-A/`
- Gerber X2 (9 layers: F/B Cu, Paste, Silkscreen, Mask, Edge.Cuts), Excellon PTH/NPTH (mm) + drill maps, `filexfer-revA-gerbers.zip`.
- BOM and CPL in JLC format for two builds of the same board:
  - **RX:** 47 placements, 24 lines.
  - **TX:** 29 placements, 22 lines; SD block not fitted, R7 = 22k (C25768).
  - J1 (plug) is DNP in both (hand-soldered). Test pads and the Fit=DNP parts (C1, C2, R3) are never placed.
- Gerber check:
  - `analyze_gerbers.py`: complete, 0 findings, 18.35 × 62.8 mm.
  - Independent raster of the copper/silk/mask/outline/drills: `fab/rev-A/gerber-check.png`. Matches the layout.
- Final DRC: 0 errors, 0 unconnected, 0 parity.
  - Warnings: 4 module silk lines past the antenna edge, 3 pour necks at the J1 shell (0.09 mm), 1 copper sliver on B.Cu (KiCad gives no location).
- **Track mitres (`tools/miter.py`, 2026-11-10):** run in place on the committed board. Nothing was rerouted, and parts, vias, pads and the USB D+/D− copper are unchanged.
  - 32 corners fixed: 30 right-angle bends cut to 45° mitres (23 at ≥ 2× track width), plus 2 short Z-jogs (SD_CMD_C at (6.23, 37.0), SD_CLK_C at (5.67, 37.7)) replaced by a single 45° diagonal.
  - 7 mitres are shorter than 2× width for lack of room: +3V3 at (−6.34, 41.9) and the SD_D0–D3 staircase at x 4.5–7.5, y 35.6–37.0.
  - 3 corners left as they are, each where two tracks meet at a pad centre (pad copper covers the corner): SD_VDD 90° at (−4.06, 42.25), SD_VDD 45° at (8.5, 41.47), SD_PWR_EN 45° at (−7.75, 43.71).
  - After the mitres, DRC is unchanged (0 / 0 / 0, same warnings), the zones were refilled, and the fab files were regenerated.
- CPL rotations are KiCad's. **Check JLC's placement preview** (SOT-23/SOT-223/module/LED orientation) when ordering.

## 7b. Schematic cleanup (2026-11-10)
- `filexfer-pcb.kicad_sch` was redrawn for readability only, with no electrical change: A3 single sheet, titled functional blocks, notes per block, filled title block.
- Conventions and references: `docs/schematic-style.md`. Review exports: `docs/img/schematic-revA.pdf` / `.png`.
- Netlist before vs after is identical (components, fields, net names and nodes). ERC 0; PCB parity 0; DRC unchanged.
- The `.kicad_sch` is now the master: `gen_sch.py` needs `--force` to overwrite it. Power symbols come from the project library `filexfer_power` (sheet-local, keeping the `/GND`-style net names).

## 8. Before ordering (checklist)
1. **Plug STEP / notch:** get the XKB U217-041N-4BV81 (C319178) STEP. Confirm seating, shell height above/below the board, and whether a board notch is needed (the reserve x ±6.5, y 0–3.9 is kept free; the leg slots limit a notch to about 9.8 mm).
2. **Sample test-fit:** hand-fit a plug and an SD socket on a bare board or a 1:1 print. Check the case fit, the SD retaining rib, the buttons and the LED window.
3. **Card-detect polarity:** the Cd switch sense isn't specified in the drawing. Bench-test with a card and set the firmware sense.
4. **Rails:** order with JLC edge rails joined by **mouse-bites, not V-cut**; fiducials on the rails.
5. **Re-check stock and prices** for every BOM line (JLC Basic/Extended may change). Lowest stock on 2026-11-10: TPS22917 C2681320 (2.2k), ESP32-S3-MINI-1-N8 C2913206 (22.7k). Extended passives: C1531, C4125.
6. **Module:** ESP32-S3-MINI-1-N8 only (IO35 used; no PSRAM variants).
7. **The committed `filexfer-pcb.kicad_pcb` is the master.** Regenerate only on a design change, because the router isn't deterministic and every rerun gives different copper. Regenerate in this order: `gen_sch.py` → `gen_pcb.py` → `route.py` → DRC (`--schematic-parity`) → `gen_fab.py`. Re-check DRC and refresh `fab/rev-A/` after every regeneration. For fab files only, run `gen_fab.py` on the committed board.
8. Choose the build: RX or TX BOM/CPL (one BOM per JLC order). TX firmware ignores BOARD_ID (stage2-parts §5 #7), so any rev A board, RX-populated included, can run TX firmware.
9. Open items: C7/C14 0402 10 µF DC-bias derating; LDO tab temperature on the bench; impedance via JLC's calculator if they offer controlled impedance.
