# WIISP

Emulador de PSP para Wii, **en desarrollo temprano**. El objetivo es que los
juegos sean jugables gracias a la emulación de alto nivel (HLE) del sistema
operativo, un dynarec MIPS→PowerPC basado en el de Wii64/Not64 y la traducción
de los gráficos de la PSP a GX. Ver [ARQUITECTURA.md](ARQUITECTURA.md).

## Estado

WIISP carga un `EBOOT.PBP`, un ELF/PRX o una **imagen de UMD (ISO, CSO o
ZSO)**, descifra los ejecutables de los juegos comerciales (`~PSP`) y los
ejecuta con un intérprete del
Allegrex y HLE del sistema operativo de la PSP (hilos, semáforos, memoria,
archivos, pantalla, mandos). **El GE (los gráficos 2D/3D de la PSP) está
completo** con dos renderizadores:

- **GX** (por defecto en el Wii): la GPU del Wii dibuja; la geometría sale de
  la CPU con las reglas del GE. Rápido: las demos 3D del PSPSDK van de 20 a
  más de 300 FPS en Dolphin, frente a 2-8 FPS por software.
- **Software exacto**: port del renderizador de PPSSPP, idéntico al bit a
  una PSP en las pruebas, pero lento en el Wii.

Pasan 275 de los 557 [pspautotests](https://github.com/hrydgard/pspautotests)
(97 de 109 de gráficos y 20 de 26 de la VFPU). Para los juegos comerciales ya
hay carga de módulos, la VFPU completa (con las funciones trascendentes
exactas al bit), sceAudio, el mezclador SAS, la lógica de Atrac3+ y sceMpeg
(con sonido en silencio por ahora: los videos se saltan), partidas guardadas
y diálogos del sistema. GTA: Liberty City Stories ya tiene todas sus
funciones del firmware. Falta el dynarec: con el intérprete, los juegos
comerciales van muy lentos.

En el Wii, las partidas guardadas van a `SAVEDATA/` dentro de la carpeta de
WIISP (`sd:/apps/wiisp/` o `sd:/wiisp/`), sin cifrar y con su `PARAM.SFO`
como en una Memory Stick. Ahí mismo queda `wiisp.log`, el registro de la
última sesión: funciones que faltan, fallos de CPU, mensajes del juego,
partidas cargadas o guardadas y, cada minuto de juego emulado, estadísticas
de rendimiento (líneas `[STATS]`: cuánto se usa la VFPU y qué instrucciones,
cambios de hilo). Si el juego se queda parado, también apunta las
primeras llamadas al firmware y en qué está esperando cada hilo
(`[DIAGNOSTICO]`). Es lo más útil para reportar un problema.

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
`.pbp`, `.prx`, `.elf` o `.bin` donde quieras (por ejemplo en `sd:/wiisp/`),
con su nombre original, y tus juegos como `.iso`, `.cso` o `.zso`.

El **Wiimote va en horizontal** (como un mando de NES: la cruceta a la
izquierda, 1 y 2 a la derecha), también en el menú.

| Wiimote en horizontal | En el menú | Durante la emulación (PSP) |
|---|---|---|
| Cruceta | moverse (izquierda/derecha: página) | stick analógico |
| 1 | cambiar SD ↔ USB; (al abrir un programa) texturas rápidas o seguras | X (cruz) |
| 2 | (al abrir un programa) renderizador GX o software | O (círculo) |
| B | carpeta anterior | triángulo |
| A | abrir carpeta / elegir | cuadrado |
| − / + | | L / R |
| B + 1 / B + 2 | | SELECT / START |
| B + A | | turbo: activar o quitar ("TURBO" en pantalla) |
| B + − / B + + | | diagnóstico (solo GX): matemática rápida o exacta / perspectiva de texturas normalizada o no |
| Inclinar hacia delante / hacia ti | | cruceta arriba / abajo |
| Girar como un volante | | cruceta izquierda / derecha |
| HOME | salir al Homebrew Channel | mantener 1 s: volver al menú |

La cruceta de la PSP por movimiento pide llegar cerca del tope (50° hacia
delante, 75° hacia ti, 55° de giro) y se suelta 15° antes, así que no salta
sin querer al jugar. Con B pulsado, 1 y 2 son SELECT y START: por eso el
triángulo tarda 0,12 s en pulsarse al mantener B (un toque corto llega al
soltarlo), y triángulo + X, + O o + cuadrado a la vez solo salen si 1, 2 o
A se pulsan antes que B.

**Turbo** (B + A o Z + Y): el GE deja de dibujar (ni vértices ni GX) y el
juego avanza varias veces más rápido con la imagen quieta; sirve para pasar
cinemáticas y cargas. En el CLI, `--turbo`.

**Diagnóstico del warping** (solo con el renderizador GX): en la esquina
sale `MR TN` (lo normal). B + − (Z + L) cambia la geometría entre `MR`
(matemática rápida, float) y `ME` (exacta, la del GE; más lenta). B + +
(Z + R) cambia la perspectiva de las texturas entre `TN` (normalizada por
triángulo, nuevo en la 0.5.4) y `TS` (sin normalizar, como hasta la 0.5.3).
Cada cambio queda en wiisp.log como `[DIAG]`.

| GameCube | En el menú | Durante la emulación (PSP) |
|---|---|---|
| Stick / cruceta | — / moverse | stick analógico / cruceta |
| A / B | elegir / atrás | X / O |
| Y / X | renderizador / SD ↔ USB y texturas | cuadrado / triángulo |
| L / R / Z | | L / R / SELECT |
| START | salir al Homebrew Channel | START; Z + START: volver al menú; Z + Y: turbo; Z + L / Z + R: diagnóstico |

El botón de apagado de la consola o del Wiimote apaga el Wii de forma
ordenada.

En la pantalla previa a ejecutar se elige el renderizador con 2 (Wiimote) o
Y (GameCube): **GX** (rápido) o **software** (exacto, lento). Con 1 o X, cómo
comprueba GX si una textura cambió: **rápidas** (las que no cambian se
vuelven a leer cada vez menos, hasta cada 16 cuadros) o **seguras** (una vez
por sincronización con el GE, como PPSSPP). Las elecciones se recuerdan. En una tele 16:9 la imagen llena la pantalla; en 4:3 ocupa el ancho.

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

### Desglose de tiempos

Cada 30 s reales, `wiisp.log` dice a dónde va el tiempo:

```
[TIEMPOS] 30.0 s reales: 2.59 s de juego (8.6%), 155 vblanks, 0 imagenes (0.00/s), 61.2 M instr (2.04 MIPS)
[TIEMPOS] CPU 17.7% syscalls 0.1% GE 63.6% rasterizar 0.0% texturas 11.4% framebuffers 6.0% ...
[TIEMPOS] GE: 309 listas, 3934962 comandos, 7825220 vertices, 1845270 primitivas | ...
[TIEMPOS] GE por dentro: comandos ...% leer vertices ...% transformar y luces ...% ... (30000 muestras)
[TIEMPOS] Dibujo: ... llamadas (... por vblank; ...% con indices, ...% reutilizan vertices); ...% listas de triangulos ...
[TIEMPOS] Vertices: ... pedidos, ... calculados (... por vblank): ...% con huesos, ...% con morph, ...% con luces ...
[TIEMPOS] Triangulos: ... recibidos (... por vblank): ...% dibujados, ...% de espaldas o sin area, ...% fuera de pantalla ...
[TIEMPOS] modo rapido (GX): 309 bajadas y 77 subidas de framebuffer, 0 imagenes desde la VRAM
[TIEMPOS] texturas: ... elegidas (...% sin leerlas), ... comprobadas (... MB), ... decodificadas, ... paletas; cambios: ...
```

`CPU` es el intérprete; `rasterizar`, el dibujo por software (modo exacto);
`texturas`, `framebuffers`, `presentar` y `esperar GX`, el backend GX.
"GE por dentro" reparte el tiempo del GE por fases (muestreo cada 1 ms).
En la línea de texturas, "sin leerlas" son las veces que bastó la copia ya
decodificada y "cambios sin aviso" los que solo se vieron al releerlas
(ARQUITECTURA.md 7).

### En Windows

`make -f Makefile.pc win` compila `build-win/wiisp-cli.exe` (con
`gcc-mingw-w64-x86-64`, sin DLLs). Con `dist/windows/probar_juego.bat` y
`dist/windows/LEEME.txt` forma el paquete para Windows: se arrastra la ISO
encima del .bat y deja `wiisp.log` y capturas BMP (`--capturas N`: una cada
N frames si la imagen cambió). Ctrl+C para limpiamente.

Programas de PSP propios para reproducir situaciones de juegos comerciales
(`tests/guest`, en C, sin PSPSDK; necesitan `gcc-mipsel-linux-gnu`):

```sh
make -C tests/guest check            # compila y compara con los .expected
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

## Seguir el desarrollo

`docs/CONTINUAR.md` resume el estado actual y los siguientes pasos, y
`tools/setup_entorno.sh` prepara lo necesario (devkitPPC, pspautotests,
PPSSPP como referencia) en una máquina nueva.

## Licencia

GPLv2 o posterior (ver [LICENSE](LICENSE)). Incluye material de referencia de
Wii64/Not64 (© Mike Slegeir y colaboradores, GPLv2+).
