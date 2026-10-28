#!/usr/bin/env python3
"""Pruebas de mutaciones rápidas: los objetos de las pruebas del PC se compilan
una vez (-O2, sin sanitizers); por cada mutación solo se recompila el archivo
mutado, se enlaza y se corren las suites elegidas parando en el primer fallo,
varias mutaciones a la vez. Las que sobreviven se repasan con todas las
suites (y, con --asan o --ppc, también con ASan o en PowerPC bajo qemu), así
que el resultado final es el mismo que corriendo todo con cada una.

Uso: mutar.py MUTACIONES [--solo listas,bbox] [--hilos N] [--asan] [--ppc]
             [--solo-estas nombre,nombre] [--dkp]

--dkp: las mutaciones se prueban con lo del Wii (ge_vertex.c, ge.c,
ge_raster.c, test_gxemit.c, interp.c y predec.c con devkitPPC y -DGEKKO,
como build_tests_dkp.sh) en qemu-ppc -cpu 750 con sigill_ppc.c: para el
código que solo existe en el Wii (paired singles).

Formato de MUTACIONES (texto exacto: tiene que aparecer una sola vez):
    === nombre de la mutación
    @ src/gpu/ge.c
    <<<
    texto de antes (una o varias líneas)
    >>>
    texto de después
    ===
Las salidas van a $SALIDA/mutar (por defecto /tmp/banco/mutar)."""
import concurrent.futures, glob, os, re, shutil, subprocess, sys, time

RAIZ = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
FLAGS = ['-std=gnu11', '-fwrapv', '-w', '-Isrc']
VARIANTES = {
    'o2': dict(cc='cc', flags=['-O2', '-g'], ld=['-lm'], run=[]),
    'asan': dict(cc='cc', flags=['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer'],
                 ld=['-fsanitize=address,undefined', '-lm'], run=[]),
    'ppc': dict(cc='powerpc-linux-gnu-gcc', flags=['-O2'], ld=['-static', '-lm'], run=['qemu-ppc']),
    'dkp': dict(cc='powerpc-linux-gnu-gcc', flags=['-O2'], ld=['-static', '-lm'], run=['qemu-ppc', '-cpu', '750']),
}
DKP = ['src/gpu/ge_vertex.c', 'src/gpu/ge.c', 'src/gpu/ge_raster.c', 'tests/test_gxemit.c', 'src/cpu/interp.c', 'src/cpu/predec.c']
DKF = ['-O2', '-fwrapv', '-std=gnu11', '-mcpu=750', '-mhard-float', '-msdata=none', '-G0', '-maix-struct-return',
       '-fno-stack-protector', '-DGEKKO', '-w', '-Isrc', '-Itests']
SIGILL = 'tools/banco/sigill_ppc.c'

def devkitppc():
    d = os.environ.get('DEVKITPPC') or os.path.join(RAIZ, '..', 'wiisp-entorno', 'devkitpro', 'devkitPPC')
    return os.path.join(d, 'bin', 'powerpc-eabi-gcc')

def fuentes():
    pat = ['src/core/*.c', 'src/loader/*.c', 'src/cpu/*.c', 'src/hle/*.c', 'src/gpu/*.c', 'tests/*.c']
    fs = [f for p in pat for f in sorted(glob.glob(p, root_dir=RAIZ))]
    return fs + ['src/frontend/filelist.c', 'src/frontend/guion.c', 'src/frontend/wiimote.c']

def leer_mutaciones(path):
    muts, cur, modo = [], None, None
    for linea in open(path, encoding='utf-8'):
        l = linea.rstrip('\n')
        if l.startswith('=== '):
            cur = dict(nombre=l[4:].strip(), archivo=None, antes=[], despues=[]); muts.append(cur); modo = None
        elif l == '===': modo = None
        elif cur is None: continue
        elif l.startswith('@ ') and modo is None: cur['archivo'] = l[2:].strip()
        elif l == '<<<': modo = 'antes'
        elif l == '>>>': modo = 'despues'
        elif modo: cur[modo].append(l)
    for m in muts:
        m['antes'] = '\n'.join(m['antes']); m['despues'] = '\n'.join(m['despues'])
    return muts

