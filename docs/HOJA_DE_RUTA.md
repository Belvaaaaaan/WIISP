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
10. 1.4 (charolas) y 1.3 (caja, solo con los BBOX del juego). En la
    0.5.21, el censo de 1.3 (`prof_bbox`) y `ge_cmd_puntero`; en la
    0.5.22, 1.3 a (`ge_bbox_rapido`), 1.3 b (`ge_bbox`) y
    `ge_cmd_matrices`; en la 0.5.23, 1.3 c (`ge_bbox_pantalla`), 1.3 d
    (`ge_bbox_llamada`) y el censo `prof_huesos`; en la 0.5.23.1, la
    prueba de cada llamada más barata (`ge_bbox_orden`, `ge_bbox_aabb`);
    en la 0.5.23.2, la banda de guarda (`ge_bbox_guarda`) y el repaso por
    triángulos (`ge_bbox_triangulos`, apagado desde la 0.5.23.3: en DBZ
    cuesta más de lo que salta); en la 0.5.23.3, las rachas de huesos
    (`ge_cmd_huesos`); en la 0.5.23.4, 1.4 (`ge_charolas` y los suyos) y
    `ge_clut_igual`.
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

Hecha (v0.5.17): 2.2 `cpu_predec`, el intérprete predecodificado: bloques
de tarjetas de 8 bytes con computed goto, reloj exacto por bloque e
invalidación por zonas de 1 KB, con `cpu_verificar` para comprobarla en el
Wii. Idéntico ciclo a ciclo al intérprete de antes (tests/test_interp.c,
pspautotests A/B). En el banco de qemu, de 28,7 a 17,4 instrucciones del
Broadway por instrucción de la PSP (addiu de 24 a 10, lw de 29 a 16).
Detalle en docs/CONTINUAR.md.

En el Wii (0.5.17) apenas ganó: 20-27 % menos instrucciones pero los mismos
ciclos (fallos de caché ×4-6, `mtctr` serializado, paso entre bloques de
~24 ciclos). v0.5.17.1: medidas de la CPU en el Wii (`prof_pmc_l2`,
`prof_pmc_ctr`, `prof_pmc_carga`, la CPU por dentro) y la memoria del
predecodificado (`predec_compacto`, `predec_sin_unir`, `predec_mem1`);
banco de caché con cachegrind (`tools/banco/medir_cache.sh`).
v0.5.17.2: `predec_sin_unir` apagado por defecto; las tarjetas solo se
cuentan al medir (`prof_cpu`); `prof_bloques` (instrucciones por bloque
ejecutado y qué cae al intérprete de siempre) y `prof_ventana_juego`
(informes cada N vblanks de juego). Detalle en docs/CONTINUAR.md.

Hecha (v0.5.18): 2.6, primera parte, `cpu_vfpu_tarjetas`: tarjetas propias
para lv.s, sv.s, lv.q, sv.q, mtv y mfv (43 % de las instrucciones VFPU de
DBZ), idénticas bit a bit a `vfpu_exec` y comprobables en el Wii con
`cpu_vfpu_verificar`. En el banco de qemu, de 164 a 45 instrucciones del
Broadway por instrucción VFPU. Siguen la aritmética simple (b) y los
paired singles (c).

Hecha (v0.5.18.1): 2.6 b, `cpu_vfpu_arit`: tarjetas para vadd, vsub, vmul,
vmov, vabs, vneg, vmin, vmax, vscl, vi2f, vf2i y vpfx sin prefijos, con la
misma cuenta que los caminos rápidos de `vfpu_exec` (funciones compartidas
en `vfpu.h`). Banco de qemu: de 263 a 150 instrucciones por instrucción
VFPU. Sigue la parte c (paired singles).

Hecha (v0.5.19): 2.3a, primera parte, `cpu_enlace` (la tarjeta del salto
elige el bloque siguiente; cierre de ~14 instrucciones con una sola carga
dependiente) y `cpu_syscall_slot` (los stubs de las importaciones sin el
intérprete de siempre). Banco de qemu: de 17,5 a 15,4 instrucciones del
Broadway por instrucción de la PSP. Pendientes: quitar los `bdnz` del bucle
(`no-branch-count-reg`) y una pila de retornos para `jr ra`.

Hecha (v0.5.19.1): sin `bdnz` en el bucle (neutro en el Wii) y dos medidas:
la pila de retornos simulada (acertaría el 99,7 % de los `jr ra`, pero son
~0,5 %) y `prof_vertices` (leer vértices por partes: pasar a pantalla y
transformar, el 18 % de la pelea).

Hecha (v0.5.20): 1.2c `ge_pantalla_rapida`: en el camino por lotes, un
vértice con x, y en [0, 4096) y z en [0, 65536) pasa a pantalla sin
comprobar rangos (nada sale fuera ni se acota), con una conversión a
entero por coordenada, el offset en float una vez por tanda y las
banderas de recorte sin mirar el rango; el resto, como siempre. Idéntico
bit a bit (tests/test_vdec.c, también en los bordes) y comprobable en el
Wii con `ge_verificar`. Banco de qemu: de 346,7 a 252,2 instrucciones por
vértice (uv16 c4444 p16), unas 90 menos también con huesos.

