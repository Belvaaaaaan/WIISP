# WIISP: arquitectura

Emulador de PSP para Wii. Este documento recoge las decisiones de diseño y
el plan por fases. Es un documento vivo: cuando una decisión cambie, se
actualiza aquí.

## 1. Filosofía

No vamos a ganar por fuerza bruta. Broadway corre a 729 MHz y el Allegrex
de la PSP a 333 MHz (muchos juegos usan 222 MHz), así que solo hay unas
2,2 veces de margen de reloj. Wii64/Not64 tenía unas 8 veces con la N64.
Por eso cada componente se diseña buscando el atajo:

1. **HLE del sistema operativo.** No emulamos el kernel ni el firmware de la
   PSP. Las llamadas al sistema (`sceDisplay…`, `sceIo…`, `sceKernel…`) se
   implementan en C nativo del Wii. Es lo que hace viable a PPSSPP.
2. **Dynarec MIPS→PPC basado en Not64**, simplificado (el Allegrex es de 32
   bits) y sin las penalizaciones que Not64 pagaba por la TLB de la N64.
3. **El GE se dibuja con GX** (la GPU del Wii). Hay además un renderizador
   por software exacto (port del de PPSSPP) que sirve de referencia, para las
   pruebas en PC y como opción "exacta" en el Wii.
4. **Lo que no se puede emular barato, se aproxima o se recorta.** Ejemplos:
   vídeos FMV, efectos de framebuffer raros, la precisión de la VFPU.
5. **Medir en hardware real lo antes posible.** Ninguna optimización se da
   por buena sin números.

## 2. Hardware comparado

| | PSP (lo emulado) | Wii (el anfitrión) |
|---|---|---|
| CPU | Allegrex, MIPS32 R4000-like, 333 MHz, **little-endian** | Broadway, PowerPC 750CL, 729 MHz, **big-endian** |
| FPU / SIMD | FPU de precisión simple + **VFPU** (128 registros, matrices 4×4) | FPU doble + *paired singles* (2 floats por instrucción) |
| Coprocesador | Media Engine (audio/vídeo) | Starlet (ARM, E/S): no se puede usar para cálculo |
| RAM | 32 MB (64 MB en PSP-2000/3000) | 24 MB MEM1 (rápida) + 64 MB MEM2 |
| Vídeo | GE, 2 MB de eDRAM, 480×272 | Hollywood/GX, 3 MB de eDRAM, framebuffer interno de hasta 640×528 |
| Caché | | L1 de 32 KB I + 32 KB D (16 KB bloqueables como scratchpad), L2 de 256 KB |

## 3. Componentes

```
                  ┌─────────────────────────── frontend (wii / host) ─┐
                  │ vídeo, SD/USB, mandos, menú                       │
                  └───────────────┬───────────────────────────────────┘
                                  │ app.c (lógica común)
  ┌──────────┐   ┌────────────────┴───┐   ┌────────────┐   ┌──────────┐
  │ loader   │──▶│ core: memoria,     │◀─▶│ HLE        │──▶│ GE → GX  │
  │ PBP/ELF/ │   │ intérprete,        │   │ (syscalls  │   │ (gpu/)   │
  │ PRX/ISO  │   │ dynarec (cpu/)     │   │ por NID)   │   └──────────┘
  └──────────┘   └────────────────────┘   │            │──▶ audio/
                                          └────────────┘
```

Estructura del repositorio:

| Ruta | Contenido |
|---|---|
| `src/core/` | Tipos, acceso little-endian y mapa de memoria de la PSP |
| `src/loader/` | PBP, PARAM.SFO, ELF/PRX (relocalizaciones e imports) |
| `src/cpu/` | Intérprete del Allegrex (enteros y FPU; la VFPU, pendiente) |
| `src/hle/` | HLE: syscalls por NID, hilos, sincronización, memoria, E/S, display, mandos |
| `tools/` | Generador de la tabla de NIDs a partir del PSPSDK |
| `src/frontend/` | Lógica común a todas las plataformas (`app.c`) |
| `src/wii/` | Frontend del Wii (libogc). Es el único sitio con `#include <gccore.h>` |
| `src/host/` | CLI de PC para depurar sin la consola |
| `tests/` | Pruebas unitarias (x86 con sanitizers y PowerPC big-endian con qemu) |
| `Archivos de Not64/`, `TXTs/` | Material de referencia (código de Wii64/Not64 y notas de investigación) |
| `src/gpu/`, `src/audio/` | Fases siguientes |

