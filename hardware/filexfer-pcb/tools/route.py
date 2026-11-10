#!/usr/bin/env python3
"""Route filexfer-pcb.kicad_pcb rev B (short case; run after gen_pcb.py), in place. 2 layers.

1. Scripted: USB pair (D+/D- swap via pair between U1 and R1/R2), plug power (+5V on a top lane
   in front of the J1 signal pads, VBUS_IN via F1), the two other crossings of the USB pair in the
   band under the J1 pads (+3V3 through the J1 pad gaps, SD_VDD), USB test-pad stubs.
2. Other nets: A* maze router, 0.1 mm grid, 2 layers, 8 directions, turn + via cost, bottom penalised
   (bottom = GND reference), clearance inflated from the copper of every other net.
3. GND: fanout via per top GND pad, module EPAD vias, stitching (board edges, plug band, grid),
   GND pours on both layers, pocket vias.
GND-only regions: top inside the module pad ring (except the SD escape corner), bottom under the
module / J3 (except the SD lane zone), bottom under the USB pair.
Run with the system python3 (KiCad 10 pcbnew).
"""
import heapq, math, os, sys
import pcbnew

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_pcb as G

PCB = G.PCB
GRID = 0.1
CLR = 0.127           # net class clearance (JLC 2-layer min 0.10)
MARGIN = 0.02         # paths run through cell centres; covers the chord sag between adjacent cells
EDGE = 0.25
VIA_D, VIA_DRILL = 0.45, 0.2     # JLC 2-layer smallest via without surcharge
HOLE2HOLE = 0.25
ZONE_CLR = 0.2
WIDTH = {'VBUS_IN': 0.6, '+5V': 0.4, '3V3_LDO': 0.4, '+3V3': 0.4, 'SD_VDD': 0.3,
         'SD_PWR_EN': 0.15, 'U0TXD_MOD': 0.15, 'U0TXD': 0.15, 'U0RXD': 0.15, 'EN': 0.15}   # -x strip lanes
X0 = -G.W / 2
NX = int(round(G.W / GRID)) + 1
NY = int(round(G.L / GRID)) + 1
DIRS = [(1, 0), (1, 1), (0, 1), (-1, 1), (-1, 0), (-1, -1), (0, -1), (1, -1)]
USB_NETS = {'USB_DP', 'USB_DM', 'USB_DP_C', 'USB_DM_C'}
SD = {'SD_CMD', 'SD_CLK', 'SD_D0', 'SD_D1', 'SD_D2', 'SD_D3', 'SD_CD'}
BOTTOM_COST = 3.0     # bottom layer is the GND reference: keep tracks there short

# GND-only regions (x0, x1, y0, y1, layer, nets allowed anyway)
YM = G.MOD_CY - 7.0                      # module near-edge pad row (y 17.2)
YB = YM - 3.4                            # bottom GND-only zones start here (behind the front-zone parts)
NOGO = [
    (-6.3, 6.3, YM + 0.55, G.L + 1, 0, set()),              # top inside the module pad ring
    (-6.0, 1.9, YB, G.L + 1, 1, set()),                     # bottom under the module / J3 (-x and centre)
    (-G.W, -6.0, YM - 0.5, G.L + 1, 1, set()),
    (-G.W, -6.0, YB, YM - 0.5, 1, {'EN', 'U0RXD', 'U0TXD', 'U0TXD_MOD', 'SD_PWR_EN', 'LED_DATA_3V3'}),   # -x band hops
    (1.9, 5.0, YB, G.L + 1, 1, SD),                         # SD ribbon under the J3 body, west of its contacts
    (5.0, G.W, YB, G.L + 1, 1, SD | {'SD_VDD', 'BOOT', '+3V3'}),   # J3 contacts + bottom +x strip
    (-1.5, 1.5, 6.6, YB, 1, USB_NETS),                      # bottom under the USB pair
]


def mm(v):
    return pcbnew.FromMM(v)


def loc(v):
    return pcbnew.ToMM(v.x) - G.OX, pcbnew.ToMM(v.y) - G.OY


def pt(x, y):
    return pcbnew.VECTOR2I(mm(G.OX + x), mm(G.OY + y))


class Shape:
    __slots__ = ('kind', 'net', 'layers', 'p', 'cl')

    def __init__(self, kind, net, layers, p, cl=CLR):
        self.kind, self.net, self.layers, self.p, self.cl = kind, net, layers, p, cl

    def bbox(self):
        k, p = self.kind, self.p
        if k == 'rect':
            return p
        if k == 'circ':
            return p[0] - p[2], p[1] - p[2], p[0] + p[2], p[1] + p[2]
        x0, y0, x1, y1, hw = p
        return min(x0, x1) - hw, min(y0, y1) - hw, max(x0, x1) + hw, max(y0, y1) + hw

    def dist(self, x, y):
        k, p = self.kind, self.p
        if k == 'rect':
            dx = max(p[0] - x, 0.0, x - p[2]); dy = max(p[1] - y, 0.0, y - p[3])
            return math.hypot(dx, dy)
        if k == 'circ':
            return max(0.0, math.hypot(x - p[0], y - p[1]) - p[2])
        x0, y0, x1, y1, hw = p
        vx, vy = x1 - x0, y1 - y0
        L2 = vx * vx + vy * vy
        t = 0.0 if L2 == 0 else max(0.0, min(1.0, ((x - x0) * vx + (y - y0) * vy) / L2))
        return max(0.0, math.hypot(x - x0 - t * vx, y - y0 - t * vy) - hw)


