# WIISP: plan de optimización para juegos comerciales

Documento de diseño, todavía sin implementar. Parte de los juegos objetivo
(Dragon Ball Z Tenkaichi Tag Team, Silent Hill Origins y GTA Liberty City
Stories / Vice City Stories) y recoge qué falta, qué han resuelto otros
emuladores y cómo encaja en el Wii. Complementa `ARQUITECTURA.md`.

## 1. El problema en números

Broadway va a 729 MHz y el Allegrex a 333 MHz: 2,2 ciclos del Wii por cada
ciclo de la PSP, y eso antes de pagar la VFPU, el GE, el audio y el HLE, que
corren en el **mismo núcleo**. La PSP reparte ese trabajo entre la CPU, la
VFPU, el GE y el Media Engine. Conclusión: no basta con un buen dynarec;
hay que **quitarle trabajo a Broadway** siempre que otro chip del Wii pueda
hacerlo (GX, DSP, Starlet) o que se pueda hacer una sola vez y guardarse.

| Chip del Wii | Qué puede quitarle a Broadway |
|---|---|
| GX (Hollywood) | Rasterizado (ya), transformación y luces (T&L) en casos simples |
| DSP (81 MHz) | Mezcla de voces de sceSas/sceAudio; decodificación ADPCM en su acelerador |
| Starlet (vía IOS) | Las lecturas de SD/USB ya las hace él: Broadway solo tiene que no quedarse esperando |
| La PC del usuario | Traducción de código optimizada y transcodificación de audio/vídeo, una sola vez |

## 2. Huecos y problemas, por impacto

1. **CPU interpretada.** Hoy es el cuello de botella (el demo `sprite` va a
   9 FPS por la CPU). Lo resuelve el dynarec (§4).
2. **Floats en little-endian.** La RAM de la PSP se guarda en LE y PowerPC
   no tiene cargas de float con inversión de bytes (`lfsbrx` no existe).
   Cada `lwc1`/`lv.s`/`lv.q` cuesta `lwbrx` + `stw` + `lfs` en vez de un
   `lfs`, con una posible penalización por cargar justo lo que se acaba de
   guardar. Es el problema de fondo de la VFPU (§5).
3. **Acceso a memoria.** Cada carga o guardado con dirección no constante
   necesita enmascarar y comprobar la región. Ver fastmem (§4.3).
4. **VFPU.** GTA la usa a fondo; sin ella no arranca la parte 3D.
5. **Descifrado y carga de módulos** (KIRK, `sceKernelLoadModule` de PRX
   cifrados del UMD). Bloquea todo lo demás: sin esto no hay juego.
6. **Atrac3+.** Decodificar música en Broadway puede costar un 10–20 % de
   CPU. GTA además mezcla muchas voces de SAS.
7. **Transformación de vértices en la CPU.** Correcta y ya rápida, pero en
   escenas con muchos polígonos compite con el juego por Broadway.
8. **E/S síncrona.** GTA lee la ciudad del UMD mientras juegas. Si cada
   lectura de la SD bloquea a Broadway, hay tirones.
9. **Memoria del Wii.** 24 MB de MEM1 + 64 MB de MEM2, con 34 MB fijos
   para la PSP. Código traducido, texturas, búferes de ISO y audio compiten
   por el resto. La caché persistente (§4.4) no puede cargarse entera a
   ciegas.
