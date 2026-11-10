"""FileXfer schematic drawing (rev A cleanup 2026-11-10; rev B short-case changes 2026-11-10): functional blocks, wires, labels, power symbols (A3, one sheet).

Used once by gen_sch.py for the 2026-11-10 readability cleanup; since then filexfer-pcb.kicad_sch is
the master. PARTS (gen_sch.py) is the electrical spec. build() traces the drawing (wires, labels, power
symbols) and refuses to write unless every pin lands on exactly the net PARTS gives it.

Conventions (docs/schematic-style.md): signal flow left -> right, supplies point up, GND points down,
local labels for inter-block nets, wires inside a block, T-junctions only, 1.27 mm grid.
Power symbols are sheet-local ("power local") so the nets keep their names (/GND, /+3V3, /+5V).
"""
import math, sys
from sexp import q, getsym, find, unq, pins as libpins

G = 1.27
TXT = 1.27


def _round(v):
    return round(v, 3)


def xform(px, py, rot, X, Y, mirror=None):
    """Library point (y up) -> sheet point (y down) for a symbol at X, Y, rotation rot (CCW, deg)."""
    vx, vy = px, -py
    if mirror == 'x':
        vy = -vy
    elif mirror == 'y':
        vx = -vx
    t = math.radians(rot)
    c, s = round(math.cos(t)), round(math.sin(t))
    return _round(X + vx * c + vy * s), _round(Y - vx * s + vy * c)