class Router:
    def __init__(self, board):
        self.b = board
        self.shapes, self.holes, self.pads = [], [], {}   # pads: net -> [(ref, pad, layers, shape)]
        self.nets = {}
        for code in range(board.GetNetCount()):
            ni = board.FindNet(code)
            if ni:
                self.nets[ni.GetNetname().lstrip('/')] = ni
        for fp in board.GetFootprints():
            for pad in fp.Pads():
                net = pad.GetNetname().lstrip('/') or ('_nc_%s_%s' % (fp.GetReference(), pad.GetNumber()))
                if net.startswith('unconnected'):
                    net = '_nc_' + net
                cx, cy = loc(pad.GetPosition())
                if pad.GetAttribute() == pcbnew.PAD_ATTRIB_NPTH:
                    r = pcbnew.ToMM(pad.GetDrillSize().x) / 2
                    self.shapes.append(Shape('circ', '_hole_', (0, 1), (cx, cy, r), cl=0.25))
                    self.holes.append((cx, cy, r))
                    continue
                layers = tuple(l for l, L in ((0, pcbnew.F_Cu), (1, pcbnew.B_Cu)) if pad.IsOnLayer(L))
                if pad.GetShape(pcbnew.F_Cu) == pcbnew.PAD_SHAPE_CIRCLE:
                    sh = Shape('circ', net, layers, (cx, cy, pcbnew.ToMM(pad.GetSize(pcbnew.F_Cu).x) / 2))
                else:
                    bb = pad.GetBoundingBox()
                    (ax, ay), (bx, by) = loc(bb.GetOrigin()), loc(bb.GetEnd())
                    sh = Shape('rect', net, layers, (min(ax, bx), min(ay, by), max(ax, bx), max(ay, by)))
                self.shapes.append(sh)
                self.pads.setdefault(net, []).append((fp.GetReference(), pad.GetNumber(), layers, sh))
                if pad.GetAttribute() == pcbnew.PAD_ATTRIB_PTH:
                    d = pad.GetDrillSize()
                    self.holes.append((cx, cy, pcbnew.ToMM(max(d.x, d.y)) / 2))
        self.ntracks = self.nvias = 0
        self.copper = {}   # net -> track/via shapes
        self.full_pads = [('U4', None), ('J1', 'SH')]   # GND pads connected solid to the pour (no thermal spokes)

    # ---------------------------------------------------------------- copper creation
    def track(self, net, layer, pts, w):
        for (xa, ya), (xb, yb) in zip(pts, pts[1:]):
            if (xa, ya) == (xb, yb):
                continue
            t = pcbnew.PCB_TRACK(self.b)
            t.SetStart(pt(xa, ya)); t.SetEnd(pt(xb, yb)); t.SetWidth(mm(w))
            t.SetLayer(pcbnew.F_Cu if layer == 0 else pcbnew.B_Cu); t.SetNet(self.nets[net])
            self.b.Add(t)
            sh = Shape('seg', net, (layer,), (xa, ya, xb, yb, w / 2))
            self.shapes.append(sh); self.copper.setdefault(net, []).append(sh)
            self.ntracks += 1

    def via(self, net, x, y):
        v = pcbnew.PCB_VIA(self.b)
        v.SetPosition(pt(x, y)); v.SetWidth(mm(VIA_D)); v.SetDrill(mm(VIA_DRILL)); v.SetNet(self.nets[net])
        self.b.Add(v)
        sh = Shape('circ', net, (0, 1), (x, y, VIA_D / 2))
        self.shapes.append(sh); self.copper.setdefault(net, []).append(sh)
        self.holes.append((x, y, VIA_DRILL / 2))
        self.nvias += 1

    # ---------------------------------------------------------------- grid
    @staticmethod
    def cxy(ix, iy):
        return X0 + ix * GRID, iy * GRID

    @staticmethod
    def ij(x, y):
        return int(round((x - X0) / GRID)), int(round(y / GRID))

    def raster(self, arr, sh, r):
        x0, y0, x1, y1 = sh.bbox()
        i0, j0 = self.ij(x0 - r, y0 - r); i1, j1 = self.ij(x1 + r, y1 + r)
        i0, j0, i1, j1 = max(i0, 0), max(j0, 0), min(i1, NX - 1), min(j1, NY - 1)
        dist = sh.dist
        for j in range(j0, j1 + 1):
            y = j * GRID; base = j * NX
            for i in range(i0, i1 + 1):
                if not arr[base + i] and dist(X0 + i * GRID, y) <= r:
                    arr[base + i] = 1

    def blocked(self, net, hw):
        tb = [bytearray(NX * NY), bytearray(NX * NY)]
        vb = bytearray(NX * NY)
        vr = VIA_D / 2
        for sh in self.shapes:
            if sh.net == net:
                continue
            for l in sh.layers:
                self.raster(tb[l], sh, hw + sh.cl + MARGIN)
            self.raster(vb, sh, vr + sh.cl + MARGIN)
        for _, _, layers, sh in self.pads.get(net, []):   # no via in own SMD pads (solder wicking)
            if len(layers) == 1:
                self.raster(vb, sh, vr + 0.05)
        for (hx, hy, hr) in self.holes:
            self.raster(vb, Shape('circ', None, (), (hx, hy, hr)), VIA_DRILL / 2 + HOLE2HOLE + MARGIN)
        # board edge
        et, ev = hw + EDGE + MARGIN, vr + EDGE + MARGIN
        for j in range(NY):
            y = j * GRID
            for i in range(NX):
                x = X0 + i * GRID
                d = min(x - X0, -X0 - x, y, G.L - y)
                if d < et:
                    tb[0][j * NX + i] = tb[1][j * NX + i] = 1
                if d < ev:
                    vb[j * NX + i] = 1
        # rule areas
        nx0, nx1, ny0, ny1 = G.NOTCH
        self.box(tb[0], nx0, nx1, ny0, ny1, hw + MARGIN); self.box(tb[1], nx0, nx1, ny0, ny1, hw + MARGIN)
        self.box(vb, nx0, nx1, ny0, ny1, vr + MARGIN)
        for x0, x1, y0, y1, l, ok in NOGO:
            if net != 'GND' and net not in ok:
                self.box(tb[l], x0, x1, y0, y1, hw + MARGIN)
                self.box(vb, x0, x1, y0, y1, vr + MARGIN)
        return tb, vb

    def box(self, arr, x0, x1, y0, y1, r):
        i0, j0 = self.ij(x0 - r, y0 - r); i1, j1 = self.ij(x1 + r, y1 + r)
        for j in range(max(j0, 0), min(j1, NY - 1) + 1):
            for i in range(max(i0, 0), min(i1, NX - 1) + 1):
                arr[j * NX + i] = 1

    def cells_of(self, sh, layers, tb):
        x0, y0, x1, y1 = sh.bbox()
        i0, j0 = self.ij(x0, y0); i1, j1 = self.ij(x1, y1)
        out = []
        for l in layers:
            for j in range(max(j0, 0), min(j1, NY - 1) + 1):
                for i in range(max(i0, 0), min(i1, NX - 1) + 1):
                    x, y = self.cxy(i, j)
                    if self.inside(sh, x, y) and not tb[l][j * NX + i]:
                        out.append((l, i, j))
        if not out:   # off-grid tiny pad: nearest cell, if free
            cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
            i, j = self.ij(cx, cy)
            out = [(l, i, j) for l in layers if not tb[l][j * NX + i]]
        return out

    @staticmethod
    def inside(sh, x, y, e=0.03):
        """Point well inside the copper (avoids end cells on a rounded pad edge)."""
        k, p = sh.kind, sh.p
        if k == 'rect':
            return p[0] + e <= x <= p[2] - e and p[1] + e <= y <= p[3] - e
        if k == 'circ':
            return math.hypot(x - p[0], y - p[1]) <= p[2] - e
        return sh.dist(x, y) <= 1e-9 and Shape('seg', None, (), p[:4] + (max(p[4] - e, 0.0),)).dist(x, y) <= 1e-9

    def islands(self, items):
        """Union-find over shapes (copper + pads) of one net -> list of groups."""
        n = len(items); par = list(range(n))
        def f(a):
            while par[a] != a:
                par[a] = par[par[a]]; a = par[a]
            return a
        for a in range(n):
            for b in range(a + 1, n):
                A, B = items[a], items[b]
                if not set(A.layers) & set(B.layers):
                    continue
                ba, bb = A.bbox(), B.bbox()
                if ba[0] > bb[2] + 0.01 or bb[0] > ba[2] + 0.01 or ba[1] > bb[3] + 0.01 or bb[1] > ba[3] + 0.01:
                    continue
                if self.overlap(A, B):
                    par[f(a)] = f(b)
        g = {}
        for a in range(n):
            g.setdefault(f(a), []).append(items[a])
        return list(g.values())

    @staticmethod
    def overlap(A, B):
        """Copper of A and B touch (sampled along B's skeleton)."""
        def skel(S):
            k, p = S.kind, S.p
            if k == 'rect':
                return [((p[0] + p[2]) / 2, (p[1] + p[3]) / 2)] + [(x, y) for x in (p[0], p[2]) for y in (p[1], p[3])]
            if k == 'circ':
                return [(p[0], p[1])]
            n = max(2, int(math.hypot(p[2] - p[0], p[3] - p[1]) / 0.05) + 1)
            return [(p[0] + (p[2] - p[0]) * t / (n - 1), p[1] + (p[3] - p[1]) * t / (n - 1)) for t in range(n)]
        def rad(S):
            return S.p[2] if S.kind == 'circ' else (S.p[4] if S.kind == 'seg' else 0.0)
        return (any(A.dist(x, y) <= rad(B) + 1e-6 for x, y in skel(B))
                or any(B.dist(x, y) <= rad(A) + 1e-6 for x, y in skel(A)))

    def join_islands(self, net, w, log):
        """Connect separate islands of a net's copper (scripted trunks + routed pieces)."""
        for _ in range(6):
            items = list(self.copper.get(net, [])) + [p[3] for p in self.pads.get(net, [])]
            groups = self.islands(items)
            if len(groups) < 2:
                return True
            groups.sort(key=len, reverse=True)
            tb, vb = self.blocked(net, w / 2)
            src = []
            for s in groups[0]:
                src += self.cells_of(s, s.layers, tb)
            tgt = []
            for g in groups[1:]:
                for s in g:
                    tgt += self.cells_of(s, s.layers, tb)
            path = self.astar(src, tgt, tb, vb)
            if not path:
                log.append(f'{net}: islands {len(groups)} FAILED')
                return False
            self.emit(net, path, w)
            log.append(f'{net}: joined islands ({len(groups)} -> {len(groups) - 1})')
        return False

    # ---------------------------------------------------------------- A*
    def astar(self, sources, targets, tb, vb, limit=8000000, bottom=BOTTOM_COST, via_cost=10.0):
        tset = set((l * NY + j) * NX + i for l, i, j in targets)
        if not tset or not sources:
            return None
        tis = [i for _, i, _ in targets]; tjs = [j for _, _, j in targets]
        bi0, bi1, bj0, bj1 = min(tis), max(tis), min(tjs), max(tjs)

        def h(i, j):
            dx = max(bi0 - i, 0, i - bi1); dy = max(bj0 - j, 0, j - bj1)
            return (dx + dy) + (math.sqrt(2) - 2) * min(dx, dy)

        g = {}; came = {}; pq = []
        for l, i, j in sources:
            s = ((l * NY + j) * NX + i) * 9 + 8
            g[s] = 0.0; came[s] = None
            heapq.heappush(pq, (h(i, j), 0.0, s))
        n = 0
        turn = [0.0, 0.35, 2.0, 6.0, 50.0, 6.0, 2.0, 0.35]
        while pq:
            f, gs, s = heapq.heappop(pq)
            if gs > g.get(s, 1e18):
                continue
            k, d = divmod(s, 9)
            if k in tset:
                path = []
                while s is not None:
                    kk = s // 9; l, rem = divmod(kk, NY * NX); j, i = divmod(rem, NX)
                    path.append((l, i, j)); s = came[s]
                return path[::-1]
            n += 1
            if n > limit:
                return None
            l, rem = divmod(k, NY * NX); j, i = divmod(rem, NX)
            lc = bottom if l == 1 else 1.0
            for nd, (dx, dy) in enumerate(DIRS):
                ii, jj = i + dx, j + dy
                if ii < 0 or jj < 0 or ii >= NX or jj >= NY:
                    continue
                c = jj * NX + ii
                if tb[l][c]:
                    continue
                if dx and dy and (tb[l][j * NX + ii] or tb[l][jj * NX + i]):
                    continue   # no corner cutting
                step = (1.41421356 if dx and dy else 1.0) * lc + (0.0 if d == 8 else turn[(nd - d) % 8])
                ns = ((l * NY + jj) * NX + ii) * 9 + nd
                ng = gs + step
                if ng < g.get(ns, 1e18):
                    g[ns] = ng; came[ns] = s
                    heapq.heappush(pq, (ng + h(ii, jj), ng, ns))
            c = j * NX + i
            ol = 1 - l
            if not vb[c] and not tb[ol][c]:
                ns = ((ol * NY + j) * NX + i) * 9 + 8
                ng = gs + via_cost
                if ng < g.get(ns, 1e18):
                    g[ns] = ng; came[ns] = s
                    heapq.heappush(pq, (ng + h(i, j), ng, ns))
        return None

    def emit(self, net, path, w):
        """Convert a cell path into tracks + vias; returns the new cells."""
        runs, cur = [], [path[0]]
        for a, b in zip(path, path[1:]):
            if a[0] != b[0]:
                runs.append(cur); cur = [b]
            else:
                cur.append(b)
        runs.append(cur)
        for ri, run in enumerate(runs):
            pts = [self.cxy(i, j) for _, i, j in run]
            simp = [pts[0]]
            for k in range(1, len(pts) - 1):
                ax, ay = simp[-1]; bx, by = pts[k]; cx, cy = pts[k + 1]
                if abs((bx - ax) * (cy - by) - (by - ay) * (cx - bx)) > 1e-9:
                    simp.append(pts[k])
            simp.append(pts[-1])
            if len(simp) > 1 and simp[0] != simp[-1]:
                self.track(net, run[0][0], simp, w)
            if ri < len(runs) - 1:
                self.via(net, *self.cxy(run[-1][1], run[-1][2]))

    def route_net(self, net, widths, log):
        pads = self.pads.get(net, [])
        if len(pads) < 2 and not self.copper.get(net):
            return True
        todo = list(pads)
        # start from existing copper (scripted tracks) or the first pad
        have_seg = list(self.copper.get(net, []))
        tree = list(have_seg) if have_seg else [todo.pop(0)[3]]
        if have_seg:
            todo = [p for p in todo if not any(self.touch(p[3], s) for s in have_seg)]
        ok = True
        while todo:
            # nearest pad to the tree
            def dist_to_tree(p):
                bx = p[3].bbox(); cx, cy = (bx[0] + bx[2]) / 2, (bx[1] + bx[3]) / 2
                return min(s.dist(cx, cy) for s in tree)
            todo.sort(key=dist_to_tree)
            ref, num, layers, sh = todo.pop(0)
            done = False
            for w in widths:
                tb, vb = self.blocked(net, w / 2)
                src = []
                for s in tree:
                    src += self.cells_of(s, s.layers, tb)
                tgt = self.cells_of(sh, layers, tb)
                path = self.astar(src, tgt, tb, vb)
                if path:
                    before = len(self.shapes)
                    self.emit(net, path, w)
                    tree += [s for s in self.shapes[before:]] + [sh]
                    log.append(f'{net}: {ref}.{num} w={w} cells={len(path)}')
                    done = True
                    break
            if not done:
                log.append(f'{net}: {ref}.{num} FAILED')
                tree.append(sh); ok = False
        return ok

    @staticmethod
    def touch(a, b):
        x0, y0, x1, y1 = a.bbox()
        return b.dist((x0 + x1) / 2, (y0 + y1) / 2) <= 0.01 and set(a.layers) & set(b.layers)

    # ---------------------------------------------------------------- GND
    def gnd_fanout(self, log):
        """A via next to every top-side GND SMD pad (except the module), via a short track."""
        for ref, num, layers, sh in self.pads.get('GND', []):
            if layers != (0,) or ref == 'U4':
                continue
            tb, vb = self.blocked('GND', 0.15)
            src = self.cells_of(sh, (0,), tb)
            x0, y0, x1, y1 = sh.bbox()
            tgt = []
            for j in range(*self.span(y0 - 1.6, y1 + 1.6)):
                for i in range(*self.span(x0 - 1.6, x1 + 1.6, NX)):
                    c = j * NX + i
                    if not tb[1][c] and not vb[c] and not tb[0][c]:
                        tgt.append((1, i, j))
            path = self.astar(src, tgt, tb, vb, limit=60000, bottom=5.0, via_cost=2.0)
            if path:
                self.emit('GND', path, 0.3)
            else:
                log.append(f'GND fanout {ref}.{num}: none (solid pour connection)')
                if not (ref[0] in 'RC' and ref[1:].isdigit()):   # 0402 passives keep spokes (tombstoning); see fix_starved
                    self.full_pads.append((ref, num))

    def span(self, a, b, n=None):
        n = n or NY
        if n == NX:
            return max(self.ij(a, 0)[0], 0), min(self.ij(b, 0)[0], NX - 1) + 1
        return max(self.ij(0, a)[1], 0), min(self.ij(0, b)[1], NY - 1) + 1

    def try_via(self, x, y, vb):
        i, j = self.ij(x, y)
        if 0 <= i < NX and 0 <= j < NY and not vb[j * NX + i]:
            xx, yy = self.cxy(i, j)
            self.via('GND', xx, yy)
            self.raster(vb, Shape('circ', None, (), (xx, yy, VIA_D / 2)), VIA_D / 2 + 0.6)
            return True
        return False

    def stitch(self, log):
        n0 = self.nvias
        for dx in (-0.825, 0.825):   # module EPAD vias (DS: vias between the 3x3 thermal pads)
            for dy in (-0.825, 0.825):
                self.via('GND', dx, G.MOD_CY + dy)
        tb, vb = self.blocked('GND', 0.15)
        for k in range(-6, 7):   # antenna-edge row inside the module ring, 1 mm pitch
            self.try_via(k * 1.0, G.MOD_CY + 5.4, vb)
        for k in range(-5, 6):   # module ring, near edge (behind the SD escape) and sides
            self.try_via(k * 1.0, YM + 3.3, vb)
        for y in (YM + 3.3, G.MOD_CY, G.MOD_CY + 3.0):
            self.try_via(-5.6, y, vb); self.try_via(5.6, y, vb)
        for x, y in ((-5.3, 7.0), (5.3, 7.0), (-4.6, 7.0), (4.6, 7.0), (-1.6, 3.9 + 0.6),
                     (1.6, 7.2), (-1.6, 7.2), (-6.0, 6.2), (6.0, 6.2)):   # both sides of the plug band
            self.try_via(x, y, vb)
        y = 1.0
        while y < G.L - 0.5:     # both long edges, 2 mm pitch
            self.try_via(-G.W / 2 + 0.75, y, vb); self.try_via(G.W / 2 - 0.75, y, vb)
            y += 2.0
        for j in range(0, int(G.L / 2.0) + 1):   # interior grid, 2 mm
            for i in range(-4, 5):
                self.try_via(i * 2.0, j * 2.0 + 0.5, vb)
        log.append(f'stitching vias: {self.nvias - n0}')

    def pour(self):
        for fp in self.b.GetFootprints():
            for pad in fp.Pads():
                if pad.GetNetname().lstrip('/') == 'GND' and any(
                        fp.GetReference() == r and (n is None or pad.GetNumber() == n) for r, n in self.full_pads):
                    pad.SetLocalZoneConnection(pcbnew.ZONE_CONNECTION_FULL)
        for L in (pcbnew.F_Cu, pcbnew.B_Cu):
            z = pcbnew.ZONE(self.b)
            z.SetLayer(L); z.SetNet(self.nets['GND']); z.SetZoneName('GND_' + ('F' if L == pcbnew.F_Cu else 'B'))
            z.SetLocalClearance(mm(ZONE_CLR)); z.SetMinThickness(mm(0.2))
            z.SetThermalReliefGap(mm(0.25)); z.SetThermalReliefSpokeWidth(mm(0.3))
            z.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL)
            z.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS)
            o = z.Outline(); o.NewOutline()
            for x, y in ((-G.W / 2, 0), (G.W / 2, 0), (G.W / 2, G.L), (-G.W / 2, G.L)):
                o.Append(mm(G.OX + x), mm(G.OY + y))
            self.b.Add(z)
        pcbnew.ZONE_FILLER(self.b).Fill(self.b.Zones())

    def fix_starved(self, log):
        """0402 GND pads keep thermal spokes; any pad kicad-cli DRC reports as starved is poured solid instead."""
        import re, subprocess, tempfile
        d = tempfile.mkdtemp(); f = os.path.join(d, 'b.kicad_pcb'); rpt = os.path.join(d, 'drc.rpt')
        pcbnew.SaveBoard(f, self.b)
        subprocess.run(['kicad-cli', 'pcb', 'drc', '-o', rpt, f], capture_output=True)
        hits = set(re.findall(r'\[starved_thermal\](?:(?!\n\[).)*?Pad (\S+) \[/GND\] of (\S+)', open(rpt).read(), re.S))
        for num, ref in sorted(hits):
            for pad in self.b.FindFootprintByReference(ref).Pads():
                if pad.GetNumber() == num:
                    pad.SetLocalZoneConnection(pcbnew.ZONE_CONNECTION_FULL)
        if hits:
            pcbnew.ZONE_FILLER(self.b).Fill(self.b.Zones())
        log.append('starved thermals -> solid: ' + (', '.join(f'{r}.{n}' for n, r in sorted(hits)) or 'none'))

    def pocket_vias(self, log):
        """Every filled GND island on either layer gets at least one via (pockets left by routing)."""
        vias = [(sh.p[0], sh.p[1]) for sh in self.copper.get('GND', []) if sh.kind == 'circ']
        vias += [((s.p[0] + s.p[2]) / 2, (s.p[1] + s.p[3]) / 2) for _, _, L, s in self.pads.get('GND', []) if L == (0, 1)]
        tb, vb = self.blocked('GND', 0.15)
        added = 0
        for z in list(self.b.Zones()):
            if z.GetIsRuleArea() or z.GetNetname().lstrip('/') != 'GND':
                continue
            L = z.GetLayer(); fp = z.GetFilledPolysList(L)
            for k in range(fp.OutlineCount()):
                o = fp.COutline(k)
                if any(o.PointInside(pt(x, y)) for x, y in vias):
                    continue
                bb = o.BBox()
                (x0, y0), (x1, y1) = loc(bb.GetOrigin()), loc(bb.GetEnd())
                done = False
                for j in range(*self.span(y0, y1)):
                    for i in range(*self.span(x0, x1, NX)):
                        if vb[j * NX + i] or tb[0][j * NX + i] or tb[1][j * NX + i]:
                            continue
                        x, y = self.cxy(i, j)
                        if o.PointInside(pt(x, y)):
                            self.via('GND', x, y); vias.append((x, y)); added += 1
                            self.raster(vb, Shape('circ', None, (), (x, y, VIA_D / 2)), VIA_D / 2 + 0.6)
                            done = True
                            break
                    if done:
                        break
                if not done:
                    log.append(f'GND island at ({x0:.1f},{y0:.1f}) {"F" if L == pcbnew.F_Cu else "B"}: no via spot')
        if added:
            pcbnew.ZONE_FILLER(self.b).Fill(self.b.Zones())
        log.append(f'pocket vias: {added}')


