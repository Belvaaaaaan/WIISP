#!/usr/bin/env python3
"""Programa de PSP grande para medir la caché del intérprete (medir_cache.sh):
N funciones distintas con bucles, cargas, escrituras, ramas y llamadas,
llamadas en un orden desigual (unas mucho más que otras), como el código
de un juego. Escribe el C por la salida estándar.

cpu_grande.py N [SEMILLA]: unos 200 bytes de MIPS por función, así que
N = 64 son unos 13 KB de código, 256 unos 50 KB y 1024 unos 200 KB.
VUELTAS se da al compilar, como en cpu_banco.c."""
import sys, random

n = int(sys.argv[1])
rnd = random.Random(int(sys.argv[2]) if len(sys.argv) > 2 else 1)
out = []
w = out.append
w('/* Generado por tools/banco/cpu_grande.py %d: no editar */' % n)
w('#include "guest.h"')
w('static u32 d[1024], e[1024];')
w('typedef u32 (*fn_t)(u32);')
for i in range(n):
    w('static u32 f%d(u32 x);' % i)
for i in range(n):
    k1, k2, k3, k4 = (rnd.getrandbits(32) | 1 for _ in range(4))
    vueltas = rnd.randint(2, 4)
    sh = rnd.randint(3, 13)
    w('static u32 __attribute__((noinline)) f%d(u32 x){' % i)
    w('\tu32 s = x ^ 0x%08xu, j;' % k1)
    w('\tfor(j = 0; j < %d; j++){' % vueltas)
    w('\t\ts += d[(x + j * 0x%xu) & 1023] * 0x%08xu;' % (k2 & 0xFFFF, k3))
    w('\t\tif(s & 0x%x) s ^= s >> %d; else s += 0x%08xu;' % (1 << rnd.randint(0, 15), sh, k4))
    w('\t\te[s & 1023] = s;')
    w('\t}')
    if rnd.random() < 0.5:
        w('\ts = (s << %d) | (s >> %d);' % (sh, 32 - sh))
    if rnd.random() < 0.3:
        w('\tif((s & 0x%x) == 0) s += (u32)(int)(short)s * 0x%xu;' % (rnd.choice([3, 5, 6, 9, 12]), rnd.getrandbits(15)))
    # Llamadas solo hacia funciones con índice mayor (sin recursión)
    if i + 1 < n and rnd.random() < 0.6:
        hijo = rnd.randint(i + 1, min(n - 1, i + 1 + max(4, n // 8)))
        w('\tif((x & 7) == %d) s += f%d(s);' % (rnd.randint(0, 7), hijo))
    w('\treturn s;')
    w('}')
w('static const fn_t tabla[%d] = {' % n)
w(',\n'.join('\tf%d' % i for i in range(n)))
w('};')
w('''int main(void){
	u32 k, s = 1, r = 12345;
	for(k = 0; k < 1024; k++){ d[k] = k * 2654435761u; e[k] = 0; }
	for(k = 0; k < VUELTAS * 4096u; k++){
		u32 a, b;
		r = r * 1103515245u + 12345u; a = (r >> 8) %% %du;
		r = r * 1103515245u + 12345u; b = (r >> 8) %% %du;
		s += tabla[a < b ? a : b](s ^ k);   /* las de índice bajo, más a menudo */
	}
	printf_("grande: %%08x\\n", s);
	return 0;
}''' % (n, n))
print('\n'.join(out))
