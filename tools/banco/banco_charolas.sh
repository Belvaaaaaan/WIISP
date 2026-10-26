#!/bin/bash
# Compara bench_charolas con distintos interruptores en muchas semillas:
#   banco_charolas.sh SEMILLAS LLAMADAS "OPCIONES_A" "OPCIONES_B" [fifo]
# Con fifo, de la línea [GX] solo cuenta la huella del FIFO (ge_charolas_envio
# cambia el número de GX_Begin). Salida en $SALIDA (por defecto /tmp/banco).
# No se compara cuántas veces se resume la paleta (lo que ahorra ge_clut_igual).
G=${SALIDA:-/tmp/banco}
n=${1:-50}; llamadas=${2:-400}; a=$3; b=$4; fifo=$5
malas=0
for s in $(seq 1 $n); do
	ra=$(qemu-ppc $G/bench_charolas $s $llamadas $a 2>/dev/null | sed 's/, [0-9]* paletas;/;/')
	rb=$(qemu-ppc $G/bench_charolas $s $llamadas $b 2>/dev/null | sed 's/, [0-9]* paletas;/;/')
	if [ -n "$fifo" ]; then
		ra=$(echo "$ra" | sed 's/^\[GX\] [^;]*; /[GX] /'); rb=$(echo "$rb" | sed 's/^\[GX\] [^;]*; /[GX] /')
	fi
	if [ "$ra" != "$rb" ]; then
		malas=$((malas + 1))
		[ $malas -le 5 ] && { echo "semilla $s distinta:"; diff <(echo "$ra") <(echo "$rb") | head -6; }
	fi
done
qemu-ppc $G/bench_charolas 1 $llamadas $b 2>&1 >/dev/null | head -1
echo "$n semillas de $llamadas llamadas: $malas distintas"
