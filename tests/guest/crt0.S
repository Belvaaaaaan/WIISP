/* WIISP - tests/guest/crt0.S: arranque de los programas de prueba */
	.set noreorder
	.section .text.start,"ax",@progbits
	.global _start
_start:
	addiu $sp, $sp, -16
	jal main
	nop
	jal sceKernelExitGame
	nop
1:	b 1b
	nop
