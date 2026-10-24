# Registro de arreglos

Cada cambio en el código lleva el comentario `//Arreglo` y nada más (desde
la 0.5.17.2; antes se marcaban con `//arreglo`). Aquí va, por versión, qué
se arregló o cambió y una descripción breve. El detalle está en
`docs/CONTINUAR.md`.

## 0.5.23

- `gpu/ge_vertex.c`: `ge_bbox_pantalla` (encendido, 1.3 c). La prueba
  exacta de las cajas de `ge_bbox` mira también la pantalla. Si la caja de
  una BBOX que la prueba de siempre da por visible está entera fuera del
  scissor (y de la región) por un mismo lado, sus llamadas se saltan como
  en `ge_bbox`. Solo cuenta la parte de la caja que el GE puede dibujar:
  con recorte de profundidad, la de delante del plano cercano; sin él, la
  de w ≥ 0. Es un programa lineal sobre la caja: el borde en coordenadas
  de recorte (`xs·x + (xc − L)·w`, con L el primer 1/16 de píxel que
  dibuja `range_of`) en las 8 esquinas y en los cruces de las 12 aristas
  con el plano que limita esa parte.
  - Margen de 2^-9 de la magnitud de los términos, más 2^-11 de la del
    centro del viewport (el redondeo del float24 en pantalla).
  - Con recorte de profundidad, si w no es claramente positiva en esa
    parte, no se salta (lo de detrás de la cámara se dibujaría dado la
    vuelta).
  - La caja guarda la pantalla (los 10 comandos: escala y centro del
    viewport en x e y, scissor, región, offset y recorte de profundidad) y
    deja de saltar si cambia.
  - Salen enseguida, sin márgenes ni programa lineal:
    - las cajas cuyo primer vértice se ve;
    - las que tienen alguna esquina dentro;
    - las que, sin márgenes, no pueden estar fuera de ningún plano y
      tienen esquinas a los cuatro lados de los bordes.
  - Con `ge_bbox_pantalla` y `ge_bbox_llamada` apagados, `ge_bbox` es el
    de la 0.5.22.
- `gpu/ge_vertex.c`: `ge_bbox_llamada` (encendido, 1.3 d). Después de una
  caja que no está entera fuera, o si cambian sus matrices o su pantalla,
  cada llamada se prueba con su propia caja. Solo triángulos, tiras y
  abanicos sin huesos, morph, índices ni through.
  - Primero su primer vértice: si se ve, se dibuja sin más.
  - Si no, la caja de sus posiciones, con la misma prueba (planos y, con
    `ge_bbox_pantalla`, pantalla).
  - Si está fuera, se salta como en `ge_bbox`.
- `gpu/ge.c`, `gpu/ge_internal.h`: `ge_sucio` con dos grupos más para
  estas pruebas:
  - B: mundo, vista o proyección cambiadas; se marca donde ya se marca T.
  - P: los 10 comandos de la pantalla.
  Con ellos, la prueba de cada llamada no vuelve a comparar las matrices
  ni la pantalla si no han cambiado. La prueba de siempre (la que decide
  el BJUMP) las sigue comparando con `memcmp`. Con `ge_verificar` se
  compara siempre y se cuenta si lo guardado no coincide con lo de ahora
  (línea `[TIEMPOS] Verificacion de ge_bbox`; tiene que dar 0).
- `gpu/ge_math.c`: `ge_setup_recip`, con `abs_det` de 2^48 o más, leía
  fuera de la tabla con un índice negativo. Pasaba con posiciones de
  pantalla enormes (un NaN da INT_MAX). En todos los demás casos el
  resultado es el mismo (comprobado en 150 millones de valores). Sin
  interruptor (arreglo).
- `gpu/ge.c`, `hle/hle.c`: `prof_huesos` (apagado, solo mide). Cuenta:
  - los datos de matrices de huesos (0x2B), y cuántos traen el valor que
    ya había;
  - las matrices completas, y cuántas iguales;
  - las rachas de 0x2A/0x2B seguidos (media, y cuántas de 12 o más);
  - las llamadas con huesos.
  Línea `[TIEMPOS] Huesos`. Va por el camino lento del bucle de comandos,
  como `prof_comandos`.
- `gpu/ge.c`: el censo de cajas (`prof_bbox`) reparte los objetos que
  pasan la caja sin dibujar nada por la clase de su caja: sin prueba
  exacta, fuera, dentro, dudosa o dudosa que cruza el plano de la cámara.
  Da también lo que cuesta probar la caja de cada llamada. Un objeto con
  llamadas saltadas cuenta como fuera. Con `prof_bbox`, la caja se
  clasifica entera (también "dentro"); sin él, la que se ve por su primer
  vértice no sigue. Las llamadas que se saltan son las mismas con y sin
  censo.
