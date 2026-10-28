#!/bin/bash
# Las microprobas del JIT (src/wii/jit_banco.c, prueba_jit) en PowerPC bajo
# qemu: todo menos lo que solo existe en el Wii (PMC, DSI, BAT). Los ciclos
# no valen (base de tiempo de qemu); sirve para ver que el código generado
# corre y da lo que tiene que dar ("fin: todo bien"). Deja $SALIDA/jit_banco.log
set -e
T=$(cd "$(dirname "$0")" && pwd)
G=${SALIDA:-/tmp/banco}
mkdir -p "$G"
cd "$T/../.."
SRC=$(ls src/core/*.c src/loader/*.c src/cpu/*.c src/hle/*.c src/gpu/*.c)
powerpc-linux-gnu-gcc -O2 -std=gnu11 -fwrapv -Wall -Wextra -Wno-unused-parameter -Isrc -static -pthread -o $G/jit_banco \
	$T/jit_banco_pc.c src/wii/jit_banco.c $SRC src/frontend/filelist.c src/frontend/guion.c src/frontend/wiimote.c -lm
qemu-ppc -cpu 750 $G/jit_banco $G/jit_banco.log
grep "PRUEBA_JIT" $G/jit_banco.log
