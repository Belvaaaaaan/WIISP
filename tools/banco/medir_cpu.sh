#!/bin/bash
# Instrucciones PowerPC por instrucción de la PSP del intérprete, en qemu:
# el CLI para PowerPC corre cpu_banco.c con 40 y con 20 vueltas y qprof.py
# resta las dos corridas (el arranque se va). Opciones del CLI detrás, por
# ejemplo: medir_cpu.sh --opt cpu_predec=0
set -e
T=$(cd "$(dirname "$0")" && pwd)
G=${SALIDA:-/tmp/banco}/cpu
mkdir -p "$G"
cd "$T/../.."
make -s -C tests/guest >/dev/null
CC="mipsel-linux-gnu-gcc -O1 -mabi=eabi -march=mips2 -mno-abicalls -fno-pic -G0 -msoft-float -mno-check-zero-division -ffreestanding -fno-builtin -nostdlib -T tests/guest/link.ld -Itests/guest"
for v in 40 20; do
	$CC -DVUELTAS=$v -o $G/banco$v.elf tests/guest/crt0.S build-pc/guest/imports.S $T/${BANCO:-cpu_banco.c} 2>/dev/null #Arreglo
done
CORE=$(ls src/core/*.c src/loader/*.c src/cpu/*.c src/hle/*.c src/gpu/*.c)
powerpc-linux-gnu-gcc -O2 -g -std=gnu11 -fwrapv -Isrc -static -pthread -o $G/cli $CORE src/frontend/app.c \
	src/frontend/filelist.c src/frontend/guion.c src/host/main.c -lm
for v in 40 20; do
	build-pc/wiisp-cli --run --quiet --frames 1000 --log $G/banco$v.log "$@" $G/banco$v.elf >/dev/null 2>&1
	qemu-ppc -d in_asm,exec,nochain -D $G/q$v.log $G/cli --run --quiet --frames 1000 "$@" $G/banco$v.elf >/dev/null 2>&1
done
n40=$(sed -n 's/^\[STATS\] \([0-9]*\) instrucciones.*/\1/p' $G/banco40.log | head -1)
n20=$(sed -n 's/^\[STATS\] \([0-9]*\) instrucciones.*/\1/p' $G/banco20.log | head -1)
python3 $T/qprof.py $G/q40.log $G/q20.log $((n40 - n20)) | head -6
v40=$(sed -n 's/^\[STATS\] .*VFPU [0-9.]*% (\([0-9]*\),.*/\1/p' $G/banco40.log | head -1) #Arreglo
v20=$(sed -n 's/^\[STATS\] .*VFPU [0-9.]*% (\([0-9]*\),.*/\1/p' $G/banco20.log | head -1) #Arreglo
[ -n "$v40" ] && [ "$v40" != "$v20" ] && echo "por instruccion VFPU:" && python3 $T/qprof.py $G/q40.log $G/q20.log $((v40 - v20)) | head -6 #Arreglo
rm -f $G/q40.log $G/q20.log
