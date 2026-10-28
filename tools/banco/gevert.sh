#!/bin/bash
# Instrucciones PowerPC por vértice (total, batch_range y el núcleo de paired
# singles de ge_vdec_ps) en el banco del GE,
# para los formatos de DBZ (sin huesos, 2, 4 y 8 huesos, y 0x19F).
# Uso: gevert.sh DIR_BANCO [opcion=valor...]  (DIR_BANCO = el $SALIDA de build_banco.sh)
T=$(cd "$(dirname "$0")" && pwd)
G=$1; shift
for vt in 11A 4301 C301 1C301 19F; do   # -cpu 750: sin AltiVec, los paired singles dan SIGILL (sigill_ppc.c)
	qemu-ppc -cpu 750 -d in_asm,exec,nochain -D $G/qa.log $G/bench 300 $vt "$@" > /dev/null
	qemu-ppc -cpu 750 -d in_asm,exec,nochain -D $G/qb.log $G/bench 600 $vt "$@" > /dev/null
	python3 -I $T/qprof.py $G/qb.log $G/qa.log 6000 > $G/prof_$vt.txt
	echo "$vt $(head -1 $G/prof_$vt.txt) | batch_range $(grep ' batch_range$' $G/prof_$vt.txt | awk '{print $1}') | ps_nucleo $(awk '/ ps_nucleo/{s+=$1} END{print s+0}' $G/prof_$vt.txt)"
done
rm -f $G/qa.log $G/qb.log
