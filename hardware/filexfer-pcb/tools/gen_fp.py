#!/usr/bin/env python3
"""Generate FileXfer custom footprints into filexfer.pretty from vendor drawings.

- microSD_SHOUHAN_TF_PUSH: SHOU HAN "TF PUSH" (LCSC C393941), drawing Rev A/0 2020-07-07,
  recommended PCB hole layout (component side). Origin = left locating hole.
- USB_A_Plug_XKB_U217-041N-4BV81: XKB drawing X-U217-041N-4BV81 rev A1 2024-10-18, "PCB LAYOUT".
  Origin = midpoint between the two locating holes.
Dimensions not given explicitly in the drawings are marked [U] in the descr and on Dwgs.User.
"""
import os

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'filexfer.pretty')
SMD = '(layers "F.Cu" "F.Paste" "F.Mask")'


def line(x1, y1, x2, y2, layer, w=0.1):
    return (f'\t(fp_line (start {x1:.3f} {y1:.3f}) (end {x2:.3f} {y2:.3f}) '
            f'(stroke (width {w}) (type solid)) (layer "{layer}"))\n')


def rect(x1, y1, x2, y2, layer, w=0.1):
    return (line(x1, y1, x2, y1, layer, w) + line(x2, y1, x2, y2, layer, w) +
            line(x2, y2, x1, y2, layer, w) + line(x1, y2, x1, y1, layer, w))


def text(kind, val, x, y, layer, hide=False):
    h = ' (hide yes)' if hide else ''
    return (f'\t(property "{kind}" "{val}" (at {x:.2f} {y:.2f} 0) (layer "{layer}"){h} '
            f'(effects (font (size 1 1) (thickness 0.15))))\n')


def note(val, x, y):
    return (f'\t(fp_text user "{val}" (at {x:.2f} {y:.2f} 0) (layer "Dwgs.User") '
            f'(effects (font (size 0.6 0.6) (thickness 0.1))))\n')


def footprint(name, descr, body, attr):
    model = f'\t(model "${{KIPRJMOD}}/3dmodels/{name}.wrl")\n'
    return (f'(footprint "{name}"\n\t(version 20241229)\n\t(generator "filexfer_gen_fp")\n'
            f'\t(layer "F.Cu")\n\t(descr "{descr}")\n\t(attr {attr})\n{body}{model})\n')


def wrl_box(path, boxes):
    """Placeholder VRML model: boxes = [(x0, x1, y0, y1, z0, z1, rgb)] in footprint mm (y down).
    KiCad VRML unit = 0.1 inch; model Y = -footprint y."""
    u = 1 / 2.54
    out = ['#VRML V2.0 utf8', '# FileXfer placeholder box model (not vendor geometry)']
    for x0, x1, y0, y1, z0, z1, rgb in boxes:
        cx, cy, cz = (x0 + x1) / 2 * u, -(y0 + y1) / 2 * u, (z0 + z1) / 2 * u
        sx, sy, sz = abs(x1 - x0) * u, abs(y1 - y0) * u, abs(z1 - z0) * u
        out.append(f'Transform {{ translation {cx:.4f} {cy:.4f} {cz:.4f} children [ Shape {{ '
                   f'appearance Appearance {{ material Material {{ diffuseColor {rgb} }} }} '
                   f'geometry Box {{ size {sx:.4f} {sy:.4f} {sz:.4f} }} }} ] }}')
    open(path, 'w').write('\n'.join(out) + '\n')


