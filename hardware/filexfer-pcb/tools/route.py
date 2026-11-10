#!/usr/bin/env python3
"""Route filexfer-pcb.kicad_pcb (run after gen_pcb.py), in place.

1. USB pair: scripted geometry (w 0.5 / gap 0.15 / GND 0.2 -> about 90 ohm, tools/zdiff.py),
   bottom-layer coupled run under the SD socket, D+/D- swap at the vias next to U1.
2. Other nets: A* maze router, 0.1 mm grid, 2 layers, 8 directions, turn + via cost, clearance
   inflated from the actual copper of every other net (pads, tracks, vias, holes, board edge).
   Power nets wide (fallback to narrower only where needed).
3. GND: fanout via per top GND pad, module EPAD vias (DS Fig 11-1), stitching (antenna edge row,
   board edges, grid), then GND pours on both layers.
GND-only regions: bottom under the module, top inside the module pad ring, top under the SD body.
Run with the system python3 (KiCad 10 pcbnew).
"""
import heapq, math, os, sys
import pcbnew

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_pcb as G

PCB = G.PCB
GRID = 0.1
CLR = 0.15            # net class clearance
MARGIN = 0.06         # grid discretisation margin
EDGE = 0.25
VIA_D, VIA_DRILL = 0.6, 0.3
HOLE2HOLE = 0.5
ZONE_CLR = 0.2        # pour clearance = coplanar gap of the USB pair
WIDTH = {'VBUS_IN': 0.6, '+5V': 0.5, '3V3_LDO': 0.5, '+3V3': 0.5, 'SD_VDD': 0.4}
X0 = -G.W / 2
NX = int(round(G.W / GRID)) + 1
NY = int(round(G.L / GRID)) + 1
DIRS = [(1, 0), (1, 1), (0, 1), (-1, 1), (-1, 0), (-1, -1), (0, -1), (1, -1)]
USB_NETS = {'USB_DP', 'USB_DM', 'USB_DP_C', 'USB_DM_C'}

