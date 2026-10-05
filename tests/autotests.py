#!/usr/bin/env python3
"""
WIISP - autotests.py
Ejecuta pspautotests (https://github.com/hrydgard/pspautotests) con el CLI
de WIISP y compara la salida con los .expected (grabados en una PSP real).

    python3 tests/autotests.py <ruta a pspautotests> [filtro...]
    python3 tests/autotests.py <ruta> --list tests/autotests_pass.txt

--list FILE   ejecuta solo los tests de FILE (uno por línea, sin extensión)
              y falla si alguno no pasa. Es lo que usa CI.
-v            muestra el diff de los que fallan

SPDX-License-Identifier: GPL-2.0-or-later
"""
import difflib, pathlib, subprocess, sys

import os, shlex
# WIISP_CLI permite usar otro binario, p. ej. "qemu-ppc build-pc/wiisp-cli-ppc"
CLI = shlex.split(os.environ.get('WIISP_CLI', '')) or \
      [str(pathlib.Path(__file__).resolve().parent.parent / 'build-pc' / 'wiisp-cli')]

def normalize(text):
    lines = text.replace('\r', '').split('\n')
    while lines and not lines[-1].strip():
        lines.pop()
    return [l.rstrip() for l in lines]

def run(prx):
    try:
        p = subprocess.run(CLI + ['--run', '--quiet', '--frames', '1200', str(prx)],
                           capture_output=True, timeout=180)
        return p.stdout.decode('utf-8', 'replace'), p.stderr.decode('utf-8', 'replace')
    except subprocess.TimeoutExpired:
        return '', 'TIMEOUT'

def main():
    args = sys.argv[1:]
    verbose = '-v' in args
    args = [a for a in args if a != '-v']
    root = pathlib.Path(args[0]) / 'tests'
    required = None
    if len(args) >= 3 and args[1] == '--list':
        required = [l.strip() for l in open(args[2]) if l.strip() and not l.startswith('#')]
        names = required
    else:
        filters = args[1:]
        names = sorted(str(p.relative_to(root).with_suffix('')) for p in root.rglob('*.expected'))
        names = [n for n in names if (root / (n + '.prx')).exists()
                 and (not filters or any(f in n for f in filters))]

    passed, failed = [], []
    for name in names:
        out, err = run(root / (name + '.prx'))
        expected = (root / (name + '.expected')).read_text(errors='replace')
        if normalize(out) == normalize(expected):
            passed.append(name)
            print('PASA  ' + name)
        else:
            failed.append(name)
            print('FALLA ' + name + ('  (' + err.strip().splitlines()[-1] + ')' if err.strip() else ''))
            if verbose:
                for l in list(difflib.unified_diff(normalize(expected), normalize(out),
                                                   'esperado', 'wiisp', lineterm='', n=1))[:40]:
                    print('      ' + l)
    print('\n%d de %d tests pasan' % (len(passed), len(names)))
    if required is not None and failed:
        sys.exit(1)

main()
