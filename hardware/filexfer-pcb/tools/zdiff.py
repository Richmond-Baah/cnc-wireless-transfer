#!/usr/bin/env python3
"""Differential impedance of an edge-coupled (coplanar-grounded) pair on a 2-layer board.

2D finite-difference Laplace solve of the cross-section, odd mode (symmetry plane = 0 V), half domain.
Zodd = 1 / (c * sqrt(C * C_air)), Zdiff = 2 * Zodd. Zero-thickness copper, no solder mask
(mask lowers Z by a few ohms). Pure Python (no numpy); h = 0.025 mm grid, SOR.

usage: zdiff.py W GAP [GND_GAP] [H] [ER]      (mm; GND_GAP 0 = no coplanar ground)
"""
import math, sys

E0, C0 = 8.854e-12, 299792458.0


def solve(w, s, g, h=1.51, er=4.5, d=0.025, air=3.0, side=5.0):
    nx = int(round((s / 2 + w + side) / d)) + 1
    ny = int(round((h + air) / d)) + 1
    jt = int(round(h / d))                        # row of the top copper (y = h)
    i0, i1 = int(round(s / 2 / d)), int(round((s / 2 + w) / d))
    ig = int(round((s / 2 + w + g) / d)) if g > 0 else None
    fixed = [[False] * ny for _ in range(nx)]
    V = [[0.0] * ny for _ in range(nx)]
    for i in range(nx):
        fixed[i][0] = fixed[i][ny - 1] = True     # bottom plane, far top
        if i0 <= i <= i1:
            fixed[i][jt] = True; V[i][jt] = 1.0
        if ig is not None and i >= ig:
            fixed[i][jt] = True                   # coplanar ground on the top layer
    for j in range(ny):
        fixed[0][j] = True                        # odd-mode symmetry plane
        fixed[nx - 1][j] = True                   # far side

    def eps(j2):  # permittivity of the cell row between j2 and j2+1 (half-cell rows)
        return er if j2 < jt else 1.0

    def run(epsr):
        ep = [epsr if j < jt else 1.0 for j in range(ny)]   # cell row j (between node j and j+1)
        w_ = 1.9
        for it in range(4000):
            dmax = 0.0
            for i in range(1, nx - 1):
                col, cl, cr, fx = V[i], V[i - 1], V[i + 1], fixed[i]
                for j in range(1, ny - 1):
                    if fx[j]:
                        continue
                    eu, el = ep[j], ep[j - 1]             # above / below the node
                    eh = (eu + el) / 2                    # horizontal neighbours
                    new = (eh * (cl[j] + cr[j]) + eu * col[j + 1] + el * col[j - 1]) / (2 * eh + eu + el)
                    dv = new - col[j]
                    col[j] += w_ * dv
                    if abs(dv) > dmax:
                        dmax = abs(dv)
            if dmax < 1e-6:
                break
        # energy: W = 1/2 sum eps |grad V|^2 over cells; C = 2W (V = 1)
        Wsum = 0.0
        for i in range(nx - 1):
            for j in range(ny - 1):
                ex = (V[i + 1][j] - V[i][j] + V[i + 1][j + 1] - V[i][j + 1]) / 2
                ey = (V[i][j + 1] - V[i][j] + V[i + 1][j + 1] - V[i + 1][j]) / 2
                Wsum += ep[j] * (ex * ex + ey * ey)
        return E0 * Wsum            # per metre (cells are square: d cancels in 2D)

    Cd = run(er)
    for i in range(nx):
        for j in range(ny):
            if not fixed[i][j]:
                V[i][j] = 0.0
    Ca = run(1.0)
    zodd = 1 / (C0 * math.sqrt(Cd * Ca))
    return 2 * zodd, Cd / Ca


if __name__ == '__main__':
    a = [float(x) for x in sys.argv[1:]]
    w, s = a[0], a[1]
    g = a[2] if len(a) > 2 else 0.0
    h = a[3] if len(a) > 3 else 1.51
    er = a[4] if len(a) > 4 else 4.5
    z, ee = solve(w, s, g, h, er)
    print(f'w={w} gap={s} gnd_gap={g} h={h} er={er}: Zdiff = {z:.1f} ohm (eeff {ee:.2f})')