def step_boxes(path, name, boxes):
    """Placeholder STEP (AP214, one B-rep solid per box) with the same geometry as wrl_box, for STEP export
    (kicad-cli pcb export step --subst-models swaps it in for the same-name .wrl). Units mm, model Y = -footprint y."""
    ents = []
    def e(txt):
        ents.append(txt); return '#%d' % len(ents)
    ac = e("APPLICATION_CONTEXT('automotive_design')")
    e("APPLICATION_PROTOCOL_DEFINITION('international standard','automotive_design',2000,%s)" % ac)
    pc = e("PRODUCT_CONTEXT('',%s,'mechanical')" % ac)
    pr = e("PRODUCT('%s','%s','FileXfer placeholder box model (not vendor geometry)',(%s))" % (name, name, pc))
    pdf = e("PRODUCT_DEFINITION_FORMATION('','',%s)" % pr)
    pdc = e("PRODUCT_DEFINITION_CONTEXT('part definition',%s,'design')" % ac)
    pd = e("PRODUCT_DEFINITION('design','',%s,%s)" % (pdf, pdc))
    pds = e("PRODUCT_DEFINITION_SHAPE('','',%s)" % pd)
    mm = e("( LENGTH_UNIT() NAMED_UNIT(*) SI_UNIT(.MILLI.,.METRE.) )")
    rad = e("( NAMED_UNIT(*) PLANE_ANGLE_UNIT() SI_UNIT($,.RADIAN.) )")
    sr = e("( NAMED_UNIT(*) SI_UNIT($,.STERADIAN.) SOLID_ANGLE_UNIT() )")
    unc = e("UNCERTAINTY_MEASURE_WITH_UNIT(LENGTH_MEASURE(1.E-07),%s,'distance_accuracy_value','confusion accuracy')" % mm)
    ctx = e("( GEOMETRIC_REPRESENTATION_CONTEXT(3) GLOBAL_UNCERTAINTY_ASSIGNED_CONTEXT((%s)) "
            "GLOBAL_UNIT_ASSIGNED_CONTEXT((%s,%s,%s)) REPRESENTATION_CONTEXT('','3D') )" % (unc, mm, rad, sr))
    f = lambda v: '%.4f' % v
    pt = lambda q: e("CARTESIAN_POINT('',(%s,%s,%s))" % tuple(map(f, q)))
    dr = lambda q: e("DIRECTION('',(%s,%s,%s))" % tuple(map(f, q)))
    o = pt((0, 0, 0)); origin = e("AXIS2_PLACEMENT_3D('',%s,%s,%s)" % (o, dr((0, 0, 1)), dr((1, 0, 0))))
    solids = []
    faces_ccw = (((0, 0, 0), (0, 1, 0), (1, 1, 0), (1, 0, 0), (0, 0, -1)), ((0, 0, 1), (1, 0, 1), (1, 1, 1), (0, 1, 1), (0, 0, 1)),
                 ((0, 0, 0), (0, 0, 1), (0, 1, 1), (0, 1, 0), (-1, 0, 0)), ((1, 0, 0), (1, 1, 0), (1, 1, 1), (1, 0, 1), (1, 0, 0)),
                 ((0, 0, 0), (1, 0, 0), (1, 0, 1), (0, 0, 1), (0, -1, 0)), ((0, 1, 0), (0, 1, 1), (1, 1, 1), (1, 1, 0), (0, 1, 0)))
    for x0, x1, y0, y1, z0, z1, _rgb in boxes:
        X = sorted((x0, x1)); Y = sorted((-y0, -y1)); Z = sorted((z0, z1))
        P = lambda k: (X[k[0]], Y[k[1]], Z[k[2]])
        verts = {}; edges = {}
        def V(k):
            if k not in verts:
                verts[k] = e("VERTEX_POINT('',%s)" % pt(P(k)))
            return verts[k]
        def E(a, b):
            if (b, a) in edges:
                return edges[(b, a)], '.F.'
            if (a, b) not in edges:
                pa, pb = P(a), P(b); d = [pb[i] - pa[i] for i in range(3)]; L = max(abs(c) for c in d)
                ln = e("LINE('',%s,%s)" % (pt(pa), e("VECTOR('',%s,%s)" % (dr([c / L for c in d]), f(L)))))
                edges[(a, b)] = e("EDGE_CURVE('',%s,%s,%s,.T.)" % (V(a), V(b), ln))
            return edges[(a, b)], '.T.'
        fl = []
        for *ks, n in faces_ccw:
            oes = []
            for i in range(4):
                ec, sense = E(ks[i], ks[(i + 1) % 4]); oes.append(e("ORIENTED_EDGE('',*,*,%s,%s)" % (ec, sense)))
            a, b = P(ks[0]), P(ks[1]); d = [b[i] - a[i] for i in range(3)]; L = max(abs(c) for c in d)
            pl = e("PLANE('',%s)" % e("AXIS2_PLACEMENT_3D('',%s,%s,%s)" % (pt(a), dr(n), dr([c / L for c in d]))))
            bound = e("FACE_OUTER_BOUND('',%s,.T.)" % e("EDGE_LOOP('',(%s))" % ','.join(oes)))
            fl.append(e("ADVANCED_FACE('',(%s),%s,.T.)" % (bound, pl)))
        solids.append(e("MANIFOLD_SOLID_BREP('',%s)" % e("CLOSED_SHELL('',(%s))" % ','.join(fl))))
    rep = e("ADVANCED_BREP_SHAPE_REPRESENTATION('%s',(%s,%s),%s)" % (name, origin, ','.join(solids), ctx))
    e("SHAPE_DEFINITION_REPRESENTATION(%s,%s)" % (pds, rep))
    hdr = ("ISO-10303-21;\nHEADER;\nFILE_DESCRIPTION(('FileXfer placeholder box model'),'2;1');\n"
           "FILE_NAME('%s.step','',(''),(''),'filexfer_gen_fp','filexfer_gen_fp','');\n"
           "FILE_SCHEMA(('AUTOMOTIVE_DESIGN { 1 0 10303 214 1 1 1 1 }'));\nENDSEC;\nDATA;\n" % name)
    open(path, 'w').write(hdr + ''.join('#%d=%s;\n' % (i + 1, t) for i, t in enumerate(ents)) + 'ENDSEC;\nEND-ISO-10303-21;\n')


