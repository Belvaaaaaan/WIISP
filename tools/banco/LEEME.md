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
  y la matriz de mundo; 3 = con 8 huesos. `LUCES=1` enciende dos luces;
  con `prof_estado=1` escribe el censo de estado. Corre la lista con el
  mismo bucle que `process_dl_queue_body` (con y sin `ge_cmd_puntero`).
  Leer y ejecutar los comandos por llamada (bucle, `mem_valid`,
  `mem_read32`, `execute` o `run_commands_ptr`), 0.5.21 sin / con
  `ge_cmd_puntero`: escena 0 399 / 264, 1 1469 / 679, 2 9392 / 7107, 3
  15856 / 12375.
- `qprof.py`: instrucciones PowerPC por función (y por unidad) a partir de
  `qemu-ppc -d in_asm,exec,nochain -D grande.log` menos una corrida corta:
  `qprof.py grande.log corta.log UNIDADES`. `qtb.py` lo mismo por bloque
  de una función.
- `qlines.py`: lo mismo por bloque con la función y la línea de código
  (`addr2line`; el banco se compila con `-g`), para repartir una función
  entre lo que tiene en línea. `qcallers.py`: quién llama a una función
  (memcpy, memset...), por el bloque que corre antes de entrar.
- `sigill_ppc.c`: emula con SIGILL lo que un PowerPC normal no tiene o es
  privilegiado (mfmsr/mtmsr, mfspr/mtspr con los GQR, psq_l con W = 1).
  `PSQ_INFORME=1` dice cuántos psq_l emuló.
- `build_tests_dkp.sh`: las pruebas del PC con ge_vertex.c, test_gxemit.c,
  interp.c y predec.c
  compilados por devkitPPC; correr `qemu-ppc $SALIDA/tests-dkp` desde la
  raíz del repositorio. Huella de test_vdec con devkitPPC: B3A2BCD3.
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
  GX. Ojo: `gpu/exact/pixel` lee memoria que la GPU de mentira no escribe;
  con `GLIBC_TUNABLES=glibc.malloc.perturb=85` en las dos sale igual.
- `gevert.sh DIR_BANCO [opcion=valor...]`: instrucciones por vértice
  (total y `batch_range`) en el banco del GE para los formatos de DBZ.
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
- `cpu_grande.py` + `medir_cache.sh`: fallos de caché por instrucción de la
  PSP con cachegrind (valgrind) sobre el CLI del PC, en programas de N
  funciones con bucles, cargas, escrituras, ramas y llamadas en orden
  desigual (`medir_cache.sh "256 1024" --opt cpu_predec=0`). La caché
  simulada es la del Broadway con el doble de bytes (en el PC punteros y
  tarjetas miden el doble). Con `ANOTAR=1`, las líneas que más fallan en la
  L1 de datos. qemu cuenta instrucciones pero no la caché: esto es lo que
  el Wii vio y qemu no (0.5.17).
