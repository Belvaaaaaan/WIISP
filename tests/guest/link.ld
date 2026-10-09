/* WIISP - tests/guest/link.ld: ejecutable de PSP absoluto en 0x08804000 */
OUTPUT_FORMAT("elf32-tradlittlemips")
OUTPUT_ARCH(mips)
ENTRY(_start)
SECTIONS
{
	. = 0x08804000;
	.text : { *(.text.start) *(.text .text.*) *(.sceStub.text) }
	.rodata.sceModuleInfo : { KEEP(*(.rodata.sceModuleInfo)) }
	.lib.stub : { __lib_stub_top = .; KEEP(*(.lib.stub)) __lib_stub_bottom = .; }
	.rodata : { *(.rodata .rodata.*) }
	.data : { *(.data .data.*) *(.sdata .sdata.*) }
	.bss : { *(.sbss .sbss.*) *(.bss .bss.*) *(COMMON) }
	/DISCARD/ : { *(.MIPS.abiflags) *(.reginfo) *(.pdr) *(.comment) *(.gnu.attributes) *(.note*) }
}
