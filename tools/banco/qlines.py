#!/usr/bin/env python3
"""Instrucciones por bloque con su función y línea de código (addr2line; el banco
se compila con -g): qlines.py binario grande.log chico.log unidades [filtro]
Sale una línea por bloque, separada por tabuladores: instrucciones por unidad,
función del bloque (la de qemu), dirección, tamaño, función en línea y línea."""
import sys, re, collections, subprocess
def prof(path):
    size = {}; name = {}; cnt = collections.Counter(); cur = None; curname = None; acc = 0
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
    return cnt, size, name
ca, sa, na = prof(sys.argv[2]); cb, _, _ = prof(sys.argv[3]); units = float(sys.argv[4])
filt = sys.argv[5] if len(sys.argv) > 5 else None
d = {k: (ca[k] - cb.get(k, 0)) * sa.get(k, 0) for k in ca}
d = {k: v for k, v in d.items() if v > 0 and (not filt or re.search(filt, na.get(k, '')))}
addrs = sorted(d)
out = subprocess.run(['powerpc-linux-gnu-addr2line', '-f', '-e', sys.argv[1]] + ['%x' % a for a in addrs], capture_output=True, text=True).stdout.split('\n')
for i, a in enumerate(addrs):
    fn = out[2*i]; loc = out[2*i+1].split('/')[-1]
    print(f'{d[a]/units:.1f}\t{na.get(a,"?")}\t{a:08x}\t{sa[a]}\t{fn}\t{loc}')
