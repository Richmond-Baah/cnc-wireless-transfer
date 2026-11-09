import sys, pcbnew
from PIL import Image, ImageDraw, ImageFont
b = pcbnew.LoadBoard(sys.argv[1]); out = sys.argv[2]
S = 26.0; OX, OY = 100.0, 100.0
X0, X1 = -16.5, 72.0      # local y range shown (plug overhang .. antenna tip)
Y0, Y1 = -10.6, 10.6      # local x range
Wp, Hp = int((X1 - X0) * S), int((Y1 - Y0) * S) * 2 + 60
img = Image.new('RGB', (Wp, Hp), 'white'); d = ImageDraw.Draw(img)
try: f = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf', 12); fb = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', 15)
except Exception: f = fb = ImageFont.load_default()
def P(v, panel):  # board nm -> image px; image x = local y (plug left), image y = local x
    lx = pcbnew.ToMM(v.x) - OX; ly = pcbnew.ToMM(v.y) - OY
    if panel == 1: lx = -lx   # bottom view mirrored so it reads as seen from below
    return ((ly - X0) * S, (lx - Y0) * S + panel * (Hp // 2) + 22)
def poly(pts, panel, **kw): d.polygon([P(p, panel) for p in pts], **kw)
for panel, side, title in ((0, pcbnew.F_Cu, 'TOP (plug at left, antenna at right)'), (1, pcbnew.B_Cu, 'BOTTOM (viewed from below)')):
    d.text((8, panel * (Hp // 2) + 2), title, fill='black', font=fb)
    # edge
    for dr in b.GetDrawings():
        if dr.GetLayer() == pcbnew.Edge_Cuts:
            sh = dr.GetEffectiveShape()
            for i in range(0, 1):
                pass
            if dr.GetShape() == pcbnew.SHAPE_T_SEGMENT:
                d.line([P(dr.GetStart(), panel), P(dr.GetEnd(), panel)], fill='black', width=3)
            else:
                pl = dr.GetPolyShape() if False else None
                d.line([P(dr.GetStart(), panel), P(dr.GetEnd(), panel)], fill='black', width=3)
    # rule areas
    for z in b.Zones():
        if z.GetIsRuleArea():
            o = z.Outline().COutline(0); pts = [o.CPoint(i) for i in range(o.PointCount())]
            poly(pts, panel, outline=(200, 0, 0), fill=(255, 225, 225))
            c = P(pts[0], panel); d.text((c[0] + 3, c[1] + 2), 'NOTCH RESERVE\n(no Cu/vias/parts)', fill=(170, 0, 0), font=f)
    for fp in b.GetFootprints():
        ref = fp.GetReference(); onF = not fp.IsFlipped()
        mine = (onF and panel == 0) or ((not onF) and panel == 1)
        for pad in fp.Pads():
            if pad.IsOnLayer(side) or pad.GetAttribute() == pcbnew.PAD_ATTRIB_NPTH:
                bb = pad.GetBoundingBox()
                col = (60, 60, 60) if pad.GetAttribute() == pcbnew.PAD_ATTRIB_NPTH else ((205, 150, 30) if mine or pad.GetAttribute() == pcbnew.PAD_ATTRIB_PTH else (230, 200, 140))
                a, c2 = P(bb.GetOrigin(), panel), P(bb.GetEnd(), panel)
                d.rectangle([min(a[0], c2[0]), min(a[1], c2[1]), max(a[0], c2[0]), max(a[1], c2[1])], fill=col)
        if not mine: continue
        cy = fp.GetCourtyard(pcbnew.F_CrtYd if onF else pcbnew.B_CrtYd)
        if cy.OutlineCount():
            o = cy.COutline(0); pts = [o.CPoint(i) for i in range(o.PointCount())]
            col = (30, 80, 200) if onF else (0, 140, 60)
            if ref == 'U4':
                col = (30, 80, 200)
            poly(pts, panel, outline=col)
        bb = fp.GetBoundingBox(False)
        c = P(bb.GetCenter(), panel)
        fpn = fp.GetFPID().GetLibItemName().wx_str()
        lab = ref
        tw = d.textlength(lab, font=f)
        d.text((c[0] - tw / 2, c[1] - 7), lab, fill=(0, 0, 150) if onF else (0, 110, 40), font=f)
    # module antenna keepout (from footprint zones)
    for fp in b.GetFootprints():
        for z in fp.Zones():
            o = z.Outline().COutline(0); pts = [o.CPoint(i) for i in range(o.PointCount())]
            poly(pts, panel, outline=(150, 0, 150))
            c = P(pts[0], panel); d.text((c[0] - 70, c[1] + 4), 'antenna\nkeep-out\n(off board)', fill=(150, 0, 150), font=f)
    # scale bar
    yb = panel * (Hp // 2) + Hp // 2 - 22
    for mmv in range(0, 70, 10):
        xx = (mmv - X0) * S; d.line([(xx, yb), (xx, yb + 6)], fill='black'); d.text((xx + 2, yb - 2), f'{mmv}', fill='black', font=f)
    d.line([((0 - X0) * S, yb), ((70 - X0) * S, yb)], fill='black')
img.save(out); print(out, img.size)
