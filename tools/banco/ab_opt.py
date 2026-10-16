#!/usr/bin/env python3
"""A/B: el mismo CLI con --opt OPCION=0 y =1: salida, huellas y captura final.
Uso: ab_opt.py CLI PSPAUTOTESTS LISTA OUTDIR OPCION [workers] [extra --opt...]"""
import os, sys, shutil, subprocess, pathlib, concurrent.futures as cf, threading
cli, src_root, lista, out, opt = sys.argv[1:6]
workers = int(sys.argv[6]) if len(sys.argv) > 6 else 4
extra = sys.argv[7:]
names = [l.strip() for l in open(lista) if l.strip() and not l.startswith('#')]
out = pathlib.Path(out); out.mkdir(parents=True, exist_ok=True)
roots = []
for w in range(workers):
    r = out / f'root{w}'
    if not r.exists():
        shutil.copytree(src_root, r, symlinks=True)
    roots.append(r)
guion = out / 'g.guion'
guion.write_text('2400 fin\n')

def run(root, name, val, tag):
    shutil.rmtree(root / 'PSP' / 'SAVEDATA', ignore_errors=True)
    # Fechas fijas: borrar SAVEDATA cambia la de PSP (y el HLE la enseña)
    for d in (root, root / 'PSP'):
        try: os.utime(d, (1700000000, 1700000000))
        except OSError: pass
    pref = out / (name.replace('/', '_') + '.' + tag)
    bmp = str(pref) + '.bmp'; ppm = str(pref) + '.ppm'
    for f in (bmp, ppm):
        try: os.remove(f)
        except OSError: pass
    args = [cli, '--run', '--quiet', '--frames', '2400', '--root', str(root),
            '--guion', str(guion), '--salida', str(pref), '--capturas-guion', '0', '--bmp', bmp,
            '--screenshot', ppm]
    for o in opt.split(','): args += ['--opt', f'{o}={val}']
    for e in extra: args += ['--fast-math'] if e == 'MR' else ['--opt', e]
    args.append(str(root / 'tests' / (name + '.prx')))
    try:
        p = subprocess.run(args, capture_output=True, timeout=300)
        so = p.stdout
    except subprocess.TimeoutExpired:
        so = b'TIMEOUT'
    try:
        h = [l for l in open(str(pref) + '.huellas') if not l.startswith('#')]
    except OSError:
        h = ['SIN HUELLAS']
    img = b''
    for f in (bmp, ppm):
        try: img += open(f, 'rb').read()
        except OSError: img += b'-'
    if img == b'--': img = None
    return so, h, img

lock = threading.Lock(); free = list(range(workers))
def job(name):
    with lock: w = free.pop()
    try:
        root = roots[w]
        a = run(root, name, '0', 'a'); b = run(root, name, '1', 'b')
        return name, a[0] == b[0], a[1] == b[1], a[2] == b[2], len(a[1]), a[2] is not None
    finally:
        with lock: free.append(w)
bad = 0; total_h = 0; imgs = 0
with cf.ThreadPoolExecutor(workers) as ex:
    for name, so_ok, h_ok, img_ok, nh, has_img in ex.map(job, names):
        total_h += nh; imgs += has_img
        if not (so_ok and h_ok and img_ok):
            bad += 1; print('DIFIERE', name, '' if so_ok else 'salida', '' if h_ok else 'huellas', '' if img_ok else 'imagen', flush=True)
print(f'{opt}: {len(names)} programas, {bad} distintos, {total_h} huellas y {imgs} imagenes comparadas')