# ---------------------------------------------------------------------------------------------------
# Placement: ref -> (x, y, rot[, mirror]).  Passives: rot 0 = vertical, pin 1 on top; 90 = horizontal,
# pin 1 left; 180 = vertical, pin 1 at the bottom.
# ---------------------------------------------------------------------------------------------------
PLACE = {
    # A  USB input & protection
    'J1': (30.48, 50.8, 0), 'F1': (63.5, 35.56, 90), 'D1': (73.66, 39.37, 270),
    'U1': (106.68, 83.82, 0), 'R2': (111.76, 50.8, 90), 'R1': (111.76, 63.5, 90),
    # B  3.3 V supply + soft-start
    'C3': (152.4, 44.45, 0), 'U2': (167.64, 43.18, 0), 'C4': (180.34, 44.45, 0),
    'C5': (198.12, 44.45, 180), 'U3': (215.9, 48.26, 0), 'C6': (236.22, 49.53, 0),
    # E  status LED + level shifter
    'R9': (292.1, 54.61, 0), 'U5': (314.96, 50.8, 0), 'C11': (302.26, 39.37, 0),
    'R10': (347.98, 50.8, 90), 'D2': (370.84, 50.8, 0), 'C12': (381.0, 39.37, 0),
    # F  reset / boot buttons, board ID
    'R4': (30.48, 121.92, 0), 'C9': (43.18, 129.54, 0),
    'R5': (30.48, 151.13, 0), 'SW1': (50.8, 160.02, 270),
    'R6': (30.48, 180.34, 0), 'R7': (30.48, 187.96, 0), 'C10': (50.8, 187.96, 0),
    # C  module
    'U4': (160.02, 160.02, 0), 'C7': (165.1, 123.19, 0), 'C8': (175.26, 123.19, 0),
    'R8': (195.58, 134.62, 90),
    # D  microSD + power switch (RX only)
    'C13': (261.62, 128.27, 180), 'R11': (246.38, 130.81, 0), 'U6': (281.94, 132.08, 0),
    'C14': (302.26, 133.35, 0), 'C15': (312.42, 133.35, 0),
    'J3': (378.46, 175.26, 0), 'R23': (337.82, 167.64, 0),
    'RN1': (287.02, 167.64, 0), 'R22': (307.34, 167.64, 180),     # pull-ups: array pins 1-4 at the bottom
}
TPS = ['+3V3', 'GND', '+5V', 'EN', 'BOOT', 'U0TXD', 'U0RXD', 'USB_DP_C', 'USB_DM_C', 'BOARD_ID']
for _i in range(10):                                                        # TP1..TP10, body left
    PLACE['TP%d' % (_i + 1)] = (35.56 + 50.8 * (_i // 5), 236.22 + 10.16 * (_i % 5), 90)

FRAMES = [   # x0, y0, x1, y1, title
    (15, 15, 135, 100, 'USB INPUT & PROTECTION'),
    (140, 15, 265, 100, '3.3 V SUPPLY + SOFT-START'),
    (270, 15, 405, 100, 'STATUS LED + 3.3 V -> 5 V LEVEL SHIFTER'),
    (15, 105, 100, 215, 'EN RC / BOOT / BOARD ID'),
    (105, 105, 215, 215, 'ESP32-S3 MODULE'),
    (220, 105, 405, 215, 'microSD + POWER SWITCH  (RX ONLY)'),
    (15, 220, 140, 285, 'TEST PADS (bottom side, pogo jig)'),
    (145, 220, 295, 285, 'GENERAL NOTES'),
]

NOTES = [
    (19, 82, 'J1 hand-soldered (DNP in assembly). F1 750 mA PTC.\n'
             'D1 SMF5.0A TVS after F1. U1 USBLC6-2P6 (SOT-666) at the plug.\n'
             'R1/R2 22 R series near the module (HDG).\n'
             'Rev B: rev A DNP reserves C1, C2, R3 removed.'),
    (144, 66, 'U2 TLV76733DRVR (WSON-6) 5 V -> 3.3 V, 1 A: P = 1.7 V x 0.25 A avg = 0.43 W;\n'
              'EN tied to IN, SNS to OUT, thermal pad to the GND plane (vias).\n'
              'U3 TPS22917 soft-start: CT (C5) to VIN per TI DS pin table;\n'
              'tR = 1.6 us/pF x 2.2 nF = 3.5 ms (at 3.6 V).\n'
              'Inrush = C x dV/dt = (22u + 10u + 0.1u + module) x 3.3 V / 3.5 ms = ~30 mA + module.\n'
              'QOD open: no quick output discharge on the main rail. Kept in rev B: the TLV767\n'
              'internal 0.5 ms soft-start alone gives ~170 uC at attach (> 50 uC), see docs.'),
    (274, 70, 'U5 SN74LV1T125 (74LVC1G125 symbol, same pinout): single-supply\n'
              'translator at VCC 5 V, VIH 2.03 V max -> 3.3 V input is valid. OE tied low.\n'
              'R9 holds DIN low during boot (LED dark). R10 100 R at DIN.'),
    (19, 198, 'EN RC: R4 10k / C9 1u (module DS). No reset button (rev B): replug.\n'
              'BOOT (IO0): R5 10k pull-up, no cap. Hold SW1 at reset -> download.\n'
              'Board ID (IO1, ADC): RX R7 = 100k -> 1.65 V;\n'
              'TX R7 = 22k (C25768) -> 0.60 V. TX firmware ignores BOARD_ID.'),
    (109, 198, 'U4 = ESP32-S3-MINI-1-N8 only (IO35 used; no PSRAM variants).\n'
               'Native USB on IO19/IO20 (USB_D-/D+). C7 10u + C8 100n at the 3V3 pin.\n'
               'SDMMC via GPIO matrix: pin order follows the layout.'),
    (224, 199, 'RX only: every part in this block is Fit=RX (not fitted on TX).\n'
               'U6 TPS22917: CT (C13 1 nF) to VIN, tR = ~1.6 ms. QOD tied to VOUT (fast discharge).\n'
               'R11 pulls ON high: SD power on by default; IO35 can switch it off.\n'
               'RN1 (4x10k array) + R22: pull-ups to the switched SD_VDD. No series R (rev B).\n'
               'J3 SHOU HAN TF PUSH, bottom side, hand-soldered. CD to GND on insert [polarity unverified].'),
    (149, 230, 'Fit field: ALL = TX + RX builds; RX = receiver only (DNP on TX); DNP = footprint only (X).\n'
               'TX build: microSD block not fitted, R7 = 22k. J1 hand-soldered in every build.\n'
               'Inter-block nets use local labels; power symbols are sheet-local (net names /GND, /+3V3, /+5V).\n'
               'Test pads: bottom-side copper pads for the production pogo jig (not in BOM/CPL).\n'
               'This sheet was laid out by tools/gen_sch.py once (2026-11-10);\n'
               'filexfer-pcb.kicad_sch is now the master. Edit it in KiCad.\n'
               'Design notes: docs/stage2-parts.md, docs/stage4-notes.md, docs/revB-notes.md, docs/schematic-style.md.'),
]


def drawing(P):
    """Wires, labels, power symbols and flags. P(ref, pin) -> sheet point."""
    W, L, PW, FL = [], [], [], []
    w = lambda *pts: W.append([(_round(p[0]), _round(p[1])) for p in pts])
    lab = lambda name, x, y, a: L.append((name, _round(x), _round(y), a))
    pw = lambda net, x, y, rot=0: PW.append((net, _round(x), _round(y), rot))

    # ---- A: USB input & protection --------------------------------------------------------------
    j1v, j1p, j1m, j1g, j1s = P('J1', '1'), P('J1', '3'), P('J1', '2'), P('J1', '4'), P('J1', 'SH')
    w(j1s, (j1s[0], 63.5), (j1g[0], 63.5)); w(j1g, (j1g[0], 63.5)); pw('GND', j1g[0], 63.5)
    w((j1g[0], 63.5), (35.56, 63.5)); FL.append(('GND', 35.56, 63.5, 180))
    w(j1v, (40.64, j1v[1]), (40.64, 35.56), P('F1', '1'))
    lab('VBUS_IN', 41.91, 35.56, 0); FL.append(('VBUS_IN', 54.61, 35.56, 0))
    w(P('F1', '2'), (88.9, 35.56)); pw('+5V', 88.9, 35.56); FL.append(('+5V', 81.28, 35.56, 0))
    pw('GND', *P('D1', '2'))
    w(j1p, P('R2', '1')); lab('USB_DP_C', 45.72, 50.8, 0)
    w(j1m, (43.18, j1m[1]), (43.18, 63.5), P('R1', '1')); lab('USB_DM_C', 45.72, 63.5, 0)
    for r, net in (('R2', 'USB_DP'), ('R1', 'USB_DM')):
        e = P(r, '2'); w(e, (e[0] + 6.35, e[1])); lab(net, e[0] + 6.35, e[1], 0)
    for pin, net, d in (('1', 'USB_DM_C', -1), ('3', 'USB_DP_C', -1), ('6', 'USB_DM_C', 1), ('4', 'USB_DP_C', 1)):
        p = P('U1', pin); w(p, (p[0] + 2.54 * d, p[1])); lab(net, p[0] + 2.54 * d, p[1], 0 if d > 0 else 180)
    pw('+5V', *P('U1', '5')); pw('GND', *P('U1', '2'))

    # ---- B: 3.3 V supply + soft-start ----------------------------------------------------------
    y = 40.64
    pw('+5V', 147.32, y); w((147.32, y), P('U2', '6')); pw('GND', *P('C3', '2'))
    en = P('U2', '4'); w(en, (157.48, en[1]), (157.48, y))                   # EN tied to IN
    g, ep = P('U2', '3'), P('U2', '7'); w(ep, g); pw('GND', *g)              # GND pins + thermal pad
    w(P('U2', '1'), (203.2, y)); lab('3V3_LDO', 176.53, y, 0); pw('GND', *P('C4', '2'))
    sn = P('U2', '2'); w(sn, (177.8, sn[1]), (177.8, y))                     # SNS to OUT
    vin, ct, on = P('U3', '1'), P('U3', '4'), P('U3', '3')
    w(vin, (203.2, vin[1]), (203.2, y))
    w(P('C5', '1'), ct); lab('SS_CT', 199.39, ct[1], 0)
    w(on, (187.96, on[1]), (187.96, y))
    pw('GND', *P('U3', '2'))
    vo = P('U3', '6'); w(vo, (246.38, vo[1])); pw('+3V3', 246.38, vo[1]); pw('GND', *P('C6', '2'))

    # ---- E: LED + level shifter ----------------------------------------------------------------
    a = P('U5', '2'); w((287.02, a[1]), a); lab('LED_DATA_3V3', 287.02, a[1], 180); pw('GND', *P('R9', '2'))
    oe = P('U5', '1'); w(oe, (oe[0], 38.1), (320.04, 38.1), (320.04, 43.18)); pw('GND', 320.04, 43.18)
    vc = P('U5', '5'); w(vc, (vc[0], 33.02)); pw('+5V', vc[0], 33.02)
    w((vc[0], 35.56), P('C11', '1')); pw('GND', *P('C11', '2')); pw('GND', *P('U5', '3'))
    w(P('U5', '4'), P('R10', '1')); lab('LED_DATA_5V', P('U5', '4')[0] + 1.27, a[1], 0)
    w(P('R10', '2'), P('D2', '3')); lab('LED_DIN', P('R10', '2')[0] + 1.27, a[1], 0)
    vd = P('D2', '4'); w(vd, (vd[0], 33.02)); pw('+5V', vd[0], 33.02)
    w((vd[0], 35.56), P('C12', '1')); pw('GND', *P('C12', '2')); pw('GND', *P('D2', '2'))

    # ---- F: reset / boot / board ID ------------------------------------------------------------
    for top, node, net, taps in (('R4', 'R4', 'EN', [('C9', '1')]),
                                 ('R5', 'R5', 'BOOT', [('SW1', '1')]),
                                 ('R6', 'R6', 'BOARD_ID', [('C10', '1')])):
        pw('+3V3', *P(top, '1'))
        n = P(node, '2'); w(n, (76.2, n[1])); lab(net, 76.2, n[1], 0)
    for r in ('C9', 'SW1', 'R7', 'C10'):
        pw('GND', *P(r, '2'))

    # ---- C: module -----------------------------------------------------------------------------
    v3 = P('U4', '3'); w(v3, (v3[0], 116.84)); pw('+3V3', v3[0], 116.84)
    w((v3[0], 119.38), P('C7', '1'), P('C8', '1')); pw('GND', *P('C7', '2')); pw('GND', *P('C8', '2'))
    pw('GND', *P('U4', '1'))
    tx = P('U4', '39'); w(tx, P('R8', '1')); lab('U0TXD_MOD', tx[0] + 1.27, tx[1], 0)
    e = P('R8', '2'); w(e, (e[0] + 2.54, e[1])); lab('U0TXD', e[0] + 2.54, e[1], 0)
    for num, net in (('40', 'U0RXD'), ('23', 'USB_DM'), ('24', 'USB_DP'), ('31', 'SD_PWR_EN'), ('30', 'LED_DATA_3V3')):
        p = P('U4', num); w(p, (p[0] + 2.54, p[1])); lab(net, p[0] + 2.54, p[1], 0)
    for num, net in (('45', 'EN'), ('4', 'BOOT'), ('5', 'BOARD_ID'), ('14', 'SD_D2'), ('15', 'SD_D3'),
                     ('16', 'SD_CMD'), ('17', 'SD_CLK'), ('18', 'SD_D0'), ('19', 'SD_D1'), ('20', 'SD_CD')):
        p = P('U4', num); w(p, (p[0] - 2.54, p[1])); lab(net, p[0] - 2.54, p[1], 180)

    # ---- D: microSD + power switch -------------------------------------------------------------
    y = 124.46
    pw('+3V3', 241.3, y); w((241.3, y), (266.7, y))
    vin, ct, on = P('U6', '1'), P('U6', '4'), P('U6', '3')
    w(vin, (266.7, vin[1]), (266.7, y))
    w(P('C13', '2'), (261.62, y)); w(P('C13', '1'), ct); lab('SD_CT', 262.89, ct[1], 0)
    w(P('R11', '1'), (246.38, y))
    w(on, (243.84, on[1])); lab('SD_PWR_EN', 243.84, on[1], 180)
    vo, qod = P('U6', '6'), P('U6', '5')
    w(vo, (320.04, vo[1])); lab('SD_VDD', 320.04, vo[1], 0)
    w(qod, (294.64, qod[1]), (294.64, vo[1]))
    pw('GND', *P('U6', '2')); pw('GND', *P('C14', '2')); pw('GND', *P('C15', '2'))
    yr = 157.48
    w((281.94, yr), (307.34, yr)); lab('SD_VDD', 298.45, yr, 0)
    for t, b, s in (('8', '1', 'SD_CMD'), ('7', '2', 'SD_D0'), ('6', '3', 'SD_D1'), ('5', '4', 'SD_D2')):
        t, b = P('RN1', t), P('RN1', b)
        w(t, (t[0], yr)); w(b, (b[0], b[1] + 2.54)); lab(s, b[0], b[1] + 2.54, 270)
    t, b = P('R22', '2'), P('R22', '1')
    w(t, (t[0], yr)); w(b, (b[0], b[1] + 2.54)); lab('SD_D3', b[0], b[1] + 2.54, 270)
    pw('+3V3', *P('R23', '1'))
    b = P('R23', '2'); w(b, (b[0], b[1] + 2.54)); lab('SD_CD', b[0], b[1] + 2.54, 270)
    for num, net in (('1', 'SD_D2'), ('2', 'SD_D3'), ('3', 'SD_CMD'), ('4', 'SD_VDD'), ('5', 'SD_CLK'),
                     ('7', 'SD_D0'), ('8', 'SD_D1'), ('9', 'SD_CD')):
        p = P('J3', num); w(p, (p[0] - 5.08, p[1])); lab(net, p[0] - 5.08, p[1], 180)
    p = P('J3', '6'); w(p, (p[0] - 2.54, p[1])); pw('GND', p[0] - 2.54, p[1], 270)
    pw('GND', *P('J3', 'SH'))

    # ---- G: test pads --------------------------------------------------------------------------
    for i, net in enumerate(TPS):
        p = P('TP%d' % (i + 1), '1'); e = (p[0] + 7.62, p[1]); w(p, e)
        if net == 'GND':
            pw('GND', *e)
        elif net.startswith('+'):
            pw(net, *e)
        else:
            lab(net, e[0], e[1], 0)
    return W, L, PW, FL


# ---------------------------------------------------------------------------------------------------
def _local_power(libname, symname, newname):
    sym = [x for x in getsym(libname, symname)]
    sym[1] = q(newname)

    def fix(n):
        for i, c in enumerate(n):
            if isinstance(c, list):
                if c[:1] == ['power']:
                    n[i] = ['power', 'local']
                elif c[:1] == ['symbol']:
                    c[1] = q(unq(c[1]).replace(symname, newname.split(':')[1], 1))
                    fix(c)
                elif c[:1] == ['property'] and unq(c[1]) == 'Description':
                    c[2] = q('Sheet-local power net ' + symname + ' (keeps the /' + symname + ' net name)')
    fix(sym)
    return sym


def _textq(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n') + '"'


def build(PARTS, mod_map, flatten, OUT, PROJECT, ROOT_UUID, uid, prop):
    libsyms, P_, kinds = {}, {}, {}
    parts = []
    for ref, libname, symname, val, fp, netmap, fields in PARTS:
        key = f'{libname}:{symname}'
        if key not in libsyms:
            libsyms[key] = flatten(libname, symname)
            if symname == 'TPS22917DBV':   # QOD tied to VOUT (DS option 1): pin 5 open_collector -> passive for ERC
                def relax(node):
                    if isinstance(node, list):
                        if node and node[0] == 'pin' and any(isinstance(c, list) and c[:1] == ['number'] and
                                                              str(c[1]).strip('"') == '5' for c in node):
                            node[1] = 'passive'
                        for c in node:
                            relax(c)
                relax(libsyms[key])
        ps = [p for p in libpins(libsyms[key]) if p['unit'] in (0, 1)]
        if netmap == 'MODULE':
            netmap = {p['num']: mod_map[p['name']] for p in ps}
        missing = [p['num'] for p in ps if p['num'] not in netmap]
        extra = set(netmap) - {p['num'] for p in ps}
        if missing or extra:
            sys.exit(f'{ref}: unmapped pins {missing} / unknown pins {sorted(extra)}')
        if ref not in PLACE:
            sys.exit(f'{ref}: no placement')
        X, Y, rot = PLACE[ref][:3]
        for p in ps:
            P_[(ref, p['num'])] = xform(p['x'], p['y'], rot, X, Y)
        parts.append((ref, key, val, fp, netmap, fields, ps, X, Y, rot))

    def P(ref, num):
        return P_[(ref, num)]

    W, L, PW, FL = drawing(P)

    # ---- trace the drawing and compare with PARTS ----------------------------------------------
    for pt in [p for poly in W for p in poly] + [(x, y) for _, x, y, _ in L] + [(x, y) for _, x, y, _ in PW] \
            + [(x, y) for _, x, y, _ in FL] + list(P_.values()):
        for v in pt:
            if abs(v / G - round(v / G)) > 1e-6:
                sys.exit(f'off 1.27 mm grid: {pt}')
    pinpts = {}
    for (ref, num), pt in P_.items():
        pinpts.setdefault(pt, []).append((ref, num))
    special = set(pinpts) | {(x, y) for _, x, y, _ in L} | {(x, y) for _, x, y, _ in PW} \
        | {(x, y) for _, x, y, _ in FL} | {p for poly in W for p in poly}
    segs = []
    for poly in W:
        for a, b in zip(poly, poly[1:]):
            if a == b:
                continue
            if a[0] != b[0] and a[1] != b[1]:
                sys.exit(f'diagonal wire {a} {b}')
            inner = sorted([s for s in special if s != a and s != b and
                            min(a[0], b[0]) <= s[0] <= max(a[0], b[0]) and min(a[1], b[1]) <= s[1] <= max(a[1], b[1])
                            and (s[0] == a[0] == b[0] or s[1] == a[1] == b[1])],
                           key=lambda s: abs(s[0] - a[0]) + abs(s[1] - a[1]))
            chain = [a] + inner + [b]
            segs += list(zip(chain, chain[1:]))
    parent = {}

    def f(x):
        parent.setdefault(x, x)
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    def u(a, b):
        parent[f(a)] = f(b)
    for a, b in segs:
        u(a, b)
    byname = {}
    for name, x, y, _ in L:
        byname.setdefault(name, []).append((x, y))
    for net, x, y, _ in PW:
        byname.setdefault(net, []).append((x, y))
    for name, pts in byname.items():
        for p in pts[1:]:
            u(pts[0], p)
    grpname = {}
    for name, pts in byname.items():
        g = f(pts[0])
        if grpname.get(g, name) != name:
            sys.exit(f'nets shorted in drawing: {name} / {grpname[g]}')
        grpname[g] = name
    ends = {}
    for a, b in segs:
        ends[a] = ends.get(a, 0) + 1
        ends[b] = ends.get(b, 0) + 1
    errs = []
    for ref, key, val, fp, netmap, fields, ps, X, Y, rot in parts:
        for p in ps:
            pt = P(ref, p['num']); want = netmap[p['num']]
            touched = pt in ends or any(pt == (x, y) for _, x, y, _ in L + PW + FL) or len(pinpts[pt]) > 1
            if want == 'NC':
                if touched:
                    errs.append(f'{ref}.{p["num"]} should be NC but is connected')
                continue
            got = grpname.get(f(pt))
            if got != want:
                errs.append(f'{ref}.{p["num"]} ({p["name"]}): drawing {got}, spec {want}')
    for net, x, y, _ in FL:
        if grpname.get(f((x, y))) != net:
            errs.append(f'PWR_FLAG {net} at {(x, y)} on {grpname.get(f((x, y)))}')
    for pt, n in ends.items():
        others = (pt in pinpts) + sum(pt == (x, y) for _, x, y, _ in L + PW + FL)
        if n == 1 and not others:
            errs.append(f'dangling wire end {pt}')
        if n >= 4:
            errs.append(f'4-way wire junction at {pt}')
    for name, x, y, _ in L:
        if (x, y) not in ends and (x, y) not in pinpts:
            errs.append(f'label {name} at {(x, y)} not on a wire')
    if errs:
        sys.exit('drawing does not match PARTS:\n  ' + '\n  '.join(errs))
    junctions = sorted(pt for pt in ends if ends[pt] + len(pinpts.get(pt, [])) +
                       sum(pt == (x, y) for _, x, y, _ in PW + FL) >= 3)

    # ---- emit ----------------------------------------------------------------------------------
    items = []
    for ref, key, val, fp, netmap, fields, ps, X, Y, rot in parts:
        fx = field_pos(ref, key, ps, X, Y, rot, libsyms[key])
        sym = ['symbol', ['lib_id', q(key)], ['at', f'{X:.2f}', f'{Y:.2f}', str(rot)], ['unit', '1'],
               ['exclude_from_sim', 'no'],
               ['in_bom', 'yes' if fields.get('Fit') != 'DNP' and not ref.startswith('TP') else 'no'],
               ['on_board', 'yes'], ['dnp', 'yes' if fields.get('Fit') == 'DNP' else 'no'],
               ['uuid', q(uid('sym', ref))],
               sfield('Reference', ref, fx['ref'], rot), sfield('Value', val, fx['val'], rot, ref.startswith('TP')),
               prop('Footprint', fp, X, Y, True), prop('Datasheet', '', X, Y, True)]
        for k, v in fields.items():
            sym.append(prop(k, v, X, Y, True))
        for p in ps:
            sym.append(['pin', q(p['num']), ['uuid', q(uid('pin', ref, p['num']))]])
        sym.append(['instances', ['project', q(PROJECT), ['path', q('/' + ROOT_UUID),
                    ['reference', q(ref)], ['unit', '1']]]])
        items.append(sym)
        for p in ps:
            if netmap[p['num']] == 'NC':
                px, py = P(ref, p['num'])
                items.append(['no_connect', ['at', f'{px:.2f}', f'{py:.2f}'], ['uuid', q(uid('nc', ref, p['num']))]])
    pmap = {'GND': ('power', 'GND'), '+3V3': ('power', '+3V3'), '+5V': ('power', '+5V')}
    for net, (lib, s) in pmap.items():
        libsyms['filexfer_power:' + s] = _local_power(lib, s, 'filexfer_power:' + s)
    libsyms['power:PWR_FLAG'] = flatten('power', 'PWR_FLAG')
    for i, (net, x, y, rot) in enumerate(PW):
        ref = '#PWR%03d' % (i + 1)
        vy = {('GND', 0): (x, y + 3.81, 0), ('GND', 270): (x - 6.35, y, 90)}.get((net, rot), (x, y - 3.81, 0))
        items.append(['symbol', ['lib_id', q('filexfer_power:' + pmap[net][1])], ['at', f'{x:.2f}', f'{y:.2f}', str(rot)],
                      ['unit', '1'], ['exclude_from_sim', 'no'], ['in_bom', 'yes'], ['on_board', 'yes'], ['dnp', 'no'],
                      ['uuid', q(uid('pwr', i))],
                      prop('Reference', ref, x, y, True), fprop('Value', net, vy[0], vy[1], vy[2], 'center', hide=rot == 270),
                      prop('Footprint', '', x, y, True), prop('Datasheet', '', x, y, True),
                      ['pin', q('1'), ['uuid', q(uid('pwrpin', i))]],
                      ['instances', ['project', q(PROJECT), ['path', q('/' + ROOT_UUID),
                       ['reference', q(ref)], ['unit', '1']]]]])
    for i, (net, x, y, rot) in enumerate(FL):
        ref = '#FLG0%d' % (i + 1)
        vy = y + 5.08 if rot == 180 else y - 3.81
        items.append(['symbol', ['lib_id', q('power:PWR_FLAG')], ['at', f'{x:.2f}', f'{y:.2f}', str(rot)], ['unit', '1'],
                      ['exclude_from_sim', 'no'], ['in_bom', 'yes'], ['on_board', 'yes'], ['dnp', 'no'],
                      ['uuid', q(uid('sym', ref))],
                      prop('Reference', ref, x, y, True), fprop('Value', 'PWR_FLAG', x, vy, 0, 'center', 1.0),
                      prop('Footprint', '', x, y, True), prop('Datasheet', '', x, y, True),
                      ['pin', q('1'), ['uuid', q(uid('pin', ref))]],
                      ['instances', ['project', q(PROJECT), ['path', q('/' + ROOT_UUID),
                       ['reference', q(ref)], ['unit', '1']]]]])
    for i, (a, b) in enumerate(segs):
        items.append(['wire', ['pts', ['xy', f'{a[0]:.2f}', f'{a[1]:.2f}'], ['xy', f'{b[0]:.2f}', f'{b[1]:.2f}']],
                      ['stroke', ['width', '0'], ['type', 'default']], ['uuid', q(uid('wire', i))]])
    for i, pt in enumerate(junctions):
        items.append(['junction', ['at', f'{pt[0]:.2f}', f'{pt[1]:.2f}'], ['diameter', '0'], ['color', '0', '0', '0', '0'],
                      ['uuid', q(uid('junc', i))]])
    for i, (name, x, y, a) in enumerate(L):
        just = ['justify', 'right', 'bottom'] if a in (180, 270) else ['justify', 'left', 'bottom']
        items.append(['label', q(name), ['at', f'{x:.2f}', f'{y:.2f}', str(a)],
                      ['effects', ['font', ['size', '1.27', '1.27']], just], ['uuid', q(uid('label', i))]])
    for i, (x0, y0, x1, y1, title) in enumerate(FRAMES):
        items.append(['rectangle', ['start', str(x0), str(y0)], ['end', str(x1), str(y1)],
                      ['stroke', ['width', '0.254'], ['type', 'dash'], ['color', '0', '0', '132', '1']],
                      ['fill', ['type', 'none']], ['uuid', q(uid('frame', i))]])
        items.append(['text', q(title), ['exclude_from_sim', 'no'], ['at', f'{x0 + 2.54:.2f}', f'{y0 + 5.08:.2f}', '0'],
                      ['effects', ['font', ['size', '2.54', '2.54'], ['thickness', '0.508'], ['bold', 'yes'],
                                   ['color', '0', '0', '132', '1']], ['justify', 'left', 'bottom']],
                      ['uuid', q(uid('ftitle', i))]])
    for i, (x, y, t) in enumerate(NOTES):
        items.append(['text', _textq(t), ['exclude_from_sim', 'no'], ['at', f'{x:.2f}', f'{y:.2f}', '0'],
                      ['effects', ['font', ['size', '1.27', '1.27']], ['justify', 'left', 'top']],
                      ['uuid', q(uid('note', i))]])
    doc = ['kicad_sch', ['version', '20250114'], ['generator', q('eeschema')], ['generator_version', q('9.0')],
           ['uuid', q(ROOT_UUID)], ['paper', q('A3')],
           ['title_block', ['title', q('FileXfer rev B (short case)')], ['date', q('2026-11-10')], ['rev', q('B')],
            ['company', q('FileXfer')],
            ['comment', '1', q('Author: Richmond Osei-Baah')],
            ['comment', '2', q('Sheet 1/1: Main (USB input, 3.3 V supply, module, microSD, LED, buttons, test pads)')],
            ['comment', '3', q('ESP32-S3-MINI-1-N8, USB-A plug, microSD (RX build)')],
            ['comment', '4', q('One board for TX and RX builds; Fit field selects the build')]],
           ['lib_symbols'] + list(libsyms.values())] + items + [
           ['sheet_instances', ['path', q('/'), ['page', q('1')]]], ['embedded_fonts', 'no']]
    from sexp import dump
    with open(OUT, 'w') as fh:
        fh.write(dump(doc) + '\n')
    print('wrote', OUT, len(parts), 'parts,', len(segs), 'wires,', len(L), 'labels,', len(PW), 'power symbols,',
          len(junctions), 'junctions')


def fprop(name, value, x, y, angle=0, just='left', size=TXT, hide=False):
    e = ['effects', ['font', ['size', str(size), str(size)]]]
    if just != 'center':
        e.append(['justify', just])
    if hide:
        e.append(['hide', 'yes'])
    return ['property', q(name), q(value), ['at', f'{x:.2f}', f'{y:.2f}', str(angle)], e]


def sfield(name, value, spec, rot, hide=False):
    """Symbol field: position absolute; KiCad stores the text angle relative to the symbol, so write -rot to
    keep the text horizontal. Rotated symbols get centred text (justification would rotate too)."""
    x, y, _, just = spec
    if rot and just != 'center':
        half = 0.53 * len(value) * TXT
        x, just = (x + half if just == 'left' else x - half), 'center'
    return fprop(name, value, _round(x), _round(y), 0 if rot == 180 else (-rot) % 360, just, hide=hide)


# ref -> {'ref': (x, y, angle, just), 'val': (...)}: absolute sheet coords, text horizontal
FIELD_OVERRIDE = {
    'U1': {'ref': (110.49, 76.2, 0, 'left'), 'val': (110.49, 78.74, 0, 'left')},
    'U3': {'ref': (215.9, 38.1, 0, 'center'), 'val': (215.9, 40.64, 0, 'center')},
    'U6': {'ref': (281.94, 121.92, 0, 'center'), 'val': (281.94, 124.46, 0, 'center')},
    'U4': {'ref': (146.05, 127.0, 0, 'left'), 'val': (163.83, 189.23, 0, 'left')},
    'U5': {'ref': (317.5, 58.42, 0, 'left'), 'val': (317.5, 60.96, 0, 'left')},
    'C5': {'ref': (195.58, 43.18, 0, 'right'), 'val': (195.58, 45.72, 0, 'right')},
    'R23': {'ref': (335.28, 166.37, 0, 'right'), 'val': (335.28, 168.91, 0, 'right')},
    'C13': {'ref': (259.08, 127.0, 0, 'right'), 'val': (259.08, 129.54, 0, 'right')},
    'RN1': {'ref': (279.4, 166.37, 0, 'right'), 'val': (279.4, 168.91, 0, 'right')},
}


def field_pos(ref, key, ps, X, Y, rot, lsym):
    if ref in FIELD_OVERRIDE:
        return FIELD_OVERRIDE[ref]
    sym = key.split(':')[1]
    if sym in ('R', 'C', 'Polyfuse'):
        if rot in (0, 180):
            return {'ref': (X + 2.54, Y - 1.27, 0, 'left'), 'val': (X + 2.54, Y + 1.27, 0, 'left')}
        return {'ref': (X, Y - 2.54, 0, 'center'), 'val': (X, Y + 2.54, 0, 'center')}
    if sym == 'D_Zener':
        return {'ref': (X + 2.54, Y - 1.27, 0, 'left'), 'val': (X + 2.54, Y + 1.27, 0, 'left')}
    if sym == 'SW_Push':
        return {'ref': (X + 2.54, Y - 1.27, 0, 'left'), 'val': (X + 2.54, Y + 1.27, 0, 'left')}
    if sym == 'TestPoint':
        return {'ref': (X - 6.35, Y, 0, 'right'), 'val': (X - 6.35, Y + 2.54, 0, 'right')}
    out = {}
    for k, name in (('ref', 'Reference'), ('val', 'Value')):     # library positions (above / below the body)
        pr = [c for c in find(lsym, 'property') if unq(c[1]) == name][0]
        at = find(pr, 'at')[0]
        eff = find(pr, 'effects')
        js = find(eff[0], 'justify') if eff else []
        just = 'left' if js and 'left' in js[0] else 'right' if js and 'right' in js[0] else 'center'
        x, y = xform(float(at[1]), float(at[2]), rot, X, Y)
        out[k] = (x, y, 0, just)
    return out
