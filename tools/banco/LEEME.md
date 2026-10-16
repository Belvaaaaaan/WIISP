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
- `qprof.py`: instrucciones PowerPC por función (y por unidad) a partir de
  `qemu-ppc -d in_asm,exec,nochain -D grande.log` menos una corrida corta:
  `qprof.py grande.log corta.log UNIDADES`. `qtb.py` lo mismo por bloque
  de una función.
- `sigill_ppc.c`: emula con SIGILL lo que un PowerPC normal no tiene o es
  privilegiado (mfmsr/mtmsr, mfspr/mtspr con los GQR, psq_l con W = 1).
  `PSQ_INFORME=1` dice cuántos psq_l emuló.
- `build_tests_dkp.sh`: las pruebas del PC con ge_vertex.c y test_gxemit.c
  compilados por devkitPPC; correr `qemu-ppc $SALIDA/tests-dkp` desde la
  raíz del repositorio. Huella de test_vdec con devkitPPC: B3A2BCD3.
- `ab_opt.py`: pspautotests A/B con un interruptor a 0 y a 1
  (`ab_opt.py CLI PSPAUTOTESTS LISTA SALIDA opcion[,opcion] [hilos] [MR]`).
  `gpu/complex` y `malloc/malloc` cambian entre corridas (fechas, malloc).
