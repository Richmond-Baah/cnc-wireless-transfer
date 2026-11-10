#!/usr/bin/env python3
"""Generate filexfer-pcb.kicad_pcb (placement only) from the schematic netlist.

Local frame: x across the board (-W/2..+W/2), y along it; y = 0 is the USB plug edge, +y toward the
antenna. KiCad position = ORIGIN + (x, y). Rotations are KiCad degrees (CCW on screen).
Run with the system python3 (pcbnew module from KiCad 10).
"""
import os, subprocess, sys, tempfile
import pcbnew
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sexp import parse, find, unq

HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
SCH = os.path.join(HERE, 'filexfer-pcb.kicad_sch')
PCB = os.path.join(HERE, 'filexfer-pcb.kicad_pcb')
LIBS = {'filexfer': os.path.join(HERE, 'filexfer.pretty')}
STOCK = '/usr/share/kicad/footprints'

OX, OY = 100.0, 100.0
W = 18.3                    # board width (rev B short case, inner 18.54; case to be widened, see revB-notes)
PLUG_EDGE_TO_HOLES = 2.92   # XKB: board edge to hole row (EasyEDA C319178)
NOTCH = (-6.5, 6.5, 0.0, 3.9)   # x0,x1,y0,y1: plug-shell zone kept free for a possible notch
MOD_CY = 24.2               # module pad-field centre (courtyard starts 8.11 before it, y 16.09)
L = MOD_CY + 7.7            # board end = antenna boundary; antenna overhangs 5.1 mm
E = 8.55                    # 0402 column beside the module (pad edge 0.25 from the board edge, as rev A)
RV = 14.75                  # row of 0402s / RN1 just in front of the module (SD vias at y ~15.85)

# ref: (x, y, rot, side)   side 'F' or 'B'.  Top = JLC assembly; bottom = J3, SW1 (hand-soldered) + test pads
P = {
    # --- plug + input protection (beside the plug, outside the notch reserve) ---------------
    'J1': (0, PLUG_EDGE_TO_HOLES, 180, 'F'),
    'F1': (-7.95, 2.6, 270, 'F'), 'D1': (7.95, 2.6, 90, 'F'),   # F1: +5V pad inboard, VBUS_IN at the edge
    # --- centre channel: USB ESD at the plug, series R at the module (D+/D- swap via between) --------
    'U1': (0, 8.45, 90, 'F'),
    'R2': (-1.0, 15.05, 270, 'F'), 'R1': (0.0, 15.05, 270, 'F'),   # connector side toward U1
    # --- +x half: LDO -> soft-start -> +3V3, then SD pull-ups in front of the module SD pins ----------
    'C3': (2.55, 8.4, 90, 'F'), 'U2': (4.85, 8.2, 180, 'F'), 'C4': (7.05, 8.4, 90, 'F'),
    'C6': (2.6, 11.8, 90, 'F'), 'C5': (4.3, 11.1, 90, 'F'), 'U3': (7.0, 11.85, 180, 'F'),
    'R23': (2.2, RV, 90, 'F'), 'RN1': (4.7, RV - 0.15, 90, 'F'),
    'C14': (6.55, RV, 90, 'F'), 'C15': (7.6, RV, 90, 'F'), 'R22': (E, RV, 90, 'F'),
    # --- -x half: LED + buffer, SD power switch --------------------------------------------------------
    'D2': (-3.3, 8.25, 0, 'F'), 'C12': (-5.55, 8.25, 90, 'F'),
    'U5': (-3.4, 12.0, 0, 'F'), 'R10': (-1.4, 11.9, 90, 'F'), 'C11': (-3.4, 14.4, 0, 'F'),
    'U6': (-7.0, 11.65, 0, 'F'),
    'R11': (-E, 19.4, 90, 'F'), 'C13': (-7.5, RV, 90, 'F'), 'R9': (-5.95, RV, 90, 'F'),   # R11: -x strip at pin 31
    # --- strips beside the module ------------------------------------------------------------------------
    # +x: board ID divider at IO1 (pin 5), BOOT pull-up at pin 4, 3V3 decoupling at pin 3 / GND pins 2, 1
    'C10': (E, 20.1, 270, 'F'), 'R7': (E, 22.05, 90, 'F'), 'R6': (E, 24.0, 90, 'F'),
    'R5': (E, 25.95, 90, 'F'), 'C8': (E, 27.9, 90, 'F'), 'C7': (E, 29.85, 270, 'F'),
    # -x: TXD series R, EN RC (the strip also carries EN / TXD / RXD / SD_PWR_EN lanes)
    'R8': (-E, 25.0, 90, 'F'), 'R4': (-E, 28.0, 270, 'F'), 'C9': (-E, 30.0, 270, 'F'),
    # --- module ----------------------------------------------------------------------------------------------
    'U4': (0, MOD_CY, 180, 'F'),
    # --- bottom: microSD under the module (slot faces -x), BOOT switch, test pads (pogo grid, 2.35 pitch) ----
    'J3': (-5.13, 27.0, 90, 'B'),
    'SW1': (4.8, 8.4, 0, 'B'),
    **{'TP%d' % n: (x, y, 0, 'B') for n, x, y in (
        (1, 2.5, 12.0), (3, 4.85, 12.0), (5, 7.2, 12.0),                     # +3V3, +5V, BOOT
        (8, -2.5, 12.0), (4, -4.85, 12.0), (6, -7.2, 12.0),                  # USB_DP_C, EN, U0TXD
        (9, -2.5, 8.7), (7, -4.85, 8.7), (2, -7.2, 8.7),                     # USB_DM_C, U0RXD, GND
        (10, 8.05, 30.3))},                                                  # BOARD_ID, strip beside J3
}


