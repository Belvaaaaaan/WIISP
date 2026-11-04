#!/usr/bin/env python3
"""
WIISP - tools/banco/Census_comparación.py
Compara dibujo a dibujo dos registros de test_brightnesscensus (.dibujos.bin),
por ejemplo GX contra el renderizador por software (Exactodesde):

    python3 Census_comparación.py A.dibujos.bin B.dibujos.bin [--top N] [--umbral U] [--mapas N]

Cada dibujo de los cuadros registrados deja el estado del GE y una miniatura
del búfer de destino (color medio de cada bloque de 16x16). Para cada dibujo
se calcula cuánto difieren A y B en su búfer antes y después del dibujo; los
que más hacen crecer la diferencia son los sospechosos, con su estado, las
coordenadas de textura que llegaron al rasterizador y un mapa de los bloques.

SPDX-License-Identifier: GPL-2.0-or-later
"""
import sys, struct, collections

args = sys.argv[1:]
def opcion(nombre, defecto):
    if nombre in args:
        i = args.index(nombre); v = args[i + 1]; del args[i:i + 2]; return type(defecto)(v)
    return defecto
TOP = opcion('--top', 25)
UMBRAL = opcion('--umbral', 1.0)
MAPAS = opcion('--mapas', 5)
if len(args) != 2:
    print(__doc__); sys.exit(2)

FMT = ['565', '5551', '4444', '8888']
TFMT = ['565', '5551', '4444', '8888', 'CLUT4', 'CLUT8', 'CLUT16', 'CLUT32', 'DXT1', 'DXT3', 'DXT5']
PRIM = ['puntos', 'lineas', 'tira de lineas', 'triangulos', 'tira', 'abanico', 'sprites', 'continuar', 'bezier', 'spline']


def leer(ruta):
    d = open(ruta, 'rb').read()
    if d[:8] != b'WIISPBC1': raise SystemExit(f'{ruta}: no es un registro de test_brightnesscensus')
    version, bloque, ncampos, gx = struct.unpack_from('<4I', d, 8)
    p = 24
    regs = []
    while p + 12 <= len(d):
        magia, bwh, cambios = struct.unpack_from('<3I', d, p); p += 12
        if magia != 0x44494255: raise SystemExit(f'{ruta}: registro roto en el byte {p - 12}')
        campos = struct.unpack_from(f'<{ncampos}I', d, p); p += 4 * ncampos
        rango = struct.unpack_from('<10f', d, p); p += 40
        bloques = []
        for _ in range(cambios):
            b, = struct.unpack_from('<H', d, p)
            bloques.append((b, d[p + 2:p + 6])); p += 6
        regs.append(dict(bw=bwh & 0xFFFF, bh=bwh >> 16, campos=campos, rango=rango, bloques=bloques))
    return dict(gx=gx, regs=regs)


def clave(r):
    c = r['campos']
    return (c[3], c[4], c[5], r['bw'], r['bh'])


def identidad(r):
    c = r['campos']
    return (c[0], c[1], c[2], c[3], c[4], c[19], c[20], c[21])


def describir(r):
    c, g = r['campos'], r['rango']
    prim = PRIM[c[2]] if c[2] < len(PRIM) else str(c[2])
    t = (f"c{c[0]} #{c[1]} {prim}: fb {c[3]:08X} {FMT[c[4] & 3]} ancho {c[5]} ({c[6]}x{c[7]})"
         f"{' borrado %03X' % (c[8] & 0x701) if c[8] & 1 else ''}, mascara RGB {c[9]:06X} alfa {c[10]:02X}")
    if c[11] & 1: t += f', logica {c[11] >> 4}'
    if c[12] & 1: t += f', mezcla {(c[12] >> 4) & 15}/{(c[12] >> 8) & 15}/{(c[12] >> 12) & 7} {c[13]:06X}/{c[14]:06X}'
    if c[15] & 1: t += f', prueba de alfa {(c[15] >> 4) & 7} ref {(c[15] >> 12) & 255:02X}'
    if c[16] & 1: t += f', stencil {(c[16] >> 4) & 7}/{(c[16] >> 12) & 255:02X} op {c[17]:06X}'
    t += f", z {'si' if c[18] & 1 else 'no'}{' tramado' if c[18] & 32 else ''}"
    if c[19]:
        tf = c[21] & 15
        t += (f"; textura {c[20]:08X} {TFMT[tf] if tf < len(TFMT) else tf} {1 << (c[22] & 15)}x{1 << ((c[22] >> 8) & 15)}"
              f" ancho {(c[21] >> 8) & 0x7FF}{' swizzle' if c[21] >> 20 & 1 else ''}, funcion {c[23]:06X}, mapeo {c[24]:06X},"
              f" wrap {c[25] & 0xFFFF:04X} filtro {c[25] >> 16:04X}")
        if 4 <= tf <= 7: t += f', paleta {c[26]:06X} en {c[27]:08X}'
    t += f"; vertices {c[28]:06X}, material {c[29]:08X}{', luces' if c[30] & 1 else ''}{', niebla' if c[30] & 2 else ''}"
    if c[31]:
        t += (f"; {c[31]} primitivas, s {g[0]:.3g}..{g[5]:.3g} t {g[1]:.3g}..{g[6]:.3g} q {g[2]:.3g}..{g[7]:.3g},"
              f" pantalla x {g[3] / 16:.0f}..{g[8] / 16:.0f} y {g[4] / 16:.0f}..{g[9] / 16:.0f}")
    else:
        t += '; ninguna primitiva llego al rasterizador'
    return t