Regla de oro: **el núcleo no sabe que existe el Wii.** Todo lo que no sea
`src/wii/` compila y se prueba en PC. Eso nos da depuración rápida, sanitizers
y pruebas automáticas. El frontend del Wii no incluye los headers del núcleo
junto con `gccore.h`, porque libogc define sus propios `u8`/`u32`. La
comunicación pasa por `frontend/app.h`, que solo usa tipos de C estándar.

## 4. Memoria

### 4.1 Mapa de la PSP (implementado en `core/memory.c`)

| Dirección | Tamaño | Región |
|---|---|---|
| `0x00010000` | 16 KB | Scratchpad |
| `0x04000000` | 2 MB | VRAM (espejos hasta `0x047FFFFF`) |
| `0x08000000` | 32 MB | RAM (kernel hasta `0x087FFFFF`, usuario desde `0x08800000`) |

Los bits `0x40000000` (acceso sin caché) y `0x80000000` (kernel) no cambian
la memoria física, así que toda dirección se enmascara con `0x3FFFFFFF`. La
RAM de la PSP se guarda **en little-endian, tal cual**. Así un `memcpy` de la
PSP o un DMA de texturas no necesitan conversión, y las cargas de 32/16 bits
usan `lwbrx`/`lhbrx`, que en Broadway cuestan lo mismo que `lwz`/`lhz`.
Comprobado: GCC ya genera `lwbrx` a partir de `rd_le32()`.

### 4.2 Presupuesto en el Wii (orientativo, se ajustará midiendo)

| Qué | Dónde | Tamaño |
|---|---|---|
| RAM de la PSP | MEM2 | 32 MB |
| VRAM de la PSP | MEM2 | 2 MB |
| Caché del dynarec (código PPC) | MEM1 (menor latencia) | 8–12 MB |
| Tablas de bloques y estado de la CPU | MEM1 | ~1–2 MB |
| Caché de texturas convertidas | MEM2 | 8–12 MB |
| Búferes del ISO/CSO | MEM2 | 2–4 MB |
| Emulador, libogc, framebuffers, FIFO de GX | MEM1 | resto |

El estado de la CPU (32 GPR, 32 FPR, 128 VFPU, HI/LO, PC… unos 1 KB) va en
el *Small Data Area* para accederlo con una sola instrucción relativa a
`r13`, igual que Not64 (ver `TXTs/R13_r2.txt`).

## 5. CPU

### 5.1 Intérprete (fase 2)

Primero un intérprete en C, completo y sencillo. No buscamos velocidad, sino
**exactitud de referencia**: el dynarec se validará comparándose contra él,
instrucción por instrucción. Es la misma idea que el `COMPARE_CORE` de Not64.
También es el salvavidas para instrucciones raras, como `decodeNInterpret`
en Not64.

### 5.2 Dynarec (fase 5): qué heredamos de Not64 y qué cambiamos

Se mantiene:

- La estructura de `Recompile.c`: `pass0` (límites de bloque y destinos de
  salto), la conversión instrucción a instrucción y `pass2` (resolver saltos).
- El manejo de delay slots, incluidas las *branch likely*, que el Allegrex
  también tiene.
- Los *holes* para funciones solapadas, la invalidación por código
  automodificable y el enlace directo entre bloques (`RecompCache_Link`).
- La caché de registros con LRU, la separación `mapRegister` /
  `mapRegisterNew` y la propagación de constantes.
- `PowerPC.h` (el emisor de instrucciones PPC).

Lo que cambia y por qué:

| Not64 | WIISP | Motivo |
|---|---|---|
| Registros MIPS de 64 bits (`mapRegister64`, mitades hi/lo) | Solo 32 bits | El Allegrex es MIPS32. Se elimina la parte más compleja de `MIPS-to-PPC.c` |
| Caché de registros con r3–r12 (10 registros) | r3–r12 + **r14–r28** (~25) | Not64 no usaba los no volátiles ("might be too costly"). Guardarlos una vez al entrar a `dyna_run` es casi gratis con bloques enlazados |
| `flushRegisters()` antes de cada acceso a memoria no constante (TLB) | Sin vaciado: vía rápida directa | La PSP no usa TLB en los juegos: dirección → `base + (addr & máscara)` |
| `lwz`/`stw` directos (N64 big-endian) | `lwbrx`/`stwbrx` (sin desplazamiento inmediato) | La PSP es little-endian. La suma del offset y la máscara se emiten aparte, o se pliegan si el registro base es constante |
| `genUpdateCount`: ~11 instrucciones por salida de bloque | Contador regresivo: `subic.` + salto | Con HLE el timing es mucho más tolerante |
| Bloques de 4 KB atados a páginas de la TLB | Tabla de bloques por página física de la PSP | La memoria es plana |
| FPU en doble precisión | Precisión simple (`fadds`, `fmuls`…) | El Allegrex solo tiene FPU simple |

