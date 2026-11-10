# Rev B (short case): design notes (2026-11-10)

Branch `pcb-short`. Feasibility: `docs/revB-feasibility.md`. Nothing here is hardware-tested.

## 1. Main-rail soft-start: inrush with TLV76733 (rev A method, stage3-notes §3)

Limit: ≤ 50 µC charge at attach (10 µF × 5 V). Charge drawn by a ramp well below 100 mA isn't counted.

| | Rev A method | Result |
|---|---|---|
| **TLV767 alone (U3/C5 removed)** | Ramp: 0.5 ms internal soft-start [V TI SLVSE84D] → I = C·dV/dt = 44 µF × 3.3 V / 0.5 ms ≈ **0.29 A** (≫ 100 mA), so the charge counts. Q = 5 V × 4.9 µF + 3.3 V × 44 µF ≈ 24.5 + 145 ≈ **170 µC** | **fails** (3.4 × the limit) |
| **TLV767 + U3 TPS22917, CT 2.2 nF (rev A circuit)** | Ramp: tR ≈ 3.5 ms → ≈ 34 mA into 45 µF (≪ 100 mA). Fast charge = 5 × 4.9 + 3.3 × 4.7 (C4) ≈ **40 µC** | **passes** |

C_3V3 = 4.7 C4 + 10 C7 + 22 C6 + 1 C9 + ~0.6 decoupling + SD rail ~4.8 + 1 (rev A sum). C4 4.7 µF is within the TLV767 C_OUT range of 1–220 µF.

**Decision:** U3 and C5 stay as in rev A (TPS22917DBVR, SOT-23-6). The Stage A estimate (0.2 A) used too little capacitance and is superseded by this section.

## 2. Schematic changes (netlist diff vs rev A)

- **Method:** `tools/gen_sch.py` (spec) and `tools/sch_layout.py` (drawing). The generator reproduced the committed rev A sheet byte-identically before any edit. I then exported both netlists and diffed them.
- **ERC:** 0 (all severities).

| Change | Netlist effect |
|---|---|
| U1 USBLC6-2SC6 → **USBLC6-2P6** (SOT-666, C15999) | value, footprint, MPN/LCSC, symbol; pins and nets unchanged (same pinout) |
| U2 LDL1117 → **TLV76733DRVR** (WSON-6, C2848334) | value, footprint, MPN/LCSC, symbol, note; IN + EN = +5V, OUT + SNS = 3V3_LDO, GND ×2 + thermal pad = GND |
| U5 → **SN74LV1T125DCKR** (SC-70-5, C75925) | footprint, MPN/LCSC (same pinout) |
| C6 22 µF 0805 → **0603** (CL10A226MQ8NRNC, C59461) | footprint, MPN/LCSC |
| **Removed:** C1, C2, R3 (DNP reserves), SW2 (RESET), R12–R17 (0 Ω SD series) | 14 components gone; SD nets merged: `SD_x_C` → `SD_x` (6 nets removed, J3 + R22 pins renamed) |
| R18–R21 → **RN1** 4 × 10k array 0402×4 (4D02WGJ0103TCE, C25725) | new RN1: 1–4 = SD_CMD / D0 / D1 / D2, 5–8 = SD_VDD |
| **Kept:** U3, C5 | unchanged |

- **Totals:** components 61 → 48; nets 61 → 55; no other component, field or node changed.
- **Title block:** rev B. Block notes updated. Export: `docs/img/schematic-revB.pdf` / `.png`.

## 3. Placement (option 1 approved: SW1 on the bottom, hand-soldered)

History: with SW1 on top, the +x front half was at 93 % courtyard fill, so no fit. The user approved option 1: SW1 goes to the bottom, U6/C13/R11 stay, and a smaller switch is used only if needed (ask first). It wasn't needed.

- **Generator:** `tools/gen_pcb.py` (P dict; placement only, no tracks).
- **Board:** 18.3 × 31.9 mm, 2 layers. The antenna overhangs 5.1 mm (37.0 of 37.5 inner length).
- **Renders:** `docs/img/revB-placement-2d.png` (top and bottom courtyards), `revB-placement-3d-top.png`, `-bottom.png`, `-iso.png`.

**DRC (placement):**
- 0 courtyard, clearance or edge errors.
- 2 × `npth_inside_courtyard`: the J3 peg holes sit in the U4 courtyard (the free ring under the module). This is intended.
  - Before marking them as DRC exclusions, confirm from the J3 drawing that the pegs don't reach the top surface (1.6 mm board) [U].
- Silk clipped at the antenna edge (warning, same as rev A).
- 145 unconnected items (not routed yet).

**Courtyard fill** (rectangle intersections, computed from the board):