def usb_pair(r):
    """J1 (D- x -1.0, D+ x +1.0) -> U1 (rot 90: D- x -0.54, D+ x +0.54) -> swap (D+ hops on the bottom)
    -> R1 (D-, x 0) / R2 (D+, x -1.0) -> module pins 23 (x 0) / 24 (x -0.85)."""
    W = 0.4
    yv1, yv2 = 11.1, 13.25                     # D+ swap vias (plug side, module side)
    r.track('USB_DM_C', 0, [(-1.0, 5.52), (-1.0, 6.6), (-0.54, 7.06), (-0.54, 9.3), (-0.85, 9.61),
                            (-0.85, 10.6), (0.0, 11.45), (0.0, 14.54)], W)
    r.track('USB_DM', 0, [(0.0, 15.56), (0.0, YM)], 0.3)
    r.track('USB_DP_C', 0, [(1.0, 5.52), (1.0, 6.6), (0.54, 7.06), (0.54, 9.3), (0.85, 9.61), (0.85, yv1)], W)
    r.via('USB_DP_C', 0.85, yv1)
    r.track('USB_DP_C', 1, [(0.85, yv1), (0.85, 11.4), (-1.0, yv2 - 0.1), (-1.0, yv2)], W)
    r.via('USB_DP_C', -1.0, yv2)
    r.track('USB_DP_C', 0, [(-1.0, yv2), (-1.0, 14.54)], W)
    r.track('USB_DP', 0, [(-1.0, 15.56), (-1.0, 16.1), (-0.85, 16.25), (-0.85, YM)], 0.3)
    # U1.2 GND: via between the diverging pair
    r.track('GND', 0, [(0.0, 9.38), (0.0, 9.9)], 0.25); r.via('GND', 0.0, 9.9)
    # test pads (bottom): TP8 from the D+ swap via, TP9 via a short top stub on D-
    r.track('USB_DP_C', 1, [(-1.0, yv2), (-2.15, 12.1)], 0.3)
    r.track('USB_DM_C', 0, [(-0.54, 8.4), (-1.3, 8.4)], 0.3); r.via('USB_DM_C', -1.3, 8.4)
    r.track('USB_DM_C', 1, [(-1.3, 8.4), (-2.2, 8.65)], 0.3)


