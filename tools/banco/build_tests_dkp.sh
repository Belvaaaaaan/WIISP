#!/bin/bash
# Pruebas del PC con ge_vertex.c, test_gxemit.c y los intérpretes (interp.c,
# predec.c) compilados con devkitPPC (el compilador de la Wii)
T=$(cd "$(dirname "$0")" && pwd)
ENTORNO=${ENTORNO:-$T/../../../wiisp-entorno}
. "$ENTORNO/env.sh"
D=${SALIDA:-/tmp/banco}
mkdir -p "$D"
cd "$T/../.."
DKF="-O2 -fwrapv -std=gnu11 -mcpu=750 -mhard-float -msdata=none -G0 -maix-struct-return -fno-stack-protector -DGEKKO -Isrc -Itests"
for f in src/gpu/ge_vertex.c tests/test_gxemit.c src/cpu/interp.c src/cpu/predec.c; do
	o=$D/$(basename $f .c)_dkp.o
	$DEVKITPPC/bin/powerpc-eabi-gcc $DKF -c $f -o $o 2>&1 | grep error
	powerpc-linux-gnu-objcopy --remove-section .gnu.attributes $o
done
SRC=$(ls src/core/*.c src/loader/*.c src/cpu/*.c src/hle/*.c src/gpu/*.c src/frontend/filelist.c src/frontend/guion.c src/frontend/wiimote.c tests/*.c | grep -v "src/gpu/ge_vertex.c\|tests/test_gxemit.c\|src/cpu/interp.c\|src/cpu/predec.c")
powerpc-linux-gnu-gcc -O2 -std=gnu11 -fwrapv -Isrc -static -o $D/tests-dkp $SRC $D/ge_vertex_dkp.o $D/test_gxemit_dkp.o $D/interp_dkp.o $D/predec_dkp.o $T/sigill_ppc.c -lm 2>&1 | grep -v "warning" | head -5
