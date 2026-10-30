# Banco de pruebas del GE en qemu

Herramientas para medir y verificar el camino del GE con el código que
genera el compilador del Wii (devkitPPC), sin el Wii. Necesitan el entorno
de `tools/setup_entorno.sh` (`ENTORNO`, por defecto `../wiisp-entorno` junto
al repositorio), `gcc-powerpc-linux-gnu` y `qemu-user`. Las salidas van a
`SALIDA` (por defecto `/tmp/banco`).

- `build_banco.sh`: compila gx_ge.c y ge_vertex.c con devkitPPC y los
  enlaza con el resto (gcc de Linux para PowerPC), `stub.c` (libogc de
  mentira) y `bench_gx.c` (tiras de 20 vértices como las de DBZ).
  `qemu-ppc $SALIDA/bench REPETICIONES VTYPE opcion=valor...`.
  Con `qprof.py` (300 y 600 repeticiones, 6000 vértices de diferencia),
  `batch_range` por vértice en la 0.5.20: 0x11A 252, 0x4301 325, 0xC301
  383, 0x1C301 503 (con `ge_pantalla_rapida=0`: 352, 425, 484, 604; la
  0.5.19.1: 347, 415, 474, 593).
- `bench_lista.c` + `bench_cmd.c` (también los compila `build_banco.sh`):
  llamadas de dibujo como las de sceGu por el bucle de comandos de `ge.c` y
  `ge_raster.c` compilados con devkitPPC. `qemu-ppc $SALIDA/bench_lista
  REPETICIONES VTYPE ESCENA opcion=valor...`: escena 0 = VERTEXTYPE, BASE,
  VADDR y PRIM; 1 = más 10 comandos de textura con paleta; 2 = más 4 huesos
  y la matriz de mundo; 3 = con 8 huesos; 4 = objetos de 4 llamadas con su
  caja (BBOX de 8 vértices s16 y BJUMP, como sceGuBeginObject) y su matriz
  de mundo, que se ven; 5 = los mismos más allá del plano lejano (la prueba
  de siempre los deja pasar y no dibujan nada: los salta `ge_bbox`); 6 =
  los mismos fuera de la pantalla por un lado (el BJUMP salta sus
  llamadas). Con el viewport al doble (los planos de ±w fuera del
  scissor; 8 y 9 con sus propios vértices de tipo 0x11A): 7 = cajas finas
  en diagonal que cruzan la cámara, fuera de la pantalla, con recorte de
  profundidad (las salta `ge_bbox_pantalla`); 8 = una caja grande que se
  ve con las llamadas fuera de la pantalla y dentro de ±w (las salta
  `ge_bbox_llamada`); 9 = la misma con las llamadas dentro (lo que cuesta
  probar la caja de cada llamada); 10 (viewport ×1, vértices 0x11A) = las
  llamadas de una caja que se ve, enteras fuera del plano x− (como DBZ).
  `LUCES=1` enciende dos luces; con `prof_estado=1` escribe el
  censo de estado; al final, una línea con las cuentas de `ge_bbox`. Corre
  la lista con el mismo bucle que `process_dl_queue_body` (con y sin
  `ge_cmd_puntero`). Leer y ejecutar los comandos por llamada (bucle,
  `mem_valid`, `mem_read32`, `execute` o `run_commands_ptr`), 0.5.21 sin /
  con `ge_cmd_puntero`: escena 0 399 / 264, 1 1469 / 679, 2 9392 / 7107, 3
  15856 / 12375. 0.5.22 (con `ge_cmd_puntero`): escena 0 256, 1 645, 2 y 3
  sin / con `ge_cmd_matrices` 7371 / 4686 y 12535 / 7890. Escenas 4, 5 y
  6 en la 0.5.22, instrucciones por llamada en total (sin el psq_l emulado
  ni lo que hace el propio banco; `qprof.py` con 0 funciones), sin
  `ge_bbox_rapido` ni `ge_bbox` / con `ge_bbox_rapido` / con los dos: 4:
  20988 / 20686 / 20787 (la prueba exacta de una caja que se ve, ~400 por
  caja: casi todo la segunda comparación de matrices); 5: 14256 / 13955 /
  7167; 6: 1270 / 988 / 988. Una caja de 8 vértices, ~1200 instrucciones
  menos con `ge_bbox_rapido` (la matriz de mundo cambia en cada objeto).
  0.5.23, contando todo menos el psq_l emulado, solo `ge_bbox` / con
  `ge_bbox_pantalla` / con `ge_bbox_llamada` / con los dos: 4: 29262 /
  29198 / 29457 / 29520; 7: 144723 / 8374 / 145799 / 8374; 8: 22165 /
  22602 / 22850 / 11088; 9: 29203 / 29640 / 29399 / 30382.
  0.5.23.1, total menos `on_sigill`, `__scalbn` y `ldexp`, sin / con
  `ge_bbox_orden` y `ge_bbox_aabb`: 4: 22313 / 22313; 7: 8130 / 8059; 8:
  10925 / 10159; 9: 23215 / 23047; 10: 10371 / 8388.
  0.5.23.2, igual, sin / con `ge_bbox_guarda` y `ge_bbox_triangulos`: 4:
  22341 / 23161; 7: 8111 / 8130; 8: 10432 / 10659; 9: 23134 / 24331; 10:
  8485 / 8515 (ninguna escena tiene llamadas que crucen la cámara: solo el
  coste).
