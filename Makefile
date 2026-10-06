#---------------------------------------------------------------------------------
# WIISP - compilación para Wii (devkitPPC + libogc)
#   make          -> wiisp.dol
#   make clean
# Para compilar y probar en PC, ver Makefile.pc
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPPC)),)
$(error "Falta DEVKITPPC en el entorno. export DEVKITPPC=<ruta>/devkitPPC")
endif

include $(DEVKITPPC)/wii_rules

#---------------------------------------------------------------------------------
TARGET		:=	wiisp
BUILD		:=	build
SOURCES		:=	src/core src/loader src/cpu src/hle src/gpu src/frontend src/wii
INCLUDES	:=	src

#---------------------------------------------------------------------------------
CFLAGS		=	-O2 -Wall -Wextra -Wno-unused-parameter -std=gnu11 $(MACHDEP) $(INCLUDE)
LDFLAGS		=	$(MACHDEP) -Wl,-Map,$(notdir $@).map

LIBS		:=	-lfat -lwiiuse -lbte -logc -lm
LIBDIRS		:=	$(PORTLIBS)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
export LD	:=	$(CC)
export OFILES	:=	$(CFILES:.c=.o)

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD) -I$(LIBOGC_INC)

export LIBPATHS	:=	-L$(LIBOGC_LIB) $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: $(BUILD) clean dist

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo limpiando ...
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).dol dist/apps/wiisp/boot.dol

# Carpeta lista para copiar a la raíz de la SD (Homebrew Channel)
dist: $(BUILD)
	@cp $(OUTPUT).dol dist/apps/wiisp/boot.dol
	@echo "Copia dist/apps a la raiz de la SD"

#---------------------------------------------------------------------------------
else

DEPENDS	:=	$(OFILES:.o=.d)

$(OUTPUT).dol: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
