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
- `bench_lista.c` + `bench_cmd.c` (también los compila `build_banco.sh`):
  llamadas de dibujo como las de sceGu por el bucle de comandos de `ge.c` y
  `ge_raster.c` compilados con devkitPPC. `qemu-ppc $SALIDA/bench_lista
  REPETICIONES VTYPE ESCENA opcion=valor...`: escena 0 = VERTEXTYPE, BASE,
  VADDR y PRIM; 1 = más 10 comandos de textura con paleta; 2 = más 4 huesos
  y la matriz de mundo; 3 = con 8 huesos. `LUCES=1` enciende dos luces;
  con `prof_estado=1` escribe el censo de estado.
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
- `ab_opt.py`: pspautotests A/B con un interruptor a 0 y a 1
  (`ab_opt.py CLI PSPAUTOTESTS LISTA SALIDA opcion[,opcion] [hilos] [MR]`).
  `gpu/complex` y `malloc/malloc` cambian entre corridas (fechas, malloc).
- `cpu_banco.c` + `medir_cpu.sh`: instrucciones PowerPC por instrucción de
  la PSP del intérprete (copias, llamadas y ramas, como el código normal de
  un juego), restando una corrida de 20 vueltas a una de 40.
  `medir_cpu.sh --opt cpu_predec=0` es el intérprete de antes: 28,7;
  con `cpu_predec` (0.5.17): 17,4.