def mm(v):
    return pcbnew.FromMM(v)


def pt(x, y):
    return pcbnew.VECTOR2I(mm(OX + x), mm(OY + y))


def netlist():
    out = os.path.join(tempfile.mkdtemp(), 'n.net')
    subprocess.run(['kicad-cli', 'sch', 'export', 'netlist', '--format', 'kicadsexpr', '-o', out, SCH],
                   check=True, capture_output=True)
    for f in os.listdir(HERE):
        if f.endswith('.lck'):
            os.remove(os.path.join(HERE, f))
    n = parse(open(out).read())
    comps = {}
    for c in find(find(n, 'components')[0], 'comp'):
        ref = unq(find(c, 'ref')[0][1])
        fields = {unq(fl[1][1]): unq(fl[2]) if len(fl) > 2 else '' for fl in find(find(c, 'fields')[0], 'field')} \
            if find(c, 'fields') else {}
        comps[ref] = dict(fp=unq(find(c, 'footprint')[0][1]), value=unq(find(c, 'value')[0][1]), fields=fields)
    nets = {}
    for net in find(find(n, 'nets')[0], 'net'):
        name = unq(find(net, 'name')[0][1])
        for node in find(net, 'node'):
            nets[(unq(find(node, 'ref')[0][1]), unq(find(node, 'pin')[0][1]))] = name
    return comps, nets


def load_fp(libfp):
    lib, name = libfp.split(':')
    path = LIBS.get(lib, os.path.join(STOCK, lib + '.pretty'))
    fp = pcbnew.FootprintLoad(path, name)
    if fp is None:
        sys.exit('footprint not found: ' + libfp)
    fp.SetFPID(pcbnew.LIB_ID(lib, name))
    return fp


def edge(board):
    r = 0.5
    x0, x1, y0, y1 = -W / 2, W / 2, 0.0, L
    def seg(a, b):
        s = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_SEGMENT)
        s.SetStart(pt(*a)); s.SetEnd(pt(*b)); s.SetLayer(pcbnew.Edge_Cuts); s.SetWidth(mm(0.05)); board.Add(s)
    def arc(c, a, b):
        s = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_ARC)
        s.SetCenter(pt(*c)); s.SetStart(pt(*a)); s.SetEnd(pt(*b)); s.SetLayer(pcbnew.Edge_Cuts)
        s.SetWidth(mm(0.05)); board.Add(s)
    seg((x0 + r, y0), (x1 - r, y0)); arc((x1 - r, y0 + r), (x1 - r, y0), (x1, y0 + r))
    seg((x1, y0 + r), (x1, y1 - r)); arc((x1 - r, y1 - r), (x1, y1 - r), (x1 - r, y1))
    seg((x1 - r, y1), (x0 + r, y1)); arc((x0 + r, y1 - r), (x0 + r, y1), (x0, y1 - r))
    seg((x0, y1 - r), (x0, y0 + r)); arc((x0 + r, y0 + r), (x0, y0 + r), (x0 + r, y0))


def keepout(board, x0, x1, y0, y1, name, tracks=True, vias=True, pour=True, pads=False, fps=False):
    z = pcbnew.ZONE(board)
    z.SetIsRuleArea(True)
    z.SetZoneName(name)
    ls = pcbnew.LSET(); ls.AddLayer(pcbnew.F_Cu); ls.AddLayer(pcbnew.B_Cu)
    z.SetLayerSet(ls)
    z.SetDoNotAllowTracks(tracks); z.SetDoNotAllowVias(vias); z.SetDoNotAllowZoneFills(pour)
    z.SetDoNotAllowPads(pads); z.SetDoNotAllowFootprints(fps)
    o = z.Outline(); o.NewOutline()
    for x, y in ((x0, y0), (x1, y0), (x1, y1), (x0, y1)):
        o.Append(mm(OX + x), mm(OY + y))
    board.Add(z)


def text(board, s, x, y, layer, size=0.8):
    t = pcbnew.PCB_TEXT(board)
    t.SetText(s); t.SetPosition(pt(x, y)); t.SetLayer(layer)
    t.SetMirrored(layer in (pcbnew.B_SilkS, pcbnew.B_Fab))
    t.SetTextSize(pcbnew.VECTOR2I(mm(size), mm(size))); t.SetTextThickness(mm(size * 0.15))
    board.Add(t)


