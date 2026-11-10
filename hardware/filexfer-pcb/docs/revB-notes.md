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
- **Board:** 18.3 × 31.9 mm, 2 layers. The antenna overhangs 5.1 mm (37.0 of 37.5 inner length). **Now 34.9 mm, see §3c.**
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

**Case notes for the regeneration** (updated 2026-11-10 for the pass-3 board, 18.3 × 34.9 mm; board frame: x across, +x = D1/TVS side; y from the board front edge = inner front face; heights use feasibility option 1, board bottom z 4.71):

| Item | Requirement |
|---|---|
| Inner width | **≥ 18.85** (board 18.30). The J3 body is flush with the −x board edge (B.Fab x −9.15); its silk outline sits 0.2 mm outside the board and is not printed |
| Inner length | board **34.9** + antenna 5.1 = 40.0, **plus 1–2 mm antenna-to-back-wall clearance → inner length 41.0–42.0** (current case 37.5: must grow 3.5–4.5 mm) |
| Front plug slot | **≥ 13.1** wide (XKB shell 12.0, 12.9 over the latches) |
| microSD | **No slot: the card is sealed inside.** Solid −x wall, plus an internal retaining rib on the −x side at **y 18.5–30.5** (J3 body x −9.15…5.35, **y 17.2…31.9** after the +3.0 / −0.5 mm shifts, flush with the −x board edge). Card insert/eject only with the case open |
| BOOT | **Floor pinhole + guide boss at (+4.8, 8.4)**. SW1 (bottom, TS-1088-xR020, 2.0 tall) actuator faces the floor, 2.71 mm above it |
| LED | **Light guide at (−3.3, 8.25), 3.2 mm deep** (D2 top side to the lid) |
| Module top | 1.60 to the lid (opt. 2: 0.90) |
| Bottom parts (J3, SW1) | lowest face z 2.71 above the floor (opt. 2: 3.41) |
| Bottom under the plug | SW1 starts y 6.6, TPs y ≥ 7.65; plug seating still [U] until the XKB STEP |
| Test pads | bottom, 1.0 mm: TP1 +3V3 (2.5, 12), TP2 GND (−7.2, 12), TP3 BOOT (7.2, 12) (rev B pass 3, §3d); pogo access with the case open. No UART pads: flashing is over the USB plug (hold BOOT through the pinhole while plugging in), so the case can stay closed |
| Module | centre (0, 27.2); antenna end at y 40.0 |

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

## 3c. 2-layer routing, pass 2 (user: stay 2-layer, free via room)

This section supersedes the §3 / §3b numbers where they differ. Its result (6 GND pockets) is superseded by §3d.

**Changes:**
- **Board +3.0 mm:** 18.3 × 34.9 mm (DL = 3.0; module centre y 27.2). Antenna overhang 5.1 mm, so the board plus antenna is 40.0 mm. **Case inner length 41–42 mm** (1–2 mm antenna clearance).
- **Board-ID divider removed** (R6, R7, C10, TP10; IO1 = no connect). Board rev and role go in factory NVS (`firmware/board_config.h` `CONFIG_FILEXFER_BOARD_FX_REV_B`; backlog `stage2-parts.md` §5 items 4 and 7). The TX build no longer needs the R7 value override; TX and RX still differ by the RX-only SD parts.
- **JLC 2-layer rules at standard price** [V jlcpcb.com/capabilities/pcb-capabilities, checked 2026-11-10]:
  - Via 0.45 / 0.2 drill (0.125 annular ring).
  - Clearance 0.127, hole clearance 0.2, hole-to-hole 0.25.
  - Written to the board and to `.kicad_pro` by `gen_pcb.py`.
- **The three approved placement changes:**
  - J3 moved 0.5 mm toward the plug.
  - C7 out of the +x strip.
  - SW1 rotated 180°.
- **Further placement changes needed to route** (flagged, not approved separately):
  - **−x front:** U6 at y 11.3; R11 between U6 and C13 (C13 rot 90); R4/C9 (EN RC) beside U6.1; R8 below the −x lanes; R9 at x −5.6. TP2/TP4/TP6 swapped so the test-pad order matches the lane order (TP6 = U0TXD at (−4.85, 12), TP4 = EN at (−7.2, 8.7), TP2 = GND at (−7.2, 12)).
  - **+x:** C7 (3V3 bulk) and R5 (BOOT pull-up) at the +x strip start (y 17 / 18). U3 moved 0.25 mm toward −x and C5 0.25 mm toward −x. Row order is C14, R22, C15, with C14/C15 at rot 270 so all SD_VDD pads sit on one line. C8 at rot 270.
