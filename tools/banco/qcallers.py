#!/usr/bin/env python3
"""Quién llama a una función: qcallers.py log funcion1,funcion2 (bloque anterior a la entrada)"""
import sys, re, collections
targets = set(sys.argv[2].split(','))
name = {}; cur = None; curname = None
with open(sys.argv[1], errors='replace') as f:
    prev = None
    calls = collections.Counter()
    for line in f:
        if line.startswith('Trace '):
            m = re.search(r'\[[0-9a-f]+/([0-9a-f]+)/', line)
            if not m: continue
            pc = int(m.group(1), 16); n = name.get(pc)
            pn = name.get(prev) if prev is not None else None
            if n in targets and pn != n and pn is not None:
                calls[(n, pn)] += 1
            prev = pc
        elif line.startswith('IN: '):
            curname = line[4:].strip(); cur = None
        elif line.startswith('0x') and curname is not None and cur is None:
            cur = int(line.split(':')[0], 16); name[cur] = curname
for (n, p), c in calls.most_common(40): print(f'{c:8d}  {n} <- {p}')
