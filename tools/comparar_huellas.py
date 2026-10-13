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