| Zone | Area | Fill | Parts |
|---|---|---|---|
| Top front zone, whole (y 6.8–16.1) | 170 mm² | **54 %** (rev A routed at 55 %) | |
| … −x half | 70 | 50 % | LED (D2, U5, R9, R10, C11, C12), SD switch (U6, C13, R11) |
| … centre channel (\|x\| < 1.65) | 31 | 37 % | U1, R1, R2 (+ USB pair, swap via) |
| … +x half | 70 | **66 %** | power (C3, U2, C4, C5, C6, U3), RN1, R23, R22, C14, C15 |
| Top strip +x beside module | 16 | 60 % | C10, R7, R6 (board ID), R5 (BOOT pull-up), C8, C7 (3V3 at pin 3) |
| Top strip −x | 16 | 30 % | R8, R4, C9, plus EN / TXD / RXD / SD_PWR_EN lanes (as rev A) |
| Bottom front (y 3.9–13.4) | 174 | 28 % | SW1, TP1–TP9 |
| Bottom strip +x beside J3 | 41 | 8 % | TP10 (rest = BOOT / +3V3 / SD_VDD lanes) |

**Routing plan behind the placement** (to be proven at routing; criteria unchanged):
- **USB:** J1 → U1 (ESD at the plug) → swap via (D+ hops once) → R1/R2 → module pins 23/24. Bottom under the pair: GND, apart from the swap via.
- **Crossings of the pair:** in the bottom band under the J1 pads (y ≈ 4–6.4, ~2.5 mm wide inside \|x\| < 5): +5V (F1 → power/D1), +3V3 (+x → U6/R4/R11) and SD_VDD (U6 → +x). That is 3 tracks, ~10–14 mm long. These are the longest bottom tracks; everything else on the bottom is short stubs.
- **SD:**
  - Module pins → a via row at y ≈ 15.85 (CD, D1, D0, CLK, CMD, then D3/D2 around the +x module corner).
  - Bottom tracks then run in x ≈ 3.5–5 under the J3 body and enter the contacts from −x, peeling in contact order.
  - SD_VDD enters J3 pin 4 from the +x strip.
  - RN1's pin order is mirrored relative to the module, so one pull-up (D1 or CMD) needs an extra via.
- **Bottom strip +x (x 7–9.1):** BOOT (SW1 → pin 4), +3V3 (→ pin 3), SD_VDD (→ J3.4). GND pour everywhere else.
- **Risk:** the +x half (66 %) and the 3-track band. If routing can't meet the criteria, the next step is a 4-layer proposal (needs approval), not a part change.

**Case clearance** (board frame: x across, +x = D1/TVS side; y from the board front edge = inner front face; heights use feasibility option 1, board bottom z 4.71):

| Item | Value | Clearance |
|---|---|---|
| Width | board 18.30 | case inner 18.54 → 0.12/side; **widen to ≥ 18.85** (unchanged) |
| Length | 31.9 + 5.1 antenna = 37.0 | 0.5 to the inner back wall |
| Module top (2.4) | z 8.71 | 1.60 to the lid (opt. 2: 0.90) |
| Bottom parts: J3 and SW1 (TS-1088-xR020, 2.0) | lowest face z 2.71 | 2.71 above the floor (opt. 2: 3.41) |
| Tallest other top part (U3/U6 SOT-23-6 ~1.45 [E]) | z ≈ 7.8 | under the module top |
| Plug shell 12.0 W (12.9 over latches) | front slot | widen 12.4 → ≥ 13.1 (unchanged) |
| Bottom under the plug | SW1 starts y 6.6; TPs y ≥ 7.65 | clear of the plug body if it straddles the board (seating [U] until the XKB STEP) |

**Case openings:**
- **BOOT pinhole: case floor at x +4.8, y 8.4.** The SW1 actuator faces the floor, 2.71 mm above it. Add a floor boss or guide tube so a 1 mm pin reaches it [E].
- **LED window: lid at x −3.3, y 8.25** (D2, top side, ~3.2 mm below the lid). Use a light pipe or thin window.
- **microSD slot: −x wall.** J3 body x −9.15…5.35, y 14.7…29.4, flush with the −x board edge. Slot ≈ y 16–28, at the J3 level under the board [E from footprint; confirm the card position from the drawing].
- **Test pads:** bottom, 1.0 mm pads.
  - Front grid 3 × 3 at 2.35 pitch (x ±2.5 / ±4.85 / ±7.2, y 8.7 / 12.0), plus TP10 at (8.05, 30.3).
  - Pogo access is with the case open (jig before casing). No pad sits under SW1 or J3.

## 4. Bring-up checklist (rev B)

- [ ] **Measure in-case temperature** (module shield + LDO) during a sustained transfer, lid closed, at room temperature and in a warm cabinet. The module is rated −40…85 °C ambient [V ESP32-S3-MINI-1 datasheet, Table 1-1]. The enclosure estimate is 67–80 °C inside the case at 40 °C ambient (`revB-feasibility.md`).
- [ ] Inrush at attach (USB current probe): fast charge ≤ 50 µC, ramp ≈ 3.5 ms.
- [ ] J3 and SW1 hand-soldered on the bottom (DNP-bottom in BOM/CPL): card insert/eject, card-detect polarity; BOOT through the floor pinhole (10 s pairing reset, download mode).
