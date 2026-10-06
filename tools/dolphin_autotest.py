#!/usr/bin/env python3
"""
WIISP - dolphin_autotest.py
Ejecuta pspautotests (o cualquier programa de PSP) con WIISP dentro de
Dolphin, el emulador de Wii, usando el modo de pruebas automáticas del .dol
(sd:/wiisp/autotest.txt, ver src/wii/main.c). Sirve para probar el backend
GX sin la consola.

    python3 tools/dolphin_autotest.py --tests ../pspautotests gpu/commands/basic gpu/texfunc/modulate
    python3 tools/dolphin_autotest.py --tests ../pspautotests @lista.txt --soft
    python3 tools/dolphin_autotest.py --run demo/EBOOT.PBP --frames 600   # FPS y captura final

Opciones:
  --dolphin RUTA   dolphin-emu-nogui (por defecto $DOLPHIN o el del PATH)
  --user DIR       carpeta de usuario de Dolphin (por defecto ~/.wiisp-dolphin)
  --elf RUTA       wiisp.elf (Dolphin no carga el .dol sin relleno; el .elf sí)
  --tests DIR      raíz de pspautotests; los nombres van sin extensión
  --run            los argumentos son programas, no tests
  --soft           renderizador por software en lugar de GX
  --frames N       máximo de frames por programa (1200)
  --secs N         límite de tiempo para Dolphin (900)
  -v               diferencias de los que fallan

Dolphin necesita una pantalla: con Xvfb, DISPLAY=:N. La SD se sincroniza
con una carpeta (WiiSDCardEnableFolderSync), que se escribe al apagarse el
Wii emulado; WIISP apaga al acabar la lista.

Los tests pasan si coinciden exactamente; "CASI" si solo difieren en
colores de hasta 6 por canal (la aritmética de GX no es la del GE).

SPDX-License-Identifier: GPL-2.0-or-later
"""
import difflib, os, pathlib, re, shutil, subprocess, sys, time

ROOT = pathlib.Path(__file__).resolve().parent.parent
HEX = re.compile(r'[0-9a-fA-F]{6,8}')


def normalize(text):
    lines = text.replace('\r', '').split('\n')
    while lines and not lines[-1].strip():
        lines.pop()
    return [l.rstrip() for l in lines]


def bytes_close(a, b, tol):
    if len(a) != len(b):
        return False
    va, vb = int(a, 16), int(b, 16)
    return all(abs(((va >> i) & 255) - ((vb >> i) & 255)) <= tol for i in range(0, len(a) * 4, 8))


def lines_close(exp, out, tol):
    if len(exp) != len(out):
        return False
    for a, b in zip(exp, out):
        if a == b:
            continue
        if HEX.split(a) != HEX.split(b):
            return False
        ha, hb = HEX.findall(a), HEX.findall(b)
        if len(ha) != len(hb) or not all(bytes_close(x, y, tol) for x, y in zip(ha, hb)):
            return False
    return True


def bmp_diff(expected, actual, tol=0):
    """Porcentaje de los 480x272 píxeles visibles que difieren en más de tol."""
    try:
        a = open(expected, 'rb').read()[54:]
        b = open(actual, 'rb').read()[54:]
    except OSError:
        return None
    if len(a) < 512 * 272 * 4 or len(b) < 512 * 272 * 4:
        return None
    bad = 0
    for y in range(272):
        ra, rb = a[y * 2048:y * 2048 + 1920], b[y * 2048:y * 2048 + 1920]
        if ra == rb:
            continue
        for x in range(0, 1920, 4):
            if max(abs(ra[x + i] - rb[x + i]) for i in range(3)) > tol:
                bad += 1
    return 100.0 * bad / (480 * 272)


def write_config(udir, sd):
    cfg = udir / 'Config'
    cfg.mkdir(parents=True, exist_ok=True)
    (cfg / 'Dolphin.ini').write_text(
        '[Core]\nGFXBackend = Software Renderer\nWiiSDCard = True\nWiiSDCardEnableFolderSync = True\n'
        '[General]\nWiiSDCardSyncFolder = %s\n[DSP]\nBackend = No Audio Output\n'
        '[Interface]\nConfirmStop = False\n[Analytics]\nPermissionAsked = True\nEnabled = False\n' % sd)
    try:
        os.remove(udir / 'Wii' / 'sd.raw')
    except OSError:
        pass


