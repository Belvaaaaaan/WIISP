# Registro de arreglos

Cada cambio en el código lleva el comentario `//Arreglo` y nada más (desde
la 0.5.17.2; antes se marcaban con `//arreglo`). Aquí va, por versión, qué
se arregló o cambió y una descripción breve. El detalle está en
`docs/CONTINUAR.md`.

## 0.5.19

- `cpu/predec.c`, `cpu/predec_bucle.h`: `cpu_enlace` (encendido). Las
  tarjetas de salto con destino fijo eligen el bloque siguiente (`nbx`)
  junto con el destino, y el cierre nuevo (`BENDD`) solo mira si ese bloque
  sigue vivo y si cabe en el reloj. Los enlaces sin hacer apuntan a un
  bloque nulo (nunca vivo). `jr`/`jalr` siguen por el cierre de siempre. Si
  el delay slot es un nop, su tarjeta hace de cierre. Diseño propuesto en
  otra conversación y comprobado aquí.
- `cpu/predec.c`: `cpu_syscall_slot` (encendido): un `syscall` en el delay
  slot de un salto (los stubs `jr ra` + `syscall` de las importaciones) ya
  no corta el bloque ni pasa por el intérprete de siempre.
- `cpu/predec_bucle.h`: con `cpu_verificar`, el cierre nuevo comprueba el
  bloque al que va (antes los enlaces se apagaban al verificar).
- `cpu/predec.c`, `cpu/predec.h`: `cpu_predec_enlaces_ok` para las
  pruebas; `PREDEC_ZONA_BITS` se puede cambiar al compilar.
- `tests/test_enlace.c` (nuevo, del mismo diseño): bucles que llaman a
  rutinas en varias zonas y reescriben su código, también desde los delay
  slots, con y sin `cpu_enlace` y con `cpu_verificar`.
- `tests/test_interp.c`: saltos con `syscall` en el delay slot en los
  programas con fallos; combinaciones con y sin los dos interruptores;
  `cpu_predec_enlaces_ok` tras cada programa; caso dirigido de un enlace a
  un bloque cambiado sin aviso con `cpu_verificar`.
- `hle/hle.h`: versión 0.5.19.

## 0.5.18.1

- `cpu/predec.c`, `cpu/predec_bucle.h`: tarjetas propias para `vadd`,
  `vsub`, `vmul`, `vmov`, `vabs`, `vneg`, `vmin`, `vmax`, `vscl`, `vi2f`,
  `vf2i` y `vpfx` (`cpu_vfpu_arit`, encendido; necesita
  `cpu_vfpu_tarjetas`). Sin prefijos pendientes hacen la misma cuenta que
  los caminos rápidos de `vfpu_exec`; con prefijos, contexto sin cargar o
  `vfpu_fast_paths` a 0, van a `vfpu_exec`. `vpfx` solo pide el contexto
  cargado. También las compara `cpu_vfpu_verificar`.
- `cpu/vfpu.h`, `cpu/vfpu.c`: `vfpu_flush`, `vfpu_canon`,
  `vfpu_nan_or_inf`, `vfpu_minmax_bits`, `vfpu_f2i_mode` y
  `vfpu_f2i_lane` pasan a `vfpu.h` y las usan `vfpu.c` y las tarjetas (la
  misma cuenta en los dos sitios); `vfpu_vidx` (la tabla de índices)
  visible.
- `tests/test_interp.c`: programas aleatorios con vmul, vscl, vdot, vmin,
  vmax, vmov, vabs, vneg, vf2i, vi2f y prefijos S y T (y D con datos de 20
  bits); cada programa con y sin `cpu_vfpu_arit`; caso dirigido con las
  doce, un prefijo pendiente y el recuento de tarjetas.
- `tools/banco/vfpu_arit_banco.c`: banco de la aritmética de la VFPU.
- `hle/hle.h`: versión 0.5.18.1.

## 0.5.18

