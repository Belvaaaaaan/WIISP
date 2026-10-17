# Hoja de ruta de rendimiento

Resumen de los documentos "Hoja de ruta de rendimiento – WIISP" y "Super
Optimizaciones – WIISP" que preparó el usuario (octubre de 2026, con una
analogía de cocina: el cocinero es el CPU del Wii, el amigo traductor el
intérprete, las tarjetitas la predecodificación, las hojas traducidas el
dynarec, el emplatador el GE emulado, el pintor GX y el refrigerador la
RAM de la PSP), más las correcciones acordadas después. Las cifras son
estimaciones de **un solo registro** (DBZ TTT ULUS10537, v0.5.6.1, Wii):
volver a medir tras cada paso.

## Reglas

1. El camino lento (el actual) nunca se borra: es la referencia.
2. Un interruptor por optimización, con el nombre de abajo, cambiable en
   vivo; línea `[OPT]` en wiisp.log con las activas.
3. Una optimización a la vez, medida en la misma escena (segundos de juego
   por cada 30 s reales).
4. Modos de verificación (hacerlo de las dos maneras y comparar) e
   imágenes de referencia cuando se pueda.
5. Contadores de acierto y coste en `[TIEMPOS]` para todo lo que "adivina"
   (caja, cachés, display lists, render-to-texture). Ganancia real =
   acierto × ahorro − coste de revisar (el LOD de Mario 64 de Kaze).
6. Primero "hacer lo mismo más barato" (resultado idéntico); después
   "adivinar qué me puedo saltar".

## Metodología acordada (octubre de 2026)

**Primero, medir (pasos 1 a 5):**

1. **Escena de prueba repetible** — hecho en la v0.5.7: guiones de botones
   grabados jugando en el Wii, huellas cada 60 cuadros y partidas de prueba
   (`docs/ESCENAS_DE_PRUEBA.md`). Dos escenas: una pelea de DBZ y un juego
   2D (Street Fighter Alpha 3 MAX o Metal Slug XX). Correr cada una dos
   veces: huellas y capturas idénticas; si no, algo depende del reloj del
   Wii en vez de los ciclos emulados.
2. **Separar la espera (0.1, `prof_espera`).** Ojo: en el Wii no hay espera
   real del vblank (sin limitador; con todos los hilos esperando, el kernel
   salta al siguiente evento), así que el 40 % de "syscalls" de los menús
   es trabajo real. Medir primero el tiempo por syscall.
3. **Interruptores** — hecho en la v0.5.7 (`src/core/opt.h`): `opt nombre
   0|1` en `autotest.txt`, `--opt` en el PC, línea `[OPT]` antes de cada
   `[TIEMPOS]`.
4. **Reconocer el Broadway:** escribir en wiisp.log DBAT0-7, HID4, SDR1 y
   GQR1-7 al arrancar, para saber si 2.5 (BAT) y 1.2b (`psq_l`) son posibles
   con esta libogc. Los PMC (0.2) esperan a cuando haga falta contar fallos
   de caché.
5. **Línea base:** las dos escenas tres veces en el Wii real (`veces 3`),
   guardando logs y capturas. La velocidad solo se mide en el Wii (Dolphin
   no emula los tiempos de caché); Dolphin sirve para la exactitud.

**Después, optimizar de una en una (pasos 6 a 11):**

6. 2.1 `cpu_sin_extra` — hecho en la v0.5.9 (abajo). Antes `step()` hacía
   `ld32(pc)` y un `switch` por instrucción. Poco código y poco riesgo; 2D de ~55 % a ~83 % según la
   estimación. Va antes que la fase 1 porque es casi gratis.
7. Medir el render-to-texture antes de hacerlo: registrar cada render
   target y quién lo consume (GE, CPU, pantalla): da el techo real de 1.1.
8. 1.2 + 1.2b: decodificadores especializados, verificados vértice a
   vértice contra el genérico.
9. 1.1: render-to-texture, con los 7 pasos de la hoja (resumidos en la Fase 1 de abajo).
10. 1.4 (charolas) y 1.3 (caja, solo con los BBOX del juego).
11. Decidir el diseño del IR y hacer 2.2 y 2.3 como IR, junto con 2.5
    (BAT). Las fases 3, 4 y 5 vienen después.

