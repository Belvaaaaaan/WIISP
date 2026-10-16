#!/usr/bin/env python3
"""Instrucciones por bloque (TB) de una función: qtb.py grande.log chico.log unidades funcion"""
import sys, re, collections
def prof(path, fn):
    size = {}; name = {}; cnt = collections.Counter(); first = {}
    cur = None; curname = None; acc = 0
    with open(path, errors='replace') as f:
        for line in f:
            if line.startswith('Trace '):
                m = re.search(r'\[[0-9a-f]+/([0-9a-f]+)/', line)
                if m: cnt[int(m.group(1), 16)] += 1
            elif line.startswith('IN: '):
                curname = line[4:].strip(); cur = None; acc = 0
            elif line.startswith('0x') and curname is not None and cur is None:
                cur = int(line.split(':')[0], 16)
            elif line.startswith('OBJD-T: ') and cur is not None:
                acc += len(line[8:].strip()) // 8
                size[cur] = acc; name[cur] = curname
    per = collections.Counter()
    for pc, n in cnt.items():
        if name.get(pc) == fn: per[pc] += n * size.get(pc, 0)
    return per, size
a, sa = prof(sys.argv[1], sys.argv[4]); b, _ = prof(sys.argv[2], sys.argv[4]); units = float(sys.argv[3])
d = collections.Counter({k: a[k] - b.get(k, 0) for k in a})
tot = sum(v for v in d.values() if v > 0)
print(f'{sys.argv[4]}: {tot/units:.1f} por unidad')
for k, v in sorted(d.items()):
    if v / units >= 0.5: print(f'  {k:08x} ({sa[k]:3d} instr) {v/units:7.1f}')