Hecha (v0.5.21): paso 10 empezado por 1.3. El GE ya emula BBOX y BJUMP
(lo que el juego descarta con sus cajas ya se salta); antes de programar
`ge_bbox`, el censo `prof_bbox` (solo mide: BBOX y su costo, lo que saltan
los BJUMP, los objetos que pasan la caja y no dibujan nada, con y sin
huesos, y las llamadas sin caja que no dibujan nada). Y, de las medidas,
`ge_cmd_puntero`: los comandos de la lista leídos de la RAM con un puntero
y los de estado (229 de 256, sin `case` en `execute`) aplicados en el
propio bucle; idéntico (tests/test_listas.c, pspautotests A/B, A/B del
estado de GX) y comprobable en el Wii con `ge_verificar`. Banco de qemu:
leer y ejecutar los comandos de una llamada de 14 comandos, de 1469 a 679
instrucciones. Detalle en docs/CONTINUAR.md.

Hecha (v0.5.22): 1.3 a `ge_bbox_rapido`: la prueba de las BBOX (la que
decide los BJUMP) con el mismo resultado bit a bit, más barata (matrices
combinadas guardadas, solo la posición de cada vértice, el conteo cortado
en cuanto cada plano tiene uno dentro); 1.3 b `ge_bbox`: si la caja de una
BBOX que la prueba de siempre da por visible está entera fuera de un mismo
plano de recorte (prueba exacta con margen), las llamadas siguientes con
las mismas matrices y todos los vértices dentro de la caja no se calculan
(cada triángulo suyo se descartaría), y lo que dejan a la siguiente
llamada se calcula igual; y `ge_cmd_matrices`: los datos y números de
matrices aplicados en el bucle de comandos. Idénticos (tests/test_bbox.c,
tests/test_listas.c, pspautotests A/B) y comprobables en el Wii con
`ge_verificar`. Medida nueva: `prof_comandos` (comandos por tipo). Banco
de qemu: una caja de 8 vértices, ~1200 instrucciones menos; los objetos
que la prueba de siempre deja pasar y no se ven, de 14256 a 7167
instrucciones por llamada; leer y ejecutar los comandos de una llamada con
4 huesos, de 7371 a 4686. Detalle en docs/CONTINUAR.md.

Hecha (v0.5.23): 1.3 c `ge_bbox_pantalla`: la prueba exacta de la caja
mira también la pantalla. Es un programa lineal sobre la parte de la caja
que el GE puede dibujar (delante del plano cercano con recorte de
profundidad, w ≥ 0 sin él), con el umbral exacto de `range_of` y márgenes
de 2^-9 y 2^-11. Rescata las cajas que la prueba de siempre deja pasar
porque cruzan el plano de la cámara o el cercano. 1.3 d `ge_bbox_llamada`:
tras una caja que no está entera fuera, cada llamada se prueba con su
propia caja (primero su primer vértice). La prueba de cada llamada no
compara las matrices ni la pantalla si no cambiaron (grupos B y P de
`ge_sucio`; la del BJUMP sigue con `memcmp`). Medida nueva: `prof_huesos`
(datos de huesos repetidos, matrices iguales, rachas de 0x2A/0x2B). Arreglo
de `ge_setup_recip` (índice negativo con `abs_det ≥ 2^48`). Idénticos
(tests/test_bbox.c con seis pruebas nuevas, 51 mutaciones, pspautotests
A/B, A/B del estado de GX, la 0.5.22 contra la 0.5.23) y comprobables en
el Wii con `ge_verificar`. Banco de qemu: cajas finas que cruzan la cámara
fuera de la pantalla, de 144723 a 8374 instrucciones por llamada; llamadas
fuera dentro de una caja que se ve, de 22165 a 11088; las que se ven,
+260 por llamada. Pendiente, sin arreglar: un vértice con la pantalla en
NaN no se marca como fuera de rango (`round_to_screen`). Detalle en
docs/CONTINUAR.md.

Hecha (v0.5.23.1): la prueba de cada llamada más barata, con el mismo
resultado. `ge_bbox_orden`: los planos ±w antes que la pantalla, cada
esquina solo contra los planos que siguen dejando todo fuera, y los bordes
de la pantalla solo cuando hacen falta. `ge_bbox_aabb`: la caja de las
posiciones en registros. Banco de qemu, llamadas enteras fuera de un
plano (como DBZ): de 10371 a 8388 instrucciones por llamada; la prueba,
de ~3900 a ~1850. Idéntica (cada ronda de las pruebas al azar, con y sin
los dos interruptores; 23 mutaciones, 22 detectadas y una equivalente;
A/B; la 0.5.23 contra la 0.5.23.1). Siguen, por este orden: 0.5.23.2
(llamadas que cruzan el plano de la cámara), 0.5.23.3 (rachas de huesos)
y 0.5.23.4 (1.4 y el diseño del IR).