- `mutar.py MUTACIONES [--solo suite,suite] [--hilos N] [--asan] [--ppc] [--dkp]`:
  pruebas de mutaciones rápidas. Compila una vez los objetos de las
  pruebas del PC (-O2, sin sanitizers; se rehacen solos si cambia un `.c`
  o un `.h`), y por cada mutación recompila solo el archivo mutado,
  enlaza y corre las suites de `--solo` con `--parar` (sale en el primer
  fallo), varias a la vez y cada una en su carpeta (algunas pruebas
  escriben archivos con nombre fijo). Las que sobreviven se repasan con
  todas las suites y, con `--asan` o `--ppc`, también con ASan o en
  PowerPC bajo qemu. Así el resultado es el mismo que corriendo todo con
  ASan para cada mutación, en una fracción del tiempo. Formato del
  archivo de mutaciones (texto exacto, una sola vez en el archivo):
  `=== nombre`, `@ archivo`, `<<<`, antes, `>>>`, después, `===`. Las de
  cada versión se guardan en `tools/banco/mutaciones/`. Una mutación que
  deja la lista dando vueltas cuenta como colgada (detectada); el límite
  es 10 veces lo que tardan las suites sin mutar (30 s como poco). Con
  `--dkp`, lo que compila `build_tests_dkp.sh` va con devkitPPC y
  `-DGEKKO` y las pruebas corren en `qemu-ppc -cpu 750` con
  `sigill_ppc.c`: para el código que solo existe en el Wii (los paired
  singles de `ge_vdec_ps`).
- `bench_charolas.c` (lo compila `build_banco.sh`) + `banco_charolas.sh
  SEMILLAS LLAMADAS "OPCIONES_A" "OPCIONES_B" [fifo]`: listas al azar por el
  bucle de `ge.c` y el renderizador de GX sobre `stub_estado.c`, con
  cambios entre llamadas (destinos, máscaras, LIGHTMODE, textura, paleta,
  sombreado, mezcla, tipo de vértice, primitiva; `CAMBIOS=N` en el entorno:
  N % de llamadas con algún cambio) y entre tramos de la lista (textura
  escrita con y sin aviso, la que está en uso sin aviso y a veces con otro
  periodo, periodo nuevo, aviso de vaciar la caché). Texturas en la RAM y
  en la VRAM (dentro y fuera de un búfer en el que se dibuja; la del
  principio, al azar por semilla), llamadas que no dibujan (máscara de
  color entera y sin escribir profundidad) y sprites de ancho cero.
  `REGISTRO=archivo`: el registro del emulador (las líneas `[VERIF]`). Con
  `ge_verificar=1` escribe una línea `VERIF` con lo comparado/distinto de
  cada verificación; `banco_charolas.sh ... verif` corre A y B con
  `ge_verificar` (que cambia lo que se emite: hay que comparar verificado
  con verificado) y exige 0 distintos.
  Compara la huella del estado de GX, la del FIFO, los contadores del GE y
  los de la caché de texturas entre dos juegos de interruptores (`fifo`:
  sin el número de `GX_Begin`, para `ge_charolas_envio`). Encontró un
  fallo de verdad en la primera versión de `ge_charolas` (ver CONTINUAR).
