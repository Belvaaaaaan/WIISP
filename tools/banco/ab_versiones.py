#!/usr/bin/env python3
"""
WIISP - tools/banco/ab_versiones.py
Dos versiones del renderizador de GX (dos wiisp-cli-gx de build_cli_gx.sh,
por ejemplo de dos commits) en las pspautotests de una lista: la salida
entera y la huella de GX ([GX] N dibujos, huella X) tienen que coincidir.
Las de savedata no entran (comparten la carpeta de partidas). Para ver en
qué dibujo y en qué función empiezan a ser distintas: GX_TRAZA=1 (ver
stub_estado.c) con un programa y diff de lo que sale por stderr.

    ab_versiones.py CLI_GX_A CLI_GX_B PSPAUTOTESTS LISTA [hilos]

SPDX-License-Identifier: GPL-2.0-or-later
"""
import sys, subprocess, re, pathlib, concurrent.futures as cf

a_cli, b_cli, root, lista = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]), sys.argv[4]
hilos = int(sys.argv[5]) if len(sys.argv) > 5 else 4
tests = [l.strip() for l in open(lista) if l.strip() and not l.startswith('#') and 'savedata' not in l]


def run(cli, t):
    r = subprocess.run(['qemu-ppc', '-E', 'MALLOC_PERTURB_=165', cli, '--run', '--quiet', '--frames', '2400', '--root', str(root),
                        str(root / 'tests' / (t + '.prx'))], capture_output=True, timeout=1200)
    return r.stdout


def both(t):
    a, b = run(a_cli, t), run(b_cli, t)
    ga, gb = re.findall(rb'\[GX\] .*', a), re.findall(rb'\[GX\] .*', b)
    return t, a == b, ga[-1].decode() if ga else '', gb[-1].decode() if gb else ''


bad = n = 0
with cf.ThreadPoolExecutor(hilos) as ex:
    for t, same, ga, gb in ex.map(both, tests):
        n += 1
        if not same:
            bad += 1
            print('DIFIERE', t, '|', ga, '|', gb, '(misma huella de GX)' if ga == gb else '', flush=True)
print('%d programas, %d distintos' % (n, bad))
