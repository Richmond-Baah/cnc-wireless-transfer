# Rev B (short case) — Stage A feasibility (2026-11-10)

Branch `pcb-short` (off `pcb`). Feasibility only: no layout or schematic change yet. Rev A (63 mm) stays archived on `pcb`.

Tags: [V] verified from a document; [E] my estimate; [U] unknown.

## Target

The `receiver_stacked` case. I took the inner size from `filexfer_case.py`, which reproduces the STEP: **18.54 W × 37.5 L × 10.31 H** [V]. The USB-A plug protrudes from the front, and the module antenna overhangs inside the case.

The case was built around a Super Mini, a USB-C→A adapter and an SD module. Its internals (adapter cradle, ledges, locators, lid hold-downs, pinhole and LED window) don't match rev B, so the case has to be regenerated from its parametric script.

## Verdict

- **Does not fit as rev A.** The rev A top-side parts other than J1 and U4 need 283 mm² of courtyard. A 31.5 mm board has 222 mm² free beside the plug and module. That is 127 % of the free area before any routing room.
- **Fits with the package and part-count changes below,** at 67 % courtyard fill in the front zone. Rev A routed at 55 % fill (283 mm² in 511 mm²). So 67 % is tight on 2 layers and comfortable on 4 layers.
- **Height** fits with 0.9–1.6 mm to spare.
- **Length** fits: 31.5 mm board + 5.1 mm antenna overhang = 36.6 mm, inside 37.5.

Sketch, checked for overlaps (top, bottom, side): `docs/img/revB-feasibility-sketch.png`.

## Length / area budget

- **Board:** 18.35 × 31.5 mm. Module body on the board from y 16.1 to 31.5 (courtyard from 15.65), antenna 31.5–36.6.
- **Plug zone:** y 0–6.86 (J1 courtyard). F1 and D1 sit beside the plug at |x| ≥ 6.78, as in rev A.
- **Front zone:** y 6.86–15.65 = 18.35 × 8.79 = 161 mm². Everything except the module-side 0402s goes here.
- **Strips beside the module:** 1.06 mm wide, 8 × 0402 as in rev A (C7, C8, R4, C9, R6, R7, C10, R8).
- **Bottom:**
  - J3 courtyard 16.41 × 17.29 at y 11.6–28.9. It ends 2.6 mm before the antenna edge, so the socket metal stays off the antenna base.
  - The slot faces the −x edge, as in rev A (the card is sealed in by the case wall).
  - 10 test pads in 2 rows × 5 at a 2.3 mm pitch (y 7.0–11.4, x −9.1…+1.2), clear of the U2 thermal vias.
  - Notch reserve x ±6.5, y 0–3.9 is kept on both layers (plug seating still [U] until the XKB STEP arrives).

## Part-by-part fit

| Item | Rev A | Rev B | Result |
|---|---|---|---|
| Plug landing / notch | J1 13.6 × 6.9; reserve x ±6.5, y 0–3.9 | same | fits; notch need still [U] (XKB STEP) |
| USB ESD U1 | USBLC6-2SC6, SOT-23-6 (14.6 mm²) | **USBLC6-2P6, SOT-666** (C15999, ST, 20k stock) ~5 mm² | change |
| Polyfuse F1 / TVS D1 | 1206 / SMF beside the plug | same place | fits |
| LDO U2 | LDL1117 SOT-223 (64.8 mm² + tab copper) | **TLV76733DRVR, WSON-6 2×2** (C2848334, 34k, $0.34; 1 A, 16 V, Tj 125 °C; thermal pad → GND) ~7 mm² | **required**; the SOT-223 alone would take 29 % of the free area |
| Main-rail soft-start U3 + C5 | TPS22917 SOT-23-6 | **removed** (decision): TLV767 internal 500 µs soft-start + fold-back current limit [V DS]. Inrush ≈ 32 µF × 3.3 V / 0.5 ms ≈ 0.2 A [E]; check against USB inrush limits | decision |
| SD switch U6 + C13, R11, C14, C15 | TPS22917 SOT-23-6 | kept | fits |
| SD series R12–R17 (0 Ω) | 6 × 0402 | **removed** (direct routes, ~5 mm module → vias → socket) | required |
| SD pull-ups R18–R22, CD R23 | 6 × 0402 | **RN1 4 × 10k array 0402×4** (C25725) + 2 × 0402 | required |
| LED buffer U5 | SN74LV1T125 SOT-23-5 | **SN74LV1T125DCKR, SC-70-5** (C75925, 1.5k stock: low; alternative DCKR-TP C50314337) | change |
| LED D2 + C12, R9, R10, C11 | — | same parts | fits |
| Buttons | SW1 BOOT + SW2 RESET, TS-1088 (21.9 mm² each, 2.0 mm tall) | **SW1 BOOT only** (decision); RESET = replug. The EN RC (R4/C9) stays. BOOT is still needed for the 10 s pairing reset and download mode | decision |
| Board ID R6/R7/C10 | strip beside the module | same | fits |
| C6 bulk | 22 µF 0805 | 22 µF 0603 (C59461, Basic) | change |
| DNP reserves C1, C2, R3 | 3 × 0402 | **removed** | required |
| microSD J3 | top, between the LDO area and the module | **bottom, under the module, hand-soldered; DNP in the JLC BOM** | as requested |
| Test pads | bottom | bottom, beside J3 (front of it) | fits |