def plug_power(r):
    """+5V: F1.2 -> top lane y 4.15 in front of the J1 signal pads (no USB copper there) -> D1, with the
    U1 VBUS pin tapped straight down between the pair. VBUS_IN: J1.1 -> via -> bottom -> via between the
    F1 pads -> F1.1 (board edge). Crossings of the USB pair under the J1 pads (bottom, shortest):
    +3V3 through the J1 pad gaps (vias x -2.25 / +2.25). SD_VDD crosses in px_corner (y 10.45)."""
    YL, YH = 4.15, 5.3
    r.track('+5V', 0, [(-7.95, 4.0), (-7.95, 4.6), (-7.25, YH)], 0.4)
    r.track('+5V', 0, [(-7.25, YH), (-4.5, YH)], 0.4)
    r.track('+5V', 0, [(-4.5, YH), (-4.5, YL), (4.5, YL), (4.5, YH)], 0.25)
    r.track('+5V', 0, [(4.5, YH), (7.0, YH), (7.95, 4.35), (7.95, 4.05)], 0.4)
    r.track('+5V', 0, [(0.0, 7.53), (0.0, YL)], 0.25)
    # VBUS_IN
    r.track('VBUS_IN', 0, [(-3.5, 5.52), (-3.5, 6.95)], 0.6); r.via('VBUS_IN', -3.5, 6.95)
    r.track('VBUS_IN', 1, [(-3.5, 6.95), (-3.75, 6.7), (-7.7, 6.7), (-7.95, 6.45), (-7.95, 2.6)], 0.6)
    r.via('VBUS_IN', -7.95, 2.6)
    r.track('VBUS_IN', 0, [(-7.95, 2.6), (-7.95, 1.2)], 0.6)
    # +3V3 crossing
    # +x: top stub from the main rail; -x: bottom band continues west to a via beside C12, then top up to R4.1
    # (feeds R4 / U6.1 / R11 / C13 through mx_corner)
    r.via('+3V3', 2.25, 5.4); r.track('+3V3', 0, [(2.25, 5.4), (2.25, 6.8)], 0.3)
    r.track('+3V3', 1, [(2.25, 5.4), (-5.8, 5.4), (-6.2, 5.8), (-6.2, 5.9)], 0.3); r.via('+3V3', -6.2, 5.9)
    r4 = loc([p for p in r.b.FindFootprintByReference('R4').Pads() if p.GetNumber() == '1'][0].GetPosition())
    r.track('+3V3', 0, [(-6.2, 5.9), (-6.2, 8.75), (-6.51, r4[1]), r4], 0.25)


