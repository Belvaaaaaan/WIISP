# Registro de arreglos

Cada cambio en el código lleva el comentario `//Arreglo` y nada más (desde
la 0.5.17.2; antes se marcaban con `//arreglo`). Aquí va, por versión, qué
se arregló o cambió y una descripción breve. El detalle está en
`docs/CONTINUAR.md`.

## 0.6.3

El aura de DBZ en GX: el stencil sin perder bits y la diferencia absoluta
exacta (cada uno con su interruptor).

- `wii/gx_stencil.h` (nuevo): la tabla de códigos de cada búfer. El alfa
  del EFB en RGBA6 tiene 6 bits: cada valor de stencil distinto recibe su
  propio código de 6 bits (el natural, `v >> 2`, si está libre; si no, el
  libre más cercano); `dec` traduce el alfa leído de una copia del EFB al
  valor de verdad. `st_monotono` (los alfas que suben de la VRAM, hasta
  64 distintos, en orden), `st_cerrar` (sin códigos nuevos tras una
  escritura que no se controla) y `st_lleno`.
- `wii/gx_ge.c`, `gx_stencil_exacto`: el stencil REPLACE/ZERO, los borrados
  por rectángulo, el alfa uniforme al promover, el de la VRAM y el de las
  filas del hueco se escriben con su código; las bajadas a la VRAM y la
  TLUT del CLUT32 que lee el alfa (`ge_rtt_gpu`) los traducen de vuelta.
  Con el interruptor apagado, o mientras un valor tiene su código natural,
  escribe lo mismo que antes. Censo en la línea `gx_stencil_exacto` de
  `[TIEMPOS]`.
- `wii/gx_ge.c`, `gx_absdiff`: la diferencia absoluta (`|fuente - destino|`)
  en el TEV con una copia del destino (`(S - D) + (D - S)` con saturación,
  tres etapas; la copia se muestrea en el píxel con una coordenada de
  textura sacada de la posición) en vez de la resta de GX (`D - S`). La
  copia se hace al empezar cada llamada y otra vez solo si una primitiva
  pisa lo ya dibujado desde la última.
- `gx_ge_prueba` (`prueba_gx`): microprueba del Wii antes de cargar el
  juego: cómo guarda GX el alfa de 6 bits (por el TEV y por
  `GX_SetDstAlpha`), si los 64 códigos y los valores de DBZ vuelven
  exactos, y si la diferencia absoluta del TEV da lo mismo que la cuenta
  en la CPU, píxel a píxel y con un cuadro grande.
- `gpu/ge_efectos.c`: el volcado queda armado desde el cuadro pedido hasta
  volcar 8 lecturas distintas (la dirección de la paleta entra en la
  clave) o pasar 1800 cuadros (`ge_efectos_cuadro_fin`, desde
  `frontend/app.c`); la tabla de escrituras con 512 sitios y búsqueda por
  hash.
- `core/opt.h`, `core/opt.c`: `gx_stencil_exacto`, `gx_absdiff` (los dos
  encendidos) y `prueba_gx`. `wii/main.c`: `prueba_gx` en el autotest.
- `tests/test_gxstencil.c` (nuevo), `tests/test_efectos.c` (el volcado entre
  cuadros y la tabla llena). `mutaciones/0.6.3.txt`.
- `hle/hle.h`, `dist/apps/wiisp/meta.xml`: versión 0.6.3.

## 0.6.2

Solo medidas: el diagnóstico del aura, el censo de la VFPU y la latencia.

- `wii/jit_banco.c`: `reloj()` con barrera de memoria y `perseguir` con
  `noipa` (el compilador sacaba la persecución de entre las dos lecturas
  del reloj y la latencia salía 0); `plausible()`: una medida por debajo
  de lo posible sale como "MEDIDA NO VALIDA" y cuenta como fallo.
- `cpu/predec.c`, `cpu/predec_bucle.h`, `cpu/predec.h`, `cpu/vfpu.c`,
  `cpu/vfpu.h`: censo de la VFPU dentro de `prof_jit`: por clase, por
  tarjeta, por `vfpu_exec` (sin tarjeta o tarjeta que se rinde), con
  prefijos y el tiempo de `vfpu_exec`; `vfpu_clase` (la clase de la
  última instrucción que hizo `vfpu_exec`); cuadre con el contador del
  predecodificado. `cpu_predec_censo` recibe la duración de la ventana.
- `gpu/ge_efectos.c` (nuevo), `gpu/ge.h`, `gpu/ge_vertex.c`: `prof_efectos`,
  el censo de los estados de stencil y alfa de cada dibujo (con lo que
  hace y no hace GX) y de las texturas leídas de la VRAM (paleta,
  destino, mezcla), con cuadre; el volcado en BMP de cada textura leída
  de la VRAM en un cuadro (fuente, alfa, índices, paleta) con el
  histograma de índices y la paleta en el registro.
- `frontend/app.c`, `frontend/app.h`: `app_guion_volcado`. `wii/main.c`:
  `volcado N` y `hasta N` en el autotest; la sincronización de GX para el
  volcado. `host/main.c`: `--volcado N` (con `--salida`). `hle/hle.c`: las
  líneas `Efectos` cada ventana.
