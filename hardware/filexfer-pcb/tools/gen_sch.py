#!/usr/bin/env python3
"""Generate the FileXfer rev B (short case) schematic from KiCad stock symbols (layout: tools/sch_layout.py).

Every symbol pin must be mapped to a net name or 'NC'; the script fails otherwise.
Run: python3 -I tools/gen_sch.py --force  (from hardware/filexfer-pcb; the .kicad_sch is the master now)
Then: kicad-cli sch erc filexfer-pcb.kicad_sch
"""
import os, sys, uuid
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sexp import parse, dump, find, unq, q, getsym, pins

PROJECT = 'filexfer-pcb'
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', PROJECT + '.kicad_sch')
ROOT_UUID = '6f1f5b8e-2c43-4f0e-9a51-0f11e0a0a001'
_ns = uuid.UUID('9b1f2a52-6a50-4f4c-8e43-31c1f0e0b001')

def uid(*k):  # deterministic UUIDs so regenerating gives a stable diff
    return str(uuid.uuid5(_ns, '/'.join(map(str, k))))

R0402, C0402, C0603, C0805 = ('Resistor_SMD:R_0402_1005Metric', 'Capacitor_SMD:C_0402_1005Metric',
                              'Capacitor_SMD:C_0603_1608Metric', 'Capacitor_SMD:C_0805_2012Metric')
TP = 'TestPoint:TestPoint_Pad_D1.0mm'
SOT236 = 'Package_TO_SOT_SMD:SOT-23-6'

# Fit: ALL = TX+RX, RX = receiver only (DNP on TX), DNP = never fitted (footprint reserve)
# (ref, lib, symbol, value, footprint, {pin: net}, extra fields)
# JLC/LCSC passives (stock + Basic/Extended checked on jlcpcb.com 2026-11-10; E = Extended)
PASSIVE = {
    (C0402, '100n'): ('CL05B104KO5NNNC', 'C1525'), (C0402, '1u'): ('CL05A105KA5NQNC', 'C52923'),
    (C0402, '10u'): ('CL05A106MQ5NUNC', 'C15525'), (C0402, '1n'): ('0402B102K500NT', 'C1523'),
    (C0402, '2.2n'): ('0402B222K500NT', 'C1531'),            # E
    (C0603, '4.7u'): ('CL10A475KO8NNNC', 'C19666'), (C0603, '22u'): ('CL10A226MQ8NRNC', 'C59461'),
    (R0402, '22'): ('0402WGF220JTCE', 'C25092'), (R0402, '0'): ('0402WGF0000TCE', 'C17168'),
    (R0402, '10k'): ('0402WGF1002TCE', 'C25744'), (R0402, '100k'): ('0402WGF1003TCE', 'C25741'),
    (R0402, '22k'): ('0402WGF2202TCE', 'C25768'), (R0402, '100'): ('0402WGF1000TCE', 'C25076'),
    (R0402, '499'): ('0402WGF4990TCE', 'C4125'),             # E
}
def _src(fp, val, f):
    if (fp, val) in PASSIVE and 'MPN' not in f:
        f = dict(f, MPN=PASSIVE[(fp, val)][0], LCSC=PASSIVE[(fp, val)][1])
    return f
def R(ref, val, a, b, fit='ALL', fp=R0402, **f):
    return (ref, 'Device', 'R', val, fp, {'1': a, '2': b}, dict(Fit=fit, **_src(fp, val, f)))
def C(ref, val, a, b, fit='ALL', fp=C0402, **f):
    return (ref, 'Device', 'C', val, fp, {'1': a, '2': b}, dict(Fit=fit, **_src(fp, val, f)))

MOD_NC = ['IO2', 'IO3', 'IO4', 'IO5', 'IO6', 'IO7', 'IO8', 'IO9', 'IO17', 'IO18', 'IO26', 'IO47',
          'IO33', 'IO34', 'IO36', 'IO37', 'IO38', 'IO39', 'IO40', 'IO41', 'IO42', 'IO45', 'IO46', 'IO21',
          'IO1',   # IO1: rev A board-ID ADC; rev B board rev lives in factory NVS
          'TXD0', 'RXD0']   # rev B: no UART pads (flash over USB download mode; logs over Wi-Fi)
