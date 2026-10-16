#!/usr/bin/env python3
"""Perfil de instrucciones PowerPC desde qemu -d in_asm,exec,nochain:
instrucciones por función, restando una corrida corta (arranque)."""
import sys, re, collections
def prof(path):
    size = {}; name = {}; cnt = collections.Counter()
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
    for pc, n in cnt.items(): per[name.get(pc, '?')] += n * size.get(pc, 0)
    return per
a = prof(sys.argv[1]); b = prof(sys.argv[2]); units = float(sys.argv[3])
d = collections.Counter({k: a[k] - b.get(k, 0) for k in a})
tot = sum(v for v in d.values() if v > 0)
print(f'total {tot/units:.0f} instrucciones por unidad')
for k, v in d.most_common(25):
    if v <= 0: break
    print(f'{v/units:8.1f}  {100*v/tot:5.1f}%  {k}')
