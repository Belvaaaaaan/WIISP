#!/bin/bash
# Instrucciones PowerPC por vértice (total y batch_range) en el banco del GE,
# para los formatos de DBZ (sin huesos, 2, 4 y 8 huesos, y 0x19F).
# Uso: gevert.sh DIR_BANCO [opcion=valor...]  (DIR_BANCO = el $SALIDA de build_banco.sh)
T=$(cd "$(dirname "$0")" && pwd)
G=$1; shift
for vt in 11A 4301 C301 1C301 19F; do
	qemu-ppc -d in_asm,exec,nochain -D $G/qa.log $G/bench 300 $vt "$@" > /dev/null
	qemu-ppc -d in_asm,exec,nochain -D $G/qb.log $G/bench 600 $vt "$@" > /dev/null
	python3 -I $T/qprof.py $G/qb.log $G/qa.log 6000 > $G/prof_$vt.txt
	echo "$vt $(head -1 $G/prof_$vt.txt) | batch_range $(grep ' batch_range$' $G/prof_$vt.txt | awk '{print $1}')"
done
rm -f $G/qa.log $G/qb.log