- `cpu/predec.c`, `cpu/predec_bucle.h`: tarjetas propias para `lv.s`,
  `sv.s`, `lv.q`, `sv.q` (columnas y filas por separado), `mtv` y `mfv`
  (`cpu_vfpu_tarjetas`, encendido). Copian los 32 bits tal cual; con el
  contexto de la VFPU sin cargar, la dirección desalineada o fuera de la
  RAM o una máscara del prefijo D pendiente (`lv.q`) van a `vfpu_exec` como
  antes. `sv.s` y `sv.q` avisan al mapa de código como `sw`. Siguen
  contando en `[STATS]` igual que `vfpu_exec`.
- `cpu/predec.c`: `cpu_vfpu_verificar` (apagado): cada una de esas
  instrucciones se hace con la tarjeta, se deshace, se hace con
  `vfpu_exec` y se comparan registros, VFPU y memoria; las diferencias van
  al registro (`[PREDEC] tarjeta VFPU distinta`).
- `cpu/predec.c`: cambiar `cpu_vfpu_tarjetas` o `cpu_vfpu_verificar` vacía
  los bloques.
- `cpu/vfpu.c`, `cpu/vfpu.h`: `vfpu_stat_now` visible para las tarjetas.
- `hle/hle.c`: "VFPU en tarjetas propias" en "CPU por dentro" (con
  `prof_cpu`) y línea `cpu_vfpu_verificar`.
- `tests/test_interp.c`: programas aleatorios con `lv.q`/`sv.q`,
  `mtv`/`mfv`/`mfvc`/`mtvc` y prefijos D; `sv.s` y `sv.q` sobre el propio
  código; contexto de la VFPU sin cargar; cada programa con tarjetas, sin
  ellas y con verificación; se comparan también los recuentos de la VFPU
  y `vfpu_live`. Casos dirigidos: `sv.q` y `sv.s` que reescriben su
  bloque, máscara del prefijo D y contexto sin cargar.
- `tools/banco/vfpu_banco.c` y `BANCO=` en `medir_cpu.sh`: banco de la
  VFPU con el costo por instrucción de la VFPU.
- `hle/hle.h`: versión 0.5.18.

## 0.5.17.2

- `core/opt.c`: `predec_sin_unir` pasa a 0 por defecto (en el Wii empeoraba
  un 0,8 %); `predec_mem1` sigue a 1.
- `core/opt.h`, `core/opt.c`: interruptores nuevos `prof_cpu`, `prof_bloques`
  y `prof_ventana_juego`, todos apagados por defecto.
- `cpu/predec.c`, `cpu/predec_bucle.h`: el bucle se compila cuatro veces
  (unido o sin unir, con o sin medidas). Las copias normales ya no cuentan
  las tarjetas de FPU y VFPU ni escriben la fase del muestreo; las de
  medida se usan con `prof_cpu` o `prof_bloques`. La memoria lenta solo se
  cuenta al medir.
- `cpu/predec.c`, `cpu/predec_bucle.h`, `cpu/predec.h`: `prof_bloques`
  cuenta los bloques ejecutados, sus instrucciones y su tamaño, los enlaces
  que aciertan o fallan (y cuántos fallos son de bloques que acaban en `jr`
  o `jalr`), y por qué y qué instrucciones caen al intérprete de siempre
  (delay slot suelto, sin tarjeta, no cabe en el reloj, fuera de la RAM).
- `cpu/predec.c`: `cpu_predec_tarjetas_ok` comprueba también que la tabla de
  manejadores es la de la copia que corresponde a los interruptores.
- `hle/hle.c`, `hle/hle.h`: `prof_ventana_juego`, el informe `[TIEMPOS]` se
  corta cada N vblanks de juego (por defecto 600, unos 10 s), en múltiplos
  exactos de N, y sigue diciendo los segundos reales. La línea "CPU por
  dentro" solo da el muestreo con `prof_cpu`; dónde está cada cosa en la
  memoria sale siempre. Líneas nuevas `Bloques (prof_bloques)` e
  `Interprete de siempre (prof_bloques)`.
- `wii/main.c`: línea `ventana N` del autotest. `host/main.c`: `--ventana N`.
- `tests/test_interp.c`: el predecodificado se compara también con las
  copias de medida; caso dirigido de los contadores de `prof_bloques` y
  cambio de copia con y sin medidas.
- `hle/hle.h`: versión 0.5.17.2.
