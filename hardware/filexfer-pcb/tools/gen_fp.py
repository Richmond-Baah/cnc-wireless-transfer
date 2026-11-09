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
    return (f'(footprint "{name}"\n\t(version 20241229)\n\t(generator "filexfer_gen_fp")\n'
            f'\t(layer "F.Cu")\n\t(descr "{descr}")\n\t(attr {attr})\n{body})\n')


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
    for name, fn in (('microSD_SHOUHAN_TF_PUSH', tf_push), ('USB_A_Plug_XKB_U217-041N-4BV81', usb_a_xkb)):
        with open(os.path.join(OUT, name + '.kicad_mod'), 'w') as f:
            f.write(fn())
        print('wrote', name)


if __name__ == '__main__':
    main()
