#!/usr/bin/env python3
"""Compara registros del autotest del Wii (wiisp.log de cada corrida) por
ventanas de juego: tiempo real, velocidad, tirones, reparto del GE, ciclos
por unidad (PMC del GE) y llamadas que salta ge_bbox.
Uso: ventanas.py NOMBRE=registro.log [NOMBRE=registro.log...]"""
import re, sys

def num(s): return float(s.replace(',', '.'))

def parse(path):
    wins = []; w = None
    for l in open(path, errors='replace'):
        if not l.startswith('[TIEMPOS]'): continue
        m = re.match(r'\[TIEMPOS\] (ventana de juego: vblanks (\d+) a (\d+)|final de la escena|hasta[^,]*), ([\d.]+) s reales: ([\d.]+) s de juego \(([\d.]+)%\).*?([\d.]+) M instr', l)
        if m:
            w = dict(a=m.group(2) or 'fin', b=m.group(3) or '', real=num(m.group(4)), juego=num(m.group(5)),
                     vel=num(m.group(6)), minstr=num(m.group(7)))
            wins.append(w); continue
        if w is None: continue
        m = re.match(r'\[TIEMPOS\] CPU ([\d.]+)% syscalls [\d.]+% GE ([\d.]+)%', l)
        if m: w.update(cpu=num(m.group(1)), ge=num(m.group(2)))
        m = re.search(r'tirones \(mas del doble y \d+ ms perdidos\): (\d+), ([\d.]+) s perdidos', l)
        if m: w.update(tir=int(m.group(1)), tir_s=num(m.group(2)))
        if l.startswith('[TIEMPOS] GE por dentro:'):
            m = re.search(r'leer vertices ([\d.]+)%', l)
            if m: w['leer'] = num(m.group(1))
        m = re.search(r'PMC del GE.*? leer (\d+) por vertice.*? ensamblar (\d+) por triangulo.*? enviar (\d+) por primitiva.*? estado (\d+) por llamada', l)
        if m: w.update(c_leer=int(m.group(1)), c_ens=int(m.group(2)), c_env=int(m.group(3)), c_est=int(m.group(4)))
        m = re.search(r'PMC del GE.*? comandos (\d+) por llamada', l)
        if m: w['c_cmd'] = int(m.group(1))
        m = re.search(r'comandos (\d+) ciclos por comando', l)
        if m: w['c_pcmd'] = int(m.group(1))
        m = re.search(r'Cajas exactas \(ge_bbox\): .*?saltadas (\d+) \(\d+ sin calcular[^;]*; (\d+) vertices\)', l)
        if m: w.update(bx_salt=int(m.group(1)), bx_vert=int(m.group(2)) / 1000.0)
    return wins

runs = [(a.split('=', 1)[0], parse(a.split('=', 1)[1])) for a in sys.argv[1:]]
for k, w in runs:
    tr = sum(x['real'] for x in w); tj = sum(x['juego'] for x in w)
    print(f"{k:10} {len(w)} ventanas, {tr:6.1f} s reales, {tj:6.2f} s de juego ({100 * tj / tr:5.1f}%), {sum(x['minstr'] for x in w):7.1f} M instr")
print()
cols = [('real', 's reales', '{:7.1f}'), ('vel', '% velocidad', '{:7.1f}'), ('tir_s', 's de tirones', '{:7.1f}'),
        ('cpu', '% CPU', '{:7.1f}'), ('ge', '% GE', '{:7.1f}'), ('leer', '% leer vertices', '{:7.1f}'),
        ('c_leer', 'ciclos por vertice', '{:7d}'), ('c_ens', 'ciclos por triangulo (ensamblar)', '{:7d}'),
        ('c_env', 'ciclos por primitiva (enviar)', '{:7d}'), ('c_est', 'ciclos por llamada (estado)', '{:7d}'),
        ('c_cmd', 'ciclos por llamada (comandos)', '{:7d}'), ('c_pcmd', 'ciclos por comando', '{:7d}'),
        ('bx_salt', 'llamadas saltadas (ge_bbox)', '{:7d}'), ('bx_vert', 'miles de vertices saltados', '{:7.0f}')]
print('ventana     ' + ''.join(f'{k[:7]:>7}' for k, _ in runs))
for i in range(min(len(w) for _, w in runs)):
    w0 = runs[0][1][i]
    print(f"{w0['a']:>5}-{w0['b']:5}")
    for c, nombre, fmt in cols:
        if any(c in w[i] for _, w in runs):
            print('            ' + ''.join(fmt.format(w[i][c]) if c in w[i] else '      -' for _, w in runs) + f'  {nombre}')
