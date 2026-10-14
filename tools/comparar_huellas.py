#!/usr/bin/env python3
"""Compara dos archivos de huellas de WIISP (<prueba>.huellas).

    python3 tools/comparar_huellas.py A.huellas B.huellas

Cada huella resume el estado emulado en un cuadro (ciclos, PC, registros y
RAM). Si dos corridas del mismo guion salen iguales, la escena es repetible;
si no, dice el primer cuadro en que se separaron, que es por donde hay que
buscar lo que depende del reloj del Wii en vez de los ciclos emulados.

Devuelve 0 si son idénticas, 1 si difieren y 2 si falta un archivo.

SPDX-License-Identifier: GPL-2.0-or-later
"""
import sys


def leer(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        return [l.rstrip('\n') for l in f if l.strip() and not l.startswith('#')]


def mapas(lineas):
    """Líneas 'mapa cuadro N: h0 h1 ...' (un resumen por MB de RAM)."""
    out = {}
    for l in lineas:
        if l.startswith('mapa cuadro '):
            cab, _, resto = l.partition(':')
            out[cab.split()[2]] = resto.split()
    return out


def zonas_distintas(a, b):
    """Qué MB de la RAM de la PSP (desde 0x08000000) difieren en cada mapa."""
    ma, mb = mapas(a), mapas(b)
    for cuadro in ma:
        if cuadro not in mb:
            continue
        dif = [i for i, (x, y) in enumerate(zip(ma[cuadro], mb[cuadro])) if x != y]
        if dif:
            zonas = ', '.join(f'{0x08000000 + (i << 20):08X}-{0x08000000 + ((i + 1) << 20) - 1:08X}' for i in dif)
            print(f'  RAM distinta en el cuadro {cuadro}: {len(dif)} MB ({zonas})')


def main():
    if len(sys.argv) != 3:
        print(__doc__.strip().splitlines()[2].strip())
        return 2
    try:
        a, b = leer(sys.argv[1]), leer(sys.argv[2])
    except OSError as e:
        print(e)
        return 2
    if not a and not b:
        print('SIN HUELLAS: ninguna corrida llego al primer punto de control')
        return 1
    zonas_distintas(a, b)
    cpu = [l for l in a if l.startswith('cuadro ')]
    cpu_b = [l for l in b if l.startswith('cuadro ')]
    if cpu and len(cpu) == len(cpu_b) and all(x.split()[:8] == y.split()[:8] for x, y in zip(cpu, cpu_b)):
        print(f'CPU identico en las {len(cpu)} huellas (ciclos, PC y registros): el juego hizo exactamente lo mismo')
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            print(f'DISTINTAS desde la huella {i + 1}:')
            print(f'  A: {x}')
            print(f'  B: {y}')
            if i:
                print(f'  (la anterior coincidía: {a[i - 1]})')
            return 1
    if len(a) != len(b):
        corta, larga = ('A', b) if len(a) < len(b) else ('B', a)
        print(f'Iguales en las {min(len(a), len(b))} primeras, pero {corta} termina antes; '
              f'la otra sigue con: {larga[min(len(a), len(b))]}')
        return 1
    print(f'IDENTICAS ({len(a)} huellas)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
