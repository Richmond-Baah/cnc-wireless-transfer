"""Minimal KiCad s-expression reader/writer and symbol-library helpers."""
import re
_tok = re.compile(r'\(|\)|"(?:\\.|[^"\\])*"|[^\s()"]+')

def parse(s):
    stack = [[]]
    for m in _tok.finditer(s):
        t = m.group(0)
        if t == '(':
            stack.append([])
        elif t == ')':
            x = stack.pop(); stack[-1].append(x)
        else:
            stack[-1].append(t)
    return stack[0][0]

def dump(x, ind=0):
    if not isinstance(x, list):
        return x
    if not any(isinstance(i, list) for i in x):
        return '(' + ' '.join(x) + ')'
    s = '('
    first = True
    for i in x:
        if isinstance(i, list):
            s += '\n' + '\t' * (ind + 1) + dump(i, ind + 1)
        else:
            s += ('' if first else ' ') + i
        first = False
    return s + '\n' + '\t' * ind + ')'

def find(x, key):
    return [i for i in x if isinstance(i, list) and i and i[0] == key]

def unq(t):
    return t[1:-1] if t.startswith('"') else t

def q(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'

LIBDIR = '/usr/share/kicad/symbols'
_cache = {}

def lib(name):
    if name not in _cache:
        _cache[name] = parse(open(f'{LIBDIR}/{name}.kicad_sym').read())
    return _cache[name]

def getsym(libname, sym):
    for s in find(lib(libname), 'symbol'):
        if unq(s[1]) == sym:
            return s
    raise KeyError(f'{libname}:{sym}')

def pins(sym):
    """Pins of a library symbol: list of dicts (x, y in lib units, y up)."""
    out = []
    def walk(x, unit):
        for i in x:
            if isinstance(i, list) and i and i[0] == 'symbol':
                parts = unq(i[1]).rsplit('_', 2)
                walk(i, int(parts[-2]) if len(parts) == 3 and parts[-2].isdigit() else unit)
            elif isinstance(i, list) and i and i[0] == 'pin':
                at = find(i, 'at')[0]
                out.append(dict(type=i[1], x=float(at[1]), y=float(at[2]),
                                a=float(at[3]) if len(at) > 3 else 0.0,
                                name=unq(find(i, 'name')[0][1]),
                                num=unq(find(i, 'number')[0][1]), unit=unit))
    walk(sym, 0)
    return out