- `core/opt.h`, `core/opt.c`: `prof_efectos`.
- `tests/test_efectos.c` (nuevo) y `tests/test_interp.c` (el censo de la
  VFPU con resultados conocidos). `mutaciones/0.6.2.txt`.
- `hle/hle.h`, `dist/apps/wiisp/meta.xml`: versión 0.6.2.

## 0.6.1

El censo completo para diseñar el IR (solo medidas; detalle en CONTINUAR).

- `cpu/predec.c`, `cpu/predec.h`: el censo `prof_jit` ya no pierde las
  cuentas de los bloques que mueren: al tirarse uno (`kill_block`) o
  vaciarse la arena (`cpu_predec_flush`), lo que llevaba se suma a lo de
  la ventana y a una tabla por pc (`censo_bloque`, `censo_tab_sumar`;
  2^17 entradas, pedida a `cpu_predec_pedir_mem` o a malloc). La curva y
  los bloques calientes salen de la tabla. `predec_config` vacía antes de
  liberar las arenas. Nuevo en el informe: lo contado de bloques muertos,
  las 25 instrucciones más ejecutadas, las cuentas de la FPU, registros
  distintos por bloque, los 4 bits altos de cada dirección al ejecutarse
  y los desalineados, y las escrituras en zonas con código por pc
  (escrituras, sobre instrucciones, bloques tirados, última dirección),
  los bloques tirados sin tocar ninguna instrucción y los avisos de fuera
  de la CPU por llamada del HLE. `cpu_predec_caida_nombre` con nombres
  para casi todas las instrucciones (también en `prof_bloques`).
- `cpu/predec_bucle.h`: `CENSO_DIR` en las cargas y escrituras de la
  copia de medir.
- `hle/hle.c`, `hle/hle.h`: el censo del HLE cuenta el tiempo de cada
  llamada en su cubeta (sin el GE, el juego de dentro ni las anidadas) y
  aparte el total; llamadas por 1000 instrucciones. `hle_llamada_actual`
  (la llamada en curso, para saber quién avisa de escrituras en código).
- `wii/main.c`: `mem2_alto` (la punta de MEM2, ejecutable o no) para la
  arena del JIT y para la tabla del censo.
- `wii/jit_banco.c`: la latencia usa el resultado (antes el compilador
  quitaba la persecución y salía 0); DBAT6 se da por libre si la mitad
  alta no es válida y ahora mapea 32 MB, con DBAT7 para los últimos 128
  KB (la RAM de la PSP entera), con la latencia por el alias, y deja las
  BAT como estaban; dos formas más de despachar (`b` con presupuesto y
  `jr` por tabla con etiqueta).
- `tests/test_interp.c`: el censo con un vaciado a mitad de ventana,
  bloques tirados por la CPU (sobre y sin tocar instrucciones) y desde
  fuera, espejos 0x4… y un `lh` desalineado. `mutaciones/0.6.1.txt`.
- `hle/hle.h`, `dist/apps/wiisp/meta.xml`: versión 0.6.1.

## 0.6.0

El paso 0 del IR/JIT: medir antes de diseñar (detalle en CONTINUAR).

- `cpu/ppc_emit.h` (nuevo): emisor de instrucciones PowerPC (formas D, X
  y M, rotaciones, saltos con huecos que se parchean después, cargar una
  constante de 32 bits). Comprobado contra el ensamblador de devkitPPC
  (65 codificaciones en `test_jit`). Los huecos con el búfer lleno
  devuelven NULL y el parche no escribe (antes se escribía fuera de la
  arena).
- `cpu/jit.c`, `cpu/jit.h` (nuevos): `jit_proto`, un prototipo que pasa a
  código PowerPC el prefijo de cada bloque del predecodificado hecho de
  instrucciones enteras que sabe traducir (ALU, desplazamientos, hi/lo,
  mult/madd, ext/ins, seb/seh, cargas y escrituras de 1, 2 y 4 bytes,
  saltos condicionales, j/jal/jr/jalr), con cinco registros de la PSP en
  registros del Broadway, la RAM en little endian con `lwbrx`/`stwbrx` y
  la comprobación de cada dirección (rango y alineación en un
  `rlwinm.`; las escrituras también miran el mapa del código). Lo que no
  cabe o falla sale a la tarjeta k con el estado de la PSP al día.
  `jit_verificar`: cada bloque nativo se deshace y se repite con el
  intérprete, comparando registros, hi/lo, pc y lo escrito. Arena de 4 MB;
  en el Wii la da el anfitrión (`jit_pedir_arena`) y sin ella no se
  compila nada. `jit_tam_estimado` (palabras de un bloque sin publicarlo,
  para el censo) y `jit_uso_regs` (qué registros lee y escribe cada
  instrucción).
