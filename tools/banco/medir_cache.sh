#!/bin/bash
# Fallos de caché por instrucción de la PSP del intérprete, con cachegrind
# (valgrind) sobre el CLI del PC: cpu_grande.py genera programas de N
# funciones (el código que se usa a la vez crece con N) y se resta una
# corrida de 8 vueltas a una de 16, como en medir_cpu.sh.
#   medir_cache.sh "64 256 1024" --opt cpu_predec=0
# La caché simulada es la del Broadway con el doble de bytes (32 KB de 8
# vías y líneas de 32 B; L2 de 256 KB), porque en el PC punteros y tarjetas
# miden el doble: L1 de 64 KB y líneas de 64 B, L2 de 512 KB. Con ANOTAR=1
# además escribe, del tamaño más grande, las líneas de predec.c e interp.c
# que más fallan en la L1 de datos.
set -e
T=$(cd "$(dirname "$0")" && pwd)
G=${SALIDA:-/tmp/banco}/cache
mkdir -p "$G"
cd "$T/../.."
TAMS=${1:-"64 256 1024"}
shift || true
make -s -C tests/guest >/dev/null
CC="mipsel-linux-gnu-gcc -O1 -mabi=eabi -march=mips2 -mno-abicalls -fno-pic -G0 -msoft-float -mno-check-zero-division -ffreestanding -fno-builtin -nostdlib -T tests/guest/link.ld -Itests/guest"
CORE=$(ls src/core/*.c src/loader/*.c src/cpu/*.c src/hle/*.c src/gpu/*.c)
gcc -O2 -g -std=gnu11 -fwrapv -Isrc -pthread -o $G/cli $CORE src/frontend/app.c \
	src/frontend/filelist.c src/frontend/guion.c src/host/main.c -lm
VG="valgrind --tool=cachegrind --cache-sim=yes --I1=65536,8,64 --D1=65536,8,64 --LL=524288,8,64"
printf '%6s %10s %8s %8s %8s %8s\n' N instr L1I L1D L2 Ir
for n in $TAMS; do
	python3 $T/cpu_grande.py $n > $G/grande$n.c
	for v in 16 8; do
		$CC -DVUELTAS=$v -o $G/g${n}_$v.elf tests/guest/crt0.S build-pc/guest/imports.S $G/grande$n.c 2>/dev/null
		$G/cli --run --quiet --frames 100000 --log $G/g${n}_$v.log "$@" $G/g${n}_$v.elf >/dev/null 2>&1
		$VG --cachegrind-out-file=$G/cg${n}_$v.out $G/cli --run --quiet --frames 100000 "$@" $G/g${n}_$v.elf >/dev/null 2>&1
	done
	python3 - $G $n <<'EOF'
import sys, re
g, n = sys.argv[1], sys.argv[2]
def leer(v):
    ins = int(re.search(r'^\[STATS\] (\d+) instrucciones', open(f'{g}/g{n}_{v}.log').read(), re.M).group(1))
    ev = sm = None
    for l in open(f'{g}/cg{n}_{v}.out'):
        if l.startswith('events:'): ev = l.split()[1:]
        if l.startswith('summary:'): sm = [int(x) for x in l.split()[1:]]
    return ins, dict(zip(ev, sm))
i1, a = leer(16); i0, b = leer(8)
d = {k: a[k] - b[k] for k in a}; ni = i1 - i0
l1d = d['D1mr'] + d['D1mw']; l2 = d['DLmr'] + d['DLmw'] + d['ILmr']
print(f"{n:>6} {ni:>10} {d['I1mr']/ni:8.4f} {l1d/ni:8.4f} {l2/ni:8.4f} {d['Ir']/ni:8.2f}")
EOF
done
if [ -n "$ANOTAR" ]; then
	n=${TAMS##* }
	python3 - $G/cg${n}_16.out <<'EOF2'
import sys, collections
f = fn = None; ev = None; tot = collections.Counter(); per = collections.Counter()
for l in open(sys.argv[1]):
    if l.startswith('events:'): ev = l.split()[1:]
    elif l.startswith('fl='): f = l[3:].strip()
    elif l.startswith('fn='): fn = l[3:].strip()
    elif l[:1].isdigit() and ev:
        x = l.split(); d = dict(zip(ev, map(int, x[1:])))
        m = d.get('D1mr', 0) + d.get('D1mw', 0)
        tot['D1'] += m; per[(f, int(x[0]), fn)] += m
print('lineas con mas fallos de L1D (%d en total):' % tot['D1'])
for (f, ln, fn), m in per.most_common(25):
    try: src = open(f).read().split('\n')[ln - 1].strip()[:70]
    except Exception: src = ''
    print('%5.1f%%  %s:%d  %s' % (100.0 * m / tot['D1'], f.split('/')[-1], ln, src))
EOF2
fi