10. **Fidelidad del kernel.** Planificación de hilos, callbacks, alarmas,
    señales del GE (tarea pendiente #12), sceUtility (guardado), sceUmd.
11. **Stencil y test de color en GX** (tarea pendiente #14). Silent Hill
    Origins usa efectos de framebuffer y probablemente stencil.
12. **Vídeos (sceMpeg/PSMF, H.264).** A velocidad completa es muy dudoso.

## 3. Lo que ya hicieron otros emuladores

| Emulador | Técnica | ¿Sirve en WIISP? |
|---|---|---|
| **Wii64/Not64** | Dynarec MIPS→PPC en el mismo hardware, caché de registros, enlace de bloques | Es nuestra base (ver `ARQUITECTURA.md` §5.2) |
| **PPSSPP** | Reemplazo de funciones conocidas por hash (`MIPSAnalyst` + `ReplaceTables`: memcpy, memset, strlen, rutinas de matrices…) | Sí, y es barato. GPL, se puede portar la tabla de hashes |
| **PPSSPP** | JIT del decodificador de vértices (una función nativa por formato) | Sí: generar PPC que lea vértices y escriba directo al FIFO de GX |
| **PPSSPP** | Auto-frameskip, "saltar efectos de búfer", calidad de splines, `compat.ini` por juego | Sí, todo |
| **PPSSPP** | Detección de esperas (`sceDisplayWaitVblank`, hilos dormidos) para no ejecutar nada mientras tanto | Sí; parte ya existe en el kernel HLE |
| **PPSSPP** | IR intermedio (MIPS→IR→nativo) con optimizaciones sobre el IR | Útil para el pase de optimización de la caché persistente |
| **Dolphin** | *Fastmem*: mapear la memoria emulada y atrapar fallos de página, reescribiendo el acceso lento solo donde falla (*backpatching*) | Posible en Broadway (§4.3), hay que investigarlo |
| **Dolphin** | *Idle skipping* de bucles de espera | Sí |
| **PCSX2** | Detección de bucles de espera (*wait loop*, *INTC spin*) | Sí, en el dynarec |
| **RPCS3** | Precompila todos los módulos PPU con LLVM la primera vez y guarda el resultado | Es la idea de la caché persistente |
| **Ryujinx** | PPTC: guarda en disco el código traducido de lo que se ejecutó (perfilado) | Es la otra mitad de la idea |
| **N64Recomp / XenonRecomp** | Recompilación estática a C y compilación con un compilador normal | El nivel máximo de la caché persistente (§4.5) |
| **Emuladores de N64 en PC** | Guardar la RAM con las palabras de 32 bits invertidas y corregir bytes y medias palabras con `addr ^ 3` / `addr ^ 2` | Alternativa al problema de los floats (§5.1) |

## 4. CPU

### 4.1 Dynarec

El diseño base está en `ARQUITECTURA.md` §5.2. Cambia una cosa: desde el
primer día el código generado tiene que poder **guardarse en disco**.

- Nada de direcciones absolutas del emulador dentro del código: todo va
  relativo a registros ancla (estado de la CPU, base de la RAM, tabla de
  funciones auxiliares).
- Los saltos entre bloques pasan por una tabla o se enlazan al cargar, nunca
  con la dirección fija de otro bloque.
- Cada bloque lleva su lista de relocalizaciones y el hash del código MIPS
  del que salió.

### 4.2 Invalidación sin coste por guardado

Comprobar en cada guardado si se pisa código traducido es caro. Los juegos de
PSP tienen que invalidar la caché de instrucciones al escribir código
(instrucción `cache`, `sceKernelIcacheInvalidateRange`/`All`) y al cargar o
descargar módulos. Con invalidar solo en esos puntos basta, como hace
PPSSPP. Un hash del bloque al reutilizarlo cubre los casos raros.

### 4.3 Fastmem en Broadway (investigar)

Hoy cada acceso enmascara la dirección. Broadway tiene MMU con tabla de
páginas. libogc mapea MEM1/MEM2 con BAT en `0x80000000`–`0xDFFFFFFF` y deja
libre la mitad baja del espacio de direcciones, justo donde vive la memoria
de la PSP (`0x08000000`, VRAM en `0x04000000`, espejo sin caché en
`0x48000000`).

Si se pueden mapear esas regiones con la tabla de páginas a la memoria real
en MEM2, la dirección de la PSP **es** la dirección del Wii:

```
lw   $t0, 16($a0)   →   addi r0, rA0, 16
                        lwbrx rT0, 0, r0
```

Dos instrucciones, sin máscara ni comprobación. Las páginas sin mapear
(registros de hardware, direcciones inválidas) producen un fallo DSI, y el
manejador reescribe ese acceso concreto a la vía lenta (*backpatching* de
Dolphin). Riesgos: convivir con libogc, el coste de la excepción y la
cobertura de la tabla de páginas. Antes de construir el dynarec encima,
hay que hacer una prueba aislada en Dolphin y en el Wii real.

### 4.4 Caché persistente de código

`sd:/wiisp/cache/<ID del juego>-<hash del EBOOT>.wjc`. No se toca la ISO.

1. **Descubrimiento estático** al primer arranque: desde el punto de
   entrada, las funciones exportadas y las **relocalizaciones del PRX**.
   Las relocalizaciones `HI16/LO16/32` que apuntan al segmento de código
   delatan punteros a funciones, que es lo que normalmente se escapa a un
   análisis estático.
2. **Perfilado** durante el juego: el código que el dynarec traduce y no
   estaba en el archivo se añade al cerrar (como el PPTC de Ryujinx).
3. **Carga**: índice por hash de bloque. Se carga lo más usado primero
   hasta llenar el presupuesto de código de MEM1; el resto se traduce al
   vuelo como siempre.
4. **Validación**: versión del traductor + hash. Si no coincide, se
   regenera. El archivo nunca es necesario para funcionar.

### 4.5 Traducción en PC

Una herramienta de PC toma el EBOOT descifrado y el perfil, y genera el
`.wjc` con optimizaciones que no caben en el Wii:

- **Nivel 1:** el mismo traductor del dynarec, con pases caros (registros
  vivos entre bloques, eliminación de código muerto, propagación de
  constantes sobre funciones completas).
- **Nivel 2:** traducir cada función MIPS a C y compilarla con el GCC de
  devkitPPC a `-O2` (estilo N64Recomp). El compilador planifica para el
  750CL mucho mejor que cualquier JIT. Requiere comprobar eventos e
  interrupciones en los bucles y una tabla para los saltos indirectos.

### 4.6 Reemplazo de funciones por hash

Se porta la tabla de PPSSPP: `memcpy`, `memset`, `strlen`, `strcpy`,
copias de matrices, etc., identificadas por el hash de su código. Se
sustituyen por versiones nativas (un `memcpy` con `lwz`/`stw` en vez de
bytes interpretados). Funciona igual con el intérprete y con el dynarec,
así que se puede hacer **antes** del dynarec.

## 5. VFPU y floats

### 5.1 El problema de los floats en LE

Opciones:

| Opción | Carga de float | Coste |
|---|---|---|
| A. Mantener LE (actual) | `lwbrx` + `stw` + `lfs` vía un búfer en la L1 bloqueada | 3 instrucciones + posible penalización |
| B. RAM con palabras invertidas | `lfs` / `psq_l` directo; bytes con `addr ^ 3`, medias palabras con `addr ^ 2` | Reescribir todo acceso del núcleo (GE, texturas, HLE); `lwl`/`lwr` y accesos desalineados más caros |
| C. Mixta | Opción A, pero los registros VFPU se quedan en FPR mientras se pueda | Solo se paga al entrar y salir de memoria |

Decisión: **C** primero, porque no rompe nada. Hay que **medir** cuánto
pesan los `lv`/`sv` en un juego real antes de plantearse B.

### 5.2 Paired singles

- Un vector de 4 se guarda en 2 FPR (`x,y` y `z,w`). `vadd.q` = 2
  `ps_add`; `vdot.q` = `ps_mul` + `ps_madd` + `ps_sum0`.
- `vmmul`/`vtfm` sobre matrices completas en FPR, con 32 FPR disponibles.
- `vrsq`, `vsin`, `vcos`: `frsqrte` + Newton, polinomios cortos. La
  exactitud bit a bit no es un objetivo.
- Prefijos (`vpfxs`/`vpfxt`/`vpfxd`) resueltos al traducir: casi siempre
  son constantes.

## 6. Gráficos

1. **JIT de vértices**: por cada formato de vértice del GE, generar PPC que
   lea el vértice y escriba al FIFO de GX. Los componentes `s8`/`u8` se
   convierten con `psq_l` cuantizado y escala por GQR, gratis. Los de 16
   bits y los floats necesitan inversión de bytes.
2. **T&L por hardware cuando se pueda**: sin skinning, proyección de forma
   estándar y luces que GX aproxima bien. Mundo × vista va a la matriz de
   posición de GX y la proyección a la de proyección. Si no encaja, se
   sigue con la transformación en CPU actual. Ahorra lo más caro del GE
   en Broadway.
3. **Frameskip**: automático (saltar el dibujo cuando se va tarde respecto
   al vblank real) y fijo. Los frames saltados procesan el estado del GE
   pero no dibujan ni presentan. Ojo: solo ahorra dibujo, no CPU emulada.
4. **Saltar efectos de framebuffer** (como PPSSPP): no copiar el
   framebuffer a la VRAM emulada cuando el juego no lo lee. El backend GX
   ya lo hace de forma perezosa, pero se puede forzar por juego.
5. **Stencil y test de color** (tarea #14).
6. **Calidad de curvas** ajustable (menos subdivisión en bezier/spline).

## 7. Audio

1. **Mezcla en el DSP**: las voces de sceSas y sceAudio se mezclan en el
   DSP (ASND de libogc mezcla PCM en el DSP; las voces VAG/ADPCM de la PSP
   necesitarían decodificarse antes o un microcódigo propio).
2. **Atrac3+ más barato**: el decodificador de FFmpeg (LGPL 2.1+, compatible
   con GPLv2+), con un modo que ignore las bandas altas: menos calidad y
   bastante menos CPU.
3. **Transcodificar la música una sola vez** (misma idea que la caché de
   código): el HLE de sceAtrac identifica cada pista por hash y, si existe
   `sd:/wiisp/cache/audio/<hash>.dsp`, la sirve ya decodificada en
   **DSP-ADPCM**, el formato nativo del Wii, que su acelerador decodifica en
   hardware. Lo genera la herramienta de PC o el Wii en segundo plano. Las
   radios de GTA son grandes: es opcional por juego.
4. Opción "sin música" como último recurso.

## 8. E/S y tiempos

- **Lecturas en un hilo aparte** (LWP de libogc): mientras Starlet lee la SD,
  Broadway sigue emulando. Es paralelismo real aunque Broadway tenga un
  solo núcleo. Encaja con `sceIoReadAsync`, que GTA usa para cargar la
  ciudad mientras juegas.
- **CSO**: descompresión zlib por bloques con caché; para máxima velocidad,
  ISO sin comprimir.
- **Detección de esperas** en hilos y bucles (`ARQUITECTURA.md` §5.2).

## 9. Orden de trabajo propuesto

| # | Qué | Por qué en este orden |
|---|---|---|
| 1 | ISO/CSO, KIRK, carga de módulos, sceUmd | Sin esto no arranca ningún juego objetivo |
| 2 | Reemplazo de funciones por hash + frameskip | Baratos y útiles ya con el intérprete |
| 3 | Prueba aislada de fastmem en Broadway | Decide el diseño de memoria del dynarec |
| 4 | Dynarec guardable | El gran salto de velocidad |
| 5 | VFPU con paired singles | GTA y casi todo el 3D |
| 6 | Audio: sceAudio, SAS en el DSP, Atrac3+ | |
| 7 | Caché persistente + herramienta de PC (nivel 1) | |
| 8 | JIT de vértices, T&L por hardware | |
| 9 | Recompilación a C (nivel 2), transcodificación de audio | El último 20 % |

## 10. Cómo medir sin tener los juegos aquí

- **imports.txt** de cada juego (con el EBOOT descifrado por PPSSPP):
  qué módulos y funciones usa.
- **Volcados de frames del GE de PPSSPP** (`.ppdmp`, desde el depurador del
  GE): contienen las listas de comandos y la memoria de un frame. Si WIISP
  aprende a reproducirlos, se puede probar el backend GX con frames reales
  de GTA o Silent Hill sin el juego. Son datos del juego: se usan en local,
  no se suben al repositorio.