- `cpu/predec.c`, `cpu/predec_bucle.h`, `cpu/predec.h`: una quinta copia
  del bucle (`run_unido_jit`) con la entrada a lo nativo (a la segunda vez
  que se entra en un bloque se compila; `alive` 2 = nativo, 3 = no se
  puede); con el JIT apagado las copias de siempre no cambian. El censo
  `prof_jit` en la copia de medir: veces que se entra en cada bloque,
  `jr` que no son `jr ra` (¿al mismo destino que la vez anterior?), la
  pila de retornos, escrituras de la CPU en zonas con código (¿sobre
  instrucciones de bloques vivos? las que más escriben), memoria lenta
  por región; el informe (`cpu_predec_censo`) saca la curva del código
  caliente (cuántas instrucciones y bloques hacen el 50/75/90/95/99 %,
  con los KB nativos que costarían), la mezcla de instrucciones, lo que
  cubriría `jit_proto` y dónde corta, el uso de cada registro, las bases
  y tamaños de los accesos a memoria y los 10 bloques más calientes con
  sus instrucciones. `PredecStats`: `jit_bloques`, `jit_instr`,
  `jit_salidas`.
- `hle/hle.c`: línea `[TIEMPOS] JIT (jit_proto)` (y `jit_verificar`),
  el censo cada ventana y el coste de cada llamada del HLE (`Censo JIT,
  HLE`; sus tablas y el búfer de `jit_tam_estimado` se piden al usarlos,
  para no quitar MEM1 sin `prof_jit`). `core/prof.h`: fase `CPF_NATIVO` ("codigo nativo" en el
  muestreo).
- `wii/jit_banco.c` (nuevo): `prueba_jit`, microprobas del Broadway con el
  emisor al empezar el autotest (líneas `[PRUEBA_JIT]`): publicar código
  (con y sin `dcbst`/`icbi`, libogc), su coste por línea, ciclos por
  salto de cada forma de despachar (b, `mtctr`+`bctr` como las tarjetas,
  `mtctr` pronto o tarde, `mtlr`+`blr`, `bl`/`blr`, `li`+`bl`, llamar a C
  con `bctrl` o `bl`), `lwz` contra `lwbrx` y `lhbrx`+`extsh`, un salto
  mal predicho, la latencia de MEM1 y MEM2 de 16 KB a 8 MB, código en
  línea recta desde MEM1 y MEM2, la CPU de la PSP con tarjetas y con
  `jit_proto` (cuatro programas pequeños, con los registros comparados),
  una DSI de ida y vuelta con `PPCExcptCurPanicFn`, la RAM de la PSP en
  0x08000000 con DBAT6 y código por un alias IBAT en 0x7FFC0000 con un
  `bl` directo a C.
- `wii/main.c`: la arena del JIT sale de la punta de MEM2 la primera vez
  que se pide (solo con `jit_proto` o `prueba_jit`), si un IBAT la cubre
  y un trozo de prueba corre ahí. `wii/broadway.c`:
  `broadway_ibat_cubre`.
- `core/opt.h`, `core/opt.c`: `jit_proto`, `jit_verificar`, `prof_jit`,
  `prueba_jit` (todos apagados).
- `tests/test_jit.c` (nuevo): el emisor contra el ensamblador y 100.000
  bloques al azar compilados y comparados con el intérprete (en PowerPC),
  y que ningún acceso hecho en nativo estuviera desalineado, fuera de la
  RAM o escribiera en una zona con código (lo que el intérprete no ve: un
  `lwbrx` desalineado da lo mismo en qemu, pero en el Wii puede dar una
  excepción de alineación).
  `tests/test_interp.c`: el JIT en la matriz de configuraciones (con
  arenas diminutas que se llenan), un control positivo (un error a
  propósito que `jit_verificar` tiene que ver), `prof_jit` en la matriz y
  una prueba del censo con tres programas de resultado conocido.
  `mutaciones/0.6.0.txt`.
- `hle/hle.h`, `dist/apps/wiisp/meta.xml`: versión 0.6.0.

## 0.5.23.7

- `gpu/ge_vertex.c`: `ge_vdec_ps` (encendido). Los vértices con huesos de
  formato 0x301 (uv de 8 bits, posición de 16, pesos de 8 bits; DBZ dibuja
  así a los personajes, con 1 a 8 huesos) se calculan de dos en dos con
  paired singles: el carril 0 es un vértice y el 1 el siguiente, y cada
  carril hace exactamente las cuentas del camino de siempre (las de
  `fast_skin_plain` y `read_vertex_body`, en el mismo orden): la mezcla de
  los huesos (`ps_madds0`, el peso de cada vértice por el elemento del
  hueso), aplicarla a la posición (`ps_madd`) y la transformación por la
  matriz combinada (`ps_muls0`, `ps_madds0`, `ps_add`). Un núcleo en
  ensamblador (`ps_nucleo_huesos`, un solo bloque: lo de ps1 nunca pasa
  por el compilador) escribe la posición y el clip de cada par en un
  búfer; el resto del vértice (uv, pantalla, niebla, luces) sigue en C
  con `read_vertex_body`, que ahora acepta la posición y el clip ya
  calculados. Lo que cambia respecto al camino de siempre, y por qué da lo
  mismo: los pesos 0 no se saltan (con todos los elementos de los huesos
  finitos, sumar 0 por un elemento no cambia nada, porque la matriz
  mezclada nunca es -0: empieza en +0 y una suma que da 0 da +0); por
  eso, en cada lote se comprueba que los huesos que usa el formato y la
  matriz combinada no tengan infinitos, NaN ni denormales, y si no, se va
  por el camino de siempre. Los merges solo mueven pesos y bytes de la
  posición (nunca denormales). Con `prof_vertices`, sin `ge_vdec_psq` o
  sin `ge_skin_rapido`, el camino de siempre.
