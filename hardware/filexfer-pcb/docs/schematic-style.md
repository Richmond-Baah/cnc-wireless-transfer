# Schematic style (rev A cleanup, 2026-11-10)

How the FileXfer schematic is drawn and why. I studied the published schematics below for their conventions and copied no circuit or artwork.

## References studied

| # | Design | Tool / format | Link |
|---|---|---|---|
| 1 | Espressif ESP32-S3-DevKitC-1 v1.1 | PDF, 2 sheets A4 | <https://dl.espressif.com/dl/schematics/SCH_ESP32-S3-DevKitC-1_V1.1_20220413.pdf> |
| 2 | SparkFun Thing Plus – ESP32-S3 (ESP32-S3-MINI-1) | KiCad 7, PDF letter | <https://github.com/sparkfun/SparkFun_Thing_Plus_ESP32-S3> (`docs/assets/board_files/SparkFun_Thing_Plus_ESP32-S3.pdf`) |
| 3 | Olimex ESP32-S3-DevKit-LiPo rev B | KiCad, PDF A3 | <https://github.com/OLIMEX/ESP32-S3-DevKit-LiPo> (`HARDWARE/ESP32-S3-DevKit-LiPo_Rev_B/ESP32-S3-DevKit-LiPo_Rev_B.pdf`) |
| 4 | TI TPS22917EVM (SLVUB64, Fig. 1) | PDF | <https://www.ti.com/lit/pdf/slvub64> |
| – | Espressif ESP32-S3-USB-OTG (tried; the download was refused, so not studied) | PDF | <https://docs.espressif.com/projects/esp-dev-kits/en/latest/_static/esp32-s3-usb-otg/schematics/SCH_ESP32-S3_USB_OTG.pdf> |

## Conventions observed

- **Block layout.** All four group the circuit into functional blocks, each with a frame and a large title:
  - Espressif: solid boxes ("USB to UART", "ESP USB"), plus a system block diagram on sheet 1.
  - SparkFun: dashed dividers ("USB OTG", "Voltage Regulation", "ESP32-S3").
  - Olimex: dashed blue frames with big italic titles ("Power Supply", "USB to UART", "Reset Button"…), all on one A3 sheet.
- **Signal flow** runs left to right. TI's EVM shows it most clearly: VIN → U1 → VOUT, with the CT/ON parts hanging below.
- **Power symbols.** Supplies point up (VBUS, +3V3 / VCC_3V3), GND points down. Every decoupling capacitor sits beside the pin it serves, with GND directly under it.
- **Net labels.** Signals between blocks use net labels; Olimex and SparkFun use them for every module GPIO. Wires stay inside a block. Label names describe the function (ESP_EN, CHIP_PU, USB_D+, LED_DATA).
- **Values.** Resistors and capacitors show reference and value next to the body. SparkFun and Espressif add the voltage and tolerance (e.g. "10uF/25V(20%)"); Olimex adds the package ("10k/R0402").
- **Unfitted parts.**
  - Espressif greys out not-fitted parts and appends "(NC)" to the value.
  - TI writes "DNP" as the value.
  - SparkFun prints the module variant note on the sheet.
- **Notes on the sheet.** Short design notes sit next to the circuit they explain:
  - SparkFun: differential-pair calculation; strapping-pin cautions.
  - Olimex: charge-current formula; VOUT divider formula; "+5V only" warnings.
  - Espressif: auto-program truth table.
- **Title block** is filled in: designer, date, revision, sheet name, and sheet x of y.

## Applied to FileXfer (`filexfer-pcb.kicad_sch`, A3, one sheet)

- **Blocks.** Seven titled dashed frames plus a notes frame:
  - USB input & protection
  - 3.3 V supply + soft-start
  - status LED + 3.3→5 V shifter
  - reset / boot / board ID
  - ESP32-S3 module
  - microSD + power switch (RX only)
  - test pads
  - general notes
- **Flow and power.** Signals run left to right. Supplies point up and GND points down. The one exception is J3 VSS, where a left-pointing GND symbol avoids crossing the card pins.
- **Wiring.**
  - T-junctions only; the generator rejects 4-way junctions.
  - Wires inside a block; local net labels between blocks. The net names didn't change, because they already describe their function.
  - Everything sits on a 1.27 mm grid, and the generator checks this.
- **Decoupling.** C7/C8 at the module 3V3 pin, C11 at U5 VCC, C12 at the LED VDD, C3/C4 at the LDO, C14/C15 at the SD switch output.
- **Unfitted parts.** DNP parts (C1, C2, R3) use KiCad's DNP attribute, which draws a red X and greys the text. The microSD block title says "RX ONLY", and its note says every part there is Fit=RX. R7's TX value is in the block note: the Value field must stay 100k to keep the netlist identical.
- **Notes per block:**
  - LDO dissipation
  - TPS22917 CT-to-VIN, tR and inrush calculation
  - SN74LV1T125 VIH check
  - EN RC and boot strapping
  - board-ID voltages
  - N8-only module
  - SD switch, pull-ups and card-detect polarity (unverified)
- **Title block:** FileXfer rev A, 2026-11-10, author Richmond Osei-Baah, sheet 1/1 "Main" and its contents.

### Choices made to keep the netlist identical

- **One sheet, not hierarchical.** KiCad prefixes local nets with the sheet path (`/USB/USB_DP_C`), so sub-sheets would rename every net and break PCB parity. A3 holds all the blocks legibly.
- **Sheet-local power symbols** (project library `filexfer_power.kicad_sym`: GND, +3V3, +5V, marked `power local`). Stock global power symbols would rename `/GND` → `GND`, `/+3V3` → `+3V3`, and so on. The local ones keep the existing names, so the schematic, the PCB and the netlist stay identical. Use these, not `power:`, for new parts on this sheet.
- **Fields.** Reference designators, values and fields are unchanged. Only field positions and visibility moved, and neither appears in the netlist.

### Verification (2026-11-10)

- **Netlist** (`kicad-cli sch export netlist`, before vs after, `tools/netlist_cmp.py`): 61 components with identical fields; 61 nets with identical names and nodes; libparts identical. The raw netlist from `(components` to the end is byte-identical. Only the header changed (export date, title block).
- **ERC:** 0 (all severities).
- **PCB:** DRC `--schematic-parity` gives 0 parity, 0 errors, 0 unconnected; the warnings are the same as before. The PCB file was not touched.
- **Review exports:** `docs/img/schematic-revA.pdf` and `docs/img/schematic-revA.png`.

### Master file

`filexfer-pcb.kicad_sch` is now the master: edit it in KiCad. `tools/gen_sch.py` (electrical spec) and `tools/sch_layout.py` (drawing) produced this layout once. Before writing, they trace the drawing and check every pin against the spec. `gen_sch.py` refuses to overwrite the sheet without `--force`. After any edit, export the netlist and compare it with `tools/netlist_cmp.py`, then run ERC and DRC with `--schematic-parity`.
