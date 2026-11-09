#!/usr/bin/env python3
"""Generate the FileXfer rev A schematic (label-per-pin style) from KiCad stock symbols.

Every symbol pin must be mapped to a net name or 'NC'; the script fails otherwise.
Run: python3 -I tools/gen_sch.py  (from hardware/filexfer-pcb)
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
def R(ref, val, a, b, fit='ALL', fp=R0402, **f):
    return (ref, 'Device', 'R', val, fp, {'1': a, '2': b}, dict(Fit=fit, **f))
def C(ref, val, a, b, fit='ALL', fp=C0402, **f):
    return (ref, 'Device', 'C', val, fp, {'1': a, '2': b}, dict(Fit=fit, **f))

MOD_NC = ['IO3', 'IO4', 'IO5', 'IO6', 'IO7', 'IO8', 'IO15', 'IO16', 'IO17', 'IO18', 'IO26', 'IO47',
          'IO33', 'IO34', 'IO35', 'IO36', 'IO37', 'IO38', 'IO39', 'IO40', 'IO41', 'IO42', 'IO45', 'IO46']
mod_map = {'GND': 'GND', '3V3': '+3V3', 'EN': 'EN', 'IO0': 'BOOT', 'IO1': 'BOARD_ID', 'IO2': 'SD_PWR_EN',
           'IO9': 'SD_D2', 'IO10': 'SD_D3', 'IO11': 'SD_CMD', 'IO12': 'SD_CLK', 'IO13': 'SD_D0',
           'IO14': 'SD_D1', 'USB_D-': 'USB_DM', 'USB_D+': 'USB_DP', 'IO21': 'SD_CD', 'IO48': 'LED_DATA_3V3',
           'TXD0': 'U0TXD_MOD', 'RXD0': 'U0RXD', **{n: 'NC' for n in MOD_NC}}

PARTS = [
    # --- USB input, protection -------------------------------------------------------------
    ('J1', 'Connector', 'USB_A', 'USB-A plug', 'filexfer:USB_A_Plug_XKB_U217-041N-4BV81',
     {'1': 'VBUS_IN', '2': 'USB_DM_C', '3': 'USB_DP_C', '4': 'GND', 'SH': 'GND'},
     dict(Fit='ALL', MPN='U217-041N-4BV81', LCSC='C319178', Note='DNP option: hand-solder first 10. XKB only (no alternate)')),
    ('F1', 'Device', 'Polyfuse', '750mA/6V', 'Fuse:Fuse_1206_3216Metric', {'1': 'VBUS_IN', '2': '+5V'},
     dict(Fit='ALL', MPN='BSMD1206-075-6V', LCSC='C883127')),
    ('D1', 'Device', 'D_Zener', 'SMF5.0A', 'Diode_SMD:D_SMF', {'1': '+5V', '2': 'GND'},
     dict(Fit='ALL', MPN='SMF5.0A (Leiditech)', LCSC='C2759874', Note='unidirectional TVS 5 V')),
    ('U1', 'Power_Protection', 'USBLC6-2SC6', 'USBLC6-2SC6', SOT236,
     {'1': 'USB_DM_C', '6': 'USB_DM_C', '3': 'USB_DP_C', '4': 'USB_DP_C', '5': '+5V', '2': 'GND'},
     dict(Fit='ALL', MPN='USBLC6-2SC6', LCSC='C7519')),
    R('R1', '22', 'USB_DM_C', 'USB_DM', Note='HDG 22/33 ohm, near module'),
    R('R2', '22', 'USB_DP_C', 'USB_DP', Note='HDG 22/33 ohm, near module'),
    C('C1', 'DNP', 'USB_DM', 'GND', fit='DNP', Note='HDG reserve, unpopulated'),
    C('C2', 'DNP', 'USB_DP', 'GND', fit='DNP', Note='HDG reserve, unpopulated'),
    R('R3', 'DNP', 'USB_DP', '+3V3', fit='DNP', Note='HDG optional D+ pull-up, value TBD'),
    # --- 3V3 supply: LDO + soft-start ---------------------------------------------------------
    ('U2', 'Regulator_Linear', 'LD1117S33TR_SOT223', 'LDL1117S33R', 'Package_TO_SOT_SMD:SOT-223-3_TabPin2',
     {'3': '+5V', '2': '3V3_LDO', '1': 'GND'},
     dict(Fit='ALL', MPN='LDL1117S33R', LCSC='C435835', Note='symbol LD1117 SOT-223: same pinout 1 GND 2 VOUT/tab 3 VIN')),
    C('C3', '4.7u', '+5V', 'GND', fp=C0603, Note='LDO in, >=1u; counts in VBUS 10u limit'),
    C('C4', '4.7u', '3V3_LDO', 'GND', fp=C0603, Note='LDO out, ceramic X7R'),
    ('U3', 'Power_Management', 'TPS22917DBV', 'TPS22917DBVR', SOT236,
     {'1': '3V3_LDO', '2': 'GND', '3': '3V3_LDO', '4': 'SS_CT', '5': 'NC', '6': '+3V3'},
     dict(Fit='ALL', MPN='TPS22917DBVR', LCSC='C2681320', Note='3V3 soft-start, tR ~3.5 ms (inrush)')),
    C('C5', '2.2n', 'SS_CT', '3V3_LDO', Note='CT to VIN per TPS22917 DS'),
    C('C6', '22u', '+3V3', 'GND', fp=C0805, Note='bulk'),
    C('C7', '10u', '+3V3', 'GND', fp=C0603, Note='at module 3V3 pin (HDG)'),
    C('C8', '100n', '+3V3', 'GND'),
    # --- Module -------------------------------------------------------------------------------
    ('U4', 'RF_Module', 'ESP32-S3-MINI-1', 'ESP32-S3-MINI-1-N8', 'filexfer:ESP32-S3-MINI-1', 'MODULE',
     dict(Fit='ALL', MPN='ESP32-S3-MINI-1-N8', LCSC='C2913206', Alt='ESP32-S3-MINI-1U-N8 C2980299 (U.FL)')),
    R('R4', '10k', '+3V3', 'EN'), C('C9', '1u', 'EN', 'GND', Note='EN RC 10k/1u (DS-M)'),
    ('SW2', 'Switch', 'SW_Push', 'RESET', 'Button_Switch_SMD:SW_SPST_TS-1088-xR020', {'1': 'EN', '2': 'GND'},
     dict(Fit='ALL', MPN='TS-1088-AR02016', LCSC='C720477')),
    R('R5', '10k', '+3V3', 'BOOT', Note='GPIO0 pull-up (HDG); no cap'),
    ('SW1', 'Switch', 'SW_Push', 'BOOT', 'Button_Switch_SMD:SW_SPST_TS-1088-xR020', {'1': 'BOOT', '2': 'GND'},
     dict(Fit='ALL', MPN='TS-1088-AR02016', LCSC='C720477')),
    R('R6', '100k', '+3V3', 'BOARD_ID', Note='board ID top'),
    R('R7', '100k', 'BOARD_ID', 'GND', Note='board ID: RX 100k (1.65 V), TX 22k (0.60 V); see board_config.h'),
    C('C10', '100n', 'BOARD_ID', 'GND', Note='ADC filter (HDG)'),
    R('R8', '499', 'U0TXD_MOD', 'U0TXD', Note='U0TXD series (HDG)'),
    # --- Debug / test ---------------------------------------------------------------------------
    ('J2', 'Connector_Generic', 'Conn_02x03_Odd_Even', 'TC2030-NL',
     'Connector:Tag-Connect_TC2030-IDC-NL_2x03_P1.27mm_Vertical',
     {'1': '+3V3', '2': 'U0TXD', '3': 'GND', '4': 'U0RXD', '5': 'EN', '6': 'BOOT'},
     dict(Fit='ALL', MPN='footprint only', Note='1 3V3, 2 TXD (ESP out), 3 GND, 4 RXD, 5 EN, 6 IO0')),
] + [
    ('TP%d' % (i + 1), 'Connector', 'TestPoint', n, TP, {'1': n}, dict(Fit='ALL', MPN='pad'))
    for i, n in enumerate(['+3V3', 'GND', '+5V', 'EN', 'BOOT', 'U0TXD', 'U0RXD', 'USB_DP', 'USB_DM', 'BOARD_ID'])
] + [
    # --- RGB LED on 5 V via SN74LV1T125 buffer ------------------------------------------------------------
    ('U5', '74xGxx', '74LVC1G125', 'SN74LV1T125', 'Package_TO_SOT_SMD:SOT-23-5',
     {'1': 'GND', '2': 'LED_DATA_3V3', '3': 'GND', '4': 'LED_DATA_5V', '5': '+5V'},
     dict(Fit='ALL', MPN='SN74LV1T125DBVR', LCSC='C473338', Note='single-supply translator, VCC 1.6-5.5 V; OE low = enabled; symbol 74LVC1G125 (same pinout)')),
    C('C11', '100n', '+5V', 'GND'),
    R('R9', '10k', 'LED_DATA_3V3', 'GND', Note='LED dark during boot'),
    R('R10', '100', 'LED_DATA_5V', 'LED_DIN'),
    ('D2', 'LED', 'WS2812B-2020', 'WS2812B-2020', 'LED_SMD:LED_WS2812B-2020_PLCC4_2.0x2.0mm',
     {'1': 'NC', '2': 'GND', '3': 'LED_DIN', '4': '+5V'},
     dict(Fit='ALL', MPN='WS2812B-2020-V6', LCSC='C52917434')),
    C('C12', '100n', '+5V', 'GND', Note='at LED VDD'),
    # --- microSD (RX only) -------------------------------------------------------------------------
    ('U6', 'Power_Management', 'TPS22917DBV', 'TPS22917DBVR', SOT236,
     {'1': '+3V3', '2': 'GND', '3': 'SD_PWR_EN', '4': 'SD_CT', '5': 'SD_QOD', '6': 'SD_VDD'},
     dict(Fit='RX', MPN='TPS22917DBVR', LCSC='C2681320', Note='SD power switch')),
    R('R24', '0', 'SD_QOD', 'SD_VDD', fit='RX', Note='QOD to VOUT: fast discharge on power-cycle (DS option 1)'),
    C('C13', '1n', 'SD_CT', '+3V3', fit='RX', Note='CT to VIN, tR ~1.6 ms'),
    R('R11', '10k', '+3V3', 'SD_PWR_EN', fit='RX', Note='SD power default ON'),
    C('C14', '10u', 'SD_VDD', 'GND', fit='RX', fp=C0603),
    C('C15', '100n', 'SD_VDD', 'GND', fit='RX'),
    ('J3', 'Connector', 'Micro_SD_Card_Det1', 'microSD push-push', 'filexfer:microSD_SHOUHAN_TF_PUSH',
     {'1': 'SD_D2_C', '2': 'SD_D3_C', '3': 'SD_CMD_C', '4': 'SD_VDD', '5': 'SD_CLK_C', '6': 'GND',
      '7': 'SD_D0_C', '8': 'SD_D1_C', '9': 'SD_CD', 'SH': 'GND'},
     dict(Fit='RX', MPN='TF PUSH (SHOU HAN)', LCSC='C393941', Note='CD switch to GND on insert [U polarity]')),
] + [
    R('R%d' % (12 + i), '0', s, s + '_C', fit='RX', Note='SDIO series (HDG), tune in layout')
    for i, s in enumerate(['SD_CLK', 'SD_CMD', 'SD_D0', 'SD_D1', 'SD_D2', 'SD_D3'])
] + [
    R('R%d' % (18 + i), '10k', s + '_C', 'SD_VDD', fit='RX', Note='SD pull-up to switched rail')
    for i, s in enumerate(['SD_CMD', 'SD_D0', 'SD_D1', 'SD_D2', 'SD_D3'])
] + [
    R('R23', '10k', '+3V3', 'SD_CD', fit='RX', Note='card detect pull-up'),
]
PWR_FLAGS = ['GND', 'VBUS_IN', '+5V']


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


def main():
    items, libsyms, used = [], {}, set()
    # place: module alone at left, then pack the rest in rows
    x0, y0, rowh = 60.0, 40.0, 0.0
    cursor_x, cursor_y = 150.0, y0
    for ref, libname, symname, val, fp, netmap, fields in PARTS:
        key = f'{libname}:{symname}'
        if key not in libsyms:
            libsyms[key] = flatten(libname, symname)
        ps = [p for p in pins(libsyms[key]) if p['unit'] in (0, 1)]
        if netmap == 'MODULE':
            netmap = {p['num']: mod_map[p['name']] for p in ps}
        missing = [p['num'] for p in ps if p['num'] not in netmap]
        extra = set(netmap) - {p['num'] for p in ps}
        if missing or extra:
            sys.exit(f'{ref}: unmapped pins {missing} / unknown pins {sorted(extra)}')
        xs = [p['x'] for p in ps] or [0]; ys = [p['y'] for p in ps] or [0]
        w, h = max(xs) - min(xs), max(ys) - min(ys)
        if ref == 'U4':
            X, Y = 91.44, 101.6
        else:
            if cursor_x + w + 60 > 560:
                cursor_x, cursor_y, rowh = 150.0, cursor_y + rowh + 25, 0.0
            X = round((cursor_x - min(xs) + 25) / 2.54) * 2.54
            Y = round((cursor_y + max(ys) + 8) / 2.54) * 2.54
            cursor_x += w + 70; rowh = max(rowh, h + 16)
        sym = ['symbol', ['lib_id', q(key)], ['at', f'{X:.2f}', f'{Y:.2f}', '0'], ['unit', '1'],
               ['exclude_from_sim', 'no'], ['in_bom', 'yes' if fields.get('Fit') != 'DNP' else 'no'],
               ['on_board', 'yes'], ['dnp', 'yes' if fields.get('Fit') == 'DNP' else 'no'],
               ['uuid', q(uid('sym', ref))],
               prop('Reference', ref, X, Y - max(ys) - 3), prop('Value', val, X, Y - min(ys) + 3),
               prop('Footprint', fp, X, Y, True), prop('Datasheet', '', X, Y, True)]
        for k, v in fields.items():
            sym.append(prop(k, v, X, Y, True))
        for p in ps:
            sym.append(['pin', q(p['num']), ['uuid', q(uid('pin', ref, p['num']))]])
        sym.append(['instances', ['project', q(PROJECT), ['path', q('/' + ROOT_UUID),
                    ['reference', q(ref)], ['unit', '1']]]])
        items.append(sym)
        for p in ps:
            px, py = X + p['x'], Y - p['y']
            net = netmap[p['num']]
            if net == 'NC':
                items.append(['no_connect', ['at', f'{px:.2f}', f'{py:.2f}'], ['uuid', q(uid('nc', ref, p['num']))]])
                continue
            used.add(net)
            la = {0: 180, 180: 0, 90: 270, 270: 90}[int(p['a']) % 360]
            just = ['justify', 'right', 'bottom'] if la in (180, 270) else ['justify', 'left', 'bottom']
            items.append(['label', q(net), ['at', f'{px:.2f}', f'{py:.2f}', str(la)],
                          ['effects', ['font', ['size', '1.27', '1.27']], just],
                          ['uuid', q(uid('lbl', ref, p['num']))]])
    # power flags
    key = 'power:PWR_FLAG'
    libsyms[key] = flatten('power', 'PWR_FLAG')
    for i, net in enumerate(PWR_FLAGS):
        X, Y = 40.64 + i * 25.4, 20.32
        ref = '#FLG0%d' % (i + 1)
        items.append(['symbol', ['lib_id', q(key)], ['at', f'{X:.2f}', f'{Y:.2f}', '0'], ['unit', '1'],
                      ['exclude_from_sim', 'no'], ['in_bom', 'yes'], ['on_board', 'yes'], ['dnp', 'no'],
                      ['uuid', q(uid('sym', ref))],
                      prop('Reference', ref, X, Y, True), prop('Value', 'PWR_FLAG', X, Y - 3),
                      prop('Footprint', '', X, Y, True), prop('Datasheet', '', X, Y, True),
                      ['pin', q('1'), ['uuid', q(uid('pin', ref))]],
                      ['instances', ['project', q(PROJECT), ['path', q('/' + ROOT_UUID),
                       ['reference', q(ref)], ['unit', '1']]]]])
        items.append(['label', q(net), ['at', f'{X:.2f}', f'{Y:.2f}', '270'],
                      ['effects', ['font', ['size', '1.27', '1.27']], ['justify', 'right', 'bottom']],
                      ['uuid', q(uid('flaglbl', net))]])
    notes = ('FileXfer rev A — generated by tools/gen_sch.py; edit the script, not this file. '
             'Fit field: ALL = TX+RX, RX = receiver only (DNP on TX), DNP = footprint reserve. '
             'Board ID R7: RX 100k, TX 22k. LED buffer U5 = SN74LV1T125 (74LVC1G125 symbol, same pinout).')
    items.append(['text', q(notes), ['exclude_from_sim', 'no'], ['at', '20', '410', '0'],
                  ['effects', ['font', ['size', '2', '2']], ['justify', 'left', 'bottom']],
                  ['uuid', q(uid('note'))]])
    doc = ['kicad_sch', ['version', '20250114'], ['generator', q('eeschema')], ['generator_version', q('9.0')],
           ['uuid', q(ROOT_UUID)], ['paper', q('A2')],
           ['title_block', ['title', q('FileXfer TX/RX')], ['rev', q('A')], ['company', q('FileXfer')],
            ['comment', '1', q('ESP32-S3-MINI-1-N8, USB-A plug, microSD (RX)')]],
           ['lib_symbols'] + list(libsyms.values())] + items + [
           ['sheet_instances', ['path', q('/'), ['page', q('1')]]], ['embedded_fonts', 'no']]
    with open(OUT, 'w') as f:
        f.write(dump(doc) + '\n')
    print('wrote', os.path.abspath(OUT), len(PARTS), 'parts, nets:', len(used))


if __name__ == '__main__':
    main()
