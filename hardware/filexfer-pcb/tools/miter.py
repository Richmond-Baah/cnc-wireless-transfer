#!/usr/bin/env python3
"""Mitre track corners in place on the committed board (no rerouting; parts, vias, pads unchanged).

Every bend of 90° or sharper between exactly two same-net tracks on one layer, away from vias and
pads, becomes a 45° mitre. The corner is cut back by c along both legs and joined with a new
segment. The mitre length (c·√2 for a 90° bend) is ≥ 2× track width where clearance allows,
otherwise as long as fits. Bends where two tracks just end at a pad or via are reported, not
changed. Zones are refilled.

Run from hardware/filexfer-pcb:  python3 tools/miter.py   (then DRC with --schematic-parity)

Rev B (pcb-short) port:
- Clearance margin from the rev B project rule (0.127 mm, JLC standard).
- A pre-pass collapses track stubs lying wholly inside a same-net via's copper (they make hidden acute
  or 90° corners), so the next track starts at the via centre.
- One targeted fix (REV_B_FIXES): the BOOT via at (7.55, 18.75) teed into the strip at 53°. It now
  drops straight into R5.2 at x 7.55.
- Corners whose copper disc is wholly covered by a pad or via are reported, not changed.
"""
import math, os, sys
import pcbnew

HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
PCB = os.path.join(HERE, 'filexfer-pcb.kicad_pcb')
CLR = 0.14          # rev B: 0.127 net-class clearance (JLC standard) + margin
EDGE = 0.245        # rev B 0.25 copper-to-edge rule; mitre ends sit on legs that already pass it (DRC re-checks)
mm, MM = pcbnew.FromMM, pcbnew.ToMM
_REMOVED = []      # keep removed items referenced until the board is saved (KiCad 10 SWIG: a freed removed item breaks GetTracks())


def loc(p):
    return round(MM(p.x) - 100, 2), round(MM(p.y) - 100, 2)


def pt(x, y):
    return pcbnew.VECTOR2I(mm(x + 100), mm(y + 100))


def via_stubs(b):
    """Collapse a track wholly inside a same-net via's copper: remove it, move the joined track end to the via.
    Plan first, then edit (b.GetTracks() breaks if items are removed while iterating)."""
    tr = [t for t in b.GetTracks() if t.GetClass() == 'PCB_TRACK']
    vias = [t for t in b.GetTracks() if t.GetClass() == 'PCB_VIA']
    plan, used = [], set()
    for v in vias:
        r = v.GetWidth(pcbnew.F_Cu) / 2; vp = pcbnew.VECTOR2I(v.GetPosition().x, v.GetPosition().y)
        near = lambda q, p: (q - p).EuclideanNorm() < 1000                  # 1 µm
        for t in tr:
            if t.GetNetCode() != v.GetNetCode() or t.m_Uuid.AsString() in used:
                continue
            ends = (t.GetStart(), t.GetEnd())
            if not any(near(q, vp) for q in ends):
                continue
            o = ends[1] if near(ends[0], vp) else ends[0]
            if (o - vp).EuclideanNorm() + t.GetWidth() / 2 > r:
                continue
            nxt = [s for s in tr if s.m_Uuid.AsString() != t.m_Uuid.AsString() and s.GetNetCode() == t.GetNetCode()
                   and s.GetLayer() == t.GetLayer() and any(near(q, o) for q in (s.GetStart(), s.GetEnd()))]
            if len(nxt) != 1:
                continue
            used.add(t.m_Uuid.AsString())
            plan.append((t, nxt[0], o, vp, (t.GetNetname().lstrip('/'), b.GetLayerName(t.GetLayer()), loc(o))))
    for t, s, o, vp, info in plan:
        if (s.GetStart() - o).EuclideanNorm() < 1000:
            s.SetStart(vp)
        else:
            s.SetEnd(vp)
    for t, *_ in plan:
        _REMOVED.append(t); b.Remove(t)
    return [p[-1] for p in plan]


def rev_b_fixes(b):
    """BOOT: via (7.55, 18.75) teed into the strip at (7.75, 18.6) at 53° from the R5.2 branch -> straight into R5.2."""
    out = []
    for t in [t for t in b.GetTracks() if t.GetClass() == 'PCB_TRACK' and t.GetNetname() == '/BOOT' and t.GetLayer() == pcbnew.F_Cu]:
        if {loc(t.GetStart()), loc(t.GetEnd())} == {(7.75, 18.6), (7.55, 18.75)}:
            w, net = t.GetWidth(), t.GetNetCode()
            _REMOVED.append(t); b.Remove(t)
            n = pcbnew.PCB_TRACK(b); n.SetStart(pt(7.55, 18.75)); n.SetEnd(pt(7.55, 18.0)); n.SetWidth(w)
            n.SetLayer(pcbnew.F_Cu); n.SetNetCode(net); b.Add(n)
            out.append(('BOOT', 'F.Cu', (7.75, 18.6), 'via branch re-joined straight into R5.2 at x 7.55 (acute tee removed)'))
    return out


