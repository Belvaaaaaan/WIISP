#!/usr/bin/env python3
"""Mutaciones del camino de GX (gx_ge.c y lo que va con él) contra el banco de
las charolas en qemu: por cada mutación se recompila solo su archivo (con
devkitPPC si es gx_ge.c, ge_vertex.c, ge_raster.c o ge.c; con el gcc de
PowerPC si no), se enlaza bench_charolas y se corre con varias semillas y
frecuencias de cambio. La referencia es el banco sin mutar con todos los
interruptores de las charolas apagados (el camino de siempre); la mutación
corre con los de por defecto. Cuenta como detectada si alguna salida (con la
huella del FIFO, no el número de GX_Begin) es distinta, si se cuelga o si no
termina bien.

Uso: mutar_gx.py MUTACIONES [--semillas N] [--hilos N] [--solo-estas a,b]
Mismo formato de MUTACIONES que mutar.py. Salidas en $SALIDA/mutar_gx."""
import concurrent.futures, glob, os, re, subprocess, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from mutar import leer_mutaciones

RAIZ = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
T = os.path.join(RAIZ, 'tools', 'banco')
ENTORNO = os.environ.get('ENTORNO', os.path.join(RAIZ, '..', 'wiisp-entorno'))
DKP = os.path.join(ENTORNO, 'devkitpro')
DKCC = os.path.join(DKP, 'devkitPPC', 'bin', 'powerpc-eabi-gcc')
DKF = ['-g', '-O2', '-fwrapv', '-std=gnu11', '-DGEKKO', '-mrvl', '-mcpu=750', '-meabi', '-mhard-float', '-msdata=none', '-G0',
       '-maix-struct-return', '-fno-stack-protector', '-DWIISP_PROF', '-I' + os.path.join(DKP, 'libogc', 'include')]
PPCF = ['-O2', '-std=gnu11', '-fwrapv', '-DWIISP_PROF', '-w']
# archivo -> (objeto, compila con devkitPPC, fuente que se compila, banderas de más)
DKP_OBJ = {
    'src/wii/gx_ge.c': ('gx_ge_traza.o', 'src/wii/gx_ge.c', ['-DWIISP_FIFO_TRAZA']),
    'src/gpu/ge_vertex.c': ('ge_vertex.o', 'src/gpu/ge_vertex.c', []),
    'src/gpu/ge_raster.c': ('ge_raster.o', 'src/gpu/ge_raster.c', []),
    'src/gpu/ge.c': ('bench_cmd.o', 'tools/banco/bench_cmd.c', []),
}
CAMBIOS = (3, 5, 10, 40)
APAGADO = ['ge_charolas=0', 'ge_charolas_envio=0', 'ge_charolas_ge=0', 'ge_charolas_tiras=0', 'ge_charolas_sprites=0', 'ge_clut_igual=0']

def ppc_fuentes():
    pat = ['src/core/*.c', 'src/loader/*.c', 'src/cpu/*.c', 'src/hle/*.c', 'src/gpu/*.c']
    fs = [f for p in pat for f in sorted(glob.glob(p, root_dir=RAIZ))]
    fs += ['src/frontend/filelist.c', 'src/frontend/guion.c', 'src/frontend/wiimote.c']
    return [f for f in fs if f not in ('src/gpu/ge_vertex.c', 'src/gpu/ge_raster.c', 'src/gpu/ge.c')]

def run(cmd, cwd=RAIZ, timeout=None):
    return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout, errors='replace')

def compila_dkp(fuente, obj, extra, incl):
    r = run([DKCC] + DKF + extra + ['-I' + i for i in incl] + ['-c', fuente, '-o', obj])
    if r.returncode: return False, r.stderr
    run(['powerpc-linux-gnu-objcopy', '--remove-section', '.gnu.attributes', obj])
    return True, ''

def compila_ppc(fuente, obj, incl):
    r = run(['powerpc-linux-gnu-gcc'] + PPCF + ['-I' + i for i in incl] + ['-c', fuente, '-o', obj])
    return r.returncode == 0, r.stderr

def obj_ppc(d, f): return os.path.join(d, f.replace('/', '__')[:-2] + '.o')

def enlaza(d, objs, binario):
    extra = [os.path.join(T, x) for x in ('stub_estado.c', 'sigill_ppc.c', 'bench_charolas.c')]
    r = run(['powerpc-linux-gnu-gcc'] + PPCF + ['-Isrc', '-static', '-o', binario] + objs + extra + ['-lm'])
    return r.returncode == 0, r.stderr

def salida(binario, semilla, cambios, opts, limite):
    env = dict(os.environ, CAMBIOS=str(cambios))
    try:
        r = subprocess.run(['qemu-ppc', binario, str(semilla), '300'] + opts, capture_output=True, text=True, env=env,
                           timeout=limite, errors='replace')
    except subprocess.TimeoutExpired:
        return None
    if r.returncode: return 'termina con %d' % r.returncode
    out = re.sub(r'^\[GX\] [^;]*; ', '[GX] ', r.stdout, flags=re.M)
    return re.sub(r', [0-9]+ paletas;', ';', out)   # lo que ahorra ge_clut_igual

