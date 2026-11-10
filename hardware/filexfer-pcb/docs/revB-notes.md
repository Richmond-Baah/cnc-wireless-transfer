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
- 0 courtyard, clearance or edge errors. Silk clipped at the antenna edge (warning, same as rev A). Unconnected items: not routed.
- **J3 pegs** (2 × NPTH Ø1.0, x −5.13, y 19.0 / 27.0) sit in the U4 courtyard on purpose:
  - Peg Ø0.80 ±0.10 at 8.00 pitch [V SHOU HAN TF PUSH drawing Rev A/0, front view]. Peg length is not dimensioned; scaled from that view it is ≈ 0.6 mm [E], below the 1.6 mm board, so the pegs don't reach the top surface.
  - Hole edge to the nearest module pad: ≥ 0.90 mm (pads 29 / 32 / 41); no peg is under a module edge pad or a centre GND pad.
  - Recorded as 2 DRC exclusions in `filexfer-pcb.kicad_pro` by `gen_pcb.py` (`drc_exclude_j3_pegs`, rewritten on each regeneration because the UUIDs change).

**Placement corrections made for routing** (no other part moved; pinhole and LED positions unchanged):
- R1, R2 rotated 270°: connector-side pad toward U1, module-side pad toward pins 23/24.
- F1 rotated 270°: +5V pad inboard (y 4.0); VBUS_IN pad at the board edge, reached by a via between the F1 pads.
- R11 (SD_PWR_EN pull-up) moved from the front row into the −x strip next to module pin 31 (x −8.55, y 19.4).

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

**Case notes for the regeneration** (board frame: x across, +x = D1/TVS side; y from the board front edge = inner front face; heights use feasibility option 1, board bottom z 4.71):

| Item | Requirement |
|---|---|
| Inner width | **≥ 18.85** (board 18.30) |
| Inner length | board 31.9 + antenna 5.1 = 37.0, **plus 1–2 mm antenna-to-back-wall clearance → inner length 38.0–39.0** (case grows slightly; current 37.5) |
| Front plug slot | **≥ 13.1** wide (XKB shell 12.0, 12.9 over the latches) |
| microSD | **No slot: the card is sealed inside.** Solid −x wall, plus an internal retaining rib on the −x side at **y 16–28** (J3 body x −9.15…5.35, y 14.7…29.4, flush with the −x board edge) |
| BOOT | **Floor pinhole + guide boss at (+4.8, 8.4)**. SW1 (bottom, TS-1088-xR020, 2.0 tall) actuator faces the floor, 2.71 mm above it |
| LED | **Light guide at (−3.3, 8.25), 3.2 mm deep** (D2 top side to the lid) |
| Module top | 1.60 to the lid (opt. 2: 0.90) |
| Bottom parts (J3, SW1) | lowest face z 2.71 above the floor (opt. 2: 3.41) |
| Bottom under the plug | SW1 starts y 6.6, TPs y ≥ 7.65; plug seating still [U] until the XKB STEP |
| Test pads | bottom, 1.0 mm, grid 2.35 pitch (x ±2.5 / ±4.85 / ±7.2, y 8.7 / 12.0) + TP10 (8.05, 30.3); pogo access with the case open |

## 3b. 2-layer routing attempt: criteria not met → 4-layer proposed

Router: `tools/route.py` rewritten for rev B (scripted USB pair, plug power and pair crossings; A* for the rest; GND fanout, stitching, pours). Evidence: `docs/img/revB-route-attempt-2layer.png` (first pass, not kept on the board).

**What worked (scripted, DRC-clean geometry):**
- USB pair J1 → U1 → swap (D+ one bottom hop, 2.6 mm) → R1/R2 → pins 23/24; U1 GND via between the diverging pair.
- **Pair crossings, shortest possible:**
  - +5V crosses on **top**, on a 0.25 mm lane at y 4.15 in front of the J1 signal pads (no USB copper there; between the notch reserve and the pads). It taps U1 VBUS straight down between the pair.
  - +3V3 crosses on the bottom through the two J1 pad gaps (vias x ±2.25): **4.5 mm** under the J1 D−/D+ pads.
  - SD_VDD crosses on the bottom under the J1 pads: ≈ 10 mm.
  - VBUS_IN: J1.1 → via → bottom 6 mm → via between the F1 pads.

