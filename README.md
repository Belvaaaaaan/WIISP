# WIISP

Emulador de PSP para Wii, **en desarrollo temprano**. El objetivo es que los
juegos sean jugables gracias a la emulación de alto nivel (HLE) del sistema
operativo, un dynarec MIPS→PowerPC basado en el de Wii64/Not64 y la traducción
de los gráficos de la PSP a GX. Ver [ARQUITECTURA.md](ARQUITECTURA.md).

## Estado

WIISP carga un `EBOOT.PBP` (o ELF/PRX) y lo ejecuta con un intérprete del
Allegrex y HLE del sistema operativo de la PSP (hilos, semáforos, memoria,
archivos, pantalla, mandos). **El GE (los gráficos 2D/3D de la PSP) está
completo** con dos renderizadores:

- **GX** (por defecto en el Wii): la GPU del Wii dibuja; la geometría sale de
  la CPU con las reglas del GE. Rápido: las demos 3D del PSPSDK van de 20 a
  más de 300 FPS en Dolphin, frente a 2-8 FPS por software.
- **Software exacto**: port del renderizador de PPSSPP, idéntico al bit a
  una PSP en las pruebas, pero lento en el Wii.

Pasan 129 de los 557 [pspautotests](https://github.com/hrydgard/pspautotests)
(97 de 109 de gráficos). Todavía no hay VFPU (muchos juegos y demos la usan),
audio ni dynarec.

Al cargar un juego escribe `imports.txt` junto al EBOOT: la lista de funciones
del firmware que usa y cuáles ya implementa WIISP.

![Hello World PSP corriendo en WIISP](docs/capturas/fase2-helloworld.png)

## Compilar para Wii

Requiere [devkitPro](https://devkitpro.org/wiki/Getting_Started) con devkitPPC
y libogc (paquete `wii-dev`).

```sh
make dist
```

Copia `dist/apps` a la raíz de la SD y abre WIISP desde el Homebrew Channel.
Aparece un menú para elegir el programa en la **SD o en un USB**: pon tus
`.pbp`, `.prx` o `.elf` donde quieras (por ejemplo en `sd:/wiisp/`), con su
nombre original.

| Botón (Wiimote / GameCube) | En el menú | Durante la emulación |
|---|---|---|
| Cruceta | moverse (izquierda/derecha: página) | cruceta de la PSP |
| A | abrir carpeta / elegir | X (cruz) |
| B | carpeta anterior | O (círculo) |
| 1 / X | cambiar SD ↔ USB | cuadrado / triángulo |
| HOME / START | salir al Homebrew Channel | HOME o Z+START: volver al menú |
| 2 / Y | (al abrir un programa) renderizador GX o software | triángulo / cuadrado |

El botón de apagado de la consola o del Wiimote apaga el Wii de forma
ordenada.

En la pantalla previa a ejecutar se elige el renderizador con 2 (Wiimote) o
Y (GameCube): **GX** (rápido) o **software** (exacto, lento). La elección se
recuerda. En una tele 16:9 la imagen llena la pantalla; en 4:3 ocupa el ancho.

Al abrir un programa, WIISP escribe junto a él su informe de imports
(`imports.txt` para un `EBOOT.PBP`, `<nombre>.imports.txt` para el resto) y
recuerda la carpeta para la próxima vez. Durante la emulación, arriba se ven
los FPS, los MIPS (millones de instrucciones de la PSP por segundo) y la
velocidad respecto a una PSP real.

El zip de GitHub Actions trae además un `EBOOT.PBP` de prueba (sintético) en
`apps/wiisp/`.

Cada push también compila el `.dol` en GitHub Actions; se descarga desde la
pestaña *Actions* (artefacto `wiisp-wii`).

## Compilar y probar en PC

```sh
make -f Makefile.pc                  # build-pc/wiisp-cli
./build-pc/wiisp-cli --all EBOOT.PBP # muestra módulo, imports y NIDs
./build-pc/wiisp-cli --run --screenshot pantalla.ppm EBOOT.PBP
./build-pc/wiisp-cli --imports imports.txt EBOOT.PBP
make -f Makefile.pc test             # pruebas (ASan + UBSan)
make -f Makefile.pc test-ppc         # pruebas en PowerPC big-endian (qemu)
```

`test-ppc` necesita `gcc-powerpc-linux-gnu` y `qemu-user`.

Pruebas con programas reales de PSP ([pspautotests](https://github.com/hrydgard/pspautotests)):

```sh
git clone https://github.com/hrydgard/pspautotests ../pspautotests
python3 tests/autotests.py ../pspautotests --list tests/autotests_pass.txt  # los que deben pasar
python3 tests/autotests.py ../pspautotests threads/ -v                      # una carpeta, con diffs
```

### En Dolphin (backend GX sin la consola)

El `.dol` tiene un modo de pruebas automáticas: si existe
`sd:/wiisp/autotest.txt`, ejecuta la lista de programas sin mandos, guarda su
salida, capturas y estadísticas (FPS, tiempo en el GE...) y apaga el Wii.
`tools/dolphin_autotest.py` prepara la SD de [Dolphin](https://dolphin-emu.org)
(como carpeta sincronizada), lo lanza y compara con los `.expected`:

```sh
make                                              # wiisp.elf (Dolphin carga el .elf)
python3 tools/dolphin_autotest.py --dolphin /ruta/dolphin-emu-nogui \
        --tests ../pspautotests gpu/commands/basic gpu/texfunc/modulate
python3 tools/dolphin_autotest.py --run --frames 600 demo/EBOOT.PBP   # FPS
```

Con GX, "CASI" quiere decir que solo cambian colores en ±6 (la aritmética de
GX no es la del GE).

## Licencia

GPLv2 o posterior (ver [LICENSE](LICENSE)). Incluye material de referencia de
Wii64/Not64 (© Mike Slegeir y colaboradores, GPLv2+).