def strips(r):
    """Lanes beside the module (strips hold no parts except C8). Pins -> band in front of the module.
    -x, inner -> outer = front -> back pin (no crossings): SD_PWR_EN, U0TXD_MOD, U0RXD, EN.
    +x: BOOT (pin 4) -> R5, +3V3 (C8 / pin 3) -> R5; C8.2 GND -> pin 2."""
    yb = 18.75
    py = {n: loc(p.GetPosition())[1] for p in r.b.FindFootprintByReference('U4').Pads()
          for n in [p.GetNetname().lstrip('/')] if n in ('SD_PWR_EN', 'U0TXD_MOD', 'U0RXD', 'EN', 'BOOT', '+3V3')}
    # LED_DATA_3V3: pin 30 -> via beside the pin -> 3 mm bottom hop west of the J3 -x shield pad -> R9 / U5
    xl, yl = [loc(p.GetPosition()) for p in r.b.FindFootprintByReference('U4').Pads()
              if p.GetNetname().lstrip('/') == 'LED_DATA_3V3'][0]
    r.track('LED_DATA_3V3', 0, [(xl, yl), (xl, yl - 0.45), (xl + 0.45, yl - 0.95)], 0.2)
    r.via('LED_DATA_3V3', xl + 0.45, yl - 0.95)
    r.track('LED_DATA_3V3', 1, [(xl + 0.45, yl - 0.95), (-6.15, yl - 1.55), (-6.15, 15.95)], 0.2)
    r.via('LED_DATA_3V3', -6.15, 15.95)
    r.track('LED_DATA_3V3', 0, [(-6.15, 15.95), (-5.6, 15.4), (-5.6, 15.26)], 0.2)
    # -x: pin order forces the lane order (a greedy router takes the outer lane first); lanes on the 0.1 grid,
    # 0.14 wide at 0.3 pitch so the maze router (0.15, clearance 0.127) can continue from their ends in the band
    # SD_PWR_EN (front-most pin) hops 2.5 mm on the bottom at its pin and takes the OUTER lane, so the lane
    # order (W->E PWR_EN, EN, RXD, TXD) matches the targets (U6.3 at the edge; EN / RXD / TXD test pads)
    for net, x in (('U0TXD_MOD', -7.6), ('U0RXD', -7.9), ('EN', -8.2)):
        r.track(net, 0, [(-7.0, py[net]), (x, py[net]), (x, 19.3)], 0.14)
    yp, xa, xo = py['SD_PWR_EN'], -6.2, -8.675
    r.track('SD_PWR_EN', 0, [(-7.0, yp), (xa, yp)], 0.15); r.via('SD_PWR_EN', xa, yp)
    r.track('SD_PWR_EN', 1, [(xa, yp), (xo, yp)], 0.15); r.via('SD_PWR_EN', xo, yp)
    r5 = {p.GetNetname().lstrip('/'): loc(p.GetPosition()) for p in r.b.FindFootprintByReference('R5').Pads()}
    c8 = {p.GetNetname().lstrip('/'): loc(p.GetPosition()) for p in r.b.FindFootprintByReference('C8').Pads()}
    yB, y3, xs = py['BOOT'], py['+3V3'], 8.35
    r.track('BOOT', 0, [(7.0, yB), (7.75, yB), (7.75, r5['BOOT'][1] + 0.6), r5['BOOT']], 0.2)
    # +3V3: R5 -> lane x 8.3 -> C8.1 (front pad) -> diagonal to pin 3, clear of the BOOT lane end and C8.2
    r.track('+3V3', 0, [r5['+3V3'], (xs, r5['+3V3'][1] + 0.6), (xs, y3 - 0.8), (7.4, y3 + 0.1), (7.0, y3)], 0.25)
    r.track('+3V3', 0, [(xs, c8['+3V3'][1]), c8['+3V3']], 0.25)
    r.track('GND', 0, [c8['GND'], (7.6, yB + 1.7), (7.0, yB + 1.7)], 0.25)


