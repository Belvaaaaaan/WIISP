#!/bin/bash
# El CLI dibujando por el renderizador de GX (gx_ge.c compilado con devkitPPC)
# sobre la libogc con estado de stub_estado.c: al salir escribe la huella del
# estado de GX en cada dibujo. qemu-ppc $SALIDA/wiisp-cli-gx --run ... programa
set -e
T=$(cd "$(dirname "$0")" && pwd)
ENTORNO=${ENTORNO:-$T/../../../wiisp-entorno}
. "$ENTORNO/env.sh"
G=${SALIDA:-/tmp/banco}
mkdir -p "$G"
cd "$T/../.."
DKF="-g -O2 -fwrapv -std=gnu11 -DGEKKO -mrvl -mcpu=750 -meabi -mhard-float -msdata=none -G0 -maix-struct-return -fno-stack-protector -DWIISP_PROF -Isrc -I$DEVKITPRO/libogc/include"
$DEVKITPPC/bin/powerpc-eabi-gcc $DKF -DWIISP_FIFO_TRAZA -c src/wii/gx_ge.c -o $G/gx_ge_cli_dkp.o
powerpc-linux-gnu-objcopy --remove-section .gnu.attributes $G/gx_ge_cli_dkp.o
SRC=$(ls src/core/*.c src/loader/*.c src/cpu/*.c src/hle/*.c src/gpu/*.c) 
# GE_VERTEX_DKP=1: también ge_vertex.c con devkitPPC (psq_l y paired singles,
# emulados por sigill_ppc.c: correr con qemu-ppc -cpu 750 o QEMU_CPU=750)
if [ -n "$GE_VERTEX_DKP" ]; then
	$DEVKITPPC/bin/powerpc-eabi-gcc -O2 -fwrapv -std=gnu11 -mcpu=750 -mhard-float -msdata=none -G0 -maix-struct-return -fno-stack-protector -DGEKKO -Isrc -c src/gpu/ge_vertex.c -o $G/ge_vertex_cli_dkp.o
	powerpc-linux-gnu-objcopy --remove-section .gnu.attributes $G/ge_vertex_cli_dkp.o
	SRC="$(echo $SRC | tr ' ' '\n' | grep -v '^src/gpu/ge_vertex.c$') $G/ge_vertex_cli_dkp.o"
fi
powerpc-linux-gnu-gcc -O2 -std=gnu11 -fwrapv -Isrc -static -pthread -o $G/wiisp-cli-gx $SRC src/frontend/app.c src/frontend/filelist.c src/frontend/guion.c src/host/main.c $G/gx_ge_cli_dkp.o $T/stub_estado.c $T/sigill_ppc.c -lm 2>&1 | grep -v "warning\|NOTE" || true