def main():
    args = sys.argv[1:]
    if not args: sys.exit(__doc__)
    def opcion(n, d=None):
        if n in args:
            i = args.index(n); v = args[i + 1]; del args[i:i + 2]; return v
        return d
    semillas = int(opcion('--semillas', '12')); hilos = int(opcion('--hilos', str(os.cpu_count() or 4)))
    estas = opcion('--solo-estas')
    muts = leer_mutaciones(args[0])
    if estas: muts = [m for m in muts if m['nombre'] in estas.split(',')]
    salida_dir = os.path.join(os.environ.get('SALIDA', '/tmp/banco'), 'mutar_gx')
    base = os.path.join(salida_dir, 'base'); os.makedirs(base, exist_ok=True)
    t0 = time.time()
    # Objetos de la base
    objs = {}
    for f, (o, src, extra) in DKP_OBJ.items():
        ok, err = compila_dkp(src, os.path.join(base, o), extra, ['src'])
        if not ok: sys.exit('no compila %s:\n%s' % (src, err))
        objs[f] = os.path.join(base, o)
    fs = ppc_fuentes()
    with concurrent.futures.ThreadPoolExecutor(hilos) as ex:
        res = list(ex.map(lambda f: compila_ppc(f, obj_ppc(base, f), ['src']), fs))
    for f, (ok, err) in zip(fs, res):
        if not ok: sys.exit('no compila %s:\n%s' % (f, err))
        objs[f] = obj_ppc(base, f)
    binario = os.path.join(base, 'bench_charolas')
    ok, err = enlaza(base, list(objs.values()), binario)
    if not ok: sys.exit('no enlaza la base:\n' + err)
    casos = [(s, c) for c in CAMBIOS for s in range(1, semillas + 1)]
    t1 = time.time()
    with concurrent.futures.ThreadPoolExecutor(hilos) as ex:
        ref = list(ex.map(lambda sc: salida(binario, sc[0], sc[1], APAGADO, 600), casos))
        con = list(ex.map(lambda sc: salida(binario, sc[0], sc[1], [], 600), casos))
    tb = (time.time() - t1) / 2
    if any(r is None or r.startswith('termina') for r in ref) or ref != con:
        sys.exit('la base no da lo mismo con y sin las charolas')
    limite = max(20, 10 * tb / max(1, len(casos)) * hilos)
    print(f'base: {time.time() - t0:.0f} s; {len(casos)} casos ({tb:.0f} s); {len(muts)} mutaciones con {hilos} hilos')

    def una(k, m):
        src = open(os.path.join(RAIZ, m['archivo']), encoding='utf-8').read()
        n = src.count(m['antes'])
        if n != 1: return 'no aplica (%d veces)' % n, ''
        d = os.path.join(salida_dir, str(k))
        msrc = os.path.join(d, m['archivo'])
        os.makedirs(os.path.dirname(msrc), exist_ok=True)
        open(msrc, 'w', encoding='utf-8').write(src.replace(m['antes'], m['despues']))
        incl = [os.path.join(d, 'src'), 'src']
        mobjs = dict(objs)
        if m['archivo'] in DKP_OBJ:
            o, s2, extra = DKP_OBJ[m['archivo']]
            s2 = msrc if s2 == m['archivo'] else s2
            ok, err = compila_dkp(s2, os.path.join(d, o), extra + ['-iquote', os.path.dirname(os.path.join(RAIZ, m['archivo']))], incl)
            mobjs[m['archivo']] = os.path.join(d, o)
        elif m['archivo'] in objs:
            ok, err = compila_ppc(msrc, os.path.join(d, 'mutado.o'), incl + ['-iquote' + os.path.dirname(os.path.join(RAIZ, m['archivo']))])
            mobjs[m['archivo']] = os.path.join(d, 'mutado.o')
        else:
            return 'archivo sin objeto', m['archivo']
        if not ok: return 'no compila', (err.strip().splitlines() or [''])[0]
        b = os.path.join(d, 'bench_charolas')
        ok, err = enlaza(d, list(mobjs.values()), b)
        if not ok: return 'no enlaza', (err.strip().splitlines() or [''])[-1]
        for (s, c), r in zip(casos, ref):
            o = salida(b, s, c, [], limite)
            if o is None: return 'colgada', 'semilla %d, cambios %d' % (s, c)
            if o != r:
                return 'detectada', 'semilla %d, cambios %d' % (s, c) if not o.startswith('termina') else o
        return 'sobrevive', ''

    with concurrent.futures.ThreadPoolExecutor(hilos) as ex:
        res = list(ex.map(lambda km: una(*km), enumerate(muts)))
    cuenta = {}
    for m, (est, det) in zip(muts, res):
        cuenta[est] = cuenta.get(est, 0) + 1
        print(f'{est:22s} {m["nombre"]}' + (f'  ({det[:120]})' if det else ''))
    print('total: ' + ', '.join(f'{v} {k}' for k, v in sorted(cuenta.items())) + f'; {time.time() - t0:.0f} s')

if __name__ == '__main__':
    main()
