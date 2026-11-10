#!/usr/bin/env python3
"""Generate JLCPCB fab files for the single FileXfer rev A board into fab/rev-A/.

Run from hardware/filexfer-pcb after gen_sch.py / gen_pcb.py / route.py:
    python3 tools/gen_fab.py

Outputs:
  gerbers/                 Gerber X2 (9 layers) + Excellon PTH/NPTH + drill maps
  filexfer-revA-gerbers.zip
  bom-RX-jlc.csv, cpl-RX-jlc.csv   receiver build (Fit ALL + RX)
  bom-TX-jlc.csv, cpl-TX-jlc.csv   transmitter build (Fit ALL; R7 = 22k)
J1 (USB-A plug) is DNP in every build (hand-soldered). Fit=DNP parts and the bottom test pads
(not in BOM/CPL) are never placed.
"""
import csv, os, shutil, subprocess, sys, zipfile

HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
PCB, SCH = 'filexfer-pcb.kicad_pcb', 'filexfer-pcb.kicad_sch'
OUT = os.path.join(HERE, 'fab', 'rev-A')
GERB = os.path.join(OUT, 'gerbers')
LAYERS = 'F.Cu,B.Cu,F.Paste,B.Paste,F.Silkscreen,B.Silkscreen,F.Mask,B.Mask,Edge.Cuts'
ALWAYS_DNP = {'J1'}                                   # plug: hand-soldered
VARIANTS = {
    'RX': dict(fit={'ALL', 'RX'}, override={}),
    'TX': dict(fit={'ALL'}, override={'R7': ('22k', '0402WGF2202TCE', 'C25768')}),
}


def run(*args):
    r = subprocess.run(['kicad-cli', *args], cwd=HERE, capture_output=True, text=True)
    if r.returncode:
        sys.exit(r.stdout + r.stderr)


def main():
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(GERB)
    run('pcb', 'export', 'gerbers', '-o', GERB, '-l', LAYERS, '--subtract-soldermask', '--check-zones', PCB)
    run('pcb', 'export', 'drill', '-o', GERB + os.sep, '--format', 'excellon', '--excellon-separate-th',
        '--excellon-units', 'mm', '--generate-map', '--map-format', 'gerberx2', PCB)
    tmp_pos, tmp_bom = os.path.join(OUT, '_pos.csv'), os.path.join(OUT, '_bom.csv')
    run('pcb', 'export', 'pos', '-o', tmp_pos, '--format', 'csv', '--units', 'mm', '--side', 'both', PCB)
    run('sch', 'export', 'bom', '-o', tmp_bom, '--fields', 'Reference,Value,Footprint,MPN,LCSC,Fit',
        '--group-by', '', SCH)
    pos = {r['Ref']: r for r in csv.DictReader(open(tmp_pos))}
    parts = list(csv.DictReader(open(tmp_bom)))
    os.remove(tmp_pos); os.remove(tmp_bom)

    with zipfile.ZipFile(os.path.join(OUT, 'filexfer-revA-gerbers.zip'), 'w', zipfile.ZIP_DEFLATED) as z:
        for f in sorted(os.listdir(GERB)):
            z.write(os.path.join(GERB, f), f)

    for name, v in VARIANTS.items():
        fitted = []
        for p in parts:
            ref = p['Reference']
            if ref in ALWAYS_DNP or p['Fit'] not in v['fit']:
                continue
            val, mpn, lcsc = v['override'].get(ref, (p['Value'], p['MPN'], p['LCSC']))
            if not lcsc:
                sys.exit(f'{ref}: no LCSC part number')
            if ref not in pos:
                sys.exit(f'{ref}: not in the placement file')
            fitted.append((ref, val, p['Footprint'].split(':')[-1], mpn, lcsc))
        groups = {}
        for ref, val, fp, mpn, lcsc in fitted:
            groups.setdefault((val, fp, lcsc, mpn), []).append(ref)
        key = lambda r: (r.rstrip('0123456789'), int(r[len(r.rstrip('0123456789')):]))
        with open(os.path.join(OUT, f'bom-{name}-jlc.csv'), 'w', newline='') as f:
            w = csv.writer(f)
            w.writerow(['Comment', 'Designator', 'Footprint', 'LCSC Part #', 'MPN', 'Qty'])
            for (val, fp, lcsc, mpn), refs in sorted(groups.items(), key=lambda g: key(sorted(g[1], key=key)[0])):
                refs.sort(key=key)
                w.writerow([val, ','.join(refs), fp, lcsc, mpn, len(refs)])
        with open(os.path.join(OUT, f'cpl-{name}-jlc.csv'), 'w', newline='') as f:
            w = csv.writer(f)
            w.writerow(['Designator', 'Mid X', 'Mid Y', 'Layer', 'Rotation'])
            for ref, *_ in sorted(fitted, key=lambda t: key(t[0])):
                r = pos[ref]
                w.writerow([ref, f"{float(r['PosX']):.4f}mm", f"{float(r['PosY']):.4f}mm",
                            'Top' if r['Side'] == 'top' else 'Bottom', f"{float(r['Rot']):.1f}"])
        print(f'{name}: {len(fitted)} placements, {len(groups)} BOM lines')
    print('wrote', os.path.relpath(OUT, HERE))


if __name__ == '__main__':
    main()