- `hle/hle.c`: la línea `[TIEMPOS] Cajas exactas` añade:
  - las cajas fuera de la pantalla, vistas por su primer vértice, dentro
    (solo con `prof_bbox`), dudosas y las que cruzan el plano de la
    cámara;
  - las llamadas probadas por su caja (y vistas por su primer vértice),
    fuera, saltadas y sus vértices.
- `core/opt.h`, `core/opt.c`: `ge_bbox_pantalla`, `ge_bbox_llamada` y
  `prof_huesos`.
- `tests/test_bbox.c`: pruebas nuevas, todas en modo exacto y rápido, con
  la VRAM y las cuentas iguales con y sin los interruptores y con
  `ge_verificar`:
  - listas al azar alrededor de la pantalla y de la cámara (viewport de
    ×1 a ×3 con signo, centros y offsets al azar, scissor y región,
    perspectiva y ortográfica, con y sin recorte de profundidad);
  - un vértice a pocos 1/16 de píxel de cada borde, también con la caja
    colocada por la traslación del mundo;
  - umbral exacto: viewport pequeño con el centro junto al borde (margen
    de menos de 1/32 de píxel), cambiando entre la caja y la llamada el
    offset, el scissor, el centro, la escala o el recorte;
  - cajas finas en diagonal que pasan por la cámara;
  - cajas que cruzan el plano cercano y solo se ven por detrás de él, con
    la z de pantalla ancha, también quitando el recorte entre la caja y la
    llamada;
  - proyecciones con w negativa delante del plano cercano;
  - el censo de cajas con los dos interruptores.
- `tests/test_listas.c`: el censo de los huesos.
- `tools/banco/bench_lista.c`: escenas 7 (cajas finas que cruzan la
  cámara, fuera de la pantalla), 8 (caja grande que se ve con las
  llamadas fuera) y 9 (la misma con las llamadas dentro). Las tres con el
  viewport al doble, y la 7 con recorte de profundidad.
- `tools/banco/ventanas.py`: columnas de cajas fuera de la pantalla y de
  llamadas probadas, saltadas y vértices por su caja.
- `hle/hle.h`, `dist/apps/wiisp/meta.xml`: versión 0.5.23.

## 0.5.22

- `gpu/ge_vertex.c`: `ge_bbox_rapido` (encendido, 1.3 a). La prueba de las
  BBOX (la aproximada de PPSSPP) se queda con su resultado de siempre pero
  más barata: `view * proj` y `mundo * vp` guardadas mientras no cambien,
  solo la posición al decodificar (con un puntero si no hay índices) y el
  formato guardado por tipo de vértice; los pesos de morph solo con morph.
  Las cuentas que deciden el resultado están en funciones compartidas con
  la prueba de siempre (`bb_mult4`, `bb_contar`), con las `fma` escritas en
  el orden en que devkitPPC compilaba la 0.5.21, y el conteo para en cuanto
  cada plano tiene un vértice dentro (el resultado solo depende de que
  alguno lo esté). Con `ge_verificar`, cada BBOX se prueba también por el
  camino de siempre y se compara.
- `gpu/ge_vertex.c`, `gpu/ge.c`: `ge_bbox` (encendido, 1.3 b). Si la caja
  de una BBOX que la prueba de siempre da por visible (las otras ya las
  salta el BJUMP: no se prueban otra vez) está, ampliada un poco, entera
  fuera de un mismo plano de recorte del GE o entera detrás de la cámara,
  con un margen de 2^-9 de la magnitud de los términos (el error del GE en
  modo exacto es menor de 2^-11), las llamadas siguientes con las mismas
  matrices cuyos vértices caen todos dentro de la caja no se calculan:
  cada triángulo suyo se descartaría por las reglas de siempre. Solo
  triángulos, tiras y abanicos sin huesos, morph, índices ni modo through.
  Queda lo que la llamada deja a la siguiente: direcciones, primitiva,
  índice de la ventana, última uv y normal y, si la siguiente primitiva
  puede continuarla (o no se sabe), los vértices de la ventana (`data_`),
  calculados como siempre. Para saberlo se mira la lista por delante
  (`ge_prim_datos_sobran`: tipo de vértice, direcciones y saltos; en
  listas con stall, sin seguir saltos). Con `ge_verificar`, las llamadas
  que se saltarían se dibujan igual y se comprueba que no salió ninguna
  primitiva y que lo que dejan es lo calculado. Línea `[TIEMPOS] Cajas
  exactas`.