# GND-only regions (x0, x1, y0, y1, layer)
MOD = (-7.7, 7.7, G.MOD_CY - 7.7, G.L + 1)
NOGO = [
    (-6.3, 6.3, MOD[2], MOD[3], 1),                                       # bottom under the module core (outer bands allowed)
    (-6.3, 6.3, G.MOD_CY - 6.3, G.MOD_CY + 6.3, 0),                        # inside the module pad ring
    (-G.W / 2 - 1, 5.35, G.SD_OY - 2.425, G.SD_OY + 12.325, 0),            # top under the SD body
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
        self.full_pads = [('U4', None)]   # GND pads connected solid to the pour (no thermal spokes)

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
        if net != 'GND':
            for x0, x1, y0, y1, l in NOGO:
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
    def astar(self, sources, targets, tb, vb, limit=8000000, bottom=1.4, via_cost=10.0):
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
        # module EPAD vias (DS Fig 11-1: vias between the 3x3 thermal pads)
        for dx in (-0.825, 0.825):
            for dy in (-0.825, 0.825):
                self.via('GND', dx, G.MOD_CY + dy)
        tb, vb = self.blocked('GND', 0.15)
        for k in range(-6, 7):   # antenna-edge row inside the module ring, 1 mm pitch
            self.try_via(k * 1.0, G.MOD_CY + 5.4, vb)
        for y in (G.L - 0.7,):   # corners beside the module
            self.try_via(-G.W / 2 + 0.75, y, vb); self.try_via(G.W / 2 - 0.75, y, vb)
        y = 1.0
        while y < G.L - 0.5:     # both long edges, 2 mm pitch
            self.try_via(-G.W / 2 + 0.75, y, vb); self.try_via(G.W / 2 - 0.75, y, vb)
            y += 2.0
        for j in range(4, int(G.L / 2.0)):   # interior grid, 2 mm
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
    """Scripted USB pair. Plug side: D- x=-0.95, D+ x=+0.95 (J1/U1); module: D+ x=-0.85, D- x=0."""
    W, WN = 0.5, 0.4
    G0, P = G.G0, G.MOD_CY - 7.0              # module bottom-row pad centre y
    yv1, ydp, yv2 = 11.3, 13.05, G0 - 0.6     # D- via, D+ via (plug end), both vias (module end)
    # D- : J1.2 -> U1.6 -> U1.1 -> via -> bottom (x -0.1) -> via (0, yv2) -> C1 -> R1 -> module 23
    r.track('USB_DM_C', 0, [(-1.0, 5.52), (-0.95, 6.5), (-0.95, yv1)], W)
    r.via('USB_DM_C', -0.95, yv1)
    r.track('USB_DM_C', 1, [(-0.95, yv1), (-0.1, yv1 + 0.85), (-0.1, yv2 - 0.8), (0.0, yv2 - 0.7), (0.0, yv2)], W)
    r.via('USB_DM_C', 0.0, yv2)
    r.track('USB_DM_C', 0, [(0.0, yv2), (0.0, G0 + 4.09)], W)
    r.track('USB_DM', 0, [(0.0, G0 + 5.11), (0.0, P)], WN)
    # D+ : J1.3 -> U1.4 -> U1.3 -> top diagonal over D- -> via -> bottom (x -0.75) -> via (-1.1, yv2) -> C2, R3 -> R2 -> 24
    r.track('USB_DP_C', 0, [(1.0, 5.52), (0.95, 6.5), (0.95, 10.8), (-1.3, ydp)], W)
    r.via('USB_DP_C', -1.3, ydp)
    r.track('USB_DP_C', 1, [(-1.3, ydp), (-0.75, ydp + 0.55), (-0.75, yv2 - 1.15), (-1.1, yv2 - 0.8), (-1.1, yv2)], W)
    r.via('USB_DP_C', -1.1, yv2)
    r.track('USB_DP_C', 0, [(-1.1, yv2), (-1.1, G0 + 4.09)], W)
    r.track('USB_DP', 0, [(-1.1, G0 + 5.11), (-0.85, G0 + 5.36), (-0.85, P)], WN)
    # test pad stubs (bottom)
    r.track('USB_DP_C', 1, [(-0.75, 20.0), (-2.2, 20.0)], 0.3)
    r.track('USB_DM_C', 1, [(-0.1, 20.0), (1.35, 20.0)], 0.3)
    r.track('USB_DP_C', 0, [(0.09, 13.6), (-0.88, 12.63)], 0.3)                   # R3 (DNP)


def sd_fan(r):
    """Scripted SD bus. J3 pins -> top stub -> via in the strip (x 7.35) -> bottom staircase lanes
    (no crossings) -> column via -> pull-up (above) / series R (below) -> module pins in edge order."""
    C, G0 = G.SD_COL, G.G0
    VX, YV = 7.35, G0 + 2.25                       # escape via x, column via y
    PU_V, PU_S = G.R1Y - 0.51, G.R1Y + 0.51        # pull-up pads: VDD, signal
    SR_C, SR_M = G.SR_Y - 0.51, G.SR_Y + 0.51      # series pads: card side, module side
    JX = 5.87
    pins = {'CD': 25.8, 'D1': 26.9, 'D0': 28.0, 'CLK': 30.2, 'CMD': 32.4, 'D3': 33.5, 'D2': 34.6}
    #        L (first vertical)  T (turn y)
    lane = {'CD': (3.99, 35.6), 'D1': (4.55, 36.3), 'D0': (5.11, 37.0), 'CLK': (5.67, 37.7),
            'CMD': (6.23, 37.0), 'D3': (6.79, 36.3), 'D2': (VX, 35.6)}
    cnet = {'CD': 'SD_CD', 'D1': 'SD_D1_C', 'D0': 'SD_D0_C', 'CLK': 'SD_CLK_C', 'CMD': 'SD_CMD_C',
            'D3': 'SD_D3_C', 'D2': 'SD_D2_C'}
    W = 0.2
    for k, y in pins.items():
        n = cnet[k]; L, T = lane[k]; xc = C[k]
        r.track(n, 0, [(JX, y), (VX, y)], W)
        r.via(n, VX, y)
        if k == 'CD':
            yend = G0 - 0.8
        else:
            yend = YV
        pts = [(VX, y), (L, y), (L, T), (xc + (0.25 if k == 'D1' else 0), T)]
        if k == 'D1':
            pts += [(3.75, YV - 0.5), (xc, YV - 0.25)]
        pts += [(xc, yend)]
        r.track(n, 1, pts, W)
        r.via(n, xc, yend)
        if k == 'CD':
            px = 2.55   # module pin 20 (IO16)
            r.track('SD_CD', 0, [(xc, yend), (xc, G.MOD_CY - 7.25), (px, G.MOD_CY - 7.0)], W)
            r.track('SD_CD', 0, [(VX, y), (7.49, 24.6)], W)          # R23 (CD pull-up)
            continue
        top = [(xc, YV), (xc, SR_C)]
        if k != 'CLK':
            top = [(xc, PU_S)] + top
        r.track(n, 0, top, W)
    # module side: series R -> module pins (16..19 on the near edge, 15/14 on the +x side)
    P0 = G.MOD_CY - 7.0
    for k, net, px in (('D1', 'SD_D1', 3.40), ('D0', 'SD_D0', 4.25), ('CLK', 'SD_CLK', 5.10), ('CMD', 'SD_CMD', 5.95)):
        xc = C[k]; yb = SR_M + 1.43
        r.track(net, 0, [(xc, SR_M), (xc, yb), (px, yb + abs(xc - px)), (px, P0)], W)
    r.track('SD_D3', 0, [(C['D3'], SR_M), (C['D3'], 46.6), (7.7, 46.8), (7.7, 48.9), (7.5, 49.1), (7.0, 49.1)], W)  # clear of corner GND pad
    r.track('SD_D2', 0, [(C['D2'], SR_M), (C['D2'], 49.55), (8.1, 49.95), (7.0, 49.95)], W)
    # SD_VDD: J3.4 -> strip lane x 8.1 -> pull-up VDD row; via towards the switch (left group)
    WV = 0.4
    r.track('SD_VDD', 0, [(JX, 31.3), (8.1, 31.3), (8.1, PU_V - 0.4), (C['D2'], PU_V)], WV)
    r.track('SD_VDD', 0, [(C['D1'], PU_V), (C['D2'], PU_V)], WV)
    r.track('SD_VDD', 0, [(2.95, G0 - 0.3), (C['D1'], PU_V)], WV)
    r.via('SD_VDD', 2.95, G0 - 0.3)
    # SD_VDD to the switch U6 (left group): bottom under the USB channel, up at (-1.95, G0-0.3)
    r.track('SD_VDD', 1, [(2.95, G0 - 0.3), (2.25, G0 + 0.4), (-1.25, G0 + 0.4), (-1.95, G0 - 0.3)], WV)
    r.via('SD_VDD', -1.95, G0 - 0.3)
    sd_power(r)


def sd_power(r):
    """U6 group (U6 at (-5.2, 43.2) rot 0): SD_VDD in at the NE pin, QOD tied to VOUT, C15 / C14 east,
    C13 (CT) south, R11 west, +3V3 from the aux trunk (-8.6) and the main trunk (-3.35)."""
    W3 = 0.3
    v = [(-1.95, 40.7), (-2.75, 40.7), (-3.05, 41.0), (-3.05, 44.42), (-2.2, 44.42)]   # via -> C14.1
    r.track('SD_VDD', 0, v, W3)
    r.track('SD_VDD', 0, [(-3.05, 42.25), (-4.06, 42.25), (-4.06, 43.2)], W3)            # VOUT pin 6, QOD pin 5
    r.track('SD_VDD', 0, [(-3.05, 43.48), (-2.2, 43.48)], W3)                             # C15.1
    r.track('SD_CT', 0, [(-4.06, 44.15), (-4.06, 44.7), (-4.88, 45.52), (-4.88, 45.75)], 0.2)   # CT -> C13.1
    r.track('+3V3', 0, [(-3.92, 45.75), (-3.25, 46.4)], W3)                               # C13.2
    r.track('GND', 0, [(-2.09, 41.6), (-1.95, 42.05), (-2.2, 42.52)], 0.25)              # C2.2, C15.2 pocket
    r.via('GND', -1.95, 42.05)
    r.via('+3V3', -3.25, 46.4)
    r.track('+3V3', 1, [(-3.25, 46.4), (-3.35, 46.3), (-3.35, 45.6)], W3)
    r.via('+3V3', -8.6, 41.9)                                                              # aux trunk
    r.track('+3V3', 0, [(-8.6, 41.9), (-6.34, 41.9), (-6.34, 42.25)], W3)                  # pin 1
    r.track('+3V3', 0, [(-7.75, 41.9), (-7.75, 42.69)], W3)                                # R11.1
    r.track('SD_PWR_EN', 0, [(-6.34, 44.15), (-7.4, 44.15), (-7.75, 43.8), (-7.75, 43.71)], 0.2)   # ON, R11.2
    r.track('SD_PWR_EN', 0, [(-7.75, 43.71), (-8.25, 44.21), (-8.25, 48.6), (-7.75, 49.1), (-7.0, 49.1)], 0.2)  # pin 31


def bus(r):
    """Front <-> module trunks, bottom layer under the SD socket, left of the USB pair.
    West group continues into the -x band beside the module; BOOT and +3V3 turn east under
    the G row (south of SD_VDD) into the +x band and come up beside pins 4 / 3."""
    Y0, W = 23.2, 0.2
    # -x group, west -> east: +3V3 (aux, R4 / left group), U0TXD, U0RXD, EN, LED
    r.track('+3V3', 1, [(-7.5, 12.0), (-8.6, 13.1), (-8.6, 55.9)], 0.3)            # from TP1
    r.via('+3V3', -8.6, 55.9)
    r.track('+3V3', 0, [(-8.6, 55.9), (-8.6, 57.0), (-8.55, 57.49)], 0.25)          # R4.1
    r.track('U0TXD', 1, [(-7.5, 16.5), (-7.95, 16.95), (-7.95, 50.7)], W)           # from TP6
    r.via('U0TXD', -7.95, 50.7)
    r.track('U0TXD', 0, [(-7.95, 50.7), (-8.55, 51.49)], W)                         # R8.2
    r.track('U0TXD_MOD', 0, [(-8.55, 52.51), (-7.95, 52.51), (-7.95, 55.4), (-7.45, 55.9), (-7.0, 55.9)], W)  # pin 39
    r.track('U0RXD', 1, [(-5.2, 16.5), (-7.35, 18.65), (-7.35, 55.95), (-7.95, 56.55)], W)   # from TP7
    r.via('U0RXD', -7.95, 56.55)
    r.track('U0RXD', 0, [(-7.95, 56.55), (-7.75, 56.75), (-7.0, 56.75)], W)        # pin 40
    r.track('EN', 1, [(-2.9, 16.5), (-6.8, 20.4), (-6.8, 58.65), (-7.45, 59.3), (-7.95, 59.3)], W)
    r.via('EN', -7.95, 59.3)
    r.track('EN', 0, [(-8.55, 58.51), (-7.95, 58.51), (-7.95, 61.0), (-7.0, 61.0)], W)   # R4.2, pin 45
    r.track('EN', 0, [(-7.95, 60.52), (-8.55, 60.52)], W)                                # C9.1
    r.track('LED_DATA_3V3', 0, [(3.3, 18.71), (4.1, 19.51), (4.1, 22.95), (3.75, 23.3), (-6.0, 23.3)], W)   # R9.1, under SW1
    r.via('LED_DATA_3V3', -6.0, 23.3)
    r.track('LED_DATA_3V3', 0, [(5.86, 18.2), (6.8, 18.2), (6.8, 20.15), (6.5, 20.45), (4.5, 20.45), (4.1, 20.85)], W)  # U5.2 (between U5 pin rows)
    # GND vias for pads that routing would otherwise box in (front-right)
    r.via('GND', 5.6, 19.85)                                                       # U5.3
    r.track('GND', 0, [(8.1, 12.05), (8.4, 11.55), (8.4, 10.9)], 0.3); r.via('GND', 8.4, 10.9)       # C6.2
    r.track('GND', 0, [(5.8, 7.72), (6.55, 7.1)], 0.3); r.via('GND', 6.55, 7.1)                      # C12.2
    # U1.2 GND: pocket enclosed by the USB pair on top -> via between the U1 pin rows
    r.track('GND', 0, [(0.0, 9.6), (0.0, 8.95)], 0.3); r.via('GND', 0.0, 8.95)
    # U3.2 GND: boxed in by the 3V3_LDO pins -> via to the left of the pad
    r.track('GND', 0, [(2.76, 12.9), (1.6, 12.75)], 0.3); r.via('GND', 1.6, 12.75)
    # U3.6 (+3V3 out of the soft-start switch): escape via beside the pad (no via in pad)
    r.track('+3V3', 0, [(5.04, 11.95), (5.2, 11.05)], 0.4); r.via('+3V3', 5.2, 11.05)
    # LED_DATA_5V: U5.4 -> via -> bottom under SW1 -> via -> R10.1
    r.track('LED_DATA_5V', 0, [(8.14, 19.15), (8.5, 19.5), (8.5, 20.0)], W)
    r.via('LED_DATA_5V', 8.5, 20.0)
    r.track('LED_DATA_5V', 1, [(8.5, 20.0), (7.9, 20.6), (3.4, 20.6), (2.4, 19.6)], W)
    r.via('LED_DATA_5V', 2.4, 19.6)
    r.track('LED_DATA_5V', 0, [(2.4, 19.6), (2.2, 19.4), (2.2, 18.71)], W)
    # LED_DIN: D2.3 -> via -> bottom -> via -> R10.2
    r.track('LED_DIN', 0, [(4.52, 8.75), (4.52, 9.75)], W)
    r.via('LED_DIN', 4.52, 9.75)
    r.track('LED_DIN', 1, [(4.52, 9.75), (4.52, 14.6), (1.9, 14.6), (1.9, 16.9)], W)
    r.via('LED_DIN', 1.9, 16.9)
    r.track('LED_DIN', 0, [(1.9, 16.9), (2.2, 17.2), (2.2, 17.69)], W)
    r.track('LED_DATA_3V3', 1, [(-6.0, 23.3), (-6.35, 23.65), (-6.35, 46.5), (-5.95, 46.9)], W)
    r.via('LED_DATA_3V3', -5.95, 46.9)
    r.track('LED_DATA_3V3', 0, [(-5.95, 46.9), (-5.95, G.MOD_CY - 7.0)], W)        # pin 30
    # +x strip: board ID divider C10 / R7 / R6 (north -> south), spine x 7.95 -> pin 5, TP10 below
    r.track('BOARD_ID', 0, [(8.55, 50.62), (7.9, 50.62), (7.9, 57.6), (7.0, 57.6)], W)   # C10.1 .. pin 5
    r.track('BOARD_ID', 0, [(7.9, 54.31), (8.55, 54.31)], W)                           # R7.1
    r.track('BOARD_ID', 0, [(7.9, 55.39), (8.55, 55.39)], W)                           # R6.2
    r.via('BOARD_ID', 7.9, 54.85)
    r.track('BOARD_ID', 1, [(7.9, 54.85), (8.35, 54.4), (8.35, 53.6)], W)              # TP10
    r.track('GND', 0, [(8.55, 51.58), (8.55, 53.29)], 0.3)                             # C10.2, R7.2
    r.via('GND', 8.55, 52.45)
    r.track('+3V3', 0, [(8.55, 56.41), (8.55, 57.3)], 0.25)                           # R6.1
    r.via('+3V3', 8.55, 57.3)
    r.track('+3V3', 1, [(8.55, 57.3), (8.6, 57.35), (8.6, 58.9), (8.25, 59.25), (7.85, 59.25)], 0.25)
    r.track('BOOT', 1, [(-2.6, Y0), (-2.6, 45.0), (7.35, 45.0), (7.35, 58.0), (7.8, 58.45), (8.0, 58.45)], W)
    r.via('BOOT', 8.0, 58.45)
    # BOOT front: SW1.1 -> R5.2 -> top across the USB pair -> via -> trunk start; TP5 (bottom) via beside SW1.1
    r.track('BOOT', 0, [(3.0, 21.4), (1.9, 21.4), (1.7, 21.6), (-1.35, 21.6), (-1.55, 21.8)], W)
    r.via('BOOT', -1.55, 21.8)
    r.track('BOOT', 1, [(-1.55, 21.8), (-2.6, 22.85), (-2.6, Y0)], W)
    r.track('BOOT', 0, [(2.45, 20.7), (2.85, 21.0)], W); r.via('BOOT', 2.45, 20.7)
    r.track('BOOT', 1, [(2.45, 20.7), (2.85, 21.1), (3.3, 21.1)], W)
    r.track('BOOT', 0, [(8.0, 58.45), (7.0, 58.45)], W)                              # pin 4
    r.track('+3V3', 1, [(-3.35, Y0), (-3.35, 45.6), (6.75, 45.6), (6.75, 58.7), (7.3, 59.25), (7.85, 59.25)], 0.5)
    r.via('+3V3', 7.85, 59.25)
    r.track('+3V3', 0, [(7.0, 59.3), (7.85, 59.25), (8.55, 59.17)], 0.3)            # pin 3, C7.1
    r.track('+3V3', 1, [(7.85, 59.25), (7.85, 62.0)], 0.3)
    r.via('+3V3', 7.85, 62.0)
    r.track('+3V3', 0, [(7.85, 62.0), (8.55, 61.98)], 0.3)                          # C8.1
    r.track('GND', 0, [(8.55, 60.13), (7.0, 60.15)], 0.3)                           # C7.2 -> module GND pin 2
    r.track('GND', 0, [(8.55, 61.02), (7.0, 61.0)], 0.3)                            # C8.2 -> module GND pin 1


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
    sd_fan(r)
    bus(r)
    power = ['VBUS_IN', '+5V', '3V3_LDO', '+3V3', 'SD_VDD']
    others = [n for n in r.pads if n not in power and n != 'GND' and not n.startswith('_')
              and n not in ('USB_DP', 'USB_DM')]

    def span(n):
        xs = [p[3].bbox() for p in r.pads[n]]
        return (max(b[2] for b in xs) - min(b[0] for b in xs)) + (max(b[3] for b in xs) - min(b[1] for b in xs))
    others.sort(key=span)
    # USB nets: only the remaining pads (TPs, C/R reserves) — the scripted copper is the tree
    def tight(n):   # nets on the module or SD socket pins: route before power takes the strips
        return any(p[0] in ('U4', 'J3') for p in r.pads[n]) or n.startswith('SD_')
    cross = ['BOOT', 'EN', 'U0TXD', 'U0RXD', 'LED_DATA_3V3']   # trunks scripted in bus()
    order = ([n for n in others if n in USB_NETS] + ['VBUS_IN', '+5V', '3V3_LDO', '+3V3']
             + [n for n in cross if n in others] + ['SD_VDD']
             + [n for n in others if n not in USB_NETS and n not in cross])
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