- `gpu/ge.h`, `hle/hle.c`: `vertices_ps` en `GeStats` y en la línea
  `[TIEMPOS] Vertices` (% con paired singles). `ge_vdec_ps_disponible`.
- `wii/broadway.c`: la autoprueba `prueba_ps` también prueba
  `psq_l`+`psq_st` sin cuentas, `ps_merge00/01/10/11`, `ps_mr`, `ps_neg`,
  `ps_abs`, `ps_nabs`, `ps_div` y `ps_sel` (contra copias de bits,
  `fneg`, `fabs`, `fnabs`, `fdivs` y `fsel`): lo que haría falta para
  llevar la pantalla (la división por w) a paired singles.
- `core/opt.h`, `core/opt.c`: `ge_vdec_ps`.
- `tests/test_vdec.c`: `test_ps_bordes`, 400 tandas de 0x301 (y 0x11A) de
  hasta 200 vértices (trozos de 64 y vértices sueltos), con pesos a 0,
  vértices con todos los pesos a 0, un hueso que es el opuesto de otro
  (sumas que dan 0) y huesos o matriz de mundo con ±0, ±infinito, NaN,
  denormales y valores enormes; contra el camino de siempre, sin
  `ge_vdec_ps` y con `ge_verificar` (bit a bit cada campo del vértice).
- `tools/banco/sigill_ppc.c`: emula los paired singles para qemu (ps1 en
  una sombra, cada carril con la operación escalar), `psq_l`/`psq_lu` y
  `psq_st`/`psq_stu` con W = 0 y 1; cuenta los denormales que `psq_st`
  convierte en cero y los que mueven los merges. `tools/banco/mutar.py`:
  `--dkp` (las mutaciones con el código del Wii en `qemu-ppc -cpu 750`).
  `mutaciones/0.5.23.7.txt`.
- `hle/hle.h`, `dist/apps/wiisp/meta.xml`: versión 0.5.23.7.

## 0.5.23.6

- `gpu/ge_vertex.c`: censo `prof_repetidos` arreglado y afinado. "Con
  huesos" se contaba con el número de pesos (que nunca es 0) y no con el
  formato de los pesos: salía 100 %. Ahora, de las llamadas con los mismos
  datos y huesos que otra del mismo cuadro, qué cambia entre las dos
  pasadas (matriz de mundo, vista, proyección, matriz de textura, algún
  comando de los grupos V, T y P, o una combinación) y cuántas tienen
  huesos; y qué comandos cambian (un resumen de 16 bits de cada comando de
  la primera pasada). Línea `[TIEMPOS] Repetidas, que cambia...`
  (`ge_repetidas_report`).
- `wii/broadway.c`, `wii/main.c`: autoprueba de paired singles
  (`prueba_ps`, apagada). Al empezar cada corrida del guion, 2^23 pares al
  azar (normales, enteros al azar, pequeños, casos raros: ±0, ±infinito,
  NaN silenciosos y señalizadores, denormales, el caso de Mario Strikers) y
  sumas que se cancelan; cada carril de `ps_add`, `ps_sub`, `ps_mul`,
  `ps_madd`, `ps_msub`, `ps_nmadd`, `ps_nmsub`, `ps_sum0`, `ps_sum1`,
  `ps_muls0/1` y `ps_madds0/1` contra `fadds`, `fsubs`, `fmuls`, `fmadds`,
  `fmsubs`, `fnmadds` y `fnmsubs`, bit a bit. Escribe FPSCR (NI, RN),
  HID2 (PSE), el caso de Strikers y lo distinto por instrucción (líneas
  `[PRUEBA]`).
- `core/opt.h`, `core/opt.c`: `prueba_ps`.
- `tests/test_listas.c`: el censo con un comando cambiado y con la matriz
  de mundo cambiada. `mutaciones/0.5.23.6.txt`.
- `hle/hle.h`, `dist/apps/wiisp/meta.xml`: versión 0.5.23.6.

## 0.5.23.5

- `wii/gx_tiles.h`, `wii/gx_ge.c`: `ge_ci8_rapido` (encendido). Los índices
  CI8 de las texturas con paleta sobre un framebuffer 565 de la GPU
  (`ge_rtt_gpu`, CLUT16) se sacan de la copia RGBA8 leyendo palabras de 32
  bits (dos texels por lectura) y escribiendo los 4 índices de una fila de
  una vez; con desplazamiento <= 13 (DBZ: 5), dos texels a la vez en las
  dos mitades de una palabra (los bits de la mitad alta que bajan no llegan
  a los 8 que deja la máscara en la baja). Mismos bytes que
  `gx_ci8_from_rgba8_565`, que queda como el camino de siempre. Con
  `ge_verificar`, se calculan también con el de siempre y se comparan.
