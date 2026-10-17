#!/usr/bin/env python3
"""
WIISP - tests/recarga.py
Prueba de recarga: cada pspautotest de la lista, una vez y tres veces
seguidas en el mismo proceso (--veces 3, como el autotest del Wii o volver
al menú). La salida de tres veces tiene que ser la de una repetida, la
imagen final la misma en cada vez y la memoria en uso tras descargar la
misma en las vueltas 2 y 3. Con un CLI compilado con -fsanitize=address la
memoria es exacta y se ven también errores y fugas
(ASAN_OPTIONS=detect_leaks=1).

    python3 tests/recarga.py CLI PSPAUTOTESTS LISTA

Las de partidas guardadas van de una en una (comparten la carpeta).

SPDX-License-Identifier: GPL-2.0-or-later
"""
import sys, subprocess, re, pathlib, concurrent.futures as cf
cli, root, lista = sys.argv[1], pathlib.Path(sys.argv[2]), sys.argv[3]
tests = [l.strip() for l in open(lista) if l.strip() and not l.startswith('#')]
def run(t):
    prx = root / 'tests' / (t + '.prx')
    base = [cli, '--run', '--quiet', '--frames', '2400', '--root', str(root)]
    try:
        a = subprocess.run(base + [str(prx)], capture_output=True, timeout=300)
        b = subprocess.run(base + ['--veces', '3', str(prx)], capture_output=True, timeout=900)
    except subprocess.TimeoutExpired:
        return t, 'tiempo agotado'
    v = re.findall(rb'vez (\d): imagen final ([0-9A-F]+), memoria en uso tras descargar: (\d+) bytes', b.stderr)
    probs = []
    if b.returncode: probs.append('codigo %d' % b.returncode)
    for m in re.findall(rb'(ERROR: [A-Za-z]+Sanitizer[^\n]*|runtime error[^\n]*|SUMMARY[^\n]*)', a.stderr + b.stderr)[:3]: probs.append(m.decode(errors='replace'))
    if b.stdout != a.stdout * 3: probs.append('salida distinta')
    if len(v) != 3: probs.append('%d vueltas' % len(v))
    else:
        if len({x[1] for x in v}) != 1: probs.append('imagen distinta')
        if v[1][2] != v[2][2]: probs.append('memoria %s/%s/%s bytes' % tuple(x[2].decode() for x in v))
    return t, ', '.join(probs) or ('ok %s bytes' % (v[2][2].decode()))
bad = 0
# Las de partidas comparten la carpeta de la tarjeta: de una en una
serie = [t for t in tests if 'savedata' in t]
with cf.ThreadPoolExecutor(8) as ex:
    res = list(ex.map(run, [t for t in tests if t not in serie])) + [run(t) for t in serie]
for t, r in res:
    if not r.startswith('ok'): bad += 1; print(t, r, flush=True)
print('%d pruebas, %d con problemas' % (len(tests), bad))