def placeholder(m3d, name, boxes):
    wrl_box(os.path.join(m3d, name + '.wrl'), boxes)
    step_boxes(os.path.join(m3d, name + '.step'), name, boxes)


def tf_push():
    b = text('Reference', 'REF**', 4.95, -13.2, 'F.SilkS') + text('Value', 'microSD_SHOUHAN_TF_PUSH', 4.95, 5.4, 'F.Fab')
    # 9 contact pads: Cd at x=-1.60, then 8..1 at 1.10 pitch; 0.70 x 1.60, centre y=-11.00
    order = ['9', '8', '7', '6', '5', '4', '3', '2', '1']   # pad 9 = Cd (card detect)
    for i, num in enumerate(order):
        b += f'\t(pad "{num}" smd rect (at {-1.60 + 1.10 * i:.3f} -11.000) (size 0.70 1.60) {SMD})\n'
    # shell tabs: rear pads 1.50 tall (y -10.75..-9.25), front pads 2.20 tall (y -1.50..+0.70)
    for x, w in ((-2.80, 1.20), (11.80, 1.60)):
        b += f'\t(pad "SH" smd rect (at {x:.3f} -10.000) (size {w:.2f} 1.50) {SMD})\n'
    for x in (-2.80, 12.70):
        b += f'\t(pad "SH" smd rect (at {x:.3f} -0.400) (size 1.20 2.20) {SMD})\n'
    for x in (0.0, 8.0):
        b += f'\t(pad "" np_thru_hole circle (at {x:.3f} 0.000) (size 1.00 1.00) (drill 1.00) (layers "*.Cu" "*.Mask"))\n'
    # body 14.75 x 14.50 centred on connector centre x=4.95; rear edge y=-10.48, front (card slot) +4.02
    b += rect(4.95 - 7.375, -10.48, 4.95 + 7.375, 4.02, 'F.Fab')
    b += rect(-3.65, -12.05, 13.55, 4.27, 'F.CrtYd', 0.05)
    b += line(4.95 - 7.375, 4.02 + 0.2, 4.95 + 7.375, 4.02 + 0.2, 'F.SilkS', 0.12)   # card slot side
    b += note('card slot (front); card protrudes 5.60 when ejected', 4.95, 6.2)
    b += note('Cd = pad 9; switch closes to shell on insert [U]', 4.95, 7.0)
    return footprint('microSD_SHOUHAN_TF_PUSH',
                     'microSD push-push, card detect, SHOU HAN TF PUSH (LCSC C393941), 14.75x14.50 mm, 2.0 mm max height. '
                     'Pads/holes from vendor recommended layout; body Y position derived from top view [U]', b, 'smd')