## Medido con el censo (v0.5.7.6, DBZ, dos reproducciones del mismo guion)

Peleas a ~7,5 % (las dos corridas a menos de 0,2 puntos). Cada 30 s reales:
267 texturas que salen de framebuffers tardan **10,2 s en decodificarse**
(~38 ms cada una, 172 MB) y 374 bajadas tardan 1,9 s. Todas las texturas que
caen en un framebuffer fallan por **formato**: el juego lee sus búferes de
480x272 (8888 y 565) y uno de 64x64 (4444) como texturas con paleta
(CLUT16/CLUT32), y ese formato iba por el decodificador genérico, texel a
texel. También lee el búfer de profundidad (0x04110000) como textura.

Primera optimización que sale de aquí: `ge_tex_clut` (abajo).

## Diagnóstico (DBZ TTT, v0.5.6.1)

| Escena | Velocidad | Dónde va el tiempo | Límite |
|---|---|---|---|
| Menús | 103,7 % | CPU 38-41 %; "syscalls" 40-44 % que es espera de vblank | nada |
| Escenas 2D | 46-63 % | CPU 80-86 %, ~10-11 MIPS | intérprete |
| Peleas 3D | 7-9 % | GE 37-45 %, texturas 30-36 %, framebuffers 7-8 %, CPU 16-20 % | GE y texturas |
| Peleas con turbo | ~34 % | CPU 91 %, ~12 MIPS | CPU |

Peleas: ~25.000 vértices por vblank, **37 % con huesos**, 0 % con índices,
~56 % de triángulos fuera de pantalla, ~2.500 llamadas de dibujo por imagen
(~20 vértices cada una), ~5,5 bajadas de framebuffer por imagen, ~280
texturas cambiadas y ~128 MB de hash cada 30 s. El intérprete gasta ~55
ciclos del Broadway por instrucción MIPS. Las escenas 2D piden ~16-25 M
instrucciones por segundo de juego, las peleas ~28-36 M.

## Correcciones acordadas al documento

- **Falta el skinning rápido** y va **antes que 1.2**: con 37 % de vértices
  con huesos, "leer vértices" (21-24 % del total) es sobre todo `ge_skin`
  con la aritmética exacta del GE (~25 vértices normales cada uno).
- Faltan también **matrices solo al cambiar** (`compute_transform_state`
  recalcula dos productos 4x4 exactos por llamada) y **color constante sin
  luces**: idénticas bit a bit, sirven también en modo exacto.
- 1.1 ya existe a medias: `texture_from_surface` (gx_ge.c) solo acepta
  coincidencia exacta de dirección y formatos simples.
- 2.5 (BAT) es arriesgada con los BAT de libogc, VRAM y scratchpad:
  después de 2.2, con respaldo `mem_bat`.
- Fase 5: comprobar primero el estado real de la salida de audio en el Wii.
- 3.3 (display lists) al final: las listas del GE llevan las matrices.

## Menú de optimizaciones (nombres de interruptor)

Hecha (v0.5.7.8): `ge_tex_clut`, texturas CLUT16/CLUT32 decodificadas de
una vez (paleta convertida una vez, índice con desplazamiento/máscara/offset
y tabla); idéntica bit a bit al decodificador genérico (tests/test_texclut.c,
400 casos). Ataca el ~34 % del tiempo de las peleas de DBZ.

Hecha (v0.5.8): `ge_skin_rapido`, solo con la matemática rápida (MR, el
modo GX normal): las matrices de los huesos se mezclan por peso en float y
se aplica una sola. En el PC, 10-19 veces más rápido por vértice con huesos;
diferencia con el exacto de 2e-5 de la suma de los términos (el redondeo de
float24), tests/test_skin.c con 600 vértices al azar. En DBZ "leer vertices"
era el 30 % de las peleas con el 47 % de vértices con huesos.