def mx_corner(r):
    """-x front corner (scripted): strip lanes -> R8 / vias -> bottom -> test pads; U6 cluster; LED hop."""
    P = lambda ref: {p.GetNumber(): loc(p.GetPosition()) for p in r.b.FindFootprintByReference(ref).Pads()}
    u6, r11, c13, r8, r9, u5, r4, c9 = (P(x) for x in ('U6', 'R11', 'C13', 'R8', 'R9', 'U5', 'R4', 'C9'))
    tp = {n: P('TP%d' % n)['1'] for n in (4, 6, 7)}
    xo = -8.675
    # SD_PWR_EN: outer lane down the edge (west of C13) into U6.3, tee to R11.2
    r.track('SD_PWR_EN', 0, [(xo, 21.25), (xo, u6['3'][1] + 0.15)], 0.15)
    r.track('SD_PWR_EN', 0, [(xo, r11['2'][1]), r11['2']], 0.15)
    # U0TXD_MOD -> R8.1; U0TXD: R8.2 -> via -> bottom -> TP6
    r.track('U0TXD_MOD', 0, [(-7.6, 19.3), (-7.6, 18.6), (r8['1'][0], 18.6), r8['1']], 0.15)
    tv = (-6.75, 15.85)
    r.track('U0TXD', 0, [r8['2'], tv], 0.15); r.via('U0TXD', *tv)
    r.track('U0TXD', 1, [tv, (tp[6][0], tv[1] - (tp[6][0] - tv[0])), tp[6]], 0.15)
    # U0RXD: lane -> via -> bottom -> TP7
    rv = (-7.75, 17.55)
    r.track('U0RXD', 0, [(-7.9, 19.3), (-7.9, 17.7), rv], 0.15); r.via('U0RXD', *rv)
    r.track('U0RXD', 1, [rv, (-7.75, 13.75), (-7.5, 13.5), (-5.7, 13.5), (-5.7, tp[7][1] + 0.85), tp[7]], 0.15)
    # EN: lane -> via west of C13 -> bottom -> TP4 -> via -> R4.2 / C9.1 (EN RC beside U6.1)
    ev, ev2 = (-8.2, 16.1), (-7.55, 7.6)
    r.track('EN', 0, [(-8.2, 19.3), ev], 0.15); r.via('EN', *ev)
    r.track('EN', 1, [ev, (-8.2, tp[4][1] + 0.8), (tp[4][0] - 0.25, tp[4][1]), tp[4]], 0.15)
    r.track('EN', 1, [tp[4], (ev2[0], tp[4][1] - 0.65), ev2], 0.15); r.via('EN', *ev2)
    r.track('EN', 0, [c9['1'], (ev2[0] - 0.2, ev2[1]), (ev2[0] + 0.2, ev2[1]), r4['2']], 0.15)
    # U6 cluster: +3V3 U6.1 -> under the U6 body -> R11.1 -> C13.2; R4.1 -> U6.1; SD_CT U6.4 -> C13.1
    r.track('+3V3', 0, [u6['1'], (-6.95, u6['1'][1]), (-6.95, r11['1'][1]), r11['1']], 0.25)
    r.track('+3V3', 0, [r11['1'], (-7.3, 14.1), c13['2']], 0.25)
    r.track('+3V3', 0, [r4['1'], (-7.6, r4['1'][1] + 0.75), (-7.6, u6['1'][1])], 0.25)
    r.track('SD_CT', 0, [u6['4'], (-6.17, u6['4'][1]), (-6.17, 15.4), (c13['1'][0], 15.4)], 0.15)
    # LED_DATA_3V3: R9.1 -> east of R9 -> between U6 and U5 -> U5.2
    r.track('LED_DATA_3V3', 0, [r9['1'], (-4.975, r9['1'][1]), (-4.975, u5['2'][1]), u5['2']], 0.15)