def compilar(var, src, obj, extra=(), orig=None):
    v = VARIANTES[var]
    os.makedirs(os.path.dirname(obj), exist_ok=True)
    if var == 'dkp' and (orig or src) in DKP:
        r = subprocess.run([devkitppc()] + DKF + list(extra) + ['-c', src, '-o', obj], cwd=RAIZ, capture_output=True, text=True)
        if r.returncode == 0:
            subprocess.run(['powerpc-linux-gnu-objcopy', '--remove-section', '.gnu.attributes', obj], cwd=RAIZ)
        return r.returncode == 0, r.stderr
    r = subprocess.run([v['cc']] + v['flags'] + FLAGS + list(extra) + ['-c', src, '-o', obj], cwd=RAIZ,
                       capture_output=True, text=True)
    return r.returncode == 0, r.stderr

def objeto(dirobj, f): return os.path.join(dirobj, f.replace('/', '__')[:-2] + '.o')

def base(var, salida, hilos):
    dirobj = os.path.join(salida, 'obj_' + var)
    fs = fuentes()
    cabeceras = max(os.path.getmtime(os.path.join(RAIZ, h)) for h in glob.glob('src/**/*.h', root_dir=RAIZ, recursive=True) +
                    glob.glob('tests/*.h', root_dir=RAIZ))
    def uno(f):
        o = objeto(dirobj, f)
        if os.path.exists(o) and os.path.getmtime(o) >= max(os.path.getmtime(os.path.join(RAIZ, f)), cabeceras): return True, ''
        return compilar(var, f, o)
    if var == 'dkp': fs = fs + [SIGILL]
    with concurrent.futures.ThreadPoolExecutor(hilos) as ex:
        for f, (ok, err) in zip(fs, ex.map(uno, fs)):
            if not ok: sys.exit(f'no compila {f} ({var}):\n{err}')
    return dirobj

def enlazar(var, dirobj, salida_bin, cambio=None):
    v = VARIANTES[var]
    objs = [objeto(dirobj, f) for f in fuentes() + ([SIGILL] if var == 'dkp' else [])]
    if cambio:
        cambios = cambio if isinstance(cambio, dict) else {cambio[0]: cambio[1]}
        objs = [next((n for f, n in cambios.items() if o == objeto(dirobj, f)), o) for o in objs]
    r = subprocess.run([v['cc']] + v['flags'] + ['-o', salida_bin] + objs + v['ld'], cwd=RAIZ, capture_output=True, text=True)
    return r.returncode == 0, r.stderr

def correr(var, binario, solo, limite):
    v = VARIANTES[var]
    cmd = v['run'] + [binario, '--parar'] + (['--solo', solo] if solo else [])
    cwd = os.path.join(os.path.dirname(binario), 'cwd_' + os.path.basename(binario))
    os.makedirs(os.path.join(cwd, 'build-pc'), exist_ok=True)
    t = time.time()
    try:
        r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=limite, errors='replace')
    except subprocess.TimeoutExpired:
        return 'colgada', time.time() - t, ''
    fallo = next((l.strip() for l in r.stdout.splitlines() if 'FALLO' in l), '')
    if r.returncode == 0: return 'sobrevive', time.time() - t, ''
    if not fallo: fallo = (r.stderr.strip().splitlines() or ['termina con %d' % r.returncode])[-1]
    return 'detectada', time.time() - t, fallo

def mutar(var, dirobj, salida, k, m, solo, limite):
    src = open(os.path.join(RAIZ, m['archivo']), encoding='utf-8').read()
    n = src.count(m['antes'])
    if n != 1: return 'no aplica (%d veces)' % n, 0, ''
    d = os.path.join(salida, 'mut', var, str(k))
    msrc = os.path.join(d, m['archivo'])
    os.makedirs(os.path.dirname(msrc), exist_ok=True)
    open(msrc, 'w', encoding='utf-8').write(src.replace(m['antes'], m['despues']))
    if m['archivo'].endswith('.h'):
        # Una cabecera: se recompilan los .c que la incluyen, con la copia mutada delante
        nombre = os.path.basename(m['archivo'])
        usan = [f for f in fuentes() if re.search(r'#include\s+"([^"]*/)?' + re.escape(nombre) + '"',
                                                    open(os.path.join(RAIZ, f), encoding='utf-8', errors='replace').read())]
        if not usan: return 'sin quien la incluya', 0, ''
        cambios = {}
        for i, f in enumerate(usan):
            o = os.path.join(d, 'mutado%d.o' % i)
            ok, err = compilar(var, f, o, ['-iquote', os.path.join(d, 'src'), '-iquote', os.path.dirname(msrc)], orig=f)
            if not ok: return 'no compila', 0, err.strip().splitlines()[0] if err.strip() else ''
            cambios[f] = o
        binario = os.path.join(d, 'tests')
        ok, err = enlazar(var, dirobj, binario, cambios)
        if not ok: return 'no enlaza', 0, err.strip().splitlines()[-1] if err.strip() else ''
        return correr(var, binario, solo, limite)
    mobj = os.path.join(d, 'mutado.o')
    ok, err = compilar(var, msrc, mobj, ['-iquote', os.path.dirname(os.path.join(RAIZ, m['archivo']))], orig=m['archivo'])
    if not ok: return 'no compila', 0, err.strip().splitlines()[0] if err.strip() else ''
    binario = os.path.join(d, 'tests')
    ok, err = enlazar(var, dirobj, binario, (m['archivo'], mobj))
    if not ok: return 'no enlaza', 0, err.strip().splitlines()[-1] if err.strip() else ''
    return correr(var, binario, solo, limite)