Asignación tentativa de registros PPC:

| Registro PPC | Uso |
|---|---|
| r1, r2, r13 | Pila y anclas del SDA (intocables, ver `TXTs/Devkitpro y optimización.txt`) |
| r0 | Temporal |
| r3–r12, r14–r26 | Caché de registros MIPS (LRU) |
| r27 | Contador regresivo de ciclos |
| r28 | Puntero al estado de la CPU (por si el SDA se queda corto) |
| r29 | Base de la RAM de la PSP (como `DYNAREG_RDRAM` en Not64) |
| r30 | Función o bloque actual (como `DYNAREG_FUNC`) |
| r31 | Cero (como `DYNAREG_ZERO`) |

Instrucciones propias del Allegrex que encajan bien en PPC:

| Allegrex | PowerPC |
|---|---|
| `ext` / `ins` | `rlwinm` / `rlwimi` |
| `clz` / `clo` | `cntlzw` (con `nor` previo para `clo`) |
| `seb` / `seh` | `extsb` / `extsh` |
| `wsbh` / `wsbw` / `rotr` | `rlwinm` / `rlwimi` |
| `min` / `max` / `movz` / `movn` | 2–3 instrucciones con `isel`; Broadway no tiene `isel`, así que van con un salto corto |

Otras optimizaciones previstas:

- **Detección de bucles inactivos** (esperas a vblank o a un flag) para
  saltar directamente al siguiente evento.
- **Bloques más grandes** siguiendo los saltos incondicionales.
- **El scratchpad de la PSP (16 KB) en la caché L1 bloqueada de Broadway**
  (16 KB). Hay que investigar si compensa.

### 5.3 VFPU (fase 6)

Es el gran cuello de botella de los juegos 3D. El plan:

- Los 128 registros se guardan como 8 matrices 4×4 en memoria.
- Las operaciones de vector y matriz más comunes (`vmmul`, `vtfm`, `vdot`,
  `vadd`, `vscl`…) se emiten con *paired singles*, mapeando pares de
  componentes a un registro FPR.
- Las funciones trascendentes (`vsin`, `vcos`, `vrsq`…) se aproximan con
  tablas o con `frsqrte` + Newton. La exactitud bit a bit no es un objetivo.
- Las operaciones raras se interpretan.

## 6. HLE

### 6.1 Llamadas al firmware (base ya implementada en el cargador)

Los juegos llaman al firmware a través de *stubs* que el cargador parchea
(`loader/elf.c`, `patch_imports`). Cada stub queda así:

```
jr      $ra
syscall <índice en la tabla de imports>
```

El índice nos da `(biblioteca, NID)`. Una tabla `NID → función C` resuelve
la llamada. Las funciones que falten se registran en un log con su NID, para
saber qué implementar a continuación.

### 6.2 Módulos por orden de prioridad

1. `ThreadManForUser` (hilos, semáforos, eventos, retardos) e
   `IoFileMgrForUser` (archivos). Los hilos de la PSP se planifican de forma
   cooperativa sobre un único hilo del Wii.
2. `sceDisplay` y `sceCtrl` (framebuffer, vblank, mandos).
3. `sceGe_user` (listas de comandos del GE → GX).
4. `SysMemUserForUser` (asignación de memoria), `sceUtility` (diálogos de
   guardado, que se pueden simplificar mucho).
5. `sceAudio`, `sceAtrac3plus`, `sceMp3`. Atrac3+ se decodifica en Broadway.
6. `sceMpeg` / `scePsmf` (vídeos). Tiene la prioridad más baja: se pueden
   saltar o reproducir a resolución y fps reducidos.

PPSSPP (GPLv2+) es la referencia principal de comportamiento. Su código se
puede portar módulo a módulo porque la licencia es compatible, pero no se
porta entero: su consumo de memoria no cabe en el Wii.

### 6.3 Discos