- `wii/gx_ge.c`: en la línea `rtt:` del informe, el tiempo de los índices
  CI8 esperando a la GPU (la copia del EFB) aparte, y lo comparado y
  distinto con `ge_verificar`.
- `gpu/ge_vertex.c`, `gpu/ge.c`, `gpu/ge.h`, `hle/hle.c`: censo
  `prof_repetidos` (apagado). Por cada llamada dibujada, una huella de sus
  datos (tipo, dirección, número y bytes de los vértices y de los índices),
  de los huesos que usa y de todo lo que lee la transformación (mundo,
  vista, proyección, matriz de textura y los comandos de los grupos V, T y
  P de `ge_sucio`). Cuenta las llamadas y vértices iguales a otra del mismo
  cuadro (entre dos cambios del framebuffer que se muestra), con los mismos
  datos y huesos pero otras matrices o estado, con solo los mismos datos, e
  iguales a una del cuadro anterior. Línea `[TIEMPOS] Repetidas`.
  `ge_grupo_de` (los grupos de un comando).
- `core/opt.h`, `core/opt.c`: `ge_ci8_rapido` y `prof_repetidos`.
- `tests/test_gxtiles.c`: los índices CI8 nuevos contra los de siempre en
  los 32 desplazamientos con 7 máscaras. `tests/test_listas.c`: el censo de
  repetidas (igual, otra matriz, otros huesos, otros datos, el mismo
  contenido en otra dirección, otro contenido en la misma, cuadro
  anterior).
- `tools/banco/bench_ci8.c` (lo compila `build_banco.sh`): instrucciones por
  texel de las dos conversiones con el código de devkitPPC.
  `tools/banco/mutar.py`: mutaciones en cabeceras (recompila los `.c` que
  la incluyen) y `--solo-estas` con `|` (nombres con comas).
  `tools/banco/ventanas.py`: filas de los índices CI8 y de repetidas.
  `mutaciones/0.5.23.5.txt`.
- `hle/hle.h`, `dist/apps/wiisp/meta.xml`: versión 0.5.23.5.

## 0.5.23.4

- `wii/gx_ge.c`: `ge_charolas` (encendido; 1.4). Los destinos (búferes en
  el EFB), la textura y el estado de GX de una llamada de dibujo valen para
  la siguiente mientras nada de lo que leen cambie: `gx_begin` ya no marca
  `targets_dirty` en cada llamada. La charola se apunta tras un
  `setup_targets` del camino de los triángulos cuando su resultado se puede
  repetir sin efectos: dibuja, sin texturas sobre un framebuffer (ni copia
  directa ni paleta desde la GPU, ni censo de esas texturas) y con la
  textura y todos sus niveles en la RAM. Sigue si no subió
  `ge_raster_gen`, ni `clut_gen`, ni hay marcas del grupo nuevo de destinos
  (ver `ge.c`). La corta todo lo que ya quitaba `tri_listo` o marcaba los
  destinos o el estado de GX (VRAM, presentar, `set_pf`, copias, líneas,
  puntos, borrados, vuelta a la CPU de las paletas, reiniciar; los sprites,
  solo sin `ge_charolas_sprites`).
  - La textura se vuelve a buscar en el primer triángulo de la llamada,
    como en el camino de siempre: con `tc_again` (la misma comprobación que
    `tc_lookup` sobre la entrada que ya era la buena) o, si desde la última
    no hubo ningún aviso de escritura, la entrada se comprobó en este mismo
    periodo y es contigua, solo con lo que `tc_lookup` apuntaría (`tc_hit`).
    Si cambió, `setup_targets` entero.
  - Con `ge_verificar` se hace todo como antes y, cuando la llamada habría
    seguido la charola, se compara lo que deja `setup_targets` con lo
    guardado, que la textura salga de una búsqueda rápida, que el estado de
    GX no haga falta emitirlo y, si el estado estaba listo, que lo siga.
- `wii/gx_ge.c`: `ge_charolas_sprites` (encendido). Los sprites ya no
  cortan la charola y la apuntan como los triángulos: una llamada de
  sprites sigue a la anterior (de sprites o de triángulos) con su misma
  clave. La textura pendiente se comprueba en el primer sprite que dibuja
  (después de descartar los de ancho o alto cero, que no buscan textura en
  el camino de siempre). Sin el interruptor, como antes.
- `gpu/ge_raster.c`, `gpu/ge_vertex.c`, `wii/gx_ge.c`:
  `ge_charolas_envio` (encendido). Los triángulos aceptados de una llamada
  (con los vértices en el caché de la llamada, para que los punteros sigan
  valiendo) van a una lista (`ge_raster_triangle_ccw_c`: los tres vértices y
  de cuál sale el color con sombreado plano, sin la copia `c2`) y se mandan
  juntos al acabar el ensamblado, antes de cualquier otra primitiva, del
  recorte o de `window_keep`. GX (`gx_triangles`) hace los primeros por
  `gx_triangle` hasta que el estado está listo y el resto con un solo
  `GX_Begin`, las mismas palabras y en el mismo orden.