def usb_a_xkb():
    b = text('Reference', 'REF**', 0, -5.0, 'F.SilkS') + text('Value', 'USB_A_Plug_XKB_U217-041N-4BV81', 0, 4.0, 'F.Fab')
    # signal pads 1.20 x 2.00, 7.00 span (2.50/2.00/2.50); pin 1 right; pad bottom edge 1.60 from hole row
    for num, x in (('1', 3.5), ('2', 1.0), ('3', -1.0), ('4', -3.5)):
        b += f'\t(pad "{num}" smd rect (at {x:.3f} -2.600) (size 1.20 2.00) {SMD})\n'
    for x in (-2.25, 2.25):
        b += f'\t(pad "" np_thru_hole circle (at {x:.3f} 0.000) (size 1.00 1.00) (drill 1.00) (layers "*.Cu" "*.Mask"))\n'
    for x in (-5.70, 5.70):
        b += (f'\t(pad "SH" thru_hole oval (at {x:.3f} 0.000) (size 1.60 4.00) (drill oval 1.00 2.50) '
              f'(layers "*.Cu" "*.Mask"))\n')
    # Board edge +2.92 from the hole row and shell front +17.78: EasyEDA footprint for C319178 (board-outline
    # + silk layers); not in the XKB drawing. Shell rear = front - 18.75 (drawing length) [U].
    EDGE, FRONT = 2.92, 17.78
    b += line(-9.0, EDGE, 9.0, EDGE, 'Dwgs.User', 0.1)
    b += note('board edge (EasyEDA C319178)', 0, EDGE + 0.6)
    b += rect(-6.0, FRONT - 18.75, 6.0, FRONT, 'F.Fab')   # shell 12.00 x 18.75
    b += rect(-6.75, -3.85, 6.75, EDGE, 'F.CrtYd', 0.05)  # on-board part only; shell overhangs edge 14.86
    b += note('shell straddles board plane (+1.90/-2.60 or +2.60/-1.90); behind-edge clearance [U] - 3D model', 0, 4.4)
    return footprint('USB_A_Plug_XKB_U217-041N-4BV81',
                     'USB 2.0 Type-A plug, XKB U217-041N-4BV81 (LCSC C319178), SMD signal pins + plated shell-leg slots. '
                     'Pads from vendor PCB layout; shell outline/board-edge position [U]', b, 'through_hole')


def main():
    os.makedirs(OUT, exist_ok=True)
    m3d = os.path.join(OUT, '..', '3dmodels')
    os.makedirs(m3d, exist_ok=True)
    metal = '0.45 0.46 0.5'
    # SD: body 14.75 x 14.50, 1.90 tall (drawing 1.80-2.00)
    placeholder(m3d, 'microSD_SHOUHAN_TF_PUSH',
            [(4.95 - 7.375, 4.95 + 7.375, -10.48, 4.02, 0.0, 1.9, metal)])
    # XKB: shell 12.00 x 4.50 x 18.75, front 17.78 from hole row (EasyEDA); assumed +1.90/-2.60 about
    # the board top (drawing gives 1.90/2.60, face not identified) [U]; tongue as a white block.
    placeholder(m3d, 'USB_A_Plug_XKB_U217-041N-4BV81',
            [(-6.0, 6.0, 17.78 - 18.75, 17.78, -2.6, 1.9, metal),
             (-5.5, 5.5, 17.78 - 9.0, 17.78 - 0.2, -2.0, -0.2, '0.95 0.95 0.95')])
    # D2: KiCad 10 has no stock STEP for LED_WS2812B-2020 (gen_pcb.py MODEL_OVERRIDE points D2 here).
    # 2.0 x 2.0 body, 0.85 tall (stage4-notes LED height) [U vendor drawing not checked]; STEP only.
    step_boxes(os.path.join(m3d, 'LED_WS2812B-2020_PLCC4_2.0x2.0mm.step'), 'LED_WS2812B-2020_PLCC4_2.0x2.0mm',
               [(-1.0, 1.0, -1.0, 1.0, 0.0, 0.85, '0.95 0.95 0.95')])
    for name, fn in (('microSD_SHOUHAN_TF_PUSH', tf_push), ('USB_A_Plug_XKB_U217-041N-4BV81', usb_a_xkb)):
        with open(os.path.join(OUT, name + '.kicad_mod'), 'w') as f:
            f.write(fn())
        print('wrote', name)


if __name__ == '__main__':
    main()
