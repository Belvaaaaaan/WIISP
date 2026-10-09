# WIISP - tests/guest/Makefile
# Programas de PSP de prueba, compilados con el gcc MIPS de Linux
# (apt-get install gcc-mipsel-linux-gnu). Cada uno se ejecuta con
# build-pc/wiisp-cli y su salida se compara con el .expected.
#
#   make -C tests/guest            # compilar
#   make -C tests/guest check      # compilar y comparar

CC      = mipsel-linux-gnu-gcc
CFLAGS  = -O1 -mabi=eabi -march=mips2 -mno-abicalls -fno-pic -G0 -msoft-float \
          -mno-check-zero-division -ffreestanding -fno-builtin -Wall -Wno-unused-function
OUT     = ../../build-pc/guest
CLI     = ../../build-pc/wiisp-cli
PROGS   = $(basename $(wildcard *.c))

all: $(addprefix $(OUT)/,$(addsuffix .elf,$(PROGS)))

$(OUT)/imports.S: imports.txt gen_imports.py ../../src/hle/nid_names.c
	@mkdir -p $(OUT)
	python3 gen_imports.py imports.txt ../../src/hle/nid_names.c > $@

$(OUT)/%.elf: %.c guest.h crt0.S link.ld $(OUT)/imports.S
	$(CC) $(CFLAGS) -nostdlib -T link.ld -o $@ crt0.S $(OUT)/imports.S $<

check: all
	@ok=0; bad=0; for p in $(PROGS); do \
		if $(CLI) --run --quiet --frames 600 $(OUT)/$$p.elf 2>/dev/null | diff -u $$p.expected - > $(OUT)/$$p.diff; \
		then echo "PASA  $$p"; ok=$$((ok+1)); else echo "FALLA $$p"; cat $(OUT)/$$p.diff; bad=$$((bad+1)); fi; \
	done; echo "$$ok pasan, $$bad fallan"; [ $$bad -eq 0 ]

.PHONY: all check