Hecha (v0.5.9): `cpu_sin_extra` (2.1). `cpu_run` en una sola función con
pc, npc, la RAM y un contador local de instrucciones en registros; el
estado global (pc, reloj, instrucciones) se pone al día solo antes de
llamar fuera (syscall, VFPU, memoria lenta, fallos) y la petición de parar
solo se mira después. Idéntica ciclo a ciclo al intérprete de antes, que
queda como `cpu_run_ref` (interp_ref.c, `cpu_sin_extra=0`):
tests/test_interp.c (3400 programas aleatorios en paralelo con los dos) y
las 275 pspautotests con huella en cada cuadro. En el Wii, un addiu pasa
de 54 a 24 instrucciones del Broadway (de 20 a 5 accesos a memoria), un lw
de ~66 a 29; bajo qemu-ppc, x2,2 MIPS. Queda el despacho por `switch`
(dos saltos indirectos para SPECIAL): eso es 2.2.

Hecha (v0.5.10): las texturas con paleta que leen framebuffers (la parte
de 1.1 que DBZ necesita), en tres interruptores: `ge_fb_relleno` (accesos
al relleno de las filas no bajan el búfer), `ge_rtt_gpu` (CLUT32 por alfa
desde una copia A8 del EFB y CLUT16 sobre 565 con índices CI8 de la copia
RGBA8, con TLUT 565 de GX; comprobación por primitiva de que no lee fuera
del búfer) y `ge_fb_hueco` (un búfer pequeño encima de unas filas de otro
no lo saca de la GPU). Según el censo de la 0.5.9.2, quita ~10 s de cada
30 s de pelea (bajadas, subidas y decodificación). Detalle en
docs/CONTINUAR.md.

Hecha (v0.5.11): `ge_z_rango` (la profundidad se vigila solo en los bytes
del bloque del swizzle: la textura pegada detrás del Z de DBZ ya no lo baja
en cada imagen, ~1,3 s de cada 30 s de pelea), `ge_mtx_cache` (las dos
multiplicaciones 4x4 exactas de cada llamada de dibujo solo cuando cambia
una matriz: casi todo el 11 % de "comandos") y `registro_diferido` (las
líneas de diagnóstico no fuerzan un fsync en la SD). Las tres idénticas
bit a bit.

Hecha (v0.5.11.1): 0.2 `prof_pmc` (PMC1-4 del Broadway por cubeta y fase
del GE). Hecha (v0.5.12): primera parte de 1.2, `ge_vdec_esp` (vértices
por lotes en MR, idénticos bit a bit: tests/test_vdec.c). Hecha (v0.5.13):
1.2 con decodificadores por formato para los de DBZ y `ge_tri_rapido`
(triángulos sin descarte de caras comprobados una vez). Hecha (v0.5.14):
1.2b `ge_vdec_psq` (psq_l para los componentes de 8 y 16 bits del lote),
`gx_envio_rapido` (el envío de cada triángulo al FIFO de GX) y
`ge_ensamblar_rapido` (descartes con banderas por vértice, sin copiar
vértices y el área una vez); el banco de qemu baja de 1694 a ~1150
instrucciones por triángulo. Todo idéntico bit a bit y comprobable en el
Wii con `ge_verificar`.

Hecha (v0.5.15): 1.4, primera parte, `ge_estado_sucio` (con el censo
`prof_estado` de la 0.5.14.1): el estado de cada llamada (formato,
transformación, rasterizador, estado de GX y la preparación de cada
triángulo) solo se rehace si un comando que lo lee cambió de valor; −2540
instrucciones por llamada en el banco de qemu, más libogc. Idéntica bit a
bit y comprobable en el Wii con `ge_verificar`. Juntar llamadas
(`ge_charolas`) va aparte, después y medido. También `prof_tirones` (los
vblanks que tardan mucho y en qué). Detalle en docs/CONTINUAR.md.

Hecha (v0.5.16): 3.5 `gx_paralelo`, primera parte (presentar sin esperar a
la GPU, con un testigo y tres XFB; la imagen sale un vblank después);
`registro_hilo` (wiisp.log desde otro hilo: los fsync ya no paran la
emulación); `prof_tirones` por imagen y relativo a lo normal; `prof_es`
(censo de las lecturas del disco, para decidir si leer en otro hilo sirve:
con la emulación determinista solo se puede esconder la espera emulada).
También la prueba de recarga (`--veces`), que encontró estado que pasaba
de un programa al siguiente (DMA, reloj, UID de LwMutex, uv y normal de
los vértices, botones del guion). Detalle en docs/CONTINUAR.md.

