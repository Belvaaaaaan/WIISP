#!/usr/bin/env python3
"""
WIISP - tools/banco/ab_versiones.py
Dos versiones del renderizador de GX (dos wiisp-cli-gx de build_cli_gx.sh,
por ejemplo de dos commits) en las pspautotests de una lista: la salida
entera y la huella de GX ([GX] N dibujos, huella X) tienen que coincidir.
Las de savedata no entran (comparten la carpeta de partidas). Para ver en
qué dibujo y en qué función empiezan a ser distintas: GX_TRAZA=1 (ver
stub_estado.c) con un programa y diff de lo que sale por stderr.

    ab_versiones.py CLI_GX_A CLI_GX_B PSPAUTOTESTS LISTA [hilos] [--a OPCION=V]... [--b OPCION=V]... [--fifo | --sin-fifo]

--a/--b: interruptores para cada lado (el mismo CLI con --opt distintos
sirve para un A/B de un interruptor del renderizador de GX). --fifo: de la
línea [GX] solo cuenta la huella del FIFO (los triángulos con su estado,
igual con un GX_Begin por triángulo o uno para muchos: ge_charolas_envio).
--sin-fifo: de la línea [GX] se quita la huella del FIFO (para comparar con
un CLI de antes de la 0.5.23.4, que no la escribe).

SPDX-License-Identifier: GPL-2.0-or-later
"""
import sys, subprocess, re, pathlib, concurrent.futures as cf

args = sys.argv[1:]
opts = {'--a': [], '--b': []}
fifo = '--fifo' in args
if fifo: args.remove('--fifo')
sin_fifo = '--sin-fifo' in args
if sin_fifo: args.remove('--sin-fifo')
for k in ('--a', '--b'):
    while k in args:
        i = args.index(k); opts[k] += ['--opt', args[i + 1]]; del args[i:i + 2]
a_cli, b_cli, root, lista = args[0], args[1], pathlib.Path(args[2]), args[3]
hilos = int(args[4]) if len(args) > 4 else 4
tests = [l.strip() for l in open(lista) if l.strip() and not l.startswith('#') and 'savedata' not in l]


def run(cli, t, extra):
    r = subprocess.run(['qemu-ppc', '-E', 'MALLOC_PERTURB_=165', cli, '--run', '--quiet', '--frames', '2400', '--root', str(root)] +
                       extra + [str(root / 'tests' / (t + '.prx'))], capture_output=True, timeout=1200)
    out = r.stdout
    if fifo: out = re.sub(rb'\[GX\] [^;]*; ', b'[GX] ', out)
    if sin_fifo: out = re.sub(rb'(\[GX\] [^;\n]*); FIFO[^\n]*', rb'\1', out)
    return out


def both(t):
    a, b = run(a_cli, t, opts['--a']), run(b_cli, t, opts['--b'])
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