- **Scripted routing** (`route.py`):
  - `mx_corner`: −x lanes in W→E order SD_PWR_EN, EN, RXD, TXD_MOD. SD_PWR_EN takes a 2.5 mm bottom hop at pin 31 to reach the outer lane.
  - `px_corner`:
    - +3V3 and SD_VDD cross under the RN1…C15 row on bottom lanes.
    - The U3 VIN↔ON tie runs along the board edge (0.2 mm, no via).
    - SD_VDD crosses the front on the bottom at y 10.45, under the U1→R1/R2 section of the USB pair (D+ already hops there).
  - LED: bottom hop. USB, plug power: as §3b. The −x +3V3 feed continues the y 5.4 bottom band under the J1 pads to a via beside C12.
  - D2 has its via in the J3 D2 pad. This is the hand-soldered bottom part, not a JLC-placed pad.
- **SD lines:**
  - CMD/CLK/D0/D1/CD: vias at y 19.2, then a grouped bottom ribbon at 0.35 pitch into J3 from −x. D2/D3 enter J3 from +x.
  - Lengths: D2 5.2, CLK 9.6, D0 12.0, D3 12.1, CMD 12.6, CD 14.6, D1 17.5 mm (D1 includes its pull-up hop).
  - 20 MHz fallback noted in firmware (`BOARD_SD_FREQ_FALLBACK_KHZ`, backlog item 7).

**Result (pass 14):** evidence in `docs/img/revB-routed-2d.png`, `revB-routed-3d-top.png`, `-bottom.png`, `-iso.png`.
- **Routing:** every net routed (272 tracks, 110 vias, 66 of them GND).
- **DRC** (kicad-cli, project rules, schematic parity):
  - 0 errors and 0 parity issues.
  - 4 silk warnings at the antenna edge, as rev A.
  - 2 J3 peg exclusions.
  - **6 unconnected items, all GND pour pockets** (listed below).
- **Bottom copper:** 185 mm of track on the bottom (top 249 mm).
  - Longest: SD_VDD 26 mm (front crossing 9 mm, plus the J3.4 lane 12 mm), SD ribbon (above), +3V3 12 mm, U0RXD 11 mm, EN 9 mm, BOOT 8 mm.

**Criteria:**
- **DRC clean:** no. 0 violations, but 6 unconnected GND pockets.
- **GND under module pads:** yes.
- **GND under the USB pair:** yes, except the D+ swap hop and the SD_VDD crossing at y 10.45 (0.3 mm track, stitching vias each side).
- **SD lines (relaxed rule):** grouped ribbon. Stitching comes from the 2 mm grid and module-ring vias; spacing along the ribbon not checked per segment. Not met on the +x side: D2/D3 run beside BOOT/+3V3/SD_VDD lanes without GND between them.
- **No tracks under the antenna:** yes.
- **Bottom mostly GND, short tracks:** no in the +x strip. Five parallel bottom lanes run at x 6.6–8.6: D2, D3, BOOT, +3V3, SD_VDD.
- **0402:** yes.

**What still blocks (the 6 GND pockets, each with no legal via spot):**

| Pocket (top unless noted) | Enclosed by | Why no via fits |
|---|---|---|
| C7.2 + U4.63 (module corner GND), x 4.7–8.1, y 16–21 | D0 pull-up track, D2/D3 pull-up tracks, BOOT strip lane, module pin row; the D3 stub (pin 15 → via) seals the module corner | Bottom underneath is slivered by the D2/D3/BOOT lanes and the CMD hop (gaps 0.3–0.47 mm; a via needs 0.70 mm) |
| C15.2 (SD_VDD bulk GND) | R22, the +3V3 via and track to C7, board edge | +3V3 and SD_VDD bottom lanes are 0.3 mm apart below it |
| J3 shield pad (bottom, 4.87, 17.7) | D1 and CMD pull-up hops, plus the SD via row at y 19.2 (0.4 mm gaps) | Top above is the C7.2 pocket (also isolated) |
| U6.2 (SD switch GND) | U6.1 / U6.3 pads, board edge, +3V3 track under the U6 body (feeds R11/C13) | 0.39 mm between pad and track |
| R9.2 (LED pull-down GND) | SD_CT track (west) and LED_DATA_3V3 track (east) | Bottom underneath: U0RXD (x −5.7) and U0TXD (x −4.85) lanes 0.7 mm apart |
| TP2 (bottom GND test pad) + 1 via | EN, U0RXD, +5V bottom hop | Top above is the U6.2 pocket |