- `gpu/ge_vertex.c`: `ge_charolas_tiras` (encendido). Las tiras sin
  descarte de caras se ensamblan en un bucle propio (`tira_charola`) con
  las reglas de `send_triangle` + `triangle_rejected_flags` + el rango de
  `range_of`, lo de la llamada en registros y los contadores sumados al
  final; un triángulo que hay que recortar con el plano cercano va por
  `send_triangle`. Con `ge_verificar`, cada decisión se compara con
  `triangle_rejected`.
- `gpu/ge_vertex.c`: `ge_charolas_ge` (encendido). Por llamada: el formato
  de vértice por puntero al guardado (sin copiar sus 76 bytes) y
  `vsource_init` sin `memset`.
- `wii/gx_ge.c`: `ge_estado_clave` (encendido). La clave del estado de GX
  de `ge_estado_sucio` se toma después de emitirlo: `apply_gx` escribe en
  el objeto de la textura el modo de repetición y el filtro (que salen del
  rasterizador, ya en la clave), y la clave de antes los tenía sin poner.
  La llamada siguiente a la primera vez de cada textura veía otra clave y
  volvía a emitir lo mismo; con charolas, la verificación lo daba por
  distinto (falso aviso en 5 pspautotests de `gpu/`).
- `wii/gx_ge.c`: verificación de las charolas, `ch_verif_listo` (la
  comprobación de que la llamada habría seguido sin preparar nada) se
  quita al empezar cada llamada y en los sprites, líneas, puntos y
  borrados: si la primera primitiva no era un triángulo, se quedaba
  pendiente y la miraba un triángulo de otra llamada (falso aviso, lo
  encontró el banco con `ge_verificar`).
- `gpu/ge.c`: `ge_clut_igual` (encendido). Un LOADCLUT con la misma
  paleta (mismo tamaño y mismos bytes, lo que `ge_load_clut` ya comparaba
  para el censo) no sube `clut_gen`: lo que depende de él sale de los bytes
  de la paleta, que no cambian. Sin él, cargar la paleta en cada llamada
  cortaba la charola en todas.
- `gpu/ge.c`, `gpu/ge_internal.h`: grupo nuevo de `ge_sucio`, D (destinos):
  FRAMEBUFPTR, FRAMEBUFWIDTH, ZBUFPTR, ZBUFWIDTH, FRAMEBUFPIXFORMAT,
  MASKRGB, MASKALPHA y LIGHTMODE (lo que `setup_targets` y `apply_gx` leen
  sin pasar por el rasterizador). `GE_SUCIO_TODO` = 63.
- `gpu/texcache.c`: `tc_again` y `tc_hit`. `core/memory.c`:
  `mem_write_notes` (cuenta los avisos de escritura).
- `hle/hle.c`, `gpu/ge.h`: líneas `[TIEMPOS] Charolas`, `Envio por
  charolas` y `Verificacion de las charolas`.
- `core/opt.h`, `core/opt.c`: `ge_charolas` (estaba reservado),
  `ge_charolas_sprites`, `ge_charolas_envio`, `ge_charolas_tiras`,
  `ge_charolas_ge`, `ge_clut_igual` y `ge_estado_clave`.
- `tests/test_listas.c`: prueba de `ge_clut_igual` (la misma paleta, más
  grande, cambiada dentro y fuera de lo que se carga, más pequeña).
- `tests/test_vdec.c`: el renderizador que lo apunta todo recibe también
  las tandas; vueltas nuevas sin el envío por charolas y sin las tiras,
  iguales a la referencia y entre sí (contadores incluidos).
- `tools/banco/bench_charolas.c` y `banco_charolas.sh`: listas al azar con
  cambios entre llamadas y entre tramos (lo que haría la CPU), corridas con
  y sin los interruptores sobre la libogc que guarda el estado. Texturas en
  la RAM y en la VRAM (dentro y fuera de un búfer en el que se dibuja; la
  del principio, al azar), llamadas que no dibujan nada (máscara de color
  entera y sin escribir profundidad), sprites de ancho cero y escrituras
  sin aviso en la textura en uso (a veces con otro periodo), paletas de
  otro tamaño y cambiadas dentro de lo que se carga. No se compara cuántas
  veces se resume la paleta (lo que ahorra `ge_clut_igual`). Con
  `REGISTRO=archivo`, el registro (las líneas `[VERIF]`); con
  `ge_verificar`, una línea `VERIF` con lo comparado y lo distinto.
  `banco_charolas.sh ... verif`: las dos corridas con `ge_verificar` y los
  distintos a 0. `mutar_gx.py --verif`: qué mutaciones ve `ge_verificar`.
  `stub_estado.c`: huella del FIFO (cada triángulo con el estado de su
  `GX_Begin`; igual con uno por triángulo o uno para muchos), con
  `gx_ge.c` compilado con `-DWIISP_FIFO_TRAZA`. `ab_versiones.py`: `--a`,
  `--b`, `--fifo` y `--sin-fifo` (contra un CLI de antes de la huella del
  FIFO). `bench_lista.c`: escena 11, como una pelea de DBZ, y 12, sprites
  en 2D de uno en uno. `ventanas.py`: por qué no siguen la charola (sin
  charola abierta, otra clave, textura) y su verificación.
  `mutaciones/0.5.23.4.txt` (GE) y `0.5.23.4_gx.txt` (GX).
  `mutar_gx.py`: mutaciones de GX contra el banco de las charolas.
  `build_tests_dkp.sh`: también `ge_raster.c`.