**What blocks 2-layer (geometry, not router quality):**
1. **SD escape.** 7 SD vias must fit in the 1.45 mm band between the RN1/C14 pads (y 15.35) and the module pad row (y 16.8).
   - On the bottom, the J3 shield pad (x 4.12–5.62, y ≤ 16.0) sits right under the D0/CLK vias. J3 must move 0.5 mm forward, which leaves the pegs 0.4 mm from the module pads.
   - The 7 lines then run as a ribbon on the bottom under J3, 4–12 mm each. That ribbon crosses under the module's signal pad row (no reference there) and cuts the bottom pour under the module.
2. **+x strip (pins 3 / 4 / 5 = +3V3 / BOOT / BOARD_ID).** Module pad edge to strip 0402 pad is 0.83 mm, so no 0.6 via fits beside a part, and the six strip parts leave no gap.
   - BOOT and +3V3 must come from the front on the bottom strip (≈ 15 mm each, beside the J3 contact row) and need a via gap. One strip part has to leave (e.g. C7 10 µF to the −x strip; C8 100 nF stays at pin 3, C6 22 µF at U3).
3. **Result of the first pass:**
   - 14 of the 23 auto-routed nets incomplete (SD_D0 / D1 / CD, +3V3, SD_VDD, SD_CT, SD_PWR_EN, LED_DATA_3V3, U0TXD / U0TXD_MOD / U0RXD, EN, BOOT, BOARD_ID); the scripted USB and VBUS_IN nets are complete.
   - 7 top GND pour islands with no via spot (front zone and strip).
   - Completing it needs the ≥ 9 long bottom tracks above, which fails "bottom mostly GND (tracks short)" and puts the SD lines and the module front edge without a continuous reference.

**Criteria (2-layer):**
- **DRC clean:** not reached (incomplete).
- **GND under module pads:** yes (top ring and EPAD vias).
- **GND under the USB pair:** yes except the swap.
- **GND under the SD lines:** no (bottom ribbon under the module edge).
- **No tracks under the antenna:** yes (off board).
- **Bottom mostly GND, tracks short:** no.
- **0402 where possible:** yes.

**Proposal (needs approval): 4 layers, GND on L2.**
- **Layers:**
  - L1: parts, short signals, GND pour.
  - L2: solid GND, no tracks.
  - L3: the pair crossings (+5V, +3V3, SD_VDD), the SD ribbon, BOOT / +3V3 / BOARD_ID to the strip, EN / TXD / RXD to the test pads.
  - L4 (bottom): J3, SW1, test pads, GND pour, stubs only.
- **What it fixes:** every routed signal has L2 directly under it (USB, SD, module edge), and the bottom stays almost pure GND.
- **Still needed on 4 layers** (via room is layer-independent): J3 0.5 mm forward, one strip part out of the +x strip, SW1 rotated 180° (BOOT pad toward the strip; same centre and pinhole).
- **USB pair:** must be re-dimensioned for L1 over L2 (JLC 4-layer prepreg ≈ 0.2 mm) [E].
- **Cost:** 4-layer costs more; quote at order time.

## 4. Bring-up checklist (rev B)

- [ ] **Measure in-case temperature** (module shield + LDO) during a sustained transfer, lid closed, at room temperature and in a warm cabinet. The module is rated −40…85 °C ambient [V ESP32-S3-MINI-1 datasheet, Table 1-1]. The enclosure estimate is 67–80 °C inside the case at 40 °C ambient (`revB-feasibility.md`).
- [ ] Inrush at attach (USB current probe): fast charge ≤ 50 µC, ramp ≈ 3.5 ms.
- [ ] J3 and SW1 hand-soldered on the bottom (DNP-bottom in BOM/CPL): card insert/eject, card-detect polarity; BOOT through the floor pinhole (10 s pairing reset, download mode).
