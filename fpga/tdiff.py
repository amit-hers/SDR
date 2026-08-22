#!/usr/bin/env python3
"""Field-by-field diff of the C++ golden trace against the RTL trace.

Reports, per field, the agreement rate and the first symbol where it differs.
The output-vector diff can only say "they stopped matching"; this says which
term stopped matching first, which is the one to fix.
"""
import sys

def load(p):
    rows, hdr = [], None
    for line in open(p):
        line = line.strip()
        if not line:
            continue
        if line.startswith('#'):
            hdr = line.lstrip('#').split()
            continue
        rows.append([int(x) for x in line.split()])
    return hdr, rows

hdr_g, g = load(sys.argv[1])
hdr_h, h = load(sys.argv[2])
assert hdr_g == hdr_h, (hdr_g, hdr_h)
n = min(len(g), len(h))
print(f"golden={len(g)} hdl={len(h)} comparing={n}\n")

first_any = None
print(f"{'field':<9}{'agree':>9}  {'first diff':>10}   golden -> hdl")
for c, name in enumerate(hdr_g):
    if name == 'n':
        continue
    ok = sum(1 for i in range(n) if g[i][c] == h[i][c])
    fd = next((i for i in range(n) if g[i][c] != h[i][c]), None)
    det = '' if fd is None else f"{g[fd][c]} -> {h[fd][c]}"
    print(f"{name:<9}{ok*100.0/n:8.1f}%  {str(fd):>10}   {det}")
    if fd is not None and (first_any is None or fd < first_any):
        first_any = fd

if first_any is not None:
    print(f"\nfirst symbol with ANY field difference: {first_any}")
    lo, hi = max(0, first_any - 2), min(n, first_any + 3)
    for i in range(lo, hi):
        mark = '>>' if i == first_any else '  '
        print(f"\n{mark} symbol {i}")
        for c, name in enumerate(hdr_g):
            if name == 'n':
                continue
            flag = '  <-- DIFF' if g[i][c] != h[i][c] else ''
            print(f"     {name:<9} golden={g[i][c]:>12}  hdl={h[i][c]:>12}{flag}")
