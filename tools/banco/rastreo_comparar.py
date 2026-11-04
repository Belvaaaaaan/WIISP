#!/usr/bin/env python3
"""
WIISP - tools/banco/rastreo_comparar.py
Compara la traza de prof_rastreo_fb (rastreo_traza) de dos registros del
mismo guion, por ejemplo GX contra software:

    rastreo_comparar.py A.log B.log [--cuadro N] [--todo]

1. Los eventos comunes (E escrituras, L lecturas, T transferencias) de cada
   cuadro, sin lo que solo dice GX (donde, camino, eventos G): la primera
   diferencia y un diff corto.
2. Los mapas (bits 0-3 de cada texel) en el mismo orden: cuántos texeles
   difieren, y por bit.
3. Las lineas "contenido" (texeles con cada bit) una al lado de la otra.
4. Las lineas "[TIEMPOS] Rastreo, lectura": el % de texeles con cada bit
   por forma de lectura, de las dos corridas.

SPDX-License-Identifier: GPL-2.0-or-later
"""
import sys, re, difflib, collections

args = sys.argv[1:]
todo = '--todo' in args
if todo: args.remove('--todo')
solo = None
if '--cuadro' in args:
    i = args.index('--cuadro'); solo = int(args[i + 1]); del args[i:i + 2]
if len(args) != 2:
    print(__doc__); sys.exit(2)

EV = re.compile(r'^\[RASTREO\] c(\d+) ([ELT]) (.*?)(?: \(x(\d+)\))?$')
MAPA = re.compile(r'^\[RASTREO\] c(\d+) mapa (\d+) \((.*)\): textura ([0-9A-F]+)')
FILA = re.compile(r'^\[RASTREO\] c(\d+) m(\d+) (\d+) ([0-9A-F]+)$')
CONT = re.compile(r'^\[RASTREO\] c(\d+) contenido \((.*)\): textura ([0-9A-F]+), (\d+) texeles en el rango; [^:]*: ([\d ]+)$')
LECT = re.compile(r'^\[TIEMPOS\] Rastreo, lectura \d+ \((\d+) dibujos[^)]*\): tex ([0-9A-F]+) (\S+) (\S+) ancho (\d+).*?paleta \S+ >>(\d+) &([0-9A-F]+).*?contenido en (\d+) lecturas \([^)]*\): ([\d. ]+)$')


def leer(ruta):
    ev = collections.defaultdict(list)
    mapas = []
    cont = []
    lect = collections.defaultdict(lambda: [0, 0, None])
    actual = None
    for l in open(ruta, encoding='latin-1'):
        l = l.rstrip('\n')
        m = EV.match(l)
        if m:
            c, t, txt, n = int(m.group(1)), m.group(2), m.group(3), int(m.group(4) or 1)
            if t == 'L': txt = txt.split(' | donde:')[0]
            ev[c].append((t + ' ' + txt, n))
            continue
        m = MAPA.match(l)
        if m:
            actual = dict(cuadro=int(m.group(1)), tex=m.group(4), filas={}, origen=m.group(3))
            mapas.append(actual)
            continue
        m = FILA.match(l)
        if m and actual is not None:
            actual['filas'][int(m.group(3))] = m.group(4)
            continue
        m = CONT.match(l)
        if m:
            cont.append((int(m.group(1)), m.group(3), int(m.group(4)), [int(x) for x in m.group(5).split()]))
            continue
        m = LECT.match(l)
        if m:
            k = (m.group(2), m.group(3), m.group(4), m.group(5), m.group(6), m.group(7))
            n = int(m.group(8))
            pct = [float(x) for x in m.group(9).split()]
            e = lect[k]
            e[0] += int(m.group(1)); e[1] += n
            e[2] = pct if e[2] is None else [(x * (e[1] - n) + y * n) / e[1] for x, y in zip(e[2], pct)]
    return ev, mapas, cont, lect


def expandir(lista):
    r = []
    for t, n in lista: r += [t] * n
    return r


a, b = leer(args[0]), leer(args[1])
print('== Eventos comunes por cuadro (E escritura, L lectura, T transferencia)')
for c in sorted(set(a[0]) | set(b[0])):
    if solo is not None and c != solo: continue
    ea, eb = expandir(a[0].get(c, [])), expandir(b[0].get(c, []))
    print(f'cuadro {c}: A {len(ea)} eventos, B {len(eb)}', end='')
    if ea == eb:
        print(': iguales'); continue
    i = next((k for k, (x, y) in enumerate(zip(ea, eb)) if x != y), min(len(ea), len(eb)))
    print(f'; primera diferencia en el evento {i + 1}')
    d = list(difflib.unified_diff(ea, eb, 'A', 'B', n=1, lineterm=''))
    for l in (d if todo else d[:40]): print('   ', l[:400])
    if not todo and len(d) > 40: print(f'    ... ({len(d) - 40} lineas mas; --todo para verlas)')

print('\n== Mapas (bits 0-3 de cada texel), en el mismo orden')
for k, (ma, mb) in enumerate(zip(a[1], b[1])):
    dif = [0, 0, 0, 0]
    total = 0
    for y in sorted(set(ma['filas']) & set(mb['filas'])):
        for x, (p, q) in enumerate(zip(ma['filas'][y], mb['filas'][y])):
            v = int(p, 16) ^ int(q, 16)
            total += 1
            for bit in range(4):
                if v >> bit & 1: dif[bit] += 1
    print(f'mapa {k + 1}: c{ma["cuadro"]} {ma["tex"]} contra c{mb["cuadro"]} {mb["tex"]}: {total} texeles; distintos por bit 0-3: {dif}')
if len(a[1]) != len(b[1]): print(f'(A tiene {len(a[1])} mapas, B {len(b[1])})')

print('\n== Contenido (texeles con cada bit), en el mismo orden')
for k, (ca, cb) in enumerate(zip(a[2], b[2])):
    print(f'{k + 1}: c{ca[0]} {ca[1]} {ca[2]} texeles: A {ca[3][:8]}  B {cb[3][:8]}')

print('\n== [TIEMPOS] Rastreo, lectura: % de texeles con cada bit (todas las ventanas)')
for k in sorted(set(a[3]) | set(b[3])):
    pa, pb = a[3].get(k), b[3].get(k)
    fa = ' '.join(f'{x:.2f}' for x in pa[2][:4]) if pa and pa[2] else '-'
    fb = ' '.join(f'{x:.2f}' for x in pb[2][:4]) if pb and pb[2] else '-'
    print(f'{k[0]} {k[1]} {k[2]} ancho {k[3]} >>{k[4]} &{k[5]}: A bits 0-3 {fa} ({pa[1] if pa else 0} muestras)  B {fb} ({pb[1] if pb else 0} muestras)')
