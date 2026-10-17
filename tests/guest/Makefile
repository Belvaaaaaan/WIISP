# WIISP - tests/guest/Makefile
# Programas de PSP de prueba, compilados con el gcc MIPS de Linux
# (apt-get install gcc-mipsel-linux-gnu). Cada uno se ejecuta con
# build-pc/wiisp-cli y su salida se compara con el .expected. Si hay un
# <programa>.guion, se ejecuta con ese guion de botones. También se carga
# dos veces seguidas en el mismo proceso (--veces 2): la salida tiene que
# ser la de una vez, repetida (nada se queda del programa anterior).
#
#   make -C tests/guest            # compilar
#   make -C tests/guest check      # compilar y comparar

CC      = mipsel-linux-gnu-gcc
CFLAGS  = -O1 -mabi=eabi -march=mips2 -mno-abicalls -fno-pic -G0 -msoft-float \
          -mno-check-zero-division -ffreestanding -fno-builtin -Wall -Wno-unused-function
OUT     = ../../build-pc/guest
CLI     = ../../build-pc/wiisp-cli
PROGS   = $(basename $(wildcard *.c))

# Los que leen del disco se ejecutan desde una imagen (ISO y CSO) con el
# programa como BOOT.BIN; DATOS.BIN lleva el patrón de mkiso.py
DISCOS  = disco
DATOS   = PSP_GAME/USRDIR/DATOS.BIN=patron:716923
# Opciones de más para el CLI (por ejemplo CLI_OPTS="--opt prof_es=1")
CLI_OPTS ?=

all: $(addprefix $(OUT)/,$(addsuffix .elf,$(PROGS))) \
     $(addprefix $(OUT)/,$(addsuffix .iso,$(DISCOS))) $(addprefix $(OUT)/,$(addsuffix .cso,$(DISCOS)))

$(OUT)/imports.S: imports.txt gen_imports.py ../../src/hle/nid_names.c
	@mkdir -p $(OUT)
	python3 gen_imports.py imports.txt ../../src/hle/nid_names.c > $@

$(OUT)/%.elf: %.c guest.h crt0.S link.ld $(OUT)/imports.S
	$(CC) $(CFLAGS) -nostdlib -T link.ld -o $@ crt0.S $(OUT)/imports.S $<

$(OUT)/%.iso: $(OUT)/%.elf mkiso.py
	python3 mkiso.py $@ PSP_GAME/SYSDIR/BOOT.BIN=$< $(DATOS)

$(OUT)/%.cso: $(OUT)/%.elf mkiso.py
	python3 mkiso.py $@ --cso PSP_GAME/SYSDIR/BOOT.BIN=$< $(DATOS)

check: all
	@ok=0; bad=0; for p in $(PROGS); do \
		g=; [ -f $$p.guion ] && g="--guion $$p.guion --salida $(OUT)/$$p"; \
		imgs=$(OUT)/$$p.elf; case " $(DISCOS) " in *" $$p "*) imgs="$(OUT)/$$p.iso $(OUT)/$$p.cso";; esac; \
		for img in $$imgs; do \
		n=$$p; case $$img in *.iso) n="$$p (ISO)";; *.cso) n="$$p (CSO)";; esac; \
		if $(CLI) --run --quiet --frames 600 $(CLI_OPTS) $$g $$img 2>/dev/null | diff -u $$p.expected - > $(OUT)/$$p.diff; \
		then echo "PASA  $$n"; ok=$$((ok+1)); else echo "FALLA $$n"; cat $(OUT)/$$p.diff; bad=$$((bad+1)); fi; \
		cat $$p.expected $$p.expected > $(OUT)/$$p.dos; \
		if $(CLI) --run --quiet --frames 600 --veces 2 $(CLI_OPTS) $$g $$img 2>/dev/null | diff -u $(OUT)/$$p.dos - > $(OUT)/$$p.diff; \
		then echo "PASA  $$n (dos veces seguidas)"; ok=$$((ok+1)); \
		else echo "FALLA $$n (dos veces seguidas)"; cat $(OUT)/$$p.diff; bad=$$((bad+1)); fi; \
		done; \
	done; echo "$$ok pasan, $$bad fallan"; [ $$bad -eq 0 ]

.PHONY: all check