def main():
    args = sys.argv[1:]
    opts = {'dolphin': os.environ.get('DOLPHIN', 'dolphin-emu-nogui'),
            'user': str(pathlib.Path.home() / '.wiisp-dolphin'),
            'elf': str(ROOT / 'wiisp.elf'), 'tests': None, 'frames': '1200', 'secs': '900'}
    run_mode = soft = verbose = False
    names = []
    while args:
        a = args.pop(0)
        if a in ('--dolphin', '--user', '--elf', '--tests', '--frames', '--secs'):
            opts[a[2:]] = args.pop(0)
        elif a == '--run':
            run_mode = True
        elif a == '--soft':
            soft = True
        elif a == '-v':
            verbose = True
        elif a.startswith('@'):
            names += [l.strip() for l in open(a[1:]) if l.strip() and not l.startswith('#')]
        else:
            names.append(a)
    if not names or (not run_mode and not opts['tests']):
        print(__doc__)
        sys.exit(2)

    udir = pathlib.Path(opts['user']).expanduser()
    sd = udir / 'sdcard'
    shutil.rmtree(sd, ignore_errors=True)
    at = sd / 'wiisp' / 'autotest'
    at.mkdir(parents=True)
    entries = []
    for i, n in enumerate(names):
        if run_mode:
            src = pathlib.Path(n)
            rel = 'p%d/%s' % (i, src.name)
            shutil.copytree(src.parent, at / ('p%d' % i))
        else:
            src = pathlib.Path(opts['tests']) / 'tests' / (n + '.prx')
            rel = n + '.prx'
            if not (at / rel).parent.exists():
                shutil.copytree(src.parent, (at / rel).parent,
                                ignore=shutil.ignore_patterns('*.expected*', '*.c', '*.cpp', 'Makefile'))
        entries.append(rel)
    with open(sd / 'wiisp' / 'autotest.txt', 'w') as f:
        f.write('frames %s\n%s\n' % (opts['frames'], 'soft' if soft else 'gx'))
        for rel in entries:
            f.write(rel + '\n')
    write_config(udir, sd)

    t0 = time.time()
    p = subprocess.Popen([opts['dolphin'], '-p', 'x11', '-u', str(udir), '-e', opts['elf']],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        p.wait(timeout=int(opts['secs']))
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()
        print('Dolphin no terminó a tiempo')
    print('Dolphin: %.0f s' % (time.time() - t0))

    ok = approx = 0
    for n, rel in zip(names, entries):
        base = at / rel
        if run_mode:
            print('== ' + n)
            try:
                print((base.parent / (base.name + '.stats')).read_text().rstrip())
            except OSError:
                print('  sin estadísticas')
            continue
        try:
            text = (base.parent / (base.name + '.out')).read_text(errors='replace')
        except OSError:
            text = None
        root = pathlib.Path(opts['tests']) / 'tests'
        exp = (root / (n + '.expected')).read_text(errors='replace')
        ref_bmp = root / (n + '.expected.bmp')
        bmp = base.parent / (base.name + '.bmp')
        text_ok = text is not None and normalize(text) == normalize(exp)
        diff = bmp_diff(ref_bmp, bmp) if ref_bmp.exists() else None
        good = text_ok and (not ref_bmp.exists() or diff == 0.0)
        close = False
        if not good and text is not None and lines_close(normalize(exp), normalize(text), 6):
            d6 = bmp_diff(ref_bmp, bmp, 6) if ref_bmp.exists() else 0.0
            close = d6 is not None and d6 < 0.5
        ok += good
        approx += good or close
        img = '' if not ref_bmp.exists() else '  [imagen: %s]' % ('sin captura' if diff is None else '%.2f%% distinto' % diff)
        print(('PASA  ' if good else 'CASI  ' if close else 'FALLA ') + n + img + ('' if text is not None else '  (sin salida)'))
        if verbose and not good and text is not None:
            for l in list(difflib.unified_diff(normalize(exp), normalize(text), 'esperado', 'wii', lineterm='', n=1))[:30]:
                print('      ' + l)
    if not run_mode:
        print('%d de %d pasan; %d con tolerancia de 6' % (ok, len(names), approx))


main()