Primera tanda acordada (todas "más barato", verificables en el PC):
0.1 `prof_espera`, **skinning rápido** (`ge_skin_rapido`), **matrices al
cambiar** (`ge_mtx_cache`), **color constante** (`ge_color_const`), 2.1
`cpu_sin_extra`. Antes, la infraestructura de interruptores.

- **Fase 0 — Medir:** 0.1 espera separada en el profiler (`prof_espera`);
  censo del estado por llamada (`prof_estado`, para 1.4); tirones
  (`prof_tirones`); lecturas del disco (`prof_es`);
  0.2 contadores MMCR0/MMCR1 y PMC1-4 del Broadway y muestreo de SRR0
  (`prof_pmc`); 0.3 contar instrucciones PPC por instrucción MIPS con
  objdump (objetivo 15-25 hoy, 6-10 predecodificado).
- **Fase 1 — GE:** 1.1 render-to-texture en la GPU (`ge_rtt_gpu`: lista
  de framebuffers vivos por rango, GX_CopyTex sin GX_DrawDone, guardar el
  EFB al cambiar de destino, formatos, atrapar lecturas del CPU por
  páginas, versión en vez de hash, copias a media resolución; empezar
  midiendo quién consume cada render target); 1.2 decodificadores de
  vértices por vtype (`ge_vdec_esp`); 1.2b psq_l y GQR para 8/16 bits
  (`ge_vdec_psq`); 1.3 caja envolvente, primero con los BBOX del juego
  (`ge_bbox`; ojo: con huesos los vértices salen de la caja); 1.4 agrupar
  llamadas con dirty flags (`ge_charolas`).
- **Fase 2 — Intérprete cacheado:** 2.1 sin coste por instrucción
  (`cpu_sin_extra`); 2.2 predecodificación con computed goto, tarjetas de
  8 bytes (`cpu_predec`); 2.3 especializar y fusionar (`cpu_fusion`); 2.4a
  camino HLE mínimo para CpuSuspendIntr/ResumeIntr y Memcpy pequeño
  (`hle_rapido`); 2.4b reemplazo de funciones por hash (`hle_reemplazo`);
  2.5 RAM de la PSP en 0x08000000 con BAT (`mem_bat`). Construirla como
  intérprete de IR al estilo de PPSSPP Core/MIPS/IR/ (IRInst de 8 bytes),
  base del dynarec.
- **Fase 3 — GX transforma:** 3.1 transformación por hardware
  (`gx_transform`, PNMTXIDX para un hueso, respaldo por juego); 3.2 caché
  de vértices convertidos (`gx_vcache`); 3.3 display lists (`gx_dlcache`);
  3.4 paletas como TLUT (`gx_tlut`); 3.5 CPU y GPU en paralelo
  (`gx_paralelo`, vigilar "esperar GX").
- **Fase 4 — Dynarec:** 4.1 emisor (`jit`), 4.2 encadenamiento y pila de
  retorno (`jit_enlace`), 4.3 lockstep contra el intérprete
  (`jit_lockstep`), 4.4 traducción nativa (`jit_nativo`), 4.5a registros
  fijos (`jit_regs`), 4.5b caché bloqueada (`cache_lock`), 4.6a idle loops
  (`jit_idle`), 4.6b backpatching por DSI (`jit_fastmem`), 4.7 bloques en
  la SD (`jit_sd`).
- **Fase 5 — DSP:** 5.1 salida con libaesnd (`dsp_salida`), 5.2 voces de
  sceSas en el DSP (`dsp_sas`), 5.3 ATRAC3plus solo si se mide.

## Estimación del documento (peleas DBZ, promedio / máximo)

Hoy 7 % → fase 1 ~17 % / 24 % → fase 2 ~24 % / 46 % → fase 3 ~37 % /
100 % → fase 4 ~45 % / 100 % con margen. Escenas 2D a 100 % desde 2.2.
Hay "dos puertas angostas": texturas/framebuffers y GE; arreglar solo una
deja las peleas en ~16 %.

## Fuentes y licencias

PPSSPP (GPLv2+, portable con aviso), PCSX-ReARMed new_dynarec (GPLv2+),
Wii64/Not64 (GPLv2+), Dolphin (GPLv2+), N64Recomp (MIT), libogc/libaesnd
(tipo BSD), sodium64 (GPLv3: solo la idea).
