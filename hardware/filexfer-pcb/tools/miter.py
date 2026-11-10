#!/usr/bin/env python3
"""Mitre track corners in place on the committed board (no rerouting; parts, vias, pads unchanged).

Every bend of 90° or sharper between exactly two same-net tracks on one layer, away from vias and
pads, becomes a 45° mitre. The corner is cut back by c along both legs and joined with a new
segment. The mitre length (c·√2 for a 90° bend) is ≥ 2× track width where clearance allows,
otherwise as long as fits. Bends where two tracks just end at a pad or via are reported, not
changed. Zones are refilled.

Run from hardware/filexfer-pcb:  python3 tools/miter.py   (then DRC with --schematic-parity)
"""
import math, os, sys
import pcbnew

HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
PCB = os.path.join(HERE, 'filexfer-pcb.kicad_pcb')
CLR = 0.16          # 0.15 net-class clearance + margin
EDGE = 0.26         # 0.25 copper-to-edge + margin
mm, MM = pcbnew.FromMM, pcbnew.ToMM


def loc(p):
    return round(MM(p.x) - 100, 2), round(MM(p.y) - 100, 2)


def main():
    b = pcbnew.LoadBoard(PCB)
    edges = [d for d in b.GetDrawings() if d.GetLayer() == pcbnew.Edge_Cuts]
    keepouts = [z for z in b.Zones() if z.GetIsRuleArea() and z.GetDoNotAllowTracks()]
    pads = [p for fp in b.GetFootprints() for p in fp.Pads()]

    def tracks():
        return [t for t in b.GetTracks() if t.GetClass() == 'PCB_TRACK']

    vias = [t for t in b.GetTracks() if t.GetClass() == 'PCB_VIA']

    def clear(seg, layer, net):
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
        if any(v.GetPosition() == P and v.IsOnLayer(layer) for v in vias):
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
            why = clear(seg, layer, net)
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
                b.Remove(t); b.Add(seg)
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
    print(f'mitred: {len(fixed)}  (mitre < 2x width: {len(short)})')
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
