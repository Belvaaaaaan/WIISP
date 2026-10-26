#!/bin/bash
# Compara bench_charolas con distintos interruptores en muchas semillas:
#   banco_charolas.sh SEMILLAS LLAMADAS "OPCIONES_A" "OPCIONES_B" [fifo|verif]
# Con verif, A y B corren además con ge_verificar=1 (que cambia lo que se
# emite: se compara verificado con verificado): lo demás tiene que salir
# igual (con fifo) y en la línea VERIF de B, todos los "distintos" a 0.
# Con fifo, de la línea [GX] solo cuenta la huella del FIFO (ge_charolas_envio
# cambia el número de GX_Begin). Salida en $SALIDA (por defecto /tmp/banco).
# No se compara cuántas veces se resume la paleta (lo que ahorra ge_clut_igual).
G=${SALIDA:-/tmp/banco}
n=${1:-50}; llamadas=${2:-400}; a=$3; b=$4; fifo=$5
malas=0
for s in $(seq 1 $n); do
	if [ "$fifo" = verif ]; then
		ra=$(qemu-ppc $G/bench_charolas $s $llamadas $a ge_verificar=1 2>/dev/null | sed 's/, [0-9]* paletas;/;/' | grep -v "^VERIF\|^Verificacion")
	else
		ra=$(qemu-ppc $G/bench_charolas $s $llamadas $a 2>/dev/null | sed 's/, [0-9]* paletas;/;/')
	fi
	if [ "$fifo" = verif ]; then
		rb=$(qemu-ppc $G/bench_charolas $s $llamadas $b ge_verificar=1 2>/dev/null | sed 's/, [0-9]* paletas;/;/')
		v=$(echo "$rb" | grep "^VERIF"); rb=$(echo "$rb" | grep -v "^VERIF\|^Verificacion")
		if echo "$v" | grep -qE '[0-9]+/[1-9]|, [1-9][0-9]* distint'; then
			echo "semilla $s: $v"; malas=$((malas + 1)); continue
		fi
		[ $s = 1 ] && echo "$v"
	else
		rb=$(qemu-ppc $G/bench_charolas $s $llamadas $b 2>/dev/null | sed 's/, [0-9]* paletas;/;/')
	fi
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