- `stub_estado.c` con `gx_ge.c` compilado con `-DWIISP_FIFO_TRAZA`: la
  línea `[GX]` añade la huella del FIFO (cada triángulo con el estado de su
  `GX_Begin`, igual con un `GX_Begin` por triángulo o uno para muchos;
  sprites, líneas y puntos enteros; las primitivas de `blit`, por su tipo y
  número de vértices). `build_cli_gx.sh` lo compila así.
- `mutar_gx.py MUTACIONES [--semillas N] [--hilos N]`: mutaciones del
  camino de GX (`gx_ge.c`, `texcache.c`, `memory.c`, `ge_vertex.c`,
  `ge_raster.c`, `ge.c`) contra el banco de las charolas: recompila solo el
  archivo mutado (con devkitPPC o con el gcc de PowerPC), enlaza
  `bench_charolas` y lo compara, semilla a semilla, con el banco sin mutar
  y las charolas apagadas (el camino de siempre), con `CAMBIOS` 3, 5, 10 y
  40 (40 casos con `--semillas 10`, ~3 min con 2 hilos). Con `--verif`,
  lo que vería `ge_verificar` en el Wii: mutación y referencia con
  `ge_verificar=1` (la referencia, sin mutar y con los interruptores por
  defecto); detectada si la línea `VERIF` del banco tiene algo distinto o
  lo demás no sale igual.
- `bench_lista` escena 11: como una pelea de DBZ (4 huesos y la matriz de
  mundo en cada llamada, la textura una sola vez); escena 12 (vtype 0):
  sprites en 2D de uno en uno con la textura una sola vez (un juego 2D, el
  texto). Instrucciones por llamada de la 0.5.23.4: ver CONTINUAR.
  Ojo al medir: el número de argumentos cambia la alineación de la pila y
  con ella lo que cuestan algunas copias (escena 12: 2.798 sin argumentos,
  2.778 con cualquiera); para comparar, pasar siempre los mismos
  interruptores, con su valor.
- `bench_ci8.c` (lo compila `build_banco.sh`; las funciones con devkitPPC):
  los índices CI8 de `ge_rtt_gpu` desde la copia RGBA8 de un 565 de
  480x272, con la conversión de siempre o con la de `ge_ci8_rapido`.
  `qemu-ppc -cpu 750 $SALIDA/bench_ci8 REPETICIONES [palabras] [desplazamiento
  máscara]` (`-cpu 750`: `dcbz` de 32 bytes); con `qprof.py` sobre dos
  corridas, instrucciones por texel. 0.5.23.5, de siempre / nuevas:
  desplazamiento 5 y máscara FF (DBZ) 13,4 / 8,0; 0 y 1 (con el rojo) 16,4 /
  10,8; 20 y FF 13,4 / 8,9.
- `mutar.py` con una cabecera (`@ src/wii/gx_tiles.h`): recompila los `.c`
  de las pruebas que la incluyen con la copia mutada delante.
  `--solo-estas a|b` si los nombres llevan comas.
- `qprof.py`: instrucciones PowerPC por función (y por unidad) a partir de
  `qemu-ppc -d in_asm,exec,nochain -D grande.log` menos una corrida corta:
  `qprof.py grande.log corta.log UNIDADES [FUNCIONES]` (lista las 25 que
  más gastan, o todas con 0; el total de la primera línea es siempre el de
  todas). `qtb.py` lo mismo por bloque de una función.
- `qlines.py`: lo mismo por bloque con la función y la línea de código
  (`addr2line`; el banco se compila con `-g`), para repartir una función
  entre lo que tiene en línea. `qcallers.py`: quién llama a una función
  (memcpy, memset...), por el bloque que corre antes de entrar.