def main():
    args = sys.argv[1:]
    if not args: sys.exit(__doc__)
    def opcion(nombre, defecto=None):
        if nombre in args:
            i = args.index(nombre); v = args[i + 1]; del args[i:i + 2]; return v
        return defecto
    def bandera(nombre):
        if nombre in args: args.remove(nombre); return True
        return False
    solo = opcion('--solo'); hilos = int(opcion('--hilos', str(os.cpu_count() or 4)))
    estas = opcion('--solo-estas'); asan = bandera('--asan'); ppc = bandera('--ppc')
    prim = 'dkp' if bandera('--dkp') else 'o2'
    muts = leer_mutaciones(args[0])
    if estas: muts = [m for m in muts if m['nombre'] in estas.split('|' if '|' in estas else ',')]
    salida = os.path.join(os.environ.get('SALIDA', '/tmp/banco'), 'mutar')
    t0 = time.time()
    dirobj = base(prim, salida, hilos)
    ok, err = enlazar(prim, dirobj, os.path.join(salida, 'tests_' + prim))
    if not ok: sys.exit('no enlaza la base:\n' + err)
    est, tb, fallo = correr(prim, os.path.join(salida, 'tests_' + prim), solo, 3600)
    if est != 'sobrevive': sys.exit('la base no pasa sus pruebas: ' + fallo)
    limite = max(30, 10 * tb)
    print(f'base: {time.time() - t0:.0f} s (las suites elegidas, {tb:.1f} s); {len(muts)} mutaciones con {hilos} hilos')
    res = [None] * len(muts)
    with concurrent.futures.ThreadPoolExecutor(hilos) as ex:
        futs = {ex.submit(mutar, prim, dirobj, salida, k, m, solo, limite): k for k, m in enumerate(muts)}
        for f in concurrent.futures.as_completed(futs):
            res[futs[f]] = f.result()
    # Las que sobreviven, con todas las suites (y con ASan o en PowerPC si se pide)
    repasos = [(prim, None)] if solo else []
    if asan: repasos.append(('asan', None))
    if ppc: repasos.append(('ppc', None))
    for var, s in repasos:
        vivas = [k for k, r in enumerate(res) if r[0] == 'sobrevive']
        if not vivas: break
        d = base(var, salida, hilos) if var != prim else dirobj
        ok, err = enlazar(var, d, os.path.join(salida, 'tests_' + var))
        if not ok: sys.exit(f'no enlaza la base ({var}):\n' + err)
        est, tb2, fallo = correr(var, os.path.join(salida, 'tests_' + var), None, 7200)
        if est != 'sobrevive': sys.exit(f'la base no pasa con todas las suites ({var}): ' + fallo)
        lim2 = max(60, 3 * tb2 * hilos)
        print(f'repaso de {len(vivas)} con todas las suites ({var})')
        with concurrent.futures.ThreadPoolExecutor(hilos) as ex:
            futs = {ex.submit(mutar, var, d, salida, k, muts[k], s, lim2): k for k in vivas}
            for f in concurrent.futures.as_completed(futs):
                r = f.result()
                if r[0] != 'sobrevive': res[futs[f]] = (r[0] + ' (' + var + ', todas)', r[1], r[2])
    cuenta = {}
    for m, (est, t, fallo) in zip(muts, res):
        clave = est.split(' (')[0]
        cuenta[clave] = cuenta.get(clave, 0) + 1
        print(f'{est:28s} {t:6.1f} s  {m["nombre"]}' + (f'\n{"":38s}{fallo[:150]}' if fallo else ''))
    print('total: ' + ', '.join(f'{v} {k}' for k, v in sorted(cuenta.items())) + f'; {time.time() - t0:.0f} s')

if __name__ == '__main__':
    main()