- `tests/test_bbox.c`, `test_censo.c`, `test_listas.c` y `frontend/app.c`: el miembro nuevo
  `triangles` de `GeHwRenderer`.
- `hle/hle.h`, `dist/apps/wiisp/meta.xml`: versión 0.5.23.4.

## 0.5.23.3

- `gpu/ge.c`: `ge_cmd_huesos` (encendido). En el bucle de comandos con
  puntero (`run_commands_ptr`), un 0x2A (número de hueso) o 0x2B (dato de
  hueso) empieza una racha: los 0x2A y 0x2B seguidos se ejecutan en un
  bucle propio (`racha_huesos`) hasta el primer comando de otro tipo, la
  parada de la lista (stall) o el final de la RAM. Deja lo mismo que uno a
  uno:
  - cada dato va al hueso `num & 0x7F` si es < 96, con los bits `arg << 8`;
  - el número cuenta en 24 bits (la máscara se pone al final: los 7 bits
    bajos no dependen de ella); un 0x2A lo pone a `arg & 0x7F`;
  - 0x2B queda a 0 si hubo algún dato; `cur_cmd` es el último comando;
    los comandos se cuentan; `ge_sucio` no cambia (los huesos no marcan
    ningún grupo).
  El byte del comando se lee aparte (`p[3]`, la RAM de la PSP está en
  little-endian) para que GCC lea el dato con `lwbrx`: ~15 instrucciones
  por comando, antes ~62. Con `ge_verificar` (y sin los censos), cada
  racha se calcula por el atajo, se deshace, se ejecuta con `execute`
  comando a comando y se compara todo (`verificar_huesos`).
- `gpu/ge.c`, `gpu/ge.h`, `hle/hle.c`: líneas `[TIEMPOS] Rachas de huesos`
  (rachas y comandos por el atajo) y `Verificacion de las rachas de
  huesos`.
- `core/opt.h`, `core/opt.c`: `ge_cmd_huesos`. `ge_bbox_triangulos` pasa a
  apagado por defecto: en DBZ cuesta más de lo que salta (ciclos de las
  peleas +0,5 a +1,0 % con él; ver CONTINUAR).
- `tests/test_listas.c`: prueba nueva de rachas (3000 listas al azar con
  0x2A y 0x2B mezclados, números por encima de 96 y junto al final de los
  24 bits, la parada dentro de la racha, la racha hasta el borde de la RAM
  y en la copia sin caché): con el atajo, sin él y por `execute`, el mismo
  estado. Las listas al azar cuentan también las rachas verificadas.
- `tests/test_main.c`, `tests/test.h`: `--solo suite,suite` (solo esas
  suites, con los interruptores por defecto que deja `test_guion` para las
  que van detrás) y `--parar` (sale en el primer fallo).
- `tools/banco/mutar.py` y `tools/banco/mutaciones/0.5.23.3.txt`: pruebas
  de mutaciones rápidas (ver `tools/banco/LEEME.md`).
- `tools/banco/ventanas.py`: filas nuevas (ciclos del PMC, banda,
  triángulos, ms del censo, rachas de huesos).
- `tools/setup_entorno.sh`: escribe `env.sh`, que leen los guiones del
  banco.
- `hle/hle.h`, `dist/apps/wiisp/meta.xml`: versión 0.5.23.3.

## 0.5.23.2

- `gpu/ge_vertex.c`: `ge_bbox_guarda` (encendido; necesita
  `ge_bbox_orden` y `ge_bbox_pantalla`). Cuando la prueba de pantalla de
  una caja no puede, se repite con las restricciones que de verdad cumple
  todo vértice que el GE dibuja (`round_to_screen`, `triangle_rejected`):
  - la banda de guarda: 0 ≤ sx ≤ 4095,97 y 0 ≤ sy < 4095,97;
  - sin recorte de profundidad, el rango z: 0 ≤ sz < 65536;
  - delante del plano cercano (con recorte) o w ≥ 0 (sin él).
  Con w > 0 son lineales en coordenadas de recorte, con márgenes como los
  de los bordes. Antes de calcular nada:
  - solo cuentan las restricciones que algún vértice de Q incumple (las
    demás no cambian nada);
  - un vértice de Q que las cumple todas y está dentro de un borde es un
    testigo: por ese borde no hay nada que hacer.
  Para los bordes que quedan se buscan multiplicadores λ ≥ 0 con
  g + mg + Σλ·b < 0 en toda la caja (el dual del programa lineal: los
  vértices con 1 a 3 restricciones, y con Σλ = 4096 para el caso vacío),
  y se comprueba con margen de redondeo. Las salidas tempranas (primer
  vértice visto, una esquina dentro, esquinas a los lados) solo cuentan
  vértices que están en la banda (y, con una escala z que no cubre el
  frustum, en el rango z). La caja guarda también la escala y el centro z
  si se salta por la banda.