- `gpu/ge_vertex.c`: `ge_bounding_box` ya no pone `uv_prescale = 0` (la
  decodificación no lo usa): con `ge_estado_sucio`, una llamada con uv del
  modo 3 después de una BBOX se quedaba con la uv sin escalar. Sin
  interruptor (arreglo).
- `gpu/ge.c`: `ge_cmd_matrices` (encendido). En el bucle de comandos, los
  datos y números de las matrices (mundo, vista, proyección, textura y
  huesos) se aplican sin pasar por `execute`, con las mismas cuentas
  (`mat_calcular`/`mat_aplicar`). Con `ge_verificar`, cada uno pasa por
  `execute` y se compara. Lo que decide el camino lento (`prof_estado`,
  `ge_verificar`, `prof_comandos`) se lee una vez por lista.
- `gpu/ge.c`, `hle/hle.c`: `prof_comandos` (apagado, solo mide): cuántos
  comandos de cada tipo ejecuta el bucle de comandos; línea
  `[TIEMPOS] Comandos por tipo` con los 16 más frecuentes.
- `core/opt.h`, `core/opt.c`: `ge_bbox` (antes reservado) y los nuevos
  `ge_bbox_rapido`, `ge_cmd_matrices` y `prof_comandos`.
- `tests/test_bbox.c`: 8.000 BBOX al azar (todos los formatos, índices,
  huesos, morph, tramos de más de 0x100, matrices raras) con la prueba de
  siempre y la rápida, en secuencias que cambian matrices entre medias;
  240 listas con objetos alrededor de la cámara (detrás, también grandes
  que la prueba de siempre deja pasar, más allá del plano lejano, a
  caballo de los bordes, también finos en la franja del borde, cajas con
  las esquinas en cualquier orden, cajas que no contienen sus vértices,
  continuaciones, PRIM vacíos o sin posición antes de una continuación y
  uv arrastradas) dibujadas por el rasterizador por software en modo
  exacto y rápido, con y sin `ge_bbox` y con `ge_verificar`: la VRAM
  entera tiene que salir igual. Una llamada saltada seguida de una
  continuación con ocho cosas distintas entre medias (nada, un PRIM de 0
  vértices, uno sin posición, uno con la dirección mala, una llamada
  nueva, con índices buenos y malos): misma imagen y las llamadas
  "ligeras" que tocan. 34 casos de lo que mira `ge_prim_datos_sobran`
  (con y sin stall: saltos, BJUMP por los dos lados, el tope de 48
  comandos y de 4 saltos, BASE, OFFSETADDR, ORIGIN, BBOX, índices).
- `tests/test_listas.c`: los 10 comandos de matrices por el atajo y su
  verificación; sin `ge_bbox` (es la prueba del bucle de comandos).
- `gpu/ge.c`, `gpu/ge_internal.h`: `ge_test_pico` para las pruebas.
- `tools/banco/ventanas.py`: columnas de llamadas y vértices que salta
  `ge_bbox` por ventana (con `-` en las corridas que no lo tienen).
- `tools/banco/bench_lista.c`: escenas 4 (objetos con caja visibles), 5
  (objetos más allá del plano lejano que pasan la caja) y 6 (objetos fuera
  de la pantalla por un lado: el BJUMP los salta); los huesos de las
  escenas 2 y 3 ya no se cargan en las otras.
- `tools/banco/qprof.py`: cuarto argumento opcional, cuántas funciones
  listar (0: todas).
- `hle/hle.h`, `dist/apps/wiisp/meta.xml`: versión 0.5.22.

## 0.5.21

- `gpu/ge.c`: `ge_cmd_puntero` (encendido). El bucle de comandos de una
  lista (`run_commands_ptr`) lee cada comando de la RAM con un puntero
  (sin `mem_valid` ni `mem_read32`; fuera de la RAM, como siempre) y los
  comandos que no tienen `case` propio en `execute` (229 de 256: los de
  estado) se aplican en el mismo bucle: valor guardado, `ge_sucio`,
  `cur_cmd` y la cuenta de comandos, sin llamar a `execute`. Los 27 con
  `case` (y todos con `prof_estado`) siguen por `execute`. La tabla
  `cmd_con_caso` está junto a `execute`: **si se añade un `case`, hay que
  añadirlo también a la tabla** (`test_listas` comprueba que son 27).
- `gpu/ge.c`: con `ge_verificar` y `ge_cmd_puntero`, cada comando sin
  `case` pasa por `execute` y se compara con lo que habría hecho el bucle
  (valor, `ge_sucio`, pc, direcciones, offset, CLUT, cuentas, estado de la
  lista, ciclos) y lo leído con el puntero con `mem_read32`: línea
  `[TIEMPOS] Verificacion de los comandos` y `[VERIF] comando`.