def drc_exclude_j3_pegs(keep):
    """J3 locating pegs (NPTH, bottom) sit in the U4 courtyard on purpose: they fall in the free ring
    between the module edge pads and its centre GND pads (>= 0.9 mm from any pad; peg protrusion
    ~0.6 mm per the SHOU HAN TF PUSH drawing, scaled, < 1.6 mm board). Record them as DRC exclusions
    in the project (UUIDs change on every regeneration, so this runs after each save)."""
    import json
    pro = os.path.join(HERE, 'filexfer-pcb.kicad_pro')   # keep: project text before any kicad-cli run
    out = os.path.join(tempfile.mkdtemp(), 'drc.json')
    subprocess.run(['kicad-cli', 'pcb', 'drc', '--format', 'json', '--severity-all', '-o', out, PCB],
                   capture_output=True)
    keys = []
    for v in json.load(open(out))['violations']:
        it = v['items']
        if v['type'] == 'npth_inside_courtyard' and 'J3' in it[0]['description'] and 'U4' in it[1]['description']:
            keys.append('npth_inside_courtyard|%d|%d|%s|%s' % (round(it[0]['pos']['x'] * 1e6), round(it[0]['pos']['y'] * 1e6),
                                                               it[0]['uuid'], it[1]['uuid']))
    d = json.loads(keep)
    d['board']['design_settings']['drc_exclusions'] = keys
    with open(pro, 'w') as f:
        json.dump(d, f, indent=2); f.write('\n')
    return len(keys)


def main():
    pro_text = open(os.path.join(HERE, 'filexfer-pcb.kicad_pro')).read()   # kicad-cli rewrites it; restored below
    comps, nets = netlist()
    missing = set(comps) - set(P)
    if missing:
        sys.exit('no placement for: ' + ', '.join(sorted(missing)))
    board = pcbnew.BOARD()
    ds = board.GetDesignSettings()
    ds.SetBoardThickness(mm(1.6))
    # JLCPCB 2-layer capabilities (min): 0.127 track/space, 0.3 drill / 0.45 via, 0.2 copper-to-edge.
    ds.m_MinClearance = mm(0.127); ds.m_TrackMinWidth = mm(0.127); ds.m_MinConn = mm(0.127)
    ds.m_ViasMinSize = mm(0.45); ds.m_ViasMinAnnularWidth = mm(0.1); ds.m_MinThroughDrill = mm(0.25)
    ds.m_HoleToHoleMin = mm(0.5); ds.m_HoleClearance = mm(0.25); ds.m_CopperEdgeClearance = mm(0.25)
    ds.m_MinSilkTextHeight = mm(0.8); ds.m_MinSilkTextThickness = mm(0.12); ds.m_SilkClearance = mm(0.0)
    nc = ds.m_NetSettings.GetDefaultNetclass()
    nc.SetClearance(mm(0.15)); nc.SetTrackWidth(mm(0.2)); nc.SetViaDiameter(mm(0.6)); nc.SetViaDrill(mm(0.3))
    board.SetCopperLayerCount(2)
    netinfo = {}
    for name in sorted(set(nets.values())):
        ni = pcbnew.NETINFO_ITEM(board, name)
        board.Add(ni)
        netinfo[name] = ni
    for ref in sorted(comps):
        c = comps[ref]
        fp = load_fp(c['fp'])
        fp.SetReference(ref)
        fp.SetValue(c['value'])
        for k in c['fields']:   # all custom symbol fields (Fit, MPN, LCSC, Note, Alt, ...)
            if k not in ('Reference', 'Value', 'Footprint', 'Datasheet', 'Description') and c['fields'].get(k):
                fp.SetField(k, c['fields'][k])
                fp.GetField(k).SetVisible(False)
        if c['fields'].get('Fit') == 'DNP':
            fp.SetDNP(True); fp.SetExcludedFromBOM(True); fp.SetExcludedFromPosFiles(True)
        if ref.startswith('TP'):  # copper only (pogo-jig pads)
            fp.SetExcludedFromBOM(True); fp.SetExcludedFromPosFiles(True)
        x, y, rot, side = P[ref]
        board.Add(fp)
        fp.SetPosition(pt(x, y))
        if side == 'B':
            fp.Flip(fp.GetPosition(), pcbnew.FLIP_DIRECTION_TOP_BOTTOM)
        fp.SetOrientationDegrees(rot)
        for pad in fp.Pads():
            n = nets.get((ref, pad.GetNumber()))
            if n:
                pad.SetNet(netinfo[n])
        fp.Reference().SetVisible(False)   # dense board: refs on F.Fab / assembly drawing only
    edge(board)
    keepout(board, *NOTCH, 'PLUG_NOTCH_RESERVE')
    text(board, 'FileXfer rev B', 0, 31.2, pcbnew.B_SilkS)
    pcbnew.SaveBoard(PCB, board)
    print(f'wrote {PCB}: {len(comps)} footprints, {len(netinfo)} nets, board {W} x {L:.1f} mm '
          f'(+5.1 antenna overhang); DRC exclusions (J3 pegs): {drc_exclude_j3_pegs(pro_text)}')


if __name__ == '__main__':
    main()