- **Fase 1:** EBOOT.PBP de homebrew sin cifrar.
- **Después:** ISO y CSO desde SD/USB. Los EBOOT.BIN de los juegos comerciales
  suelen estar cifrados (`~PSP`) y habrá que descifrarlos con la
  implementación de KIRK (también en PPSSPP); el cargador ya detecta ese caso.

## 7. Gráficos: GE → GX

El GE tiene dos renderizadores que comparten todo lo anterior a la
rasterización (`src/gpu`):

- **Procesador de listas** (`ge.c`): cola de listas, señales, llamadas,
  contexto, transferencias y CLUT, fiel al hardware (pspautotests).
- **Geometría** (`ge_vertex.c`): decodificación de vértices, morph,
  skinning, transformación, luces, texgen, recorte en el plano cercano,
  curvas bezier/spline, sprites e inmediatos con la aritmética del GE que
  midió PPSSPP. Da vértices en coordenadas de pantalla de la PSP.
- **Rasterizador por software** (`ge_raster.c`): port del de PPSSPP,
  exacto al bit en las pruebas `gpu/exact`. Lento en el Wii (unos 7 Mpíxel/s
  en un PC): para pruebas y como modo "exacto".
- **Backend GX** (`src/wii/gx_ge.c`, el modo por defecto en el Wii). GX solo
  rasteriza; la geometría sigue en la CPU, así que las reglas raras del GE
  (recorte, culling, sprites, inmediatos) son las mismas en los dos modos.

| PSP (GE) | Wii (GX) | Cómo |
|---|---|---|
| Vértices ya en pantalla (x, y en 1/16 de píxel, z de 16 bits) | Proyección ortográfica | x, y tal cual; z × 256 en el Z de 24 bits |
| Texturas con perspectiva | GX interpola lineal en pantalla con proyección ortográfica | s/w, t/w y 1/w en la normal; generación 3×4 que divide por píxel. El color se interpola lineal, como en la PSP |
| Rango de profundidad MINZ/MAXZ | Recorte en z | El viewport de GX convierte [minz, maxz] en su rango de recorte |
| Función de textura, doblado, especular, niebla, tramado | TEV | Una etapa por cosa; el tramado con una textura 4×4 en coordenadas de pantalla |
| Mezcla con factores fijos o alfa de destino | Factores de GX | Fijos: premultiplicar en el TEV o usar el alfa de la fuente como constante. Alfa de destino uniforme: constante |
| MIN/MAX/diferencia, SUB con factores | No existen | Aproximados |
| Stencil (en el alfa del framebuffer) | No existe | Escritura con alfa de destino constante (ZERO, REPLACE); el test se ignora |
| Framebuffers en la VRAM | EFB de 640×528 | El que se dibuja vive en el EFB; los demás en texturas (copias del EFB). Color en RGB8 (exacto) mientras el alfa sea uniforme o siga en la VRAM; RGBA6 solo si hace falta alfa por píxel |
| La CPU lee o escribe la VRAM | | Un gancho en `memory.c` baja de la GPU lo que se lee y marca lo escrito para subirlo antes de volver a dibujar |
| Render a textura | `GX_CopyTex` | Si la textura es un framebuffer en la GPU, se usa su copia directamente |
| Texturas swizzled, CLUT, DXT | RGBA8 / RGB565 | Caché por contenido (hash), mipmaps cuando los niveles van a la mitad |
| 480×272 | XFB del televisor | Compuesto con GX: en 16:9 llena la pantalla, en 4:3 ocupa el ancho |

Las diferencias con el hardware son de ±1 en los colores (la aritmética del
TEV y de la mezcla de GX), el alfa de 6 bits cuando hace falta alfa por
píxel, el stencil y los modos de mezcla que GX no tiene.

## 8. Plan por fases

| Fase | Objetivo | Estado |
|---|---|---|
| 1 | Esqueleto devkitPPC/libogc, mapa de memoria, cargador PBP/SFO/ELF/PRX con relocalizaciones e imports parcheados, CLI de PC, pruebas en x86 y PPC big-endian, CI | ✅ |
| 2 | Intérprete Allegrex completo (sin VFPU), HLE mínimo (hilos, archivos, display, ctrl), framebuffer de la PSP mostrado tal cual en el Wii | ✅ |
| 3 | Correr los *samples* del PSPSDK y homebrew sencillo; ampliar el HLE guiado por pspautotests e imports.txt. Hecho: menú SD/USB, medidor de FPS/MIPS, sceRtc, sceUtility, sceSuspend, directorios/stat, Mt19937, red simulada | ⏳ |
| 4 | GE: listas, geometría, renderizador por software exacto y backend GX (primitivas, texturas con caché, framebuffers, render a textura) | ✅ |
| 5 | Dynarec basado en Not64, validado contra el intérprete | |
| 6 | VFPU con paired singles, skinning, audio (sceAudio, Atrac3+) | |
| 7 | ISO/CSO, descifrado, compatibilidad con juegos comerciales, menú | |

