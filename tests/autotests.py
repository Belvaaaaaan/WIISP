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
import difflib, os, pathlib, shutil, subprocess, sys, tempfile

import os, shlex
# WIISP_CLI permite usar otro binario, p. ej. "qemu-ppc build-pc/wiisp-cli-ppc"
CLI = shlex.split(os.environ.get('WIISP_CLI', '')) or \
      [str(pathlib.Path(__file__).resolve().parent.parent / 'build-pc' / 'wiisp-cli')]

def normalize(text):
    lines = text.replace('\r', '').split('\n')
    while lines and not lines[-1].strip():
        lines.pop()
    return [l.rstrip() for l in lines]

ROOT = None   # carpeta de pspautotests: hace de ms0:/, como el --root de PPSSPP

def run(prx, bmp=None):
    args = CLI + ['--run', '--quiet', '--frames', '2400']
    if ROOT:
        args += ['--root', ROOT]
        # Cada test empieza con la Memory Stick sin partidas, como al grabarlos
        shutil.rmtree(os.path.join(ROOT, 'PSP', 'SAVEDATA'), ignore_errors=True)
    if bmp:
        args += ['--bmp', bmp]
    try:
        p = subprocess.run(args + [str(prx)], capture_output=True, timeout=180)
        return p.stdout.decode('utf-8', 'replace'), p.stderr.decode('utf-8', 'replace')
    except subprocess.TimeoutExpired:
        return '', 'TIMEOUT'

def bmp_diff(expected, actual):
    """Porcentaje de píxeles (480x272 visibles) con RGB distinto; None si no hay captura."""
    try:
        a = open(expected, 'rb').read()[54:]
        b = open(actual, 'rb').read()[54:]
    except OSError:
        return None
    if len(b) < 512 * 272 * 4 or len(a) < 512 * 272 * 4:
        return None
    bad = 0
    for y in range(272):
        ra = a[y * 2048:y * 2048 + 480 * 4]
        rb = b[y * 2048:y * 2048 + 480 * 4]
        if ra == rb:
            continue
        for x in range(0, 480 * 4, 4):
            if ra[x:x + 3] != rb[x:x + 3]:
                bad += 1
    return 100.0 * bad / (480 * 272)

def main():
    args = sys.argv[1:]
    verbose = '-v' in args
    args = [a for a in args if a != '-v']
    global ROOT
    ROOT = str(pathlib.Path(args[0]))
    root = pathlib.Path(ROOT) / 'tests'
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
    tmpdir = tempfile.mkdtemp(prefix='wiisp_')
    for name in names:
        ref_bmp = root / (name + '.expected.bmp')
        out_bmp = os.path.join(tmpdir, name.replace('/', '_') + '.bmp') if ref_bmp.exists() else None
        out, err = run(root / (name + '.prx'), out_bmp)
        expected = (root / (name + '.expected')).read_text(errors='replace')
        text_ok = normalize(out) == normalize(expected)
        diff = bmp_diff(ref_bmp, out_bmp) if out_bmp else None
        img = ''
        if out_bmp:
            img = '  [imagen: %s]' % ('sin captura' if diff is None else '%.2f%% distinto' % diff)
        if text_ok and (out_bmp is None or diff == 0.0):
            passed.append(name)
            print('PASA  ' + name + img)
        else:
            failed.append(name)
            print('FALLA ' + name + img + ('  (' + err.strip().splitlines()[-1] + ')' if err.strip() else ''))
            if verbose:
                for l in list(difflib.unified_diff(normalize(expected), normalize(out),
                                                   'esperado', 'wiisp', lineterm='', n=1))[:40]:
                    print('      ' + l)
    print('\n%d de %d tests pasan' % (len(passed), len(names)))
    if required is not None and failed:
        sys.exit(1)

main()