- `sigill_ppc.c`: emula con SIGILL lo que un PowerPC normal no tiene o es
  privilegiado (mfmsr/mtmsr, mfspr/mtspr con los GQR, psq_l y psq_lu,
  psq_st y psq_stu con W = 0 y 1) y las instrucciones de paired singles
  (opcode 4: sumas, productos, `ps_madd`, `ps_madds0/1`, `ps_muls0/1`,
  `ps_sum0/1`, merges, `ps_mr`, `ps_neg`, `ps_abs`, `ps_nabs`, `ps_div`,
  `ps_sel`). ps1 va en una sombra; cada carril con la operación escalar
  de precisión simple (lo que `prueba_ps` comprobó en el Wii). `psq_st` de
  float convierte un denormal en cero (como Dolphin) y lo cuenta, igual
  que los merges que mueven un denormal. `PSQ_INFORME=1` dice cuántos
  psq_l y paired singles emuló. Correr con `qemu-ppc -cpu 750` (sin
  AltiVec: con otra CPU, algún opcode 4 podría ser una instrucción
  AltiVec válida en vez de dar SIGILL). En el perfil de `qprof.py`, la
  instrucción emulada cuenta en su función (está en el bloque traducido),
  y el manejador aparte (`on_sigill`, `lane_a`, `dequant`...).
- `build_tests_dkp.sh`: las pruebas del PC con ge_vertex.c, test_gxemit.c,
  interp.c y predec.c
  compilados por devkitPPC; correr `qemu-ppc -cpu 750 $SALIDA/tests-dkp`
  desde la raíz del repositorio. Huella de test_vdec con devkitPPC:
  B3A2BCD3.
- `build_cli_gx.sh` + `stub_estado.c`: el CLI dibujando por gx_ge.c
  (compilado con devkitPPC) sobre una libogc de mentira que guarda el
  estado de GX: al salir escribe `[GX] N dibujos, huella X`, la de todo el
  estado de GX en cada dibujo. Con `ab_opt.py` (CLI = un guion que llame a
  `qemu-ppc $SALIDA/wiisp-cli-gx`) compara dos versiones del renderizador
  de GX en todas las pspautotests.
  Los punteros se numeran por orden de aparición (la huella no depende de
  dónde caiga cada cosa en el montón). Con `GX_TRAZA=1` escribe por stderr
  cada evento y, en cada dibujo, la huella de cada grupo de funciones: con
  diff entre dos versiones se ve en qué dibujo y qué función cambia.
- `ab_versiones.py`: dos `wiisp-cli-gx` (por ejemplo del commit anterior,
  con `git worktree`) en las pspautotests: misma salida y misma huella de
  GX. Con `--a OPCION=V` y `--b OPCION=V`, el mismo CLI con interruptores
  distintos; con `--fifo`, de la línea `[GX]` solo la huella del FIFO; con
  `--sin-fifo`, sin ella (contra un CLI de antes de la 0.5.23.4). `gpu/exact/pixel` lee memoria que la GPU de mentira no escribe (los
  destinos de `GX_CopyTex`/`GX_CopyDisp`): desde la 0.5.23.2 el script
  corre qemu con `MALLOC_PERTURB_=165` y sale igual en las dos.
- `gevert.sh DIR_BANCO [opcion=valor...]`: instrucciones por vértice
  (total, `batch_range` y el núcleo de `ge_vdec_ps`) en el banco del GE
  para los formatos de DBZ. 0.5.23.7, `batch_range` más el núcleo, con
  `ge_vdec_ps`: 0x4301 282, 0xC301 322, 0x1C301 402 (sin: 324, 383, 502;
  0x11A sigue en 252).
- `ventanas.py sin=a.log con=b.log ...`: compara los registros del
  autotest del Wii por ventanas de juego (tiempo, velocidad, tirones, CPU
  y GE, leer vértices y ciclos por unidad del PMC del GE, también de
  "comandos" por llamada y por comando).
- `ab_opt.py`: pspautotests A/B con un interruptor a 0 y a 1
  (`ab_opt.py CLI PSPAUTOTESTS LISTA SALIDA opcion[,opcion] [hilos] [MR]`).
  `gpu/complex` y `malloc/malloc` cambian entre corridas (fechas, malloc).