The root cause is the same in both corners. The +x and −x corners each have to pass 4–5 nets between the front zone and the module (+x: D2, D3, BOOT, +3V3, SD_VDD; −x: SD_PWR_EN, EN, RXD, TXD, plus U6/C13/R11/LED). On 2 layers those nets take both layers locally, so the GND pockets between them cannot be stitched.

## 3d. 2-layer routing, pass 3: every GND pin on main GND

User requirements: every GND pin connects to main GND; stay 2-layer; drop the UART/EN pads; 0 Ω jumpers allowed where needed.

**Schematic** (`gen_sch.py`; ERC 0; 37 parts):
- **R8 removed.** TXD0/RXD0 (pins 39/40) are no-connect.
- **Test pads now 3:** TP1 +3V3 (2.5, 12), TP2 GND (−7.2, 12), TP3 BOOT (7.2, 12), all bottom.
  - Removed: EN, U0TXD, U0RXD and also the +5V and USB D+/D− pads. The USB pads are gone so the pair has no stubs.
  - Flashing is over USB in download mode (hold BOOT, plug in). Reset is a replug. RX logs go over Wi-Fi (backlog item 1).
- **RN1 pins reassigned** W→E to D1, D0, CMD, D2, matching the module pin order. All four pull-ups now run on top with no crossings, which removed the D1/CMD bottom hops that sealed the J3 shield pocket.

**Placement:**
- R4/C9 (EN RC) moved to the end of the EN lane (−7.25 / −6.25, 17.3).
- C14 (10 µF) moved to the U6 output (−4.0, 10.2). C15 (100 nF) now sits next to the card at (6.55, 14.75). x 8.55 in the row is left empty.

**Routing:**
- **−x:**
  - EN takes the inner lane to R4/C9. SD_PWR_EN takes the outer lane (2.5 mm bottom hop at pin 31).
  - +3V3 reaches U6.1 from the y 5.4 band, then runs a 7 mm bottom lane at x −8.2 to C13/R11 and R4.
  - U6.2 has a GND via in the U6 body. R9.2 has its own GND via.
- **+x:** as §3c, plus GND vias for C15.2 (6.45, 15.8) and C7.2 / U4.63 (6.05, 17.9).
- **SD_VDD crossing** moved under the J1 pads (y 5.975, beside the +3V3 band). The bottom under U1 → R1/R2 → module is now GND except the D+ swap hop.
- **D2 via** moved out of the J3 D2 pad, to (6.10, 21.71) in front of it. D3 via moved to (5.70, 21.25). No via sits in any SMD pad.
- **All 115 vias tented both sides** (JLC standard).

**Result (pass 16):**
- **DRC** (kicad-cli, project rules, schematic parity):
  - **0 errors and 0 unconnected items.** Parity 0.
  - Warnings: only the 4 silk-at-antenna-edge ones (as rev A). 2 J3 peg exclusions.
- **GND pins:** all 63 GND pads are in the main GND connectivity cluster (KiCad connectivity from U4.65; ratsnest 0).
  - That includes U6.2, U4.63, C7.2, C15.2, R9.2, the J3 shield pads, J3.6, TP2 and the J1 shield.
  - One 0.23 mm² top fill sliver sits beside C7.2. It touches only that pad, which reaches its via by a track.
- **0 Ω jumpers added: none** (rerouting was enough).
- **Copper:** 258 tracks, 115 vias (78 GND).
  - Bottom track 158 mm (top 233 mm).
  - Longest bottom runs: SD_VDD 34 mm (crossing under the J1 pads 20 mm, plus the J3.4 lane 12 mm), +3V3 19 mm (y 5.4 band, x −8.2 lane, +x 2.8 mm crossing).
  - SD ribbon: CD 14.6, D1 13.2, D0 12.0, D3 11.6, CLK 9.6, CMD 7.3, D2 6.1 mm.
- **Renders:** `docs/img/revB-routed-2d.png`, `revB-routed-3d-top.png`, `-bottom.png`, `-iso.png`.

**Criteria:**
- **DRC clean:** yes.
- **GND under module pads:** yes.
- **GND under the USB pair:** yes, except the D+ swap hop.
- **SD (relaxed rule):** grouped ribbon on the bottom. D2/D3 still run beside the BOOT/+3V3/SD_VDD lanes on the +x side.
- **No tracks under the antenna:** yes.
- **0402:** yes.
- **Bottom mostly GND:** the +x strip carries 5 lanes. SD_VDD is long, and the crossing band under the J1 pads holds +3V3 and SD_VDD.

## 3e. Design review (kicad skill), 2026-11-10