def dif(a, b):
    if not a: return 0.0, 0.0
    s = sa = 0
    for x, y in zip(a, b):
        s += max(abs(x[0] - y[0]), abs(x[1] - y[1]), abs(x[2] - y[2]))
        sa += abs(x[3] - y[3])
    return s / len(a), sa / len(a)


def mapa(a, b, bw, bh):
    filas = []
    for y in range(bh):
        f = ''
        for x in range(bw):
            i = y * bw + x
            m = max(abs(a[i][k] - b[i][k]) for k in range(3))
            f += ' .:-=+*#%@'[min(9, m * 10 // 64)] if m else ' '
        filas.append(f)
    return filas


A, B = leer(args[0]), leer(args[1])
print(f"A: {args[0]} ({'GX' if A['gx'] else 'software'}, {len(A['regs'])} dibujos)")
print(f"B: {args[1]} ({'GX' if B['gx'] else 'software'}, {len(B['regs'])} dibujos)")
estado = [collections.defaultdict(lambda: None), collections.defaultdict(lambda: None)]
filas = []
desalineados = 0
for ra, rb in zip(A['regs'], B['regs']):
    if identidad(ra) != identidad(rb):
        desalineados += 1
        if desalineados <= 5: print('DESALINEADO:\n  A ' + describir(ra) + '\n  B ' + describir(rb))
        continue
    k = clave(ra)
    n = ra['bw'] * ra['bh']
    antes = []
    for e, r in ((0, ra), (1, rb)):
        if estado[e][k] is None: estado[e][k] = [bytes(4)] * n
        antes.append(list(estado[e][k]))
        for b, v in r['bloques']:
            if b < n: estado[e][k][b] = v
    d0, _ = dif(antes[0], antes[1])
    d1, da = dif(estado[0][k], estado[1][k])
    solo = sum(1 for i in range(n) if (antes[0][i] != estado[0][k][i]) != (antes[1][i] != estado[1][k][i]))
    peores = sum(1 for i in range(n) if max(abs(estado[0][k][i][c] - estado[1][k][i][c]) for c in range(3)) >= 32 and
                 max(abs(antes[0][i][c] - antes[1][i][c]) for c in range(3)) < 32)
    filas.append(dict(ra=ra, d0=d0, d1=d1, da=da, solo=solo, peores=peores, k=k, a=list(estado[0][k]), b=list(estado[1][k])))
if len(A['regs']) != len(B['regs']): print(f"(numero de dibujos distinto: A {len(A['regs'])}, B {len(B['regs'])})")
print(f'{len(filas)} dibujos comparados, {desalineados} desalineados')

print('\n== Diferencia al final de cada cuadro, por bufer (media del peor canal por bloque, 0-255)')
fin = {}
for f in filas: fin[(f['ra']['campos'][0], f['k'])] = f
for (cu, k), f in sorted(fin.items()):
    print(f"c{cu} fb {k[0]:08X} {FMT[k[1] & 3]} ancho {k[2]} ({k[3]}x{k[4]} bloques): {f['d1']:.1f} (alfa {f['da']:.1f})")

print(f'\n== Primer dibujo que hace crecer la diferencia mas de {UMBRAL} o empeora 4 bloques o mas')
primero = next((f for f in filas if f['d1'] - f['d0'] > UMBRAL or f['peores'] >= 4), None)
if primero:
    print(f"{describir(primero['ra'])}\n  diferencia {primero['d0']:.1f} -> {primero['d1']:.1f}, bloques que empeoran {primero['peores']}, "
          f"bloques que solo cambia uno de los dos {primero['solo']}")
else:
    print('ninguno')

print(f'\n== Los {TOP} dibujos que mas hacen crecer la diferencia')
orden = sorted(filas, key=lambda f: (f['d1'] - f['d0']) * f['ra']['bw'] * f['ra']['bh'], reverse=True)
for i, f in enumerate(orden[:TOP]):
    print(f"{i + 1}. {describir(f['ra'])}\n   diferencia {f['d0']:.1f} -> {f['d1']:.1f} (alfa {f['da']:.1f}), bloques que empeoran {f['peores']}, "
          f"bloques que solo cambia uno de los dos {f['solo']}")

print(f'\n== Mapas de los {MAPAS} primeros (cuanto difieren A y B en cada bloque despues del dibujo: " " igual ... "@" mucho)')
for f in orden[:MAPAS]:
    r = f['ra']
    print(f"c{r['campos'][0]} #{r['campos'][1]} fb {r['campos'][3]:08X}:")
    for l in mapa(f['a'], f['b'], r['bw'], r['bh']): print('   |' + l + '|')
