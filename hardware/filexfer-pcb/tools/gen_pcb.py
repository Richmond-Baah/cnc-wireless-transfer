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
W = 18.3                    # board width (case inner 18.54)
PLUG_EDGE_TO_HOLES = 2.92   # XKB: board edge to hole row (EasyEDA C319178)
NOTCH = (-6.5, 6.5, 0.0, 3.9)   # x0,x1,y0,y1: plug-shell zone kept free for a possible notch
SD_Y0 = 23.75               # SD courtyard start (local y)
SD_OY = SD_Y0 + 3.65        # SD footprint origin y (courtyard starts 3.65 before origin)
G0 = SD_Y0 + 17.25          # passives rows between SD and module (6 mm; USB channel in the middle)
MOD_CY = G0 + 6.05 + 8.0    # module pad-field centre (courtyard starts 8.0 before it)
L = MOD_CY + 7.7            # board end = antenna boundary; antenna overhangs 5.1 mm
E = W / 2 - 0.6             # 0402 column next to the board edge (pad edge 0.29 from edge)
R1Y, R2Y = G0 + 0.98, G0 + 2.96
SD_COL = {'CD': 2.3, 'D1': 3.5, 'D0': 4.5, 'CLK': 5.5, 'CMD': 6.5, 'D3': 7.5, 'D2': 8.5}
SR_Y = G0 + 3.56

# ref: (x, y, rot, side)   side 'F' or 'B'
P = {
    # --- plug + input protection (beside the plug, outside the notch reserve) ---------------
    'J1': (0, PLUG_EDGE_TO_HOLES, 180, 'F'),
    'F1': (-7.95, 2.6, 90, 'F'), 'D1': (7.95, 2.6, 90, 'F'),
    # --- row A: USB ESD, LDO input cap, LED ---------------------------------------------------
    'U1': (0, 8.95, 90, 'F'),
    'C3': (-5.0, 7.75, 0, 'F'),
    'D2': (3.6, 8.2, 0, 'F'), 'C12': (5.8, 8.2, 90, 'F'),
    # --- row B: USB series R, LDO, soft-start, LED buffer -----------------------------------------
    'U2': (-5.4, 15.55, 90, 'F'),
    'U3': (3.9, 12.9, 0, 'F'), 'C5': (6.55, 12.9, 90, 'F'), 'C6': (8.1, 13.0, 90, 'F'),
    'C4': (3.2, 15.6, 0, 'F'),
    'U5': (7.0, 18.2, 0, 'F'), 'R10': (2.2, 18.2, 90, 'F'), 'R9': (3.3, 18.2, 90, 'F'),
    'C11': (4.35, 18.2, 90, 'F'),
    # --- row C: buttons (TC2030 dropped: bottom test pads are the pogo-jig target) ---------------
    'SW2': (-5.5, 21.9, 0, 'F'), 'SW1': (5.5, 21.9, 0, 'F'), 'R5': (1.9, 21.9, 90, 'F'),
    # --- SD socket (card slot faces -x) ------------------------------------------------------------
    'J3': (-W / 2 + 4.02, SD_OY, 270, 'F'),
    # --- row G (between SD and module): SD series R, pull-ups, SD switch, board ID ----------------
    # SD columns, pitch 1.0 (x = SD_COL): pull-up at R1Y (VDD pad up), via, series R at SR_Y
    **{r: (SD_COL[n], SR_Y, 90, 'F') for r, n in (('R15', 'D1'), ('R14', 'D0'), ('R12', 'CLK'),
                                                  ('R13', 'CMD'), ('R17', 'D3'), ('R16', 'D2'))},
    **{r: (SD_COL[n], R1Y, 90, 'F') for r, n in (('R20', 'D1'), ('R19', 'D0'), ('R18', 'CMD'),
                                                 ('R22', 'D3'), ('R21', 'D2'))},
    'R23': (8.0, 24.6, 180, 'F'),                   # CD pull-up, strip beside J3
    # USB channel (HDG: series R and reserves near the chip): DP line x = -1.1, DM line x = 0
    'R2': (-1.1, G0 + 4.6, 270, 'F'), 'R1': (0.0, G0 + 4.6, 270, 'F'),
    'C2': (-1.61, G0 + 0.6, 180, 'F'), 'C1': (0.51, G0 + 0.6, 0, 'F'), 'R3': (0.6, 13.6, 0, 'F'),        # DNP D+ pull-up, beside the D+ via at the plug end
    # SD power switch group (route.py sd_power): U6 west pins +3V3 / GND / ON, east VOUT / VOUT / CT
    'U6': (-5.2, 43.2, 0, 'F'), 'R11': (-7.75, 43.2, 270, 'F'), 'C13': (-4.4, 45.75, 0, 'F'),
    'C15': (-2.2, 43.0, 90, 'F'), 'C14': (-2.2, 44.9, 270, 'F'),
    # board ID divider: strip beside the module's +x pins (IO1, pin 5)
    'C10': (E, 51.1, 270, 'F'), 'R7': (E, 53.8, 90, 'F'), 'R6': (E, 55.9, 90, 'F'),
    # --- module + strips beside it -----------------------------------------------------------
    'U4': (0, MOD_CY, 180, 'F'),
    'C7': (E, 59.65, 270, 'F'), 'C8': (E, 61.5, 90, 'F'),   # GND pads level with module GND pins 2 / 1
    'R8': (-E, 52.0, 90, 'F'), 'R4': (-E, 58.0, 270, 'F'), 'C9': (-E, 61.0, 270, 'F'),   # -x strip (route.py bus)
    # --- bottom: test pads (copper only, no assembly) under the power section ----------------------
    **{'TP%d' % (i + 1): (xy[0], xy[1], 0, 'B') for i, xy in enumerate(
        [(-7.5, 12.0), (-5.2, 12.0), (7.5, 9.0), (-2.9, 16.5), (2.9, 16.5),
         (-7.5, 16.5), (-5.2, 16.5), (-2.2, 20.0), (1.35, 20.0), (8.35, 53.6)])},   # TP10 under the ID divider
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


def main():
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
    text(board, 'FileXfer rev A', 0, 32.5, pcbnew.B_SilkS)
    pcbnew.SaveBoard(PCB, board)
    print(f'wrote {PCB}: {len(comps)} footprints, {len(netinfo)} nets, board {W} x {L:.1f} mm '
          f'(+5.1 antenna overhang)')


if __name__ == '__main__':
    main()