- `cpu_banco.c` + `medir_cpu.sh`: instrucciones PowerPC por instrucción de
  la PSP del intérprete (copias, llamadas y ramas, como el código normal de
  un juego), restando una corrida de 20 vueltas a una de 40.
  `medir_cpu.sh --opt cpu_predec=0` es el intérprete de antes: 28,7;
  con `cpu_predec` (0.5.17): 17,4; 0.5.17.1: 17,5; 0.5.17.2: 17,3 (con
  `--opt prof_cpu=1` 17,5; con `prof_bloques` 25,9); 0.5.19 15,4 (con
  `--opt cpu_enlace=0` 17,5: las tarjetas de salto cargan siempre el
  sucesor).
  Con `BANCO=vfpu_banco.c` corre otro programa (`vfpu_banco.c`: lv.q y
  sv.q por columnas y filas, lv.s, sv.s, mtv y mfv) y da además las
  instrucciones por instrucción VFPU: 0.5.17.2 164; 0.5.18 45 (con
  `--opt cpu_vfpu_tarjetas=0`, 164). `BANCO=vfpu_arit_banco.c`: vmul,
  vsub, vmin, vmax, vmov, vf2i, vi2f, vscl, vadd, vpfx y vabs; 0.5.18.1
  150 por instrucción VFPU (con `--opt cpu_vfpu_arit=0`, 263).
- `jit_banco.sh` (+ `jit_banco_pc.c`): las microprobas del JIT
  (`src/wii/jit_banco.c`, `prueba_jit` en el Wii) en PowerPC bajo qemu,
  sin lo que solo existe en el Wii (PMC, DSI, BAT): los ciclos no valen,
  pero dice si el código que generan corre y da lo que tiene que dar
  ("fin: todo bien"). Probarlo antes de llevar al Wii una microprueba
  nueva: en la 0.6.0 encontró una cadena de `lwz` que empezaba en una
  palabra a 0 (en el Wii, una DSI).
- Volcados de efectos (0.6.2): `wiisp-cli --opt prof_efectos=1 --volcado
  N --salida PREFIJO programa` guarda en BMP cada textura leída de la VRAM
  en el cuadro N (fuente, alfa, índices, resultado con la paleta), con su
  histograma y su paleta en el registro; en el autotest del Wii, `volcado
  N` (y `hasta N` para cortar una corrida con guion).
  Desde la 0.6.3 el volcado queda armado desde el cuadro N hasta volcar
  8 lecturas distintas o pasar 1800 cuadros.
- `prueba_gx` (0.6.3, `gx_ge_prueba` en `gx_ge.c`): con `PRUEBA_GX=1`, el
  CLI de GX (`build_cli_gx.sh`) la corre al empezar sobre la libogc de
  mentira: los números no valen (no pinta nada) y sale "(MAL)", pero se
  ve que el código corre entero. En el Wii, si GX guarda el alfa como se
  supuso: TEV y `GX_SetDstAlpha` 64/256 y 256/256 (los 64 primeros dan lo
  mismo de las dos formas), códigos 64/64, DBZ 8/8 y la diferencia
  absoluta 1024 de 1024 en los dos casos.
- Mutaciones del JIT: `mutar.py mutaciones/0.6.0.txt --dkp --solo
  interp,jit` (en `qemu-ppc -cpu 750` el código nativo corre de verdad).
  Las de la 0.6.3: `mutar.py mutaciones/0.6.3.txt --solo
  gxstencil,efectos`.
- `cpu_grande.py` + `medir_cache.sh`: fallos de caché por instrucción de la
  PSP con cachegrind (valgrind) sobre el CLI del PC, en programas de N
  funciones con bucles, cargas, escrituras, ramas y llamadas en orden
  desigual (`medir_cache.sh "256 1024" --opt cpu_predec=0`). La caché
  simulada es la del Broadway con el doble de bytes (en el PC punteros y
  tarjetas miden el doble). Con `ANOTAR=1`, las líneas que más fallan en la
  L1 de datos. qemu cuenta instrucciones pero no la caché: esto es lo que
  el Wii vio y qemu no (0.5.17).