- `gpu/ge.c`, `gpu/ge_vertex.c`, `hle/hle.c`, `gpu/ge.h`: censo
  `prof_bbox` (apagado, solo mide) para decidir 1.3 `ge_bbox`: BBOX
  (vértices de caja, tiempo, visibles, fuera, con 0 vértices, los que pasan
  sin probar), BJUMP (tomados, y las llamadas y vértices que saltan), los
  objetos que pasan la caja (hasta el destino del BJUMP) y cuántos no
  dibujan nada (con y sin huesos, enteros fuera de pantalla), y las
  llamadas fuera de objetos que no dibujan nada (el techo de saltarse
  llamadas enteras). Tres líneas `[TIEMPOS] Censo de cajas`.
- `gpu/ge.c`, `gpu/ge_internal.h`: `ge_test_run_list` y
  `ge_test_cmd_con_caso` para las pruebas.
- `core/opt.h`, `core/opt.c`: interruptores `ge_cmd_puntero` y `prof_bbox`.
- `tests/test_bbox.c`: una lista con objetos que dibujan, que se saltan,
  que pasan la caja sin dibujar (con y sin huesos), vacíos y llamadas sin
  caja; cada cuenta del censo, y sin el censo lo mismo dibujado.
- `tests/test_listas.c`: 600 listas al azar en la RAM (también pegadas a
  su final), la VRAM y sus espejos, la scratchpad y alias sin caché o de
  kernel, con JUMP, CALL/RET, BJUMP, ORIGIN y OFFSETADDR entre zonas: el
  estado final con y sin `ge_cmd_puntero` (y con `ge_verificar`) tiene que
  ser el mismo; y cada uno de los 229 comandos sin `case`, 8 veces.
- `tools/banco/bench_cmd.c`: el banco de listas usa el mismo bucle que
  `process_dl_queue_body` (con y sin `ge_cmd_puntero`).
- `tools/banco/ventanas.py`: columnas de ciclos de "comandos" por llamada
  y por comando.
- `hle/hle.h`: versión 0.5.21.

## 0.5.20

- `gpu/ge_vertex.c`: `ge_pantalla_rapida` (encendido). En el camino por
  lotes (`batch_range`), si x e y caen en [0, 4096) y z en [0, 65536), el
  paso a pantalla (`pantalla_lote`) no comprueba rangos ni acota: una sola
  conversión a entero por coordenada (para un valor positivo truncar es
  redondear hacia abajo), el suelo con el truco de 1,5 * 2^23 sin más
  comprobaciones, el offset en float una vez por tanda y las banderas de
  recorte sin mirar el rango de pantalla (siempre `GE_CF_SMALL`). Fuera de
  ese rango, NaN o el camino de uno en uno, `round_to_screen` como
  siempre. La z se redondea donde se usa.
- `core/opt.h`, `core/opt.c`: interruptor `ge_pantalla_rapida`.
- `cpu/predec.c`, `hle/hle.c`: con `prof_bloques`, un bloque de 65
  instrucciones (salto en la 63 con su delay slot) se contaba fuera de la
  tabla de tamaños y sumaba en los enlaces al siguiente; ahora va al
  último grupo ("33-65"). Lo encontró UBSan en las pruebas.
- `tests/test_vdec.c`: cada caso también con el camino por lotes sin
  `ge_pantalla_rapida`; prueba dirigida de los bordes (x, y alrededor de 0
  y de 4096, z alrededor de 0 y de 65536, -0, NaN, infinitos, w negativa,
  offsets de 0 a 0xFFFF) comparando cada vértice con `ge_verificar`.
- `hle/hle.h`: versión 0.5.20.

## 0.5.19.1

- `cpu/predec.c`: el bucle del predecodificado (las cuatro copias) se
  compila con `optimize("no-branch-count-reg")`: sin `bdnz`, el registro
  CTR solo lo usa el salto a la siguiente tarjeta. Sin interruptor (es un
  cambio del compilador).
- `cpu/predec_bucle.h`, `cpu/predec.c`, `hle/hle.c`: con `prof_bloques`,
  una pila de retornos simulada (32 entradas; se apila en jal, jalr y los
  bltzal/bgezal tomados, se desapila en `jr ra`, también en los stubs):
  línea `[TIEMPOS] Pila de retornos simulada`. Solo mide.
- `core/prof.c`, `core/prof.h`, `gpu/ge_vertex.c`, `hle/hle.c`:
  `prof_vertices` (apagado): "leer vertices" por partes con el muestreo
  (preparar, decodificar, huesos, transformar, pantalla, luces uv y
  niebla, camino genérico): línea `[TIEMPOS] Leer vertices por dentro`.
- `hle/hle.h`: versión 0.5.19.1.

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
