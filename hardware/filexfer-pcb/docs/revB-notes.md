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

## 3. Placement: blocked (needs a decision)

**Board:** 18.3 × 31.9 mm. The module pad field is centred at y 24.2 and its courtyard starts at y 16.2. The antenna overhangs 5.1 mm, ending 0.5 mm before the inner case end.

**J3 on the bottom (works):**
- Rotation 90, origin (−5.13, 27.0); courtyard y 13.4–30.7.
- Contacts at x 5.87 under the module's SD pins, so the SD lines run module pad → via → contact in about 2–3 mm.
- The two J3 locating-peg holes (1.0 mm NPTH, x −5.13, y 19.0 / 27.0) fall in the free ring between the module's edge pads and its centre GND grid, 1.1 mm clear of both.

**Top front zone:** y 6.8–16.2 (9.4 mm) × 18.3 mm.
- The USB pair is fixed in the centre: J1 pins at x ±1, module pins at x 0 / −0.85. Every net that crosses sides must cross the pair.
- Crossing on the bottom under the pair breaks its GND reference (criterion 4). The only clean crossing is the bottom band under the J1 pads (y 5.1–6.4), which has room for about 3 thin tracks.

**Pure rectangle packing** (courtyards only, zero routing space; 12 × 0402 already moved into the module-side strips):

| +x half (70 mm²) | −x half | Result |
|---|---|---|
| power + SD (natural: module 3V3 and SD pins are on +x) | LED + BOOT | **+x 93 %: no fit** |
| power + LED | SD + BOOT | 83 % / 70 %; adds crossings (LED, SD_VDD) |
| power + BOOT | SD + LED | 85 % / 69 % |

**Groups:**

| Group | Parts | Courtyard |
|---|---|---|
| Power | U2, C3, C4, U3, C5, C6 | 37.9 mm² |
| SD | U6, C13, C14, C15, RN1, R23 | 27.8 mm² |
| LED | U5, C11, R10, D2, C12 | 20.7 mm² |
| BOOT | SW1 | 21.9 mm² |

**Why it doesn't work:**
- Every split leaves one half at ≥ 83 % courtyard fill before any routing. Rev A routed at 55 %.
- Filling the module-side strips with parts leaves no top corridor for TXD/RXD/EN/BOOT/+3V3. Those would run on the bottom under J3, which fails "bottom mostly GND, tracks short".
- **2-layer fails, and 4-layer does not fix it.** The limit is area, not layers.

**Options (need approval):**
1. **SW1 to the bottom, hand-soldered with J3.** BOOT pinhole moves to the case floor. Frees 21.9 mm²: halves ≈ 54 % / 68 %; 2-layer becomes plausible.
2. **Smaller BOOT switch** (part change, top side); frees ≈ 12–15 mm².
3. **Remove the SD power switch U6 / C13 / R11**: frees ≈ 19 mm². Loses SD power-cycling (firmware SD_PWR_EN).
4. 4-layer helps the crossings and the long nets, but only on top of 1–3.

## 4. Bring-up checklist (rev B)

- [ ] **Measure in-case temperature** (module shield + LDO) during a sustained transfer, lid closed, at room temperature and in a warm cabinet. The module is rated −40…85 °C ambient [V ESP32-S3-MINI-1 datasheet, Table 1-1]. The enclosure estimate is 67–80 °C inside the case at 40 °C ambient (`revB-feasibility.md`).
- [ ] Inrush at attach (USB current probe): fast charge ≤ 50 µC, ramp ≈ 3.5 ms.
- [ ] J3 hand-soldered on the bottom: card insert/eject, card detect polarity.