def px_corner(r):
    """+x front corner (scripted): the RN1/C14/R22/C15 row spans the board, so +3V3 (to C7/R5/strip) and
    SD_VDD (to J3.4) cross under it on short bottom lanes. Bottom lanes W->E: D2 6.6, D3 6.95, BOOT 7.45,
    +3V3 8.0, SD_VDD 8.6. U3 VIN (pad 1) <-> ON (pad 3) tie runs along the board edge (no via)."""
    P = lambda ref: {p.GetNumber(): loc(p.GetPosition()) for p in r.b.FindFootprintByReference(ref).Pads()}
    u3, u6, rn, c14, c15, r22, c7, r5 = (P(x) for x in ('U3', 'U6', 'RN1', 'C14', 'C15', 'R22', 'C7', 'R5'))
    tp5 = P('TP5')['1']
    r.track('3V3_LDO', 0, [u3['1'], (8.8, u3['1'][1]), (8.8, u3['3'][1]), u3['3']], 0.2)
    # +3V3: U3.6 -> south of the row -> via -> bottom 8.0 -> via -> C7.1 -> R5.1
    r.track('+3V3', 0, [u3['6'], (u3['6'][0] + 0.7, 13.5), (7.95, 13.5)], 0.3); r.via('+3V3', 7.95, 13.5)
    r.track('+3V3', 1, [(7.95, 13.5), (8.0, 13.55), (8.0, 16.35)], 0.3); r.via('+3V3', 8.0, 16.35)
    r.track('+3V3', 0, [(8.0, 16.35), (c7['1'][0], 16.83), c7['1'], r5['1']], 0.3)
    # SD_VDD: U6 -> via -> bottom across the front (y 10.25) -> via -> RN1 -> row -> C15 -> via -> bottom 8.6 -> J3.4
    r.track('SD_VDD', 0, [u6['5'], u6['6'], (-5.2, 9.75)], 0.3); r.via('SD_VDD', -5.2, 9.75)
    r.track('SD_VDD', 1, [(-5.2, 9.75), (-4.5, 10.45), (1.0, 10.45), (1.25, 10.7), (3.3, 10.7), (3.55, 10.95), (3.55, 13.45)], 0.3)
    r.via('SD_VDD', 3.55, 13.45)
    r.track('GND', 0, [(2.6, 11.0), (2.6, 10.0)], 0.3); r.via('GND', 2.6, 10.0)   # C6.2 (pocket)
    r.track('SD_VDD', 0, [(3.55, 13.45), (3.95, 13.85), (3.95, 14.1), (rn['5'][0], 14.1), c14['1'], r22['2'], c15['1'],
                          (c15['1'][0], 13.5)], 0.3)
    r.via('SD_VDD', c15['1'][0], 13.5)
    j4 = P('J3')['4']
    r.track('SD_VDD', 1, [(c15['1'][0], 13.5), (8.6, 13.55), (8.6, j4[1] - 0.3), (8.3, j4[1]), (j4[0] + 0.4, j4[1])], 0.3)
    # D3: R22.1 -> via -> bottom 6.95 -> J3 D3 pad (east end)
    j = P('J3')
    r.track('SD_D3', 0, [r22['1'], (r22['1'][0], 15.5), (6.95, 16.15)], 0.2); r.via('SD_D3', 6.95, 16.15)
    r.track('SD_D3', 1, [(6.95, 16.15), (6.95, j['2'][1]), (6.4, j['2'][1])], 0.2)
    # D2: RN1.4 -> via -> bottom 6.6 -> J3 D2 pad
    r.track('SD_D2', 0, [rn['4'], (rn['4'][0], 15.5), (6.45, 16.5), (6.45, 16.9)], 0.2); r.via('SD_D2', 6.45, 16.9)
    r.track('SD_D2', 1, [(6.45, 16.9), (6.62, 17.07), (6.62, 21.7)], 0.2)
    # BOOT: R5.2 -> via -> bottom 7.45 -> TP5
    r.track('BOOT', 0, [(7.75, 18.6), (7.55, 18.75)], 0.2); r.via('BOOT', 7.55, 18.75)
    r.track('BOOT', 1, [(7.55, 18.75), (7.45, 18.65), (7.45, 12.6), tp5], 0.2)


