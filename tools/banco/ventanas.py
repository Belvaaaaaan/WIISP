#!/usr/bin/env python3
"""Compara registros del autotest del Wii (wiisp.log de cada corrida) por
ventanas de juego: tiempo real, velocidad, tirones, reparto del GE, ciclos
por unidad (PMC del GE) y llamadas que salta ge_bbox (también por su caja).
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
        m = re.search(r'cajas fuera de la pantalla (\d+),.*?por su caja \(ge_bbox_llamada\): (\d+) llamadas probadas.*?saltadas (\d+) con (\d+) vertices', l)
        if m: w.update(bx_pant=int(m.group(1)), bx_ll_prob=int(m.group(2)), bx_ll_salt=int(m.group(3)), bx_ll_vert=int(m.group(4)) / 1000.0)
        m = re.search(r'por la banda de guarda \(ge_bbox_guarda\): (\d+) cajas y (\d+) llamadas fuera \((\d+) probadas\); por sus triangulos \(ge_bbox_triangulos\): (\d+) llamadas fuera \((\d+) probadas\)', l)
        if m: w.update(bx_gu_caj=int(m.group(1)), bx_gu_ll=int(m.group(2)), bx_tri=int(m.group(4)), bx_tri_prob=int(m.group(5)))
        m = re.match(r'\[TIEMPOS\] PMC: (\d+) M ciclos', l)
        if m: w['mciclos'] = int(m.group(1))
        m = re.search(r'Rachas de huesos \(ge_cmd_huesos\): (\d+) rachas con (\d+) comandos', l)
        if m: w.update(hu_rachas=int(m.group(1)), hu_cmds=int(m.group(2)) / 1000.0)
        m = re.search(r'Charolas \(ge_charolas\): (\d+) llamadas, (\d+)% ', l)
        if m: w.update(ch_ll=int(m.group(1)), ch_pct=int(m.group(2)))
        m = re.search(r'Charolas \(ge_charolas\):.*?sin charola abierta (\d+), con otra clave (\d+), textura que volver a buscar (\d+)', l)
        if m: w.update(ch_sin=int(m.group(1)), ch_clave=int(m.group(2)), ch_tex=int(m.group(3)))
        m = re.search(r'Verificacion de las charolas \(ge_verificar\): (\d+) llamadas.*?comparadas, (\d+) distintas', l)
        if m: w.update(chv=int(m.group(1)), chv_mal=int(m.group(2)))
        m = re.search(r'Envio por charolas \(ge_charolas_envio\): (\d+) tandas, (\d+) triangulos.*?y (\d+) por el camino', l)
        if m: w.update(ce_tandas=int(m.group(1)), ce_juntos=int(m.group(2)) / 1000.0, ce_sueltos=int(m.group(3)) / 1000.0)
        m = re.search(r'probar la caja de cada llamada ([\d.]+) ms \(de ellos leer sus posiciones ([\d.]+) ms.*?repasar sus triangulos ([\d.]+) ms', l)
        if m: w.update(ms_ll=num(m.group(1)), ms_tri=num(m.group(3)))
    return wins

runs = [(a.split('=', 1)[0], parse(a.split('=', 1)[1])) for a in sys.argv[1:]]
for k, w in runs:
    tr = sum(x['real'] for x in w); tj = sum(x['juego'] for x in w)
    print(f"{k:10} {len(w)} ventanas, {tr:6.1f} s reales, {tj:6.2f} s de juego ({100 * tj / tr:5.1f}%), {sum(x['minstr'] for x in w):7.1f} M instr")
print()
cols = [('real', 's reales', '{:7.1f}'), ('vel', '% velocidad', '{:7.1f}'), ('tir_s', 's de tirones', '{:7.1f}'),
        ('mciclos', 'M ciclos (PMC)', '{:7d}'),
        ('cpu', '% CPU', '{:7.1f}'), ('ge', '% GE', '{:7.1f}'), ('leer', '% leer vertices', '{:7.1f}'),
        ('c_leer', 'ciclos por vertice', '{:7d}'), ('c_ens', 'ciclos por triangulo (ensamblar)', '{:7d}'),
        ('c_env', 'ciclos por primitiva (enviar)', '{:7d}'), ('c_est', 'ciclos por llamada (estado)', '{:7d}'),
        ('c_cmd', 'ciclos por llamada (comandos)', '{:7d}'), ('c_pcmd', 'ciclos por comando', '{:7d}'),
        ('bx_salt', 'llamadas saltadas (ge_bbox)', '{:7d}'), ('bx_vert', 'miles de vertices saltados', '{:7.0f}'),
        ('bx_pant', 'cajas fuera de la pantalla', '{:7d}'), ('bx_ll_prob', 'llamadas probadas por su caja', '{:7d}'),
        ('bx_ll_salt', 'saltadas por su caja', '{:7d}'), ('bx_ll_vert', 'miles de vertices (por su caja)', '{:7.0f}'),
        ('bx_gu_caj', 'cajas fuera por la banda', '{:7d}'), ('bx_gu_ll', 'llamadas fuera por la banda', '{:7d}'),
        ('bx_tri_prob', 'llamadas repasadas por triangulos', '{:7d}'), ('bx_tri', 'fuera por sus triangulos', '{:7d}'),
        ('ms_ll', 'ms probando la caja de cada llamada', '{:7.0f}'), ('ms_tri', 'ms repasando triangulos', '{:7.0f}'),
        ('hu_rachas', 'rachas de huesos (ge_cmd_huesos)', '{:7d}'), ('hu_cmds', 'miles de comandos en rachas', '{:7.0f}'),
        ('ch_ll', 'llamadas (ge_charolas)', '{:7d}'), ('ch_pct', '% que siguen la charola', '{:7d}'),
        ('ch_sin', 'sin charola abierta', '{:7d}'), ('ch_clave', 'con otra clave', '{:7d}'),
        ('ch_tex', 'textura que volver a buscar', '{:7d}'), ('chv', 'charolas comparadas (ge_verificar)', '{:7d}'),
        ('chv_mal', 'charolas distintas (tiene que ser 0)', '{:7d}'),
        ('ce_tandas', 'tandas (ge_charolas_envio)', '{:7d}'), ('ce_juntos', 'miles de triangulos juntos', '{:7.0f}'),
        ('ce_sueltos', 'miles de triangulos sueltos', '{:7.0f}')]
print('ventana     ' + ''.join(f'{k[:7]:>7}' for k, _ in runs))
for i in range(min(len(w) for _, w in runs)):
    w0 = runs[0][1][i]
    print(f"{w0['a']:>5}-{w0['b']:5}")
    for c, nombre, fmt in cols:
        if any(c in w[i] for _, w in runs):
            print('            ' + ''.join(fmt.format(w[i][c]) if c in w[i] else '      -' for _, w in runs) + f'  {nombre}')