El plan de optimización para juegos comerciales pesados (fastmem, caché
persistente de código, VFPU, audio en el DSP, E/S asíncrona) está en
`docs/OPTIMIZACION.md`.

## 9. Metodología de pruebas

1. **Primero en PC, después en el Wii.** Todo se desarrolla con el CLI
   (`wiisp-cli --run`), que usa exactamente el mismo núcleo. El Wii confirma
   cada hito.
2. **pspautotests** (https://github.com/hrydgard/pspautotests): cientos de
   programas de PSP con la salida que dan en una PSP real (`.expected`).
   `tests/autotests.py` los ejecuta y compara. `tests/autotests_pass.txt`
   lista los que ya pasan: CI falla si alguno deja de pasar (en x86 y en
   PowerPC big-endian con qemu). Cada test que se arregla se añade a la lista.
3. **imports.txt**: al cargar un juego, WIISP escribe junto al EBOOT la lista
   de funciones del firmware que usa, con su nombre y si ya está
   implementada. Es la lista de tareas del HLE por juego.
4. **Log de NIDs sin implementar**: al ejecutarse, cada función que falta se
   avisa una vez (`[HLE] sin implementar: ...`) y devuelve 0.
5. **Los NIDs no se escriben a mano**: las funciones HLE se registran por
   nombre y `tools/gen_nids.py` genera la tabla NID → nombre desde los stubs
   del PSPSDK.

Estado de la fase 2 (comprobado con pspautotests): CPU entera y saltos,
división, carga/almacenamiento, ll/sc, FPU básica (sin flags IEEE, modos de
redondeo ni anulación de denormales), hilos, semáforos, event flags, malloc
y string. Faltan: VFPU, callbacks, alarmas, mutex, mailboxes, message pipes,
FPL/VPL, sceIoDopen y la mayoría de módulos de sistema.

## 10. Convenciones

- C (`gnu11`), sin dependencias más allá de libogc/libfat en el Wii.
- Comentarios en español. El texto que se muestra en la consola del Wii va
  **sin acentos**, porque su fuente no muestra UTF-8.
- Los datos de la PSP se leen siempre con `rd_le*`/`wr_le*`/`mem_read*`,
  nunca con casts directos.
- Todo archivo de entrada es no confiable: se valida cada offset. Las pruebas
  truncan y corrompen archivos al azar con ASan/UBSan activados.
- Cada cambio del núcleo se prueba en x86 **y** en PowerPC big-endian
  (`make -f Makefile.pc test test-ppc`). CI lo hace en cada push.

## 11. Licencia y créditos

WIISP se distribuye bajo **GPLv2 o posterior** (ver `LICENSE`). Esto nos
permite reutilizar:

- **Wii64/Not64**, © 2007-2010 Mike Slegeir y colaboradores (GPLv2+): el
  dynarec MIPS→PPC.
- **PPSSPP**, © Henrik Rydgård y colaboradores (GPLv2+): referencia de HLE,
  formatos y comportamiento.
- **PSPSDK** (pspdev, licencia BSD): los nombres de NIDs de
  `src/hle/nid_names.c` se generan desde sus stubs.
- **pspautotests**: banco de pruebas (se descarga aparte, no se incluye).

El código derivado de ambos conserva sus avisos de copyright originales.

## 12. Riesgos conocidos

- **VFPU y CPU en juegos 3D pesados:** el margen de 2,2× es justo. Hay que
  medir pronto con homebrew 3D en hardware real.
- **Memoria:** 88 MB en total con 34 MB fijos para la PSP. Los juegos que
  necesitan 64 MB (pocos) quedan fuera, salvo trucos.
- **Vídeos FMV (H.264):** probablemente no a velocidad completa.
- **Stencil y efectos de framebuffer:** requerirán aproximaciones o arreglos
  por juego.