- **Fill:**
  - Without the two decisions (keep SW2 and U3): 91 % → does not fit.
  - With them: 67 %.
  - Also dropping U6 (no SD power cycling, not recommended) would give 56 %, which is comfortable on 2 layers.
- **Firmware impact of the changes:** none. EN, BOOT, LED, SD pins and SD_PWR_EN are unchanged; R7 / board ID is unchanged.

## Height stack (current case: plug axis fixed at z 6.66 by the front slot)

| | XKB face option 1 (plug centre 0.35 above board top) | option 2 (0.35 below) |
|---|---|---|
| Board bottom / top | 4.71 / 6.31 | 5.41 / 7.01 |
| Module top (2.4) | 8.71 → **1.60 clear** to the lid | 9.41 → **0.90 clear** |
| SW1 top (2.0) | 8.31 | 9.01 |
| microSD bottom (2.0, bottom side) | 2.71 above the floor | 3.41 |
| Plug shell (4.5) | 4.41–8.91 | same (fixed by the case) |

- **Stack:** microSD + PCB + module = 6.0 mm, against 10.31 inner → fits. The board needs side ledges at z ≈ 4.7–5.4, since the SD socket isn't load-bearing.
- **Thinner case:** a rev B case could drop to about 7 mm inner by lowering the plug axis to sit just above the SD socket [E].

## Case changes needed (outside the PCB, for the case regeneration)

- **Inner width ≥ 18.85.** 18.54 leaves 0.1 mm per side for an 18.35 board. The board can't be narrower, because the 0402 strips beside the module need the 1.06 mm.
- **Front plug slot 12.4 → ≥ 13.1.** XKB shell is 12.00 wide, 12.90 over the latches [V drawing]; where the latches sit is [U].
- **Plug protrusion:** the board front edge sits at the inner front face, so the plug protrudes 14.86 − 1.0 ≈ 13.9 mm. That is more than the ~12 mm mating depth → fine.
- **New internals:** ledges, lid hold-downs, BOOT pinhole over SW1 (x ≈ −6, y ≈ 8.8), LED window over D2 (x ≈ +7.2, y ≈ 13.8), SD card retaining rib on the −x wall.

## LDO and enclosure temperature [E]

- **LDO dissipation:** (5.0 V − 3.3 V) × 0.25 A = 0.43 W (0.49 W at 5.25 V).
- **Enclosure:** outer 20.9 × 39.9 × 11.9 mm, 31.2 cm². Natural convection + radiation h ≈ 10–15 W/m²K → **21–32 °C/W** case to ambient. I gave no credit for heat leaving through the USB plug into the host.

| Load (average) | Total heat | Board / internal | TLV767 Tj (ΨJB 40.8 °C/W [V] + 3–8 °C local) |
|---|---|---|---|
| Sustained transfer 0.25 A | 0.83 W module + 0.43 W LDO = 1.26 W | +27–40 °C → **67–80 °C** at 40 °C cabinet | **≈ 88–106 °C** (limit 125 °C) |
| Typical 0.10 A (Wi-Fi idle + bursts) | 0.50 W | +11–16 °C → 51–56 °C | ≈ 60–65 °C |

- **The package change costs nothing thermally.** A SOT-223 LDL1117 (θJC 15 °C/W, tab = VOUT, little tab copper on this board) lands within a few °C of the same Tj. The **enclosure dominates**.
- **Bigger risk: the module, not the LDO.** ESP32-S3-MINI-1-N8 is rated to an 85 °C ambient [U, check DS]. Sustained full-rate transfer in a 40 °C cabinet puts the inside of the case at 67–80 °C.
- **Mitigations:** case vents (`vents.py` exists), a duty / TX-power limit in firmware, or a slightly larger case. Bench-measure in the case before committing.
- **Comparison with rev A:** rev A's 80–83 °C estimate treated the cabinet as the local ambient, without enclosure rise, so the two estimates aren't directly comparable.

## Assembly plan (as requested)

- **JLC Economic PCBA, top side only:** every SMD part on the top.
- **Hand-soldered by you:**
  - J1 plug (already DNP).
  - J3 microSD on the bottom. Mark it **DNP-bottom** in the BOM/CPL: its 9 contact pads (0.70 × 1.60 at 1.10 pitch) and 4 shell pads are reachable at the socket edges per the drawing; full hand-solderability is [U] until test-fit.
- **Economic-tier limits** (minimum board size, rail requirement for an 18 × 31.5 mm board) are [U]. Check at order time; the rev A rails note (mouse-bites, not V-cut) still applies.
- **Extended-part loading fees:** TLV767, USBLC6-2P6, SN74LV1T125DCKR, TPS22917, RN1 and the plug-side parts.

## Recommendation for Stage B

1. **Accept the required changes:**
   - LDO → TLV76733DRVR
   - USBLC6-2P6
   - SC-70 buffer
   - 22 µF 0603
   - drop R12–R17
   - drop C1/C2/R3
   - pull-ups → RN1 array
2. **Decide:** drop SW2 (RESET) and U3/C5 (main-rail soft-start, after an inrush check). Both are needed to fit.
3. **Prefer 4 layers** (inner GND/power). It eases the 67 % fill and gives the SD-under-module signals and the module GND a solid reference. 2 layers is possible but borderline.
4. **Before layout:** get the XKB U217 STEP (seating face → board height, notch) and the TS-1088 3D model; regenerate the case for rev B.
