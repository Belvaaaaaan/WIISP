#!/bin/bash
# Construye el banco de GX: gx_ge.c (y opcionalmente ge_vertex.c) con devkitPPC, el resto con gcc de Linux
set -e
T=$(cd "$(dirname "$0")" && pwd)
ENTORNO=${ENTORNO:-$T/../../../wiisp-entorno}
. "$ENTORNO/env.sh"
G=${SALIDA:-/tmp/banco}
mkdir -p "$G"
cd "$T/../.."
DKF="-g -O2 -fwrapv -std=gnu11 -DGEKKO -mrvl -mcpu=750 -meabi -mhard-float -msdata=none -G0 -maix-struct-return -fno-stack-protector -DWIISP_PROF -Isrc -I$DEVKITPRO/libogc/include"
for f in src/wii/gx_ge.c src/gpu/ge_vertex.c src/gpu/ge_raster.c $T/bench_cmd.c; do
	o=$G/$(basename $f .c)_dkp.o
	$DEVKITPPC/bin/powerpc-eabi-gcc $DKF -c $f -o $o
	powerpc-linux-gnu-objcopy --remove-section .gnu.attributes $o
done
SRC=$(ls src/core/*.c src/loader/*.c src/cpu/*.c src/hle/*.c src/gpu/*.c src/frontend/filelist.c src/frontend/guion.c src/frontend/wiimote.c | grep -v "src/gpu/ge_vertex.c")
powerpc-linux-gnu-gcc -O2 -std=gnu11 -fwrapv -DWIISP_PROF -Isrc -static -o $G/bench $SRC $G/gx_ge_dkp.o $G/ge_vertex_dkp.o $T/stub.c $T/sigill_ppc.c $T/bench_gx.c -lm 2>&1 | grep -v "warning\|NOTE" || true
# El banco de listas: el bucle de comandos (ge.c, incluido en bench_cmd.c) y el rasterizador también con devkitPPC
SRC2=$(echo "$SRC" | tr ' ' '\n' | grep -v "src/gpu/ge_raster.c\|src/gpu/ge.c$")
powerpc-linux-gnu-gcc -O2 -std=gnu11 -fwrapv -DWIISP_PROF -Isrc -static -o $G/bench_lista $SRC2 $G/gx_ge_dkp.o $G/ge_vertex_dkp.o $G/ge_raster_dkp.o $G/bench_cmd_dkp.o $T/stub.c $T/sigill_ppc.c $T/bench_lista.c -lm 2>&1 | grep -v "warning\|NOTE" || true