Separate check of the pass-16 board (committed in 3b55b02) and the regenerated fab files. Analyzer JSON is kept out of the repo.

**Ran:**
- `analyze_schematic.py`, `analyze_pcb.py --full --schematic`, `cross_analysis.py`, `analyze_gerbers.py` (on `fab/rev-B/gerbers`).
- kicad-cli DRC and ERC.
- Manual geometry checks: pad-to-edge distances, GND connectivity per pad, via-in-pad, tenting, zone pad connection.
- An independent gerber raster: `fab/rev-B/gerber-check.png`, drawn from the gerber/drill files only by a small parser, not by KiCad.

**Not run / limits:**
- **EMC analyzer:** the `emc` skill is not installed.
- **Thermal analyzer and lifecycle audit:** both fail on missing skill modules (`datasheet_types`, `datasheet_extract_cache`).
- **SPICE:** no simulator is installed.
- **No `datasheets/` folder.** Pin checks rely on the earlier stage notes' datasheet citations, plus one new check: **TLV76733 against TI SLVSE84D Table 5-1** (pin 1 OUT, 2 SNS → OUT, 3 / 5 GND "all ground pins must be grounded", 4 EN = IN allowed, 6 IN, pad → GND), which matches.
- No prior skill review exists to diff against.

**Results:**
- **Blockers: none.**
- **DRC:** 0 errors, 0 unconnected, 0 parity. **ERC:** 0.
- **Gerbers:** complete. 9 layers + PTH/NPTH, 18.35 × 34.95 mm, 115 vias at 0.2 mm, 4 NPTH at 1.0 mm (J1 legs and J3 pegs), 2 PTH at 1.0 mm (J1 shell).
- **Tenting:** no via mask openings on either side. B.Mask opens only J3, SW1 and TP1–3.
- **BOM/CPL:** J1, J3, SW1 and the TPs are absent from both builds; all placements are top-side (single-sided assembly).
  - RX: 31 placements, 19 lines. TX: 23 placements, 17 lines (no U6/C13/C14/RN1).

**Findings triaged:**

| Analyzer finding | Verdict |
|---|---|
| VM-001 LED_DATA_3V3 3.3 → 5 V without a shifter | False positive: U5 SN74LV1T125 at VCC 5 V is the translator [stage4-notes, TI SCLS745D] |
| UC-001 / UC-002 no VBUS decoupling / ESD at J1 | False positive: they sit after F1 on +5V (C3 4.7 µF, C11, C12; D1 SMF5.0A, U1 VBUS) |
| PM-002 C8 / C7 / R5 / U3 / U6 / F1 / D1 "0.0–0.35 mm from edge" | Body/courtyard measure. **Pad copper ≥ 0.29 mm** from the edge (C8 0.29, U4 0.30, F1 0.33, U6 0.35); DRC edge rule 0.25 passes; JLC routed-edge copper minimum 0.2. Order with rails (§5) |
| FD-001 no fiducials | As rev A: fiducials go on the JLC edge rails |
| BV-001 via 0.48 mm from edge (SD_PWR_EN at x −8.675) | Via copper 0.255 mm from the edge, which passes the 0.25 rule |
| TB-001 0402 tombstoning (medium) | GND pads use thermal relief (0.3 mm spokes, 0.25 gap) except U4 and J1 SH; accepted |
| GR-004 B.Paste 15 flashes | B.Paste covers the hand-soldered J3/SW1/TP pads only; no bottom stencil is needed (DNP-bottom) |
| TE-001 test coverage 6 % | By decision (§3d) |
| DA-001 3V3_LDO: 1 cap for 2 ICs | C4 4.7 µF sits between U2.OUT and U3.VIN (1.3 / 1.9 mm); accepted |
| J3 has no ESD | As rev A: the card is sealed inside the case (no slot) |

**Manual findings:**
- **USB pair:** D+ 14.8 mm, D− 14.3 mm (0.5 mm skew, one via pair on D+). That is negligible at full speed (12 Mbit/s).
- **SD_VDD:** 0.3 mm wide, 42 mm total, about 0.07 Ω on 1 oz copper.
  - That is about 14 mV at a 200 mA card peak, plus the TPS22917 R_ON. The current capacity is far above the card's needs.
  - **Accepted for card current.**
- **U6 input cap (suggestion, not changed):**
  - U6.VIN (+3V3) has no local bulk cap. The nearest are C13 (1 nF, CT, 3.7 mm) and C6 (22 µF, 9.6 mm via the y 5.4 band and the x −8.2 lane).
  - The SD rail ramps slowly (CT 1 nF, tR ≈ 1.6 ms → ≈ 21 mA into C14), and C14 10 µF carries card transients, so this is low risk.
  - Optional for rev C: add a 1 µF at U6.1.

