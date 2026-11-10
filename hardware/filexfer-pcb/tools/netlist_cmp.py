#!/usr/bin/env python3
"""Compare two KiCad netlists (kicadsexpr): components + fields, nets (names + nodes), libparts.
Usage: python3 -I tools/netlist_cmp.py before.net after.net"""
import sys, re
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from sexp import parse, find, unq
def load(f):
    t = parse(open(f).read())
    comps = {}
    for c in find(find(t, 'components')[0], 'comp'):
        ref = unq(find(c, 'ref')[0][1])
        d = {k: unq(find(c, k)[0][1]) for k in ('value', 'footprint') if find(c, k)}
        d['fields'] = sorted((unq(find(x, 'name')[0][1]), unq(x[2]) if len(x) > 2 and not isinstance(x[2], list) else '')
                             for x in find(find(c, 'fields')[0], 'field')) if find(c, 'fields') else []
        d['props'] = sorted((unq(find(x, 'name')[0][1]), unq(find(x, 'value')[0][1]) if find(x, 'value') else '')
                            for x in find(c, 'property'))
        d['lib'] = str(find(c, 'libsource')[0]) if find(c, 'libsource') else ''
        d['tstamps'] = str(find(c, 'tstamps')[0]) if find(c, 'tstamps') else ''
        d['sheet'] = str(find(c, 'sheetpath')[0]) if find(c, 'sheetpath') else ''
        comps[ref] = d
    nets = {}
    for n in find(find(t, 'nets')[0], 'net'):
        name = unq(find(n, 'name')[0][1])
        nets[name] = sorted(tuple(unq(find(x, k)[0][1]) if find(x, k) else '' for k in ('ref', 'pin', 'pinfunction', 'pintype'))
                            for x in find(n, 'node'))
    libparts = sorted(str(x) for x in find(find(t, 'libparts')[0], 'libpart')) if find(t, 'libparts') else []
    return comps, nets, libparts
a, b = load(sys.argv[1]), load(sys.argv[2])
print('components: %d vs %d, identical: %s' % (len(a[0]), len(b[0]), a[0] == b[0]))
for r in sorted(set(a[0]) | set(b[0])):
    if a[0].get(r) != b[0].get(r):
        print('  diff', r, a[0].get(r), '\n      ', b[0].get(r))
print('nets: %d vs %d, identical (names + nodes): %s' % (len(a[1]), len(b[1]), a[1] == b[1]))
for n in sorted(set(a[1]) | set(b[1])):
    if a[1].get(n) != b[1].get(n):
        print('  diff', n, a[1].get(n), '->', b[1].get(n))
print('libparts identical:', a[2] == b[2])