def sd_escape(r):
    """SD: D3 / D2 (+x side pins) drop through the module corner (vias just inside the pad ring); CMD, CLK,
    D0, D1, CD get a via in the band straight in front of their pin, then a grouped bottom ribbon west of
    the J3 contacts (east -> west = J3 pad order), each line turning east into its pad."""
    pins = {p.GetNetname().lstrip('/'): loc(p.GetPosition()) for p in r.b.FindFootprintByReference('U4').Pads()}
    j3 = {p.GetNetname().lstrip('/'): loc(p.GetPosition()) for p in r.b.FindFootprintByReference('J3').Pads()
          if p.GetNumber() not in ('SH', '')}
    xin = j3['SD_CMD'][0] - 0.8 + 0.1                      # pad west end (+0.1 inside)
    w = 0.2
    # D3: pin -> via inside the ring -> bottom west of the D2 pad -> D3 pad from the west
    x3, y3 = pins['SD_D3']; xv = 6.15
    r.track('SD_D3', 0, [(x3, y3), (xv, y3)], w); r.via('SD_D3', xv, y3)
    r.track('SD_D3', 1, [(xv, y3), (4.6, y3), (4.6, j3['SD_D3'][1]), (xin, j3['SD_D3'][1])], w)
    # D2: pin -> via inside the ring, landing on the D2 pad (via in pad, hand-soldered DNP part)
    x2, y2 = pins['SD_D2']
    r.track('SD_D2', 0, [(x2, y2), (xv, y2)], w); r.via('SD_D2', xv, y2)
    yv = YM - 1.0
    for net, xl in (('SD_CMD', 4.0), ('SD_CLK', 3.65), ('SD_D0', 3.3), ('SD_D1', 2.95), ('SD_CD', 2.6)):
        xp, yp = pins[net]
        r.track(net, 0, [(xp, yp), (xp, yv)], w); r.via(net, xp, yv)
        r.track(net, 1, [(xp, yv), (xl, yv + (xp - xl)), (xl, j3[net][1]), (xin, j3[net][1])], w)
    # pull-ups: RN1 pin order (W->E CMD, D0, D1, D2) is mirrored vs the module (W->E D1, D0, CMD), so D0 goes
    # straight on top and D1 / CMD hop on the bottom in front of the J3 +x shield pad (y < 16.9).
    rn = {p.GetNetname().lstrip('/'): loc(p.GetPosition()) for p in r.b.FindFootprintByReference('RN1').Pads()
          if p.GetNumber() in '1234'}
    x0 = pins['SD_D0'][0]
    r.track('SD_D0', 0, [(x0, yv), (x0, yv - 0.4), (rn['SD_D0'][0], yv - 0.6), rn['SD_D0']], w)
    x1 = pins['SD_D1'][0]; yh = 16.5
    r.track('SD_D1', 1, [(x1, yv), (x1, yh), (rn['SD_D1'][0], yh)], w); r.via('SD_D1', rn['SD_D1'][0], yh)
    r.track('SD_D1', 0, [(rn['SD_D1'][0], yh), rn['SD_D1']], w)
    xc = pins['SD_CMD'][0]; yc = 15.9; xcv = rn['SD_CMD'][0] - 0.05
    r.track('SD_CMD', 1, [(xc, yv), (xc, yc), (xcv, yc)], w); r.via('SD_CMD', xcv, yc)
    r.track('SD_CMD', 0, [(xcv, yc), rn['SD_CMD']], w)


def main():
    board = pcbnew.LoadBoard(PCB)
    for t in list(board.GetTracks()):
        board.Remove(t)
    for z in list(board.Zones()):
        if not z.GetIsRuleArea():
            board.Remove(z)
    r = Router(board)
    log = []
    usb_pair(r)
    plug_power(r)
    strips(r)
    mx_corner(r)
    px_corner(r)
    sd_escape(r)
    power = ['+5V', '3V3_LDO', '+3V3', 'SD_VDD']
    others = [n for n in r.pads if n not in power and n != 'GND' and not n.startswith('_')
              and n not in USB_NETS and n != 'VBUS_IN']

    def span(n):
        xs = [p[3].bbox() for p in r.pads[n]]
        return (max(b[2] for b in xs) - min(b[0] for b in xs)) + (max(b[3] for b in xs) - min(b[1] for b in xs))
    others.sort(key=span)
    sd = [n for n in ('SD_D2', 'SD_D3', 'SD_CMD', 'SD_CLK', 'SD_D0', 'SD_D1', 'SD_CD') if n in others]
    lanes = [n for n in ('LED_DATA_3V3', 'SD_CT', 'EN', 'SD_PWR_EN', 'U0RXD', 'U0TXD_MOD', 'U0TXD', 'BOOT') if n in others]
    order = sd + lanes + power + [n for n in others if n not in sd and n not in lanes]
    failed = []
    for net in order:
        w = WIDTH.get(net, 0.2)
        widths = [w] + [x for x in (0.3, 0.2) if x < w]
        if not r.route_net(net, widths, log):
            failed.append(net)
        elif not r.join_islands(net, 0.3 if net in WIDTH else 0.2, log):
            failed.append(net)
        print(log[-1] if log else net, flush=True)
    r.gnd_fanout(log)
    r.stitch(log)
    r.pour()
    r.pocket_vias(log)
    r.fix_starved(log)
    pcbnew.SaveBoard(PCB, board)
    print('\n'.join(l for l in log if 'FAILED' in l or 'fanout' in l or 'stitch' in l or 'island' in l or 'pocket' in l or 'starved' in l))
    print(f'tracks {r.ntracks}, vias {r.nvias}, failed nets: {failed}')


if __name__ == '__main__':
    main()