## 4. Bring-up checklist (rev B)

- [ ] **Measure in-case temperature** (module shield + LDO) during a sustained transfer, lid closed, at room temperature and in a warm cabinet. The module is rated −40…85 °C ambient [V ESP32-S3-MINI-1 datasheet, Table 1-1]. The enclosure estimate is 67–80 °C inside the case at 40 °C ambient (`revB-feasibility.md`).
- [ ] Inrush at attach (USB current probe): fast charge ≤ 50 µC, ramp ≈ 3.5 ms.
- [ ] J3 and SW1 hand-soldered on the bottom (DNP-bottom in BOM/CPL): card insert/eject, card-detect polarity; BOOT through the floor pinhole (10 s pairing reset, download mode).
- [ ] **No UART pads on rev B.** Flash over USB: hold BOOT, plug in (ROM download mode), `idf.py -p <port> flash`; replug to run. Logs: TX over the USB-Serial-JTAG secondary console, already enabled (`transmitter/sdkconfig:1302-1303`, `CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y`; primary stays UART0). It is in `sdkconfig` only, not `sdkconfig.defaults`, so a regenerated sdkconfig would lose it (backlog §5.9). RX: debug build with TinyUSB MSC+CDC (backlog §5.10), Wi-Fi logs (§5.1). A wire to module pin 39 (TXD0) is a last resort only.
- [ ] Pogo/probe pads: TP1 +3V3, TP2 GND, TP3 BOOT only (bottom, case open).

## 5. Before ordering (rev B checklist)

1. **Fab files:** `fab/rev-B/` (gen_fab.py on the committed board, 2026-11-10). Upload `filexfer-revB-gerbers.zip`, and use the BOM/CPL for one build (RX or TX) per order.
2. **Board spec:** 2 layers, 1.6 mm FR-4, 1 oz, 18.3 × 34.9 mm. Min track 0.15, clearance 0.127, via 0.45/0.2 (JLC standard tier, §3c). All vias tented (no plugging option needed).
3. **Rails:** order with JLC edge rails joined by **mouse-bites, not V-cut**, with fiducials on the rails. Pad copper sits ≥ 0.29 mm from the long edges (C8, U4, F1, U6). The economic-tier minimum board size is still [U]: check at order time.
4. **DNP / hand-solder:** J1 (USB-A plug, top), J3 (microSD, bottom) and SW1 (BOOT, bottom) are not in the BOM/CPL. Order them separately: plug U217-041N-4BV81 C319178, SW1 TS-1088-AR02016 C720477, socket SHOU HAN TF PUSH C393941 (LCSC field set on J3 in the schematic and board; DNP parts are left out of the JLC BOM). TP1–3 are bare pads.
5. **Check JLC's placement preview** for rotations: U2 (WSON-6), U1 (SOT-666), U5 (SC-70-5), RN1, D2 (WS2812B-2020 pin 1).
6. **Stock and price:** re-check every line. Extended parts: TLV76733 C2848334, USBLC6-2P6 C15999, SN74LV1T125 C75925, TPS22917 C2681320, RN1 C25725, WS2812B-2020 C52917434, SMF5.0A C2759874, BSMD1206 C883127, C1531, C1523.
7. **Case:** case notes in §3, updated for the 34.9 mm board. Board STEP with all parts: `fab/rev-B/filexfer-revB.step`. J1, J3 and D2 are placeholder boxes from `tools/gen_fp.py` (not vendor geometry; the J1 height split about the board is [U]). The case inner length must grow to 41–42 mm before the boards are useful in a case.
8. **The committed `filexfer-pcb.kicad_pcb` is the master** (the router isn't deterministic). Regenerate fab files only with `python3 tools/gen_fab.py`. After any design change: gen_sch → gen_pcb → route → DRC (`--schematic-parity`) → gen_fab → gerber check.
9. **Firmware before bring-up:**
   - `CONFIG_FILEXFER_BOARD_FX_REV_B` board config.
   - TX console in `sdkconfig.defaults` (stage2-parts §5 #9).
   - RX debug MSC+CDC build (§5 #10).
   - Factory-NVS board rev (§5 #4).
10. **Open:**
    - J3 peg length [U] (pegs into the module courtyard).
    - XKB plug STEP for seating.
    - Card-detect polarity (bench).
    - 0402 10 µF DC-bias derating (C7, C14).

