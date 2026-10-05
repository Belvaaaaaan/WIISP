# WIISP

Emulador de PSP para Wii, **en desarrollo temprano**. El objetivo es que los
juegos sean jugables gracias a la emulación de alto nivel (HLE) del sistema
operativo, un dynarec MIPS→PowerPC basado en el de Wii64/Not64 y la traducción
de los gráficos de la PSP a GX. Ver [ARQUITECTURA.md](ARQUITECTURA.md).

## Estado

**Fase 1 completa.** El emulador carga un `EBOOT.PBP` (o un ELF/PRX) en la
memoria emulada, aplica las relocalizaciones, lee el `PARAM.SFO` y prepara
las llamadas al firmware para el HLE. Todavía no ejecuta código.

## Compilar para Wii

Requiere [devkitPro](https://devkitpro.org/wiki/Getting_Started) con devkitPPC
y libogc (paquete `wii-dev`).

```sh
make dist
```

Copia `dist/apps` a la raíz de la SD y tu homebrew de PSP a
`sd:/wiisp/EBOOT.PBP`. Después abre WIISP desde el Homebrew Channel.

Cada push también compila el `.dol` en GitHub Actions; se descarga desde la
pestaña *Actions* (artefacto `wiisp-wii`).

## Compilar y probar en PC

```sh
make -f Makefile.pc                  # build-pc/wiisp-cli
./build-pc/wiisp-cli --all EBOOT.PBP # muestra módulo, imports y NIDs
make -f Makefile.pc test             # pruebas (ASan + UBSan)
make -f Makefile.pc test-ppc         # pruebas en PowerPC big-endian (qemu)
```

`test-ppc` necesita `gcc-powerpc-linux-gnu` y `qemu-user`.

## Licencia

GPLv2 o posterior (ver [LICENSE](LICENSE)). Incluye material de referencia de
Wii64/Not64 (© Mike Slegeir y colaboradores, GPLv2+).