- `gpu/ge_vertex.c`: `ge_bbox_triangulos` (encendido; necesita
  `ge_bbox_llamada` y `ge_bbox_pantalla`). Si la prueba de la caja de una
  llamada (o su primer vértice) no la salta, se repasan sus posiciones
  triángulo a triángulo (lista, tira o abanico) con las reglas de
  descarte del GE. A cada vértice se le marca solo lo que es seguro con el
  margen de 2^-9 de su magnitud:
  - w < 0, detrás del plano cercano o delante de él;
  - fuera de la banda o del rango z;
  - más allá de un borde de la pantalla o fuera de un plano de recorte.
  Un triángulo se descarta con seguridad si:
  - los tres tienen w < 0, o están fuera del mismo plano;
  - con recorte, los tres están detrás del plano cercano, o uno delante
    está fuera de la banda, o los tres (con w > 0) más allá del mismo
    borde: el polígono recortado también lo está, porque los puntos del
    corte son combinaciones de los vértices;
  - sin recorte, uno tiene w < 0 o está fuera de la banda o de z, o los
    tres están más allá del mismo borde.
  Si todos se descartan, la llamada se salta como en `ge_bbox`. Al primer
  triángulo que podría dibujarse, para. Un vértice dentro del frustum y de
  la pantalla sale sin calcular su margen.
- `gpu/ge.c`, `gpu/ge.h`, `hle/hle.c`: la línea `Cajas exactas` añade las
  cajas y llamadas fuera por la banda y por sus triángulos (y cuántas se
  probaron). El censo de cajas (`prof_bbox`) añade lo que cuesta repasar
  los triángulos y, para las llamadas probadas por su caja que no dibujan
  nada: con qué salida de la prueba se quedaron, cuántas tienen recorte de
  profundidad, cómo son sus vértices (detrás de la cámara, detrás del
  plano cercano o fuera de z, fuera de la banda, en la pantalla, fuera de
  ella) y sus triángulos, y cuántas tienen todos sus triángulos
  descartados con seguridad.
- `core/opt.h`, `core/opt.c`: `ge_bbox_guarda` y `ge_bbox_triangulos`.
- `tests/test_bbox.c`: las pruebas al azar tienen un modo más con los dos
  interruptores (imagen y primitivas iguales a las de referencia). Pruebas
  nuevas: llamadas que cruzan el plano de la cámara a su alrededor; un
  vértice junto al límite de la banda, del rango z, del plano cercano o de
  w = 0; y una escala z que deja fuera lo más cercano del frustum,
  cambiada entre la caja y las llamadas.
- `tools/banco/ab_versiones.py`: qemu con `MALLOC_PERTURB_=165`. El stub de
  GX no escribe en los destinos de `GX_CopyTex`/`GX_CopyDisp`, y lo que el
  emulador lee de ahí es lo que hubiera en la memoria: cambiaba con cada
  binario (`gpu/exact/pixel` salía distinto entre versiones sin que
  cambiara nada; ya estaba anotado en `tools/banco/LEEME.md`).
- `hle/hle.h`, `dist/apps/wiisp/meta.xml`: versión 0.5.23.2.

## 0.5.23.1

- `gpu/ge_vertex.c`: `ge_bbox_orden` (encendido). La prueba exacta de una
  caja (`bb_fuera`) da lo mismo con menos cuentas:
  - las esquinas suman primero x + y y luego z (el mismo redondeo);
  - mientras algún plano sin margen deja fuera todas las esquinas, cada
    esquina nueva solo se compara con esos planos (si sigue fuera de
    alguno, no puede estar dentro);
  - los bordes de la pantalla se calculan solo para las esquinas que
    pueden estar dentro o cuando hace falta el programa lineal;
  - con un plano que deja fuera las 8 esquinas sin margen, los planos
    con margen se miran antes que la pantalla, y solo esos;
  - sin ninguno, los planos con margen no se miran (no pueden dar nada).
  El camino de la 0.5.23 queda como `bb_fuera_lento`.
- `gpu/ge_vertex.c`: `ge_bbox_aabb` (encendido). La caja de las
  posiciones de cada llamada (s8 y s16) con los mínimos y máximos en
  registros; float, como antes. El camino de la 0.5.23 queda como
  `bb_aabb_lento`.
- `gpu/ge.c`, `gpu/ge.h`: el censo de cajas (`prof_bbox`) separa lo que
  cuesta leer las posiciones de cada llamada, y cuántas llamadas y
  vértices se leen.
- `core/opt.h`, `core/opt.c`: `ge_bbox_orden` y `ge_bbox_aabb`.
- `tests/test_bbox.c`: en las pruebas al azar, cada ronda se corre otra vez
  con los dos interruptores al revés y todas las cuentas (`ge_stats`
  entera) y la imagen tienen que salir iguales. Prueba nueva: cajas
  detrás de la cámara a menos del margen (traslaciones grandes que se
  cancelan).
- `tools/banco/bench_lista.c`: escena 10, como DBZ: llamadas enteras fuera
  del plano x− con el viewport ×1.
- `hle/hle.h`, `dist/apps/wiisp/meta.xml`: versión 0.5.23.1.

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