def main():
    b = pcbnew.LoadBoard(PCB)
    pre = via_stubs(b) + [(n, l, w) for n, l, w, _ in rev_b_fixes(b)]
    edges = [d for d in b.GetDrawings() if d.GetLayer() == pcbnew.Edge_Cuts]
    keepouts = [z for z in b.Zones() if z.GetIsRuleArea() and z.GetDoNotAllowTracks()]
    pads = [p for fp in b.GetFootprints() for p in fp.Pads()]

    def tracks():
        return [t for t in b.GetTracks() if t.GetClass() == 'PCB_TRACK']

    vias = [t for t in b.GetTracks() if t.GetClass() == 'PCB_VIA']

    def clear(seg, layer, net, legs=()):
        sh = seg.GetEffectiveShape(layer)
        for t in tracks():
            if t.GetNetCode() != net and t.GetLayer() == layer and t.GetEffectiveShape(layer).Collide(sh, mm(CLR)):
                return 'track ' + t.GetNetname().lstrip('/')
        for v in vias:
            if v.GetNetCode() != net and v.GetEffectiveShape(layer).Collide(sh, mm(CLR)):
                return 'via ' + v.GetNetname().lstrip('/')
        for p in pads:
            if p.IsOnLayer(layer) and p.GetNetCode() != net and p.GetEffectiveShape(layer).Collide(sh, mm(CLR)):
                return 'pad ' + p.GetParentFootprint().GetReference() + '.' + p.GetNumber()
            if p.GetAttribute() == pcbnew.PAD_ATTRIB_NPTH and p.GetEffectiveHoleShape().Collide(sh, mm(0.25)):
                return 'hole ' + p.GetParentFootprint().GetReference()
        for e in edges:
            if e.GetEffectiveShape().Collide(sh, mm(EDGE)):
                # a mitre lies inside its corner, so it is never nearer a straight edge than its legs:
                # accept it when a leg is already that close (that leg passes DRC as committed)
                if any(e.GetEffectiveShape().Collide(l.GetEffectiveShape(layer), mm(EDGE)) for l in legs):
                    continue
                return 'board edge'
        for z in keepouts:
            if z.IsOnLayer(layer) and z.Outline().Collide(pcbnew.SEG(seg.GetStart(), seg.GetEnd()), seg.GetWidth() // 2):
                return 'keep-out ' + z.GetZoneName()
        return None

    fixed, short, skipped = [], [], []
    while True:
        ends = {}
        for t in tracks():
            for e in (t.GetStart(), t.GetEnd()):
                ends.setdefault((t.GetNetCode(), t.GetLayer(), e.x, e.y), []).append(t)
        todo = None
        for (net, layer, x, y), ts in ends.items():
            if len(ts) != 2 or (net, layer, x, y) in {s[0] for s in skipped}:
                continue
            P = pcbnew.VECTOR2I(x, y)
            legs = []
            for t in ts:
                o = t.GetEnd() if (t.GetStart().x, t.GetStart().y) == (x, y) else t.GetStart()
                d = math.hypot(o.x - x, o.y - y)
                legs.append((t, o, d, ((o.x - x) / d, (o.y - y) / d)))
            cosang = legs[0][3][0] * legs[1][3][0] + legs[0][3][1] * legs[1][3][1]
            if math.degrees(math.acos(max(-1.0, min(1.0, cosang)))) > 91:
                continue
            todo = (net, layer, x, y, P, legs)
            break
        if not todo:
            break
        net, layer, x, y, P, legs = todo
        key = (net, layer, x, y)
        name, where = legs[0][0].GetNetname().lstrip('/'), loc(P)
        if any(v.GetPosition() == P and v.IsOnLayer(layer) for v in vias):   # track ends at via centre: corner inside via copper
            skipped.append((key, name, where, 'two tracks end at a via (no copper corner to cut)')); continue
        if any(p.IsOnLayer(layer) and p.HitTest(P) for p in pads):
            skipped.append((key, name, where, 'two tracks end inside a pad (corner covered by pad copper)')); continue
        w = min(MM(l[0].GetWidth()) for l in legs)
        ang = math.acos(max(-1.0, min(1.0, legs[0][3][0] * legs[1][3][0] + legs[0][3][1] * legs[1][3][1])))
        if abs(math.degrees(ang) - 90) > 1:
            skipped.append((key, name, where, f'acute {math.degrees(ang):.0f}° bend: needs a two-segment mitre')); continue
        # leg budget: keep ≥ 0.05 mm of each leg, and half a leg if its far end is another corner
        caps = []
        for t, o, d, u in legs:
            far = ends.get((net, layer, o.x, o.y), [])
            caps.append(MM(d) * (0.45 if len(far) == 2 else 1.0) - 0.05)
        cmax = min(caps)
        done = None
        for f in (2.0, 1.8, 1.6, 1.5, 1.414, 1.2, 1.0, 0.8, 0.6, 0.45, 0.3):
            c = f * w
            if c > cmax or c < 0.1:
                continue
            A = pcbnew.VECTOR2I(int(x + legs[0][3][0] * mm(c)), int(y + legs[0][3][1] * mm(c)))
            B = pcbnew.VECTOR2I(int(x + legs[1][3][0] * mm(c)), int(y + legs[1][3][1] * mm(c)))
            seg = pcbnew.PCB_TRACK(b)
            seg.SetStart(A); seg.SetEnd(B); seg.SetWidth(mm(w)); seg.SetLayer(layer); seg.SetNetCode(net)
            why = clear(seg, layer, net, [l[0] for l in legs])
            if why is None:
                done = (c, A, B, seg); break
            last = why
        if not done and cmax < 0.1:
            # short Z-jog (90° corner, short leg, opposite 90° corner): replace the jog by one 45° diagonal
            zj = None
            for i, (t, o, d, u) in enumerate(legs):
                far = ends.get((net, layer, o.x, o.y), [])
                if len(far) != 2 or any(v.GetPosition() == o for v in vias):
                    continue
                t2 = far[0] if far[1] is t else far[1]
                o2 = t2.GetEnd() if (t2.GetStart().x, t2.GetStart().y) == (o.x, o.y) else t2.GetStart()
                d2 = math.hypot(o2.x - o.x, o2.y - o.y); u2 = ((o2.x - o.x) / d2, (o2.y - o.y) / d2)
                tl, ol, dl, ul = legs[1 - i]
                if abs(u2[0] + ul[0]) > 1e-3 or abs(u2[1] + ul[1]) > 1e-3:
                    continue                     # not a Z (legs must leave in opposite directions)
                h = MM(d) / 2
                if MM(dl) < h + 0.05 or MM(d2) < h + 0.05:
                    continue
                P2 = pcbnew.VECTOR2I(int(x + ul[0] * mm(h)), int(y + ul[1] * mm(h)))
                O2 = pcbnew.VECTOR2I(int(o.x + u2[0] * mm(h)), int(o.y + u2[1] * mm(h)))
                seg = pcbnew.PCB_TRACK(b)
                seg.SetStart(P2); seg.SetEnd(O2); seg.SetWidth(mm(w)); seg.SetLayer(layer); seg.SetNetCode(net)
                if clear(seg, layer, net) is None:
                    zj = (t, tl, P2, t2, o, O2, seg, MM(d)); break
            if zj:
                t, tl, P2, t2, o, O2, seg, jog = zj
                for tt, pt_, N in ((tl, P, P2), (t2, o, O2)):
                    if (tt.GetStart().x, tt.GetStart().y) == (pt_.x, pt_.y):
                        tt.SetStart(N)
                    else:
                        tt.SetEnd(N)
                _REMOVED.append(t); b.Remove(t); b.Add(seg)
                fixed.append((name, where, round(w, 2), round(MM(seg.GetLength()), 2), f'Z-jog {jog:.2f} mm -> one 45° diagonal'))
                continue
        if not done:
            skipped.append((key, name, where, f'no clearance for any mitre ({last if cmax >= 0.1 else "legs too short"})'))
            continue
        c, A, B, seg = done
        for (t, o, d, u), N in zip(legs, (A, B)):
            if (t.GetStart().x, t.GetStart().y) == (x, y):
                t.SetStart(N)
            else:
                t.SetEnd(N)
        b.Add(seg)
        length = c * math.sqrt(2)
        fixed.append((name, where, round(w, 2), round(length, 2)))
        if length < 2 * w - 1e-6:
            short.append((name, where, round(w, 2), round(length, 2)))

    pcbnew.ZONE_FILLER(b).Fill(b.Zones())
    pcbnew.SaveBoard(PCB, b)
    print(f'pre-pass (via stubs / targeted): {len(pre)}', pre)
    print(f'mitred: {len(fixed)}  (mitre < 2x width: {len(short)})')
    for f in fixed:
        print('   ok', *f[:4])
    for f in fixed:
        if len(f) > 4:
            print('  ', *f)
    for s in short:
        print('  short mitre', s)
    print(f'not changed: {len(skipped)}')
    for _, n, wh, why in skipped:
        print('  ', n, wh, why)


if __name__ == '__main__':
    main()