mod_map = {'GND': 'GND', '3V3': '+3V3', 'EN': 'EN', 'IO0': 'BOOT', 'IO35': 'SD_PWR_EN',
           # SDMMC via GPIO matrix, pins in module-edge order = resistor column order (routing, stage 4)
           'IO10': 'SD_D2', 'IO11': 'SD_D3', 'IO12': 'SD_CMD', 'IO13': 'SD_CLK', 'IO14': 'SD_D0',
           'IO15': 'SD_D1', 'IO16': 'SD_CD', 'USB_D-': 'USB_DM', 'USB_D+': 'USB_DP', 'IO48': 'LED_DATA_3V3',
           **{n: 'NC' for n in MOD_NC}}

PARTS = [
    # --- USB input, protection -------------------------------------------------------------
    ('J1', 'Connector', 'USB_A', 'USB-A plug', 'filexfer:USB_A_Plug_XKB_U217-041N-4BV81',
     {'1': 'VBUS_IN', '2': 'USB_DM_C', '3': 'USB_DP_C', '4': 'GND', 'SH': 'GND'},
     dict(Fit='ALL', MPN='U217-041N-4BV81', LCSC='C319178', Note='DNP option: hand-solder first 10. XKB only (no alternate)')),
    ('F1', 'Device', 'Polyfuse', '750mA/6V', 'Fuse:Fuse_1206_3216Metric', {'1': 'VBUS_IN', '2': '+5V'},
     dict(Fit='ALL', MPN='BSMD1206-075-6V', LCSC='C883127')),
    ('D1', 'Device', 'D_Zener', 'SMF5.0A', 'Diode_SMD:D_SMF', {'1': '+5V', '2': 'GND'},
     dict(Fit='ALL', MPN='SMF5.0A (Leiditech)', LCSC='C2759874', Note='unidirectional TVS 5 V')),
    ('U1', 'Power_Protection', 'USBLC6-2P6', 'USBLC6-2P6', 'Package_TO_SOT_SMD:SOT-666',
     {'1': 'USB_DM_C', '6': 'USB_DM_C', '3': 'USB_DP_C', '4': 'USB_DP_C', '5': '+5V', '2': 'GND'},
     dict(Fit='ALL', MPN='USBLC6-2P6', LCSC='C15999')),
    R('R1', '22', 'USB_DM_C', 'USB_DM', Note='HDG 22/33 ohm, near module'),
    R('R2', '22', 'USB_DP_C', 'USB_DP', Note='HDG 22/33 ohm, near module'),
    # --- 3V3 supply: LDO + soft-start ---------------------------------------------------------
    ('U2', 'Regulator_Linear', 'TLV76733DRVx', 'TLV76733DRVR', 'Package_SON:WSON-6-1EP_2x2mm_P0.65mm_EP1x1.6mm',
     {'6': '+5V', '4': '+5V', '1': '3V3_LDO', '2': '3V3_LDO', '3': 'GND', '5': 'GND', '7': 'GND'},
     dict(Fit='ALL', MPN='TLV76733DRVR', LCSC='C2848334', Note='1 A, 16 V, WSON-6 2x2; EN tied to IN, SNS to OUT, thermal pad to GND (TI SLVSE84D)')),
    C('C3', '4.7u', '+5V', 'GND', fp=C0603, Note='LDO in, >=1u; counts in VBUS 10u limit'),
    C('C4', '4.7u', '3V3_LDO', 'GND', fp=C0603, Note='LDO out, ceramic X7R'),
    ('U3', 'Power_Management', 'TPS22917DBV', 'TPS22917DBVR', SOT236,
     {'1': '3V3_LDO', '2': 'GND', '3': '3V3_LDO', '4': 'SS_CT', '5': 'NC', '6': '+3V3'},
     dict(Fit='ALL', MPN='TPS22917DBVR', LCSC='C2681320', Note='3V3 soft-start, tR ~3.5 ms (inrush)')),
    C('C5', '2.2n', 'SS_CT', '3V3_LDO', Note='CT to VIN per TPS22917 DS'),
    C('C6', '22u', '+3V3', 'GND', fp=C0603, Note='bulk'),
    C('C7', '10u', '+3V3', 'GND', Note='at module 3V3 pin (HDG); 0402 X5R 6.3V (CL05A106MQ5NUNC C15525) to fit 1.15 mm strip'),
    C('C8', '100n', '+3V3', 'GND'),
    # --- Module -------------------------------------------------------------------------------
    ('U4', 'RF_Module', 'ESP32-S3-MINI-1', 'ESP32-S3-MINI-1-N8', 'filexfer:ESP32-S3-MINI-1', 'MODULE',
     dict(Fit='ALL', MPN='ESP32-S3-MINI-1-N8', LCSC='C2913206', Note='ESP32-S3-MINI-1-N8 only (IO35 used; no PSRAM variants)', Alt='ESP32-S3-MINI-1U-N8 C2980299 (U.FL; also no PSRAM)')),
    R('R4', '10k', '+3V3', 'EN'), C('C9', '1u', 'EN', 'GND', Note='EN RC 10k/1u (DS-M)'),
    R('R5', '10k', '+3V3', 'BOOT', Note='GPIO0 pull-up (HDG); no cap'),
    ('SW1', 'Switch', 'SW_Push', 'BOOT', 'Button_Switch_SMD:SW_SPST_TS-1088-xR020', {'1': 'BOOT', '2': 'GND'},
     dict(Fit='ALL', MPN='TS-1088-AR02016', LCSC='C720477')),
    # --- Debug / test ---------------------------------------------------------------------------
] + [
    ('TP%d' % (i + 1), 'Connector', 'TestPoint', n, TP, {'1': n}, dict(Fit='ALL', MPN='pad'))
    for i, n in enumerate(['+3V3', 'GND', 'BOOT'])   # rev B: EN / UART / +5V / USB pads removed
] + [
    # --- RGB LED on 5 V via SN74LV1T125 buffer ------------------------------------------------------------
    ('U5', '74xGxx', '74LVC1G125', 'SN74LV1T125', 'Package_TO_SOT_SMD:SOT-353_SC-70-5',
     {'1': 'GND', '2': 'LED_DATA_3V3', '3': 'GND', '4': 'LED_DATA_5V', '5': '+5V'},
     dict(Fit='ALL', MPN='SN74LV1T125DCKR', LCSC='C75925', Note='single-supply translator, VCC 1.6-5.5 V; OE low = enabled; symbol 74LVC1G125 (same pinout)')),
    C('C11', '100n', '+5V', 'GND'),
    R('R9', '10k', 'LED_DATA_3V3', 'GND', Note='LED dark during boot'),
    R('R10', '100', 'LED_DATA_5V', 'LED_DIN'),
    ('D2', 'LED', 'WS2812B-2020', 'WS2812B-2020', 'LED_SMD:LED_WS2812B-2020_PLCC4_2.0x2.0mm',
     {'1': 'NC', '2': 'GND', '3': 'LED_DIN', '4': '+5V'},
     dict(Fit='ALL', MPN='WS2812B-2020-V6', LCSC='C52917434')),
    C('C12', '100n', '+5V', 'GND', Note='at LED VDD'),
    # --- microSD (RX only) -------------------------------------------------------------------------
    ('U6', 'Power_Management', 'TPS22917DBV', 'TPS22917DBVR', SOT236,
     {'1': '+3V3', '2': 'GND', '3': 'SD_PWR_EN', '4': 'SD_CT', '5': 'SD_VDD', '6': 'SD_VDD'},   # QOD tied to VOUT (DS option 1, fast discharge)
     dict(Fit='RX', MPN='TPS22917DBVR', LCSC='C2681320', Note='SD power switch')),
    C('C13', '1n', 'SD_CT', '+3V3', fit='RX', Note='CT to VIN, tR ~1.6 ms'),
    R('R11', '10k', '+3V3', 'SD_PWR_EN', fit='RX', Note='SD power default ON'),
    C('C14', '10u', 'SD_VDD', 'GND', fit='RX', Note='0402 X5R 6.3V (CL05A106MQ5NUNC C15525), fits beside U6'),
    C('C15', '100n', 'SD_VDD', 'GND', fit='RX'),
    ('J3', 'Connector', 'Micro_SD_Card_Det1', 'microSD push-push', 'filexfer:microSD_SHOUHAN_TF_PUSH',
     {'1': 'SD_D2', '2': 'SD_D3', '3': 'SD_CMD', '4': 'SD_VDD', '5': 'SD_CLK', '6': 'GND',
      '7': 'SD_D0', '8': 'SD_D1', '9': 'SD_CD', 'SH': 'GND'},
     dict(Fit='RX', MPN='TF PUSH (SHOU HAN)', LCSC='C393941', Note='CD switch to GND on insert [U polarity]')),
    ('RN1', 'Device', 'R_Pack04', '4x10k', 'Resistor_SMD:R_Array_Convex_4x0402',
     {'1': 'SD_D1', '2': 'SD_D0', '3': 'SD_CMD', '4': 'SD_D2',   # W->E = module pin order (no crossings)
      '5': 'SD_VDD', '6': 'SD_VDD', '7': 'SD_VDD', '8': 'SD_VDD'},
     dict(Fit='RX', MPN='4D02WGJ0103TCE', LCSC='C25725', Note='SD pull-ups to switched rail (replaces rev A R18-R21)')),
    R('R22', '10k', 'SD_D3', 'SD_VDD', fit='RX', Note='SD pull-up to switched rail'),
    R('R23', '10k', '+3V3', 'SD_CD', fit='RX', Note='card detect pull-up'),
]
def flatten(libname, symname):
    sym = getsym(libname, symname)
    ext = find(sym, 'extends')
    if not ext:
        out = [x for x in sym]
        out[1] = q(f'{libname}:{symname}')
        return out
    parent = flatten(libname, unq(ext[0][1]))
    pname = unq(parent[1]).split(':', 1)[1]
    child_props = {unq(p[1]): p for p in find(sym, 'property')}
    out = ['symbol', q(f'{libname}:{symname}')]
    for item in parent[2:]:
        if isinstance(item, list) and item[0] == 'property' and unq(item[1]) in child_props:
            out.append(child_props.pop(unq(item[1])))
        elif isinstance(item, list) and item[0] == 'symbol':
            sub = list(item); sub[1] = q(unq(item[1]).replace(pname, symname, 1)); out.append(sub)
        else:
            out.append(item)
    for p in child_props.values():
        out.insert(2, p)
    return out


def prop(name, value, x, y, hide=False):
    e = ['effects', ['font', ['size', '1.27', '1.27']]]
    if hide:
        e.append(['hide', 'yes'])
    return ['property', q(name), q(value), ['at', f'{x:.2f}', f'{y:.2f}', '0'], e]


from sch_layout import build   # drawing (blocks, wires, labels); PARTS above stays the electrical spec


def main():
    # Since the 2026-11-10 cleanup filexfer-pcb.kicad_sch is the master (edit it in KiCad).
    # Regenerating would drop any hand edits, so it needs an explicit --force.
    if os.path.exists(OUT) and '--force' not in sys.argv:
        sys.exit('filexfer-pcb.kicad_sch is the master; refusing to overwrite (use --force, then diff the netlist)')
    build(PARTS, mod_map, flatten, OUT, PROJECT, ROOT_UUID, uid, prop)


if __name__ == '__main__':
    main()