Hecha (v0.5.23.2): rescatar las llamadas que cruzan el plano de la
cámara. `ge_bbox_guarda`: la prueba de pantalla de la caja con las
restricciones que cumple todo vértice que el GE dibuja (la banda de guarda
y, sin recorte, el rango z), con un certificado (multiplicadores del dual
del programa lineal) comprobado con margen. Solo cambia algo cuando lo
visible de la caja se sale muy lejos y da la vuelta, o con una escala z
que no cubre el frustum. `ge_bbox_triangulos`: si la caja de una llamada
no la salta, se repasan sus posiciones triángulo a triángulo con las
reglas de descarte del GE (un vértice detrás de la cámara o fuera de la
banda descarta el triángulo entero, y eso la caja no lo ve). Idéntico:
pruebas con un vértice en cada límite, 36 mutaciones (25 detectadas, el
resto de margen, equivalentes o solo de poder), A/B, 0.5.23.1 contra
0.5.23.2. Censo nuevo de por qué no se saltan las llamadas que no dibujan
nada. Siguen 0.5.23.3 (rachas de huesos) y 0.5.23.4 (1.4 y el IR).

Resultado de la 0.5.23.2 en el Wii: imagen igual y verificación con 0
distintas, pero la banda no salta nada en DBZ y el repaso por triángulos
cuesta más de lo que salta (peleas +0,5 a +1,0 % de ciclos): se apaga
`ge_bbox_triangulos` por defecto.

Hecha (v0.5.23.3): las rachas de huesos más baratas. `ge_cmd_huesos`: los
0x2A/0x2B seguidos (57 % de los comandos de la pelea) se ejecutan en un
bucle propio, con el mismo estado que uno a uno, hasta otro comando, la
parada o el final de la RAM. De ~62 a ~15 instrucciones por comando de
hueso; banco con 8 huesos, −7,2 % por llamada. Mutaciones con
`tools/banco/mutar.py` (rápidas, mismo resultado). Sigue 0.5.23.4 (1.4
`ge_charolas` y el diseño del IR).

Hecha (v0.5.23.4): 1.4, juntar llamadas. `ge_charolas`: los destinos, la
textura y el estado de GX de una llamada valen para la siguiente mientras
no cambie nada de lo que leen (clave: el estado del rasterizador, la
paleta y los búferes); la textura se vuelve a comprobar con lo mismo que
`tc_lookup`. `ge_charolas_sprites`: también entre sprites.
`ge_charolas_envio`: los triángulos de una llamada con un solo `GX_Begin`.
`ge_charolas_tiras`: las tiras en un bucle propio. `ge_charolas_ge`: el
formato de vértice por puntero. `ge_clut_igual`: un LOADCLUT con la misma
paleta no la da por cambiada. `ge_estado_clave`: la clave del estado de GX,
después de emitirlo. Banco de qemu: llamada como la pelea de DBZ
−24 %, sprites de uno en uno −36 %, textura y paleta en cada llamada −31 %.
Idéntico (banco al azar con la libogc que guarda el estado y la huella del
FIFO, que encontró un fallo de verdad antes de entregar; mutaciones del GE
y de GX; A/B; la 0.5.23.3 contra la 0.5.23.4). El diseño del IR pasa a la
versión siguiente. Candidatos vistos: los vértices de los sprites por el
decodificador por lotes (~370 instrucciones por vértice por el genérico) y
una charola parcial (solo la textura) si en DBZ pesa "con otra clave".

Resultado de la 0.5.23.4 en el Wii: la pelea −9 a −10 % de ciclos frente a
la 0.5.23.3 (de 44 a 49-51 % de velocidad), imágenes iguales y verificación
a 0.

Hecha (v0.5.23.5): `ge_ci8_rapido`, los índices CI8 de las texturas sobre
framebuffer (~3,6 % de la pelea en la 0.5.23.4) por palabras y de dos en
dos: de 13,4 a 8,0 instrucciones por texel y de 4 a 0,75 accesos a memoria,
mismos bytes; el informe separa lo que esperan a la GPU. Censo
`prof_repetidos`: ¿cuántos vértices de la pelea se calculan dos veces en el
mismo cuadro (los mismos datos, huesos y matrices), o con los mismos datos y
huesos y otra cámara? Decide si hay una caché de vértices transformados o de
huesos que valga la pena. Sigue: según el censo, esa caché; si no, el diseño
del IR.

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
  (`ge_bbox_rapido`, la prueba de siempre más barata; `ge_bbox`, saltarse
  las llamadas de cajas enteras fuera; `ge_bbox_pantalla`, también fuera
  de la pantalla; `ge_bbox_llamada`, la caja de cada llamada; `ge_bbox_guarda`, la banda de
  guarda; `ge_bbox_triangulos`, el repaso por triángulos; ojo: con
  huesos los vértices salen de la caja; censos `prof_bbox` y
  `prof_huesos`); 1.4 agrupar llamadas con dirty flags
  (`ge_charolas`); comandos de la lista con un puntero (`ge_cmd_puntero`)
  y los datos de matrices en el bucle (`ge_cmd_matrices`; censo
  `prof_comandos`).
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
