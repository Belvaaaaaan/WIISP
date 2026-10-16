# Cómo seguir con WIISP (traspaso entre sesiones)

Este documento resume dónde se quedó el trabajo, para seguir en una sesión
nueva sin perder nada. Léelo junto con `README.md`, `ARQUITECTURA.md` y
`docs/OPTIMIZACION.md`.

## Reglas del proyecto

- Todo en español: respuestas, comentarios del código y mensajes de commit.
  El texto que se ve en la pantalla del Wii va **sin acentos** (la fuente no
  los tiene).
- Rama de trabajo: `claude/psp-wii-emulator-dt141p` hasta la v0.4.3; la
  v0.4.4 sigue encima en `claude/wiisp-psp-wii-emulator-gr0b83` y la v0.5.7
  en `claude/happy-euler-b460be` (la rama que asigna cada sesión puede
  cambiar: partir siempre de la más reciente). No abrir pull requests si no
  se piden.
- Comentarios del código: explicar qué hace y por qué, **sin citar otros
  emuladores** ("igual que X", "estilo de Y"); se ve poco profesional. Los
  avisos de copyright del código portado (GPL) sí se quedan. Las
  referencias a otros proyectos van en los documentos, no en el código.
- Nunca subir datos de juegos (ISO, EBOOT descifrados, volcados): son de
  Sony/Rockstar.
- PPSSPP (GPLv2+) es la referencia de comportamiento: ante una duda de cómo
  se comporta la PSP, se mira cómo lo hace PPSSPP y se porta.
- Antes de subir algo: `make -f Makefile.pc test`, pspautotests en x86 (y en
  PowerPC si se toca algo sensible al orden de bytes) y compilar para el Wii.

## Preparar el entorno (sesión nueva)

```sh
sh tools/setup_entorno.sh ../wiisp-entorno   # pspautotests, PPSSPP, devkitPPC
export DEVKITPRO=$PWD/../wiisp-entorno/devkitpro
export DEVKITPPC=$DEVKITPRO/devkitPPC
make -f Makefile.pc test                       # pruebas unitarias (x86)
python3 tests/autotests.py ../wiisp-entorno/pspautotests --list tests/autotests_pass.txt
make -j8                                       # wiisp.dol para el Wii
```

- PowerPC big-endian (como el Wii): `apt-get install gcc-powerpc-linux-gnu
  libc6-dev-powerpc-cross qemu-user`, luego `make -f Makefile.pc test-ppc
  cli-ppc` y `WIISP_CLI="qemu-ppc build-pc/wiisp-cli-ppc" python3
  tests/autotests.py ...`.
- devkitPPC sale de la imagen Docker oficial: el script la baja de
  `mirror.gcr.io` (sin límite) y, si no responde, de Docker Hub (100
  manifiestos por hora y por IP: en la nube suele dar "429").
  `pkg.devkitpro.org` está bloqueado en el entorno de la nube.
- Programas de PSP de prueba (`tests/guest`): `apt-get install
  gcc-mipsel-linux-gnu` y `make -C tests/guest check`.
- Dolphin (opcional, lento de compilar) sirve para probar el .dol:
  `tools/dolphin_autotest.py` (necesita Xvfb).
- CLI de PC: `build-pc/wiisp-cli --run --frames N --log salida.log juego.iso`
  deja el mismo registro que `wiisp.log` en el Wii, y al llegar al límite de
  frames vuelca el estado de los hilos (`[DIAGNOSTICO]`).

**Paquete para el usuario**: un zip con `apps/wiisp/boot.dol` (copia de
`wiisp.dol`) y `apps/wiisp/meta.xml` (copia de `dist/apps/wiisp/meta.xml`,
subiendo su `<version>`). Se descomprime en la raíz de la SD.
**Con cada compilación se entrega también su `autotest.txt`** (pedido del
usuario), escrito para lo que esa versión necesita medir. Ojo: WIISP usa
primero `sd:/wiisp/autotest.txt` y solo si no existe el de `apps/wiisp/`;
un autotest viejo en `sd:/wiisp/` gana al del zip (pasó con la 0.5.14.1).
Corridas: normalmente "sin" y "con" (con `.log` y `.capturas.txt`); "verif"
solo cuando hay un camino rápido nuevo, y con `capturas 0`.

## Objetivo actual: que arranque GTA: Liberty City Stories (ULUS10041)

El usuario lo prueba en su Wii real desde una ISO en USB y manda
`wiisp.log` (carpeta `apps/wiisp`). Nosotros no tenemos el juego: todo se
diagnostica con ese registro. En él:

- `[LLAMADA] hN funcion(args) = resultado @ms`: las primeras 8000 llamadas
  del juego al firmware (hN = número de hilo).
- `[DIAGNOSTICO]`, `[HILO]`, `[CALLBACK]`, `[SEMA]`, `[EVF]`, `[ULTIMA]`:
  estado de cada hilo y en qué espera, cuando el juego se atasca, al pulsar
  HOME o tras un fallo de CPU.
- `[STATS]`: instrucciones, % de VFPU, cambios de hilo, llamadas del HLE al
  juego (callbacks/interrupciones).

### Historia del diagnóstico

1. **v0.4.1**: el juego se paraba tras ~6 M de instrucciones con 0 llamadas
   del HLE al juego. Faltaban los callbacks de la PSP (eran de mentira) y la
   Memory Stick respondía "no soportado". Arreglado en el commit `bef18eb`
   (v0.4.2): callbacks, Memory Stick por `sceIoDevctl`, rutas como en la
   PSP (`DISC0:` en mayúsculas, `\`), E/S asíncrona, diagnóstico.
2. **v0.4.2** (log del usuario): el arranque ya avanza (corre el callback de
   energía, el hilo `threadmain` trabaja). El hilo `threadmain` recorre el
   disco entero con `sceIoDopen`/`sceIoDread` desde `DISC0:/PSP_GAME/`.
   WIISP devolvía `.` y `..` en los directorios del UMD, así que GTA entraba
   en `./././...` y `../..` sin fin (solo paraba al quedarse sin
   descriptores, error 0x80020320). **PPSSPP no devuelve `.` ni `..` en el
   UMD** (`ISOFileSystem::GetDirListing`). Arreglado en el commit de este
   traspaso (v0.4.3), **aún sin probar en el Wii**.

Secuencia de arranque de GTA LCS que ya se ve en el log (coincide con los
logs de PPSSPP publicados): `user_main` crea `stupidthread` y se borra;
`stupidthread` espera el fin de `user_main` (falla con 0x80020198, es lo
normal), crea `threadmain` (prio 0x38, VFPU) y se borra; `threadmain` crea
`SysManager` (prio 0x6F: crea los callbacks `PowerCallback`, `UMDCallback`,
`ExitCallback` y hace `sceKernelDelayThreadCB(10000)` en bucle), activa el
UMD y recorre el disco. Según PPSSPP después vienen `UmdStreamThread`
(prio 0x20), `eecdstreamThread` (0x40), `memstick` (0x14, comprueba
partidas con sceUtilitySavedata modo SIZES) y el video de introducción.

### Qué hay en el commit de este traspaso

- `io.c`: `sceIoDread` del UMD sin `.` ni `..` (la prueba de `tests/test_io.c`
  actualizada).
- `kernel.c`: **los callbacks corren como código normal del hilo**, como en
  PPSSPP. Antes se ejecutaban en un bucle anidado de `cpu_run` y una espera
  dentro de un callback no esperaba de verdad. Ahora cada hilo tiene una
  pila `cbf[MAX_CB_DEPTH]` (`CbFrame`: CPU y espera interrumpida); el
  callback vuelve por `HLE_THREAD_CB_TRAMPOLINE` (0x08000020, syscall
  `HLE_SYSCALL_THREAD_CB_RETURN`) a `kernel_thread_cb_return`, que ejecuta
  el siguiente callback pendiente o restaura el hilo y vuelve a comprobar
  su espera (`recheck_wait`, que ahora cubre también LwMutex, FPL, objetos
  borrados durante el callback y esperas del HLE con un gancho).
  Funciones: `start_callback`, `enter_callback`, `next_pending_callback`,
  `resume_after_callbacks`, `kernel_cb_wait` (la espera empieza y, si hay
  callbacks pendientes, se ejecutan ya), `sceKernelCheckCallback`.
- APIs nuevas del kernel, **aún sin usar**: `kernel_wait_object_timeout`,
  `kernel_wake_object_mask`, `kernel_set_wait_recheck`, `KWAIT_UMD`. Son
  para las esperas del UMD (siguiente paso 2).
- `host/main.c`: el CLI vuelca el estado al llegar al límite de frames.
- `tools/setup_entorno.sh`: este entorno en una sesión nueva.

Pruebas: 360 comprobaciones unitarias y 214/214 pspautotests en x86; una
prueba sintética de callbacks (notificar, CheckCallback, SleepThreadCB en
otro hilo) da OK. No se repitió la pasada en PowerPC.

### Sesión siguiente (v0.4.4)

- **Captura del usuario** (sin `wiisp.log`): `[STATS]` crece 107 944
  instrucciones por minuto emulado, ~207 cambios de hilo por segundo y
  **0 llamadas del HLE al juego**; 205 887 frames en 5,1 s (todos los hilos
  esperan y los frames pasan en vacío). Echando cuentas (6,15 M en 57
  minutos), el juego casi no ejecutó nada antes de quedarse así: solo hay
  un hilo que se despierta ~100 veces por segundo (como el bucle
  `sceKernelDelayThreadCB(10000)` de `SysManager`). Coincide con lo descrito
  para la v0.4.1 (~6 M, 0 llamadas); no se sabe qué versión era porque el
  programa no la mostraba. **Desde la v0.4.4 la versión sale en el menú y
  en la primera línea de `wiisp.log`.** Hace falta el `wiisp.log` (sus
  `[DIAGNOSTICO]` de "atasco" dicen qué espera cada hilo).
- Paso 2 (UMD) hecho: commit "UMD como en PPSSPP". Pasan umd/api,
  umd/register y umd/wait (umd/io y raw_access necesitan una ISO).
- Paso 3 (LwMutex) hecho: commit "LwMutex como objetos del kernel". De
  paso: plazos de espera como la PSP (mínimo 205 us + 30), tiempo restante
  escrito al despertar, `sceKernelCancelSema`, `sceKernelCancelEventFlag`,
  y `SCE_KERNEL_ERROR_ILLEGAL_ATTR` corregido (0x80020191). **Los 12
  pspautotests de threads/callbacks que ya pasan validan los callbacks
  como código del hilo contra una PSP real.** Total 244/244.
- Siguen fallando en threads/callbacks: `callbacks` y `waittypes` (faltan
  los mutex normales `sceKernelCreateMutex`...), `exit` (validación de
  `sceKernelRegisterExitCallback` y `LoadExecForUser_362A956B`), `count`
  (orden: el callback notificado más veces corre antes), `create` (máximo
  64 callbacks; la PSP admite 1024+), `combos` (coste de notificar a un
  hilo mejor que duerme: 10-25 us).

### v0.4.5 (misma sesión, sin el Wii a mano)

- Paso 4 hecho: event flags por orden de llegada, errores, comprobaciones
  y costes de PPSSPP (los 9 de threads/events pasan).
- Mutex normales del kernel (los 11 de threads/mutex pasan).
- Callbacks pendientes por orden de creación; hasta 1024.
- Paso 5 (parcial): el hilo raíz y los module_start de los PRX usan
  `module_start_thread_parameter` (prioridad, pila, atributos), como
  PPSSPP. Los PRX del disco que PPSSPP finge ya se fingían en module.c.
- Paso 7 (parcial): RegisterExitCallback según SDK, WakeupThread propio.
- 266/266 pspautotests en x86 **y en PowerPC** (qemu), 361 unitarias.
- Siguen fallando en threads/callbacks: `combos` (coste de notificar),
  `exit` (`LoadExecForUser_362A956B`) y `waittypes` (faltan mailboxes,
  message pipes, VPL...).

### Diagnóstico del wiisp.log de la v0.4.5 (DBZ TTT y GTA VCS)

**GTA: Vice City Stories (ULES00502)**: arranca bien (recorre el disco,
carga sceATRAC3plus y sceMpeg, partidas, UmdStreamThread) y empieza el
video de introducción. El registro se corta a los 332 ms emulados, sin
`[DIAGNOSTICO]` ni `[STATS]`: el emulador entero se congela. Causa:

- `MPEGreadThread` (h5) llama a `sceMpegRingbufferPut`, que llama a la
  función de lectura del juego con `kernel_call_guest_sp` (mpeg.c:348).
  Esa función corre en un **bucle anidado de `cpu_run` sin salida**
  (kernel.c, `kernel_call_guest_sp`): mientras no vuelva, no hay cambios
  de hilo, ni vblank, ni eventos, ni se lee el mando, ni avanza el frame.
- La función de lectura del juego hace `WaitSema(UmdStreamSema)`,
  `SetEventFlag(UmdStreamEventFlag, 1)` (las dos últimas líneas de h5) y
  luego espera el bit 2 con `WaitEventFlag`, que pone `UmdStreamThread`
  (h3) al terminar de leer. Dentro de la llamada anidada no se puede
  esperar (`wait_current` da 0x800201A7) y h3 nunca corre: el juego se
  queda en un bucle para siempre dentro del syscall.
- PPSSPP (`sceMpeg.cpp`, `hleEnqueueCall` + `PostPutAction`) ejecuta ese
  callback como código normal del hilo que llamó, después del syscall:
  puede esperar y los demás hilos siguen. Es lo mismo que ya se hizo con
  los callbacks de hilo (`HLE_THREAD_CB_TRAMPOLINE`).
- Por qué el registro no muestra el final: la traza omite las llamadas que
  repiten una de las 6 anteriores, y `fsync` solo se hace al escribir una
  línea nueva; en libfat lo no sincronizado se pierde al apagar.
- Por qué se ve la consola: el juego fijó el framebuffer a 0
  (`sceDisplaySetFrameBuf(0, ...)`), y sin framebuffer
  `video_draw_psp_frame` no dibuja nada (tampoco el contador de FPS).
- **GTA LCS (ULUS10041) confirmado** con su wiisp.log (modo exacto, se
  congela a los ~10 s reales, 290 ms emulados): idéntico. `MPEGreadThread`
  (h6) hace `RingbufferAvailableSize`, `WaitSema(UmdStreamSema)`,
  `SetEventFlag(UmdStreamEventFlag, 1)` dentro del callback de
  `sceMpegRingbufferPut` y ahí se para. Fuera del video, la misma rutina
  (h1, líneas 588-589 del registro) sigue con
  `WaitEventFlag(UmdStreamEventFlag, 2, CLEAR)`, que espera a
  UmdStreamThread (h3). La captura antigua (todo ocioso, 0 llamadas)
  era de una versión anterior y es otro síntoma.

**Dragon Ball Z: Tenkaichi Tag Team (ULUS10537)**: la libc del juego
falla al iniciar y ejecuta `break`. Le faltan
`sceKernelExtendThreadStack` (0xBC80EC7C: ejecuta una función del juego
con otra pila; también es una llamada al juego desde el HLE),
`sceKernelStopUnloadSelfModuleWithStatus` (0x8F2DF740) y
`sceKernelMemset` (0xA089ECA4, debe devolver el destino). Sin nombre en
nid_names.c los dos primeros.

### v0.4.6: corregida la congelación del video de GTA

- `kernel_enqueue_call` (kernel.c): llamadas al juego como código del hilo
  (ver ARQUITECTURA.md 6.4). `sceMpegRingbufferPut` la usa (varias rondas
  encadenadas si el ringbuffer da la vuelta, como `PostPutAction`); solo
  desde una interrupción sigue con `kernel_call_guest_sp`. El registro
  marca esas llamadas con "= (llama al juego)".
- DBZ TTT: `sceKernelExtendThreadStack` (pila nueva y la misma vía),
  `sceKernelMemset` (devuelve el destino) y
  `sceKernelStopUnloadSelfModuleWithStatus`.
- `tests/guest`: programas de PSP en C compilados con el gcc MIPS de
  Ubuntu (`-mabi=eabi`, imports generados desde nid_names.c) y ejecutados
  con el CLI. `mpeg_put.c` reproduce la lectura del video de GTA (con la
  v0.4.5 el callback recibe 0x800201A7 y no lee nada); `extend_stack.c`.
  Útil para reproducir lo que hagan los juegos sin tenerlos.
- Pendiente de ver con el juego real: qué hace GTA después del video. El
  video se "salta" leyendo el archivo entero por UmdStreamThread con los
  tiempos de E/S de la PSP, así que puede tardar (pantalla con la consola).

### v0.4.7: el CLI para Windows y la traza fuera de la pantalla

- wiisp.log de la v0.4.6 (GTA LCS, modo exacto, detenido con HOME a los
  333 ms): **ya no se congela**. Lee el primer video (1,19 MB en rondas de
  64 KB por UmdStreamThread), lo da por terminado, cierra sus hilos, salta
  con `sceIoLseek` a 0x03358000 y empieza el segundo. Iba lentísimo en
  tiempo real: 1,76 M instrucciones en todo el rato. Causa probable: cada
  `[LLAMADA]` se escribía también en la consola del Wii (cada línea
  desplaza la pantalla) y en el USB.
- Ahora la traza `[LLAMADA]` va solo a wiisp.log (no a la pantalla ni a
  stderr) y lo escrito se sincroniza también una vez por frame
  (`log_tick`), para no perder el final si el juego deja de llamar al HLE.
- CLI para Windows (`make -f Makefile.pc win`, paquete en dist/windows):
  el usuario (Windows 11) ejecuta sus juegos en el PC, mucho más rápido
  que en el Wii, y manda wiisp.log y capturas. Probado con Wine: ELF e ISO
  con rutas de Windows y espacios, el .bat completo. Nuevas opciones:
  `--capturas N`, BMP si el nombre acaba en .bmp, progreso cada 10 s
  reales, Ctrl+C limpio. Las capturas PPM/BMP repiten los bits altos al
  pasar de 16 a 24 bits (idénticas a las de pspautotests).
- La carpeta de la ISO (ms0:/) se calcula también con rutas `\`.

### v0.4.8: la memoria libre salía negativa

- wiisp.log del CLI de Windows (v0.4.7, GTA LCS): pasa **todos los videos
  de introducción**, descarga los módulos de video (StopModule /
  UnloadModule) y al empezar a cargar el juego cae con un fallo de CPU
  (escritura en 0x00000008, pc 08AA4274, threadmain). Causa:
  `sceKernelMaxFreeMemSize` devolvía 0xFFFE0000; GTA creaba su montón
  `StreamingHeap` con ese tamaño menos 0x80000, `CreateFpl` fallaba y la
  reserva siguiente devolvía un puntero nulo.
- Error en `find_gap` (kernel.c): al saltar un bloque ocupado y reiniciar
  el recorrido no se reiniciaba `next`; con bloques pegados en cierto orden
  de la tabla (pilas de hilos reservadas desde arriba) el hueco salía
  negativo (-0x20000, la pila de threadmain). También afectaba a
  TotalFreeMemSize y a las reservas desde arriba (fallaban sin motivo).
  `tests/guest/memory.c` lo reproduce.
- Sin capturas: el juego aún no había fijado ningún framebuffer
  (`sceDisplaySetFrameBuf(0, ...)`).

### v0.4.9: ¡GTA LCS arranca en el Wii! Desglose de tiempos

- wiisp.log de la v0.4.8 en el Wii: GTA LCS pasa la introducción, carga y
  corre (2 minutos de juego sin un error). Muy lento (0,3-0,5 FPS, 0,2
  MIPS, 3 %), pero el usuario lo tenía en **modo exacto** (rasterizado por
  software): con 0,2 MIPS la CPU emulada usaba menos del 5 % del tiempo.
  Minuto 1 de juego: 1.762 M instrucciones (la carga); minuto 2: 225 M.
- `[TIEMPOS]` cada 30 s reales (ver ARQUITECTURA.md 6.4). Ejemplo con la
  demo cubevfpu en el CLI: modo exacto 98 % "rasterizar", ~90 imágenes/s;
  con `--null-gpu`, 30.000 imágenes/s. En el Wii también cuenta texturas
  decodificadas y framebuffers bajados/subidos de GX.
- Siguiente: el usuario prueba en **modo rápido (GX)** y manda el registro;
  según el reparto, atacar lo que más pese (OPTIMIZACION.md: frameskip,
  caché de texturas con paletas nativas de GX, caché de vértices, T&L de
  GX, reemplazo de funciones, dynarec).

### v0.4.10: texturas sin releer en cada dibujo y el GE por dentro

Registro de la v0.4.9 en el Wii, GTA LCS en modo rápido (GX), ~50 min
reales, 100 s de juego sin errores. Reparto estable por cada 30 s reales:
GE 61-66 %, CPU 17-18 %, texturas 10-12 %, framebuffers 5-7 %, esperar GX
~1 %; ~8,5 % de velocidad (~5 vblanks/s). En la carga, CPU 84 % y E/S 11 %.
Por vblank: ~41.000 vértices calculados, ~26.000 comandos, ~8.200
triángulos dibujados (unos 2.100 ciclos del Wii por vértice). Un tramo con
solo 67 texturas decodificadas gastó igual un 10,4 % en texturas: el coste
era **comprobarlas**, no decodificarlas. GTA no cambia de framebuffer al
mostrar (copia su imagen al de la pantalla), así que "imagenes" sale 0.

Lo que cambia (ARQUITECTURA.md 7, "Cuándo se vuelve a leer una textura"):

- Antes: cada llamada de dibujo que cambiaba de textura leía la textura
  entera (hash), y la paleta (1 KB) en todas; `TEXFLUSH`, que el SDK manda
  al elegir cada textura, anulaba el único atajo.
- Ahora `gpu/texcache.c` (con pruebas en el PC): paleta una vez por carga;
  cada textura como mucho una vez por periodo entre sincronizaciones
  CPU-GE (como PPSSPP); espaciado de 1, 2, 4, 8 y 16 cuadros para las que
  no cambian (botón 1/X en el Wii antes de ejecutar: "texturas
  rapidas/seguras", en `wiisp.cfg`); avisos de escritura por páginas de
  4 KB desde `core/memory.c`; vistazo de 8 palabras; texturas inestables.
  Tabla hash en vez de recorrer las 256 entradas, y un hash con cargas de
  32 bits.
- `mem_valid` ya no pasa por los ganchos de la VRAM: antes, comprobar el
  origen de una transferencia del GE lo marcaba como escrito por la CPU y
  obligaba a subirlo otra vez a GX (posible ahorro en las "subidas" de
  GTA, que copia su imagen con transferencias).
- `[TIEMPOS]` con más líneas: "GE por dentro" (muestreo cada 1 ms por
  fases), "Dibujo" (llamadas por vblank, con índices, reutilizan vértices,
  tipos), "Vertices" (pedidos frente a calculados, huesos, morph, luces),
  "Triangulos" (dibujados, de espaldas, fuera de pantalla, recortados) y
  la caché de texturas.
- `tests/guest/ge_draw.c`: 8 mallas con índices, luz y textura con paleta;
  sirve para ver esas líneas en el CLI.

**Qué mirar en el próximo registro de GTA**: cuánto bajan "texturas" y
"subidas"; si hay "cambios sin aviso" o texturas raras en pantalla (si
las hay, probar "texturas seguras" con 1/X y comparar); y el reparto de
"GE por dentro" y "Dibujo"/"Vertices"/"Triangulos", que decide el siguiente
paso: reutilizar vértices en más casos (respetando que la PSP arrastra uv
y normal del vértice anterior cuando faltan, por eso hoy se leen en
orden), enviar por lotes a GX, o no rehacer el estado de GX en cada
llamada.

### Registro de la v0.4.10 (GTA LCS, modo rápido, ~4,5 min)

- Velocidad 8,8-9,6 % (antes ~8,5 %). Texturas 6,6-9 % (antes 10-12 %):
  95-98 % de usos sin leerlas, 0 cambios sin aviso, 0 inestables.
- GE 63-67 % por dentro: transformar y luces 21-26 %, ensamblar 12-18 %,
  leer vértices 9,5-20 %, comandos 7-8 %, enviar a GX 2-4 %, estado ~1 %.
- **GTA no usa índices** (0 %): tiras (65-75 %) y listas. Reutilizar
  vértices no le sirve; enviar por lotes tampoco pesa (2-4 %).
- ~1.250 llamadas de dibujo por vblank (~40 vértices cada una); ~48.000
  triángulos recibidos y solo un 16-25 % dibujados (30-47 % de espaldas,
  28-56 % fuera de pantalla): se transforma todo para tirar la mayoría.
- Luces encendidas en el 93-95 % de los vértices pero con 0,1-0,3 luces de
  media: casi siempre el color es ambiente + material.
- Con un 4 % de vértices con huesos, "leer vertices" pasa de 9,5 a 20 %: el
  skinning (`ge_skin`) no tiene camino rápido, siempre la aritmética exacta
  del GE.
- Cada llamada recalcula dos productos de matrices 4x4 con la aritmética
  del GE (`compute_transform_state`) aunque no cambien.
- 800-2.000 texturas decodificadas cada 30 s frente a ~60 cambios con
  aviso (caché pequeña o la misma textura con otra paleta); ~420 cargas de
  paleta por vblank. Framebuffers 6 % (2 bajadas y 0,5 subidas por vblank;
  las subidas no bajaron con el cambio de `mem_valid`).
- Orden propuesto: skinning rápido, matrices solo al cambiar, color
  constante sin luces, descartar antes de calcular todo, paletas de GX
  (TLUT), copia de framebuffer en la GPU, T&L por hardware.

### v0.5.0: el Wiimote solo como mando de PSP

A petición del usuario, antes de seguir optimizando. Wiimote en horizontal
(`frontend/wiimote.c`, independiente de libogc, con `tests/test_wiimote.c`):
cruceta = stick analógico, 1 = X, 2 = O, B = triángulo, A = cuadrado,
- = L, + = R, B+1 = SELECT, B+2 = START, la cruceta de la PSP con el
movimiento (inclinar 50° adelante / 75° hacia ti, girar 55° como un
volante; histéresis de 15°, 3 informes seguidos, sin contar sacudidas) y
HOME mantenido 1 s para volver al menú ("SALIR..." en pantalla). El menú
también va con el Wiimote en horizontal. GameCube igual que antes.
Pendientes: Wiimote + Nunchuk y mando Clásico.

Sin probar aún en el Wii: los signos de los ejes del acelerómetro salen
de WiiBrew (en horizontal +x hacia el jugador, +y hacia el sensor). Si
inclinar hacia delante da "abajo", basta con cambiar el signo en
`wm_accel`. wiisp.log apunta los cambios (`[MANDO] cruceta por
movimiento: ...` con los grados).

### v0.5.1: el juego no reaccionaba a los mandos

wiisp.log de la v0.5.0: el Wiimote se leía en plena partida (líneas
`[MANDO]` de la cruceta por movimiento) y GTA llama a
`sceCtrlReadBufferPositive` cada 2 vblanks, pero no reaccionó a nada, ni
con el mando de GameCube. A ~9 % de velocidad, GTA lee el mando unas 2,6
veces por segundo real y solo veía lo que estuviera pulsado justo en ese
instante: un toque corto caía entre dos lecturas. Ahora (`hle/display.c`)
lo que se pulsa entre dos lecturas del juego se le entrega en la siguiente
aunque ya se haya soltado, y wiisp.log apunta lo que lee el juego
(`[MANDO] el juego lee: START; stick 128, 128 @12345 ms`, los primeros 300
cambios por programa). Si el juego lee los botones y aun así no reacciona,
el fallo está en otra parte (por ejemplo, el estado del juego).

El registro tenía 2 MB en total y se cortaba en el segundo juego de la
sesión: ahora son 2 MB por programa (8 MB en total).

### v0.5.2: la causa real de que GTA ignorara el mando

El registro de la v0.5.1 mostraba que GTA leía bien los botones (START, X,
el stick...) y aun así no reaccionaba. `sceCtrlReadBufferPositive` espera
al siguiente vblank, y al despertar el núcleo ponía v0 = 0
(`wake(i, 0)` en `kernel_vblank`): el juego recibía "0 muestras leídas" y
tiraba los datos. La traza decía "= 1" porque se escribe antes de la
espera. Ahora la espera del vblank conserva su valor de retorno
(`kernel_wait_vblank_ret`) y la lectura sigue a PPSSPP (`__CtrlReadBuffer`):
con muestras sin leer devuelve en el acto cuántas copió; si no, espera y
devuelve 1; más de 64 da 0x80000104. `tests/guest/ctrl_read.c` lo
reproduce (antes daba 0 siempre).

### v0.5.3: turbo

El usuario confirmó que con la v0.5.2 los mandos ya funcionan en GTA. Para
no esperar media hora a la cinemática inicial: turbo con B + A (Wiimote) o
Z + Y (GameCube), "TURBO" en pantalla. `ge_turbo` hace que PRIM, bezier y
spline solo avancen sus direcciones (sin vértices ni dibujo); el resto del
GE sigue igual. `ge_draw` en el CLI: 2,6 s normal, 0,2 s con --null-gpu,
0,03 s con --turbo. Pendiente: save states (pedido; plan en la
conversación: guardar módulo a módulo como el DoState de PPSSPP, con una
prueba de guardar/cargar en PC) y el "warping" de los modelos que ve el
usuario (probar la misma escena en modo exacto para saber si es de GX).

### v0.5.4: diagnóstico del warping

El usuario confirmó que el warping (texturas que "nadan" como en PS1) solo
sale con GX, no en modo exacto. Descartado con el cubo de pspautotests
(renderizador exacto, `--fast-math` contra exacta): solo 30 píxeles de
borde cambian de verdad, ningún desplazamiento de textura. Recorte,
posiciones (1/16 de píxel) y profundidad son el mismo código en los dos
modos. Sospechoso: la perspectiva de texturas en GX (proyección ortográfica,
s/w, t/w y q = 1/w por la normal, división s/q por píxel); q muy pequeña
con w grande, y el hardware podría interpolarla con poca precisión (Dolphin
lo hace en float, no sirve de referencia). Arreglo candidato: multiplicar
el triángulo por su w más cercana (`tex_scale3` en gx_ge.c; s/q igual, q en
(0, 1]; 3 multiplicaciones por vértice). Versión de diagnóstico con dos
interruptores durante el juego (B + − / B + +, Z + L / Z + R): matemática
MR/ME y texturas TN/TS, en la esquina y en wiisp.log (`[DIAG]`). Con B
pulsado primero, − y + ya no son L/R (como A). Si TN quita el warping:
dejarlo y quitar los interruptores cuando ya no hagan falta. Si no: probar
ME; si tampoco, la perspectiva nativa de GX (GX_PERSPECTIVE con
(X·w, Y·w, −w); la profundidad saldría de w y habría que resolverla).

### v0.5.5: sceKernelCheckThreadStack en Kernel_Library (DBZ TTT)

DBZ Tenkaichi Tag Team la importa de Kernel_Library (NID 0xD13BDE95), donde
no estaba (solo en ThreadManForUser): devolvía 0, su libc creía que no le
quedaba pila y se descargaba. Ahora está en las dos bibliotecas y, como
PPSSPP, devuelve |sp - base de la pila en uso|: la extendida mientras dura
un sceKernelExtendThreadStack (campo `cur_stack` del hilo, que el final de
la llamada restaura) y -1 fuera de un hilo. tests/guest/extend_stack lo
comprueba fuera, dentro y al volver de la pila extendida. Sin el juego aquí:
falta ver hasta dónde llega ahora.

### v0.5.6: buzones de mensajes (Mbx) y sceImpose (DBZ TTT)

Con la 0.5.5, DBZ pasa CheckThreadStack (0x1FFD0) y se cae a los 0,13 s:
crea un buzón con `sceKernelCreateMbx` (no implementada, daba 0), un hilo
espera en `sceKernelReceiveMbxCB` (también 0, sin escribir el mensaje) y
lee el mensaje de 0x00000008. Hecho como PPSSPP: Create/Delete/Send/
Receive(CB)/Poll/CancelReceive/ReferMbxStatus, lista circular en la
memoria del juego (cabeza = el primero en salir), hilos por llegada o
prioridad (0x100), mensajes por prioridad (0x400), plazo que vence en el
acto con 0-1 us (`kernel_chance`, el __KernelChance de PPSSPP), recheck al
volver de callbacks y la corrección de la cabeza de ReferMbxStatus. Ojo:
en PPSSPP `Memory::WriteUnchecked_U32(valor, dirección)`. 9 pspautotests
nuevos (threads/mbx/*): 275/275. También sceImpose (idioma y botón,
batería, avisos del UMD y HOME, apagado de pantalla) y el NID 0x469989AD
de scePower (SetClockFrequency de SDK 6.3x). `sceUtilityLoadModule(0x108)`
da 0x8002013C a propósito: necesita los módulos de red cargados (PPSSPP).

### v0.5.6.x: arreglos para que arranque DBZ TTT

El usuario pidió numerar así (0.5.6.1, 0.5.6.2...) mientras se arregla
DBZ. 0.5.6.1, con el registro de la 0.5.6 (llegaba a 1,2 s con audio):
faltaban `sceKernelMemcpy` (Kernel_Library 0x1839852A: no copiaba y el
hilo 1 acababa saltando a 0x0004FF00), `sceDisplayWaitVblankStartMulti`
(0x40F1469C) y su CB (0x77ED8B3A) (`kernel_wait_vblanks`: wait_b cuenta
los vblanks que faltan) y `scePowerCheckWlanCoexistenceClock` (0xA85880D0,
1 = PSP-2000). display/vblankmulti: la parte Multi coincide; sigue
fallando por `sceDisplayIsVblank` (siempre 0) y sceDisplayWaitVblank
dentro del vblank, que ya fallaban.

### v0.5.7: teclado, escenas repetibles e interruptores

- Integrado el teclado en pantalla (OSK) que hizo el usuario
  (`src/wii/osk.c`, `sceUtilityOsk` en `utility.c`, `tests/test_osk.c`).
- Paso 1 de la metodología: **guiones de botones** (`src/frontend/guion.c`,
  formato en `guion.h`). `app.c` los reproduce (el mando real se ignora) o
  los graba (lo que llega por `app_set_input`, con el stick redondeado y
  ya redondeado para el juego), y escribe **huellas** del estado cada 60
  cuadros (ciclos, PC, registros, RAM muestreada; RAM entera en capturas y
  al final). Mismo resultado en x86 y en PowerPC (`tests/guest/guion_eco`).
  Partidas de prueba en `<juego>.partidas` (vaciada y rellenada desde
  `<juego>.partidas_base`): lo que el juego guarde no cambia la siguiente
  corrida. Mientras hay guion, el OSK no se abre.
- Wii: al abrir un juego, **+/R graba un guion**. `autotest.txt` acepta
  rutas con dispositivo, `veces N` y `opt NOMBRE 0|1`; con guion, cada
  corrida deja `.log` (con `[TIEMPOS]`), `.huellas`, capturas y `.stats`.
  Guía para el usuario: `docs/ESCENAS_DE_PRUEBA.md`.
- PC: `--guion`, `--salida`, `--grabar`, `--partidas`, `--opt`.
  `tools/comparar_huellas.py` dice el primer cuadro en que dos corridas se
  separan.
- Paso 3: **interruptores** en `src/core/opt.h` con todos los nombres de la
  hoja de ruta (ninguno implementado aún: `lista = 0`). Línea `[OPT]` antes
  de cada `[TIEMPOS]`.
- La hora del RTC ya salía de los ciclos emulados (`HLE_EPOCH_BASE` +
  `hle_now_us`): no hizo falta fijarla.
- Hallazgo para el paso 2: en el Wii **no hay espera real del vblank** (ni
  `VIDEO_WaitVSync` ni limitador en el bucle; con todos los hilos esperando,
  `kernel_run_until` salta al siguiente evento). El 40 % de "syscalls" de
  los menús de DBZ no es espera: hay que medir qué syscalls se lo llevan
  antes de decidir dónde va `PROF_ESPERA`.

### v0.5.7.1: reproducir desde el menú y capturas periódicas

- Al abrir un juego, **+/R** pasa por `guion no` → `GRABAR` → `REPRODUCIR`.
  Reproducir deja `<juego>.rN.*` (primer N libre: log propio con
  `[TIEMPOS]`, huellas, capturas) y vuelve al menú sin apagar; la opción
  vuelve a `no` tras cada corrida para no grabar encima sin querer.
- Con guion (grabando o reproduciendo), una captura cada 400 cuadros
  (`capturas N` en autotest.txt, `--capturas-guion N` en el PC; 0 = no),
  apuntada en `<prefijo>.capturas.txt` con las opciones del momento
  (GX/software, MR/ME, TN/TS, texturas, turbo y `[OPT]`). Las periódicas no
  calculan la huella de 32 MB (solo las que pide el guion). Las capturas no
  cambian la emulación (mismas huellas con y sin ellas).

### v0.5.7.2: capturas desde el menú

- Al abrir un juego, **-/L** activa o quita las capturas cada 400 cuadros
  (se guarda en wiisp.cfg, cuarta línea `capturas-si`/`capturas-no`). Con
  ellas activas también se capturan las partidas normales, sin guion
  (`<juego>.jN.*`, `app_capturas_sin_guion`).

### v0.5.7.3: HOME en el menú dejaba una pantalla roja

- La "pantalla roja" es la de error de libogc 3.1 (`__libogc_panic`: borra
  el XFB 0xC1700000 con YUV 0x316D31B8 y espera RESET o Z del mando de
  GameCube). Al salir con HOME se desmontaba la SD con la ISO y los
  archivos del juego todavía abiertos (se cierran al cargar otro); al
  terminar, newlib cerraba esos FILE sobre una SD desmontada y reventaba.
- Ahora `app_shutdown()` cierra guion, archivos del juego, módulo e imagen
  del UMD antes de desmontar; `shutdown_and_exit()` (wii/main.c) además
  espera a la GPU y pone la imagen en negro. También al apagar con el
  botón y al terminar `autotest.txt`. Deja "[WIISP] saliendo" en wiisp.log.
- Si vuelve a pasar: foto de la pantalla roja (trae PC, LR y la pila) y
  mirar si wiisp.log termina en "[WIISP] saliendo".

### v0.5.7.4: el teclado dentro del guion

- Registro del usuario (0.5.7.3): la salida con HOME ya llega a
  "[WIISP] saliendo". El teclado funciona ("Enter Player Name" en DBZ), pero
  al grabar un guion se devolvía el texto inicial (vacío) y DBZ volvía a
  pedir el nombre sin fin: la grabación quedó atascada ahí.
- Ahora, con guion, `guion_osk` (app.c) hace de teclado: grabando abre el
  de verdad y escribe `N teclado "TEXTO"` (o `teclado cancelar`);
  reproduciendo devuelve esas líneas en orden. Prueba de invitado
  `tests/guest/guion_teclado` (incluye comillas, '#', cancelar y quedarse
  sin líneas) e ida y vuelta grabar/reproducir con huellas idénticas.
- Visto en ese registro, para el paso 2: con el teclado abierto el tiempo
  real cuenta como "syscalls" (81,9 % en esos 45 s) y en los menús de DBZ
  sale 166,7 % de velocidad: no hay limitador de cuadros.

### v0.5.7.5: al grabar se parte de las partidas reales

- El usuario no podía cargar ni guardar al grabar: `<juego>.partidas`
  empezaba vacía (por eso DBZ pedía el nombre: perfil nuevo). Ahora, al
  grabar en el Wii, `<juego>.partidas_base` se rehace como copia de las
  carpetas de SAVEDATA que empiezan por el ID del juego
  (`guion_foto_partidas`); grabación y reproducciones parten de ahí y las
  partidas reales no se tocan.

### v0.5.7.6: medir antes de optimizar (pasos 4 y 7)

- Registro de la 0.5.7.5 (DBZ, grabando): menús 114 % (sin limitador),
  escenas 2D 37-53 % (CPU 67-89 %), peleas 7-8 % (GE 30-40 %, texturas
  36-42 %, framebuffers 8-9 %, CPU 13-17 %; "leer vertices" 15-25 % con
  38-55 % de vértices con huesos; ~300 texturas decodificadas y ~360 bajadas
  de framebuffer cada 30 s, ~290 "cambios con aviso").
- Paso 4: `src/wii/broadway.c` escribe al arrancar `[BROADWAY]`: PVR, MSR,
  HID0/1/2/4, L2CR, SDR1, los 8 DBAT/IBAT decodificados (si HID4[SBE]),
  cuáles están libres, GQR0-7 y las arenas de MEM1/MEM2.
- Paso 7: censo de destinos de dibujo en gx_ge.c (líneas `[TIEMPOS] rtt:`
  cada 30 s): bajadas por quién las pidió (textura/CPU/pantalla/GE) con
  tiempo; decodificadas por origen (framebuffer/otra VRAM/RAM) con tiempo y
  MB; texturas dentro de un framebuffer usadas desde la GPU o por qué no
  (formato, swizzle, no exacta, ya en VRAM, alfa, sin copia); los 5 búferes
  con más actividad. La quién-pidió sale de la cubeta del desglose
  (`prof_cur`).

### v0.5.7.7: primeras huellas del Wii y mapa de la RAM

- Dos reproducciones de DBZ (0.5.7.6, 5047 cuadros): **CPU idéntico en las
  85 huellas** (ciclos, PC y registros): la escena se repite. La RAM
  muestreada difiere en 42 de 85 y la entera al final: algo que el juego no
  usa para decidir (imagen o audio, probablemente). Ahora las huellas con
  RAM entera llevan `mapa cuadro N:` con un resumen por MB, y
  `comparar_huellas.py` dice qué MB difieren.
- Corregido: la cabecera de las huellas apuntaba el renderizador antes de
  que el frontend lo eligiera (la r1 decía "software"); ahora se escribe
  con la primera huella.
- `[BROADWAY]` (0.5.7.6): DBAT 2, 3, 6 y 7 libres (HID4[SBE] activo, BAT
  0/1 = 0x80/0xC0 MEM1, 4/5 = 0x90/0xD0 MEM2), SDR1 = 0, GQR0-7 = 0, HID2
  E0000000 (paired singles, psq y write gather activos), L2 activa, MEM1
  libre 0x8072F000-0x81800000 (~16,8 MB), MEM2 libre 0x92206000-0x933C6F60.
  2.5 (mem_bat) y 1.2b (psq_l) son posibles.

### v0.5.7.8: primera optimización, `ge_tex_clut`

- Los .log de r1/r2 (0.5.7.6) dieron el dato: en las peleas, las texturas
  sacadas de framebuffers se leen como CLUT16/CLUT32 y el decodificador
  genérico tardaba ~38 ms cada una (10,2 s de cada 30). `ge_tex_clut`
  (`ge_texture_decode_clut16_32` en ge_raster.c, usado por decode_into en
  gx_ge.c) las decodifica con la paleta convertida una vez. Activada por
  defecto; `opt ge_tex_clut 0` vuelve al camino de antes.
- Tiempos de r1 y r2 a menos de 0,2 puntos: la escena sirve para medir.

### v0.5.7.9: ge_tex_clut, segunda parte, y censo de paletas

- Medido con r3/r4 (0.5.7.8): peleas de 7,6 % a 9,5 %, escena entera de
  386 s a 330 s, CPU idéntico. Las texturas de framebuffers bajaron de 38 a
  15,7 ms cada una: el resto era escribir los tiles texel a texel y borrar
  antes ~640 KB. RAM distinta entre corridas: 3 MB (0x08B, 0x08C, 0x08E),
  las direcciones de la mezcla de sceSas y de Atrac: audio.
- Ahora: CLUT16/CLUT32 por franjas de 4 filas (`ge_clut16_32_prepare` +
  `_rows`) escritas con `gx_tile_band_rgba8` (src/wii/gx_tiles.h, dcbz en
  el Wii) y sin memset cuando todos los niveles miden múltiplos de 4.
  tests/test_gxtiles.c compara con la escritura texel a texel.
- Censo: `rtt: paleta sobre framebuffer:` con formato, búfer leído,
  desplazamiento, máscara, offset, paleta y tamaño, para decidir si se puede
  hacer en la GPU (copia de un canal del EFB + TLUT).

### v0.5.8: skinning rápido (`ge_skin_rapido`)

- Medido con r5/r6 (0.5.7.9): peleas de 9,5 % a 10,4 %, escena de 330 s a
  307 s; textura de framebuffer de 15,7 a 7,1 ms. Lo que más pesa ahora en
  las peleas: "leer vertices" 30 % (47 % de vértices con huesos), bajadas de
  framebuffer ~9 %, decodificar ~9 %.
- Censo de paletas de DBZ: CLUT32 sobre 8888 con desplazamiento 24 y
  máscara 7F (el alfa, 7 bits; paleta 565), CLUT16 sobre 565 con
  desplazamiento 5 y máscara FF (mezcla verde y rojo: no es un canal de GX)
  y CLUT16 sobre 4444 de 64x64 con un solo bit. El paso a la GPU con copia
  de un canal + TLUT no encaja directo: el alfa del EFB es de 6 bits y el
  565 no se separa por canales. Se queda en la CPU por ahora.
- `fast_skin` (ge_vertex.c) con ge_fast_math: matrices mezcladas por peso
  en float y una sola multiplicación. Modo exacto (ME) sin cambios.
  `ge_test_decode_vertex` expone la decodificación para tests/test_skin.c.

### v0.5.9: el intérprete sin coste por instrucción (`cpu_sin_extra`, 2.1)

- Medido con r7/r8 (0.5.8): peleas de DBZ a 12-15 FPS según el usuario
  (13,8 % de velocidad), escena de 307 s a 254 s. En las peleas, CPU
  24,4 %; en las escenas 2D, la CPU es casi todo (~11,6 MIPS).
- `cpu_run` (src/cpu/interp.c) reescrito en una sola función: pc, npc, el
  puntero a la RAM y su tamaño en variables locales; un contador local de
  32 bits en vez de sumar en cada instrucción a `cpu_cycles` y
  `cpu_executed` (64 bits, en memoria); lectura de la instrucción directa de
  la RAM; caminos lentos de memoria fuera de línea; switch con un caso para
  cada valor (sin comprobar el rango). El estado global se pone al día solo
  antes de llamar fuera (syscall, VFPU, memoria lenta, fallos) y
  `cpu_stop_requested` solo se mira después de esas llamadas: son las
  únicas que pueden pedir parar (no hay nadie más que lo ponga).
- El de antes queda como `cpu_run_ref` (src/cpu/interp_ref.c) y se usa con
  `cpu_sin_extra=0`. Un cambio en una instrucción va en los dos.
- Verificación: tests/test_interp.c ejecuta 3400 programas aleatorios
  (2,1 M instrucciones: todas las familias, saltos likely, riesgo de bc1x,
  RAM/espejos/scratchpad/VRAM, VFPU, mfic/mtic, fallos y saltos a
  direcciones inválidas) en trozos aleatorios con los dos intérpretes y
  compara todo el estado; detecta los errores metidos a propósito
  (prueba de mutaciones). Las 275 pspautotests con huella en **cada**
  cuadro (`-DHUELLA_CADA=1`), `cpu_sin_extra=0` contra `=1`: CPU, ciclos y
  pc idénticos en las 8477 huellas, y los 7 programas guest también.
- Velocidad: en el Wii (objdump de interp.o), un addiu pasa de 54
  instrucciones del Broadway con 20 accesos a memoria a 24 con 5; un lw de
  ~66 a 29. Bajo qemu-ppc (código PowerPC), pspautotests malloc pasa de
  18,8 a 40,9 MIPS (x2,2). En el PC (x86) apenas cambia (~5 %): ahí manda
  el salto indirecto del switch.
- Hallazgo: con la misma opción, dos ejecuciones de malloc o de algunas de
  savedata dan distinta la RAM del último MB (la CPU igual). Volcando la
  RAM: son fechas reales del PC (año, mes, día, hora, minuto y segundos que
  cambian) que el HLE copia de los archivos: `st_mtime` en sceIoGetstat
  (io.c, write_stat_common) y `st_ctime/st_atime/st_mtime` en las listas de
  partidas (utility.c). Al reproducir un guion, las partidas se copian de
  nuevo (guion_preparar_partidas) y tienen fecha nueva: es muy
  probablemente la diferencia de RAM entre r1/r2... de DBZ. Propuesta:
  conservar las fechas al copiar las partidas (y usar mtime en vez de
  atime), para que dos reproducciones den la misma RAM.

### v0.5.9.1: paso 0 del plan de las texturas de framebuffer (solo medir)

Con r9/r10 (0.5.9): escena de 241 s a 180,5 s; menús 128-132 %, 2D 75-79 %,
peleas 14-17 %; la CPU de las peleas pasó del 25 % al 17 %. En las peleas,
texturas (21 %) y framebuffers (18 %) salen casi todo de dos texturas con
paleta que leen framebuffers (CLUT32 sobre 0x04088000 y 0x04000000,
índice = alfa & 7F; CLUT16 sobre el 565 de 0x04158000) declaradas de
512x512 sobre búferes de 480x272. Plan acordado con el usuario (documento
"Optimizaciones de RGBA8" y revisión): recorte de filas y columnas, CLUT32
según el modo de alfa del origen (copia A8 del EFB usada como CI8 + TLUT
en A_GPU, índices desde la VRAM en A_VRAM, textura constante en
A_UNIFORM), CLUT16 como CI8 desde una copia GB8, anillo de TLUT, y evitar
las bajadas por solape del 4444 de 64x64.

Este paso solo mide (censo `rtt:` de [TIEMPOS], gx_ge.c):
- `rtt: paleta sobre framebuffer: <dirección> ...` ahora por dirección, con
  una segunda línea: primitivas (sprites, through), filtro lineal, clamp,
  límites de u/v en los vértices, **última columna y fila leídas** (el
  vecino del filtro lineal solo cuenta si pesa; ojo: si la fila máxima es
  272 o más, el recorte tocaría el Z de 0x04110000) y el modo de alfa del
  búfer de origen (uniforme con su valor, en VRAM o en GPU).
- `rtt: bajadas por motivo`: textura, arrastradas por el rango de una
  textura, solape al crear otro destino, crecer, sin sitio, copia de
  bloques del GE, carga de paleta, otros comandos del GE (con sus códigos
  en otra línea), capturas; cada uno con la espera a la GPU
  (GX_DrawDone) y la conversión por separado; y las subidas VRAM->GPU.
- `rtt: solape: X bajado al crear Y`: los pares de búferes que se pisan.
- Cada búfer: cómo empieza cada uso (borrado completo, copia desde su
  textura, subida desde la VRAM) y su modo de alfa.
- `ge.cur_cmd`: el comando del GE en curso (para saber quién lee la VRAM).
- El texto del renderizador en hle_profile_report pasa a 6 KB (estático).

### v0.5.9.2: censo completo para optimizar las texturas de framebuffer

Lo que dijo el censo de la 0.5.9.1 (r3/r4, peleas, cada 30 s): la cadena
cuesta ~10,8 s (36 %): decodificar 4,2 s, bajadas 4,6 s y **subidas
VRAM->GPU 2,0 s**; la espera a la GPU es poca (~0,6 ms por bajada), lo caro
es convertir (~6,6 ms por búfer de 480x272). CLUT32 (0x04088000 y
0x04000000) siempre con alfa A_GPU; CLUT16 sobre el 565 lee hasta la fila
271 sin vecino (recorte exacto); CLUT32 lee la fila 272 con peso (la mitad
de los sprites): Z o la fila 0 de 0x04088000. Las bajadas "por el GE" son
cargas de paleta dentro de 0x04000000. El 4444 de 64x64 y el 565 comparten
0x04158000: bajar el 4444 cuesta poco, pero el 565 se vuelve a subir entero
en cada imagen.

Lo que añade este censo (líneas `rtt:`):
- Por motivo de bajada, las filas que tocaba el acceso que la provocó
  (lo que bastaría bajar) y cuántas solo tocaban el relleno de las filas.
- Cada carga de paleta dentro de un búfer: dirección, bytes, filas y
  columnas del búfer, si es solo relleno y si los bytes se repiten.
- Pares de solape: veces, cuántas con bajada y qué filas del viejo pisa el
  nuevo (lo que habría que volver a subir si se conservara su copia).
- Por textura con paleta: tamaño del origen, coordenada máxima muestreada,
  sprites que leen fuera del origen y el peso máximo de esa fila/columna,
  cambios de paleta, destino y mezcla, y las formas de los sprites
  (tamaño, u = ku·x + du, v = kv·y + dv).
- Por búfer: si el primer dibujo es un sprite que lo tapa entero (opaco o
  con mezcla), escrituras de la VRAM y ms de bajar, subir y decodificar.

### v0.5.10: las texturas de framebuffer de DBZ sin bajar ni decodificar

Lo que dijo el censo de la 0.5.9.2 (r6, pelea, 30 s): la cadena costaba
~10,7 s (36 %). Por búfer: el 565 de 0x04158000 se bajaba (1,5 s), se
subía entero en cada imagen (1,8 s) y se decodificaba (1,4 s); 0x04088000
se bajaba (0,9 s) y decodificaba (1,9 s); 0x04000000 se bajaba por una
**paleta de 16 bytes en el relleno de su fila 0** (0x04000780, columnas
480-483, casi siempre los mismos bytes) y se decodificaba (0,9 s); el Z se
bajaba entero (1,1 s) solo porque caía en el MB de la textura de
0x04088000. Los sprites del efecto: CLUT32 en tiras de 32x272 (1:1 y
desplazadas ~0,7 texel), CLUT16 en tiras de 32x128 con ±1 texel; dentro de
lo que se dibuja (tijera) nadie lee la fila 272 ni la columna 480. El 4444
de 64x64 pisa las filas 0-7 del 565.

Tres optimizaciones, cada una con su interruptor (activadas):
- `ge_fb_relleno`: un acceso que solo toca el relleno de una fila de un
  búfer (columnas desde el ancho dibujado, que GX nunca escribe) no lo baja
  ni lo invalida (`only_padding` en el gancho de la VRAM). Exacto.
- `ge_rtt_gpu`: texturas con paleta 565 que leen un framebuffer que está en
  la GPU, con CI8 + TLUT de GX (`clut_from_surface`):
  - CLUT32 que lee el alfa (desplazamiento >= 24) de un 8888 con el alfa en
    la GPU (A_GPU): los índices son una copia A8 del EFB que `efb_to_tex`
    hace junto a la RGBA8 (desde el segundo uso, `a8_want`);
  - CLUT16 sobre un 565: índices CI8 calculados de la copia RGBA8 (solo la
    línea de verde y azul de cada tile; `gx_ci8_from_rgba8_565`,
    wii/gx_tiles.h), una vez por cada cambio del búfer;
  - TLUT RGB565 en un anillo de 64 (GX_LoadTlut lee la memoria cuando la
    GPU llega al comando);
  - la textura de GX tiene el tamaño de la de la PSP (512x512, mismas
    coordenadas que el camino de la CPU) y cada primitiva comprueba antes
    que no lee más allá del búfer de origen (`clut_gpu_sprite_ok`,
    `clut_gpu_triangle_ok`; líneas y puntos siempre por la CPU): si lee,
    vuelve al camino de la CPU, donde la PSP leería otra memoria.
  - tests/test_gxtiles.c: los índices + TLUT expandida como GX dan el mismo
    color que bajar y decodificar como antes (400 casos, bit a bit). Falta
    confirmar en el Wii que GX expande la TLUT 565 por réplica de bits y
    que la copia A8 da el mismo alfa que la RGBA8 (capturas sin/con).
- `ge_fb_hueco`: cuando se crea un búfer encima de pocas filas (<= 1/4) de
  otro, el grande no sale de la GPU: esas filas se bajan y quedan como
  hueco (manda la VRAM: no se bajan, el búfer no se usa como textura ni se
  muestra); cuando el grande vuelve, se tira lo que pisa el hueco y al
  restaurar solo se suben esas filas (`keep_with_hole`, `vram_to_tex_rows`).

Además: cada captura apunta la huella de la imagen en `.capturas.txt`
(`tools/comparar_capturas.py` compara dos corridas) y `autotest.txt` tiene
`sufijo X` para correr el mismo juego con otras opciones. Línea nueva del
censo: `rtt: paleta desde la GPU ...` con usos, vueltas a la CPU, índices
calculados, TLUT cargadas, accesos al relleno y huecos.

### v0.5.11: profundidad, matrices y registro

Resultado de la 0.5.10 en el Wii (sin/con): las 12 capturas con la misma
huella de imagen; la escena de 209,4 s a 158,1 s y las peleas del ~15 % al
21-27 %. En las peleas con la 0.5.10: GE 55-61 %, CPU 24-32 %, texturas
~3 %, framebuffers 1-5 %. `tools/comparar_capturas.py` no leía los nombres
con espacios (arreglado).

Tres cosas nuevas, cada una con su interruptor (activadas):
- `ge_z_rango`: el rango vigilado de un búfer de profundidad era su rango
  lineal ±64 KB (por el swizzle). La textura CLUT16 de 64x64 de
  0x04158000 (detrás del Z de 0x04110000-0x04154000) lo bajaba entero en
  cada imagen: 192 bajadas, 1,3 s de cada 30 s de pelea. Ahora el rango
  se redondea al bloque del que el swizzle no saca ningún byte
  (`mem_vram_swizzle_block`, memory.h: 0x800 sin traducción, 0x2000 <<
  k con ella; 16 KB con la de arranque, así que para DBZ es el rango
  exacto). tests/test_main.c lo comprueba para todas las traducciones
  y todos los offsets (y que el bloque no es más grande de lo necesario).
- `ge_mtx_cache` (de la primera tanda): `compute_transform_state` hacía en
  cada llamada de dibujo dos productos 4x4 con la aritmética del GE
  (~5400 instrucciones del Broadway, según objdump); con ~362.000
  llamadas cada 30 s de pelea, eso era casi todo el 11 % de "comandos".
  Ahora guarda (mundo*vista)*proyección, mundo*vista y la dirección de
  vista con las tres matrices de entrada y solo rehace los productos si
  alguna cambió (memcmp). Mismos bits: las pspautotests dan las mismas
  huellas, salida e imágenes con 0 y 1 (exacto y MR).
- `registro_diferido`: cada línea del registro en un segundo nuevo hacía
  un fsync en la SD (10-40 ms parado). Las líneas `[MANDO]` (los 300
  primeros cambios de botón de cada juego, también jugando) costaban así
  el 2-9 % de cada bloque. Ahora las de diagnóstico (`hle_log_quiet`:
  `[MANDO]`, `[BROADWAY]`...) no lo piden: llegan a la tarjeta con la
  siguiente sincronización o en `log_tick` como mucho a los 10 s. Las
  normales y `[CPU]` siguen como antes (1 s / en el acto).

Censo nuevo en `[TIEMPOS]`: matrices calculadas y reutilizadas, triángulos
sin descarte de caras (se procesan en los dos sentidos) y los formatos de
vértice más usados (`Formatos de vertice`: vtype, componentes, % de
vértices, llamadas y primitivas). Es para diseñar 1.2/1.2b.

### v0.5.11.1: contadores del Broadway (`prof_pmc`, 0.2)

Con `opt prof_pmc 1` (apagado por defecto), en cada cambio de cubeta y de
fase del GE se leen PMC1-4 del Broadway (MMCR0/MMCR1, eventos del manual
del 750CL: ciclos, instrucciones terminadas, fallos de la L1 de datos y
saltos mal predichos; `broadway_pmc_init`, wii/broadway.c) y lo contado
se suma a lo que se estaba haciendo (`prof_pmc_account`, core/prof.c).
Líneas nuevas en `[TIEMPOS]`:
- `PMC: N M ciclos; CPU x% (IPC, L1D y saltos por 1000 instrucciones)...`
  por cubeta;
- `PMC de la CPU`: ciclos e instrucciones del Broadway por instrucción de
  la PSP;
- `PMC del GE`: ciclos por llamada (comandos, estado), por vértice (leer,
  transformar), por triángulo recibido (ensamblar) y por primitiva
  dibujada (enviar), y por comando.
Cuesta unas pocas instrucciones por cambio de fase (~3 por vértice), así
que mide un poco de más: va encendido en las dos corridas del autotest.
tests/test_guion.c lo prueba con un lector falso (incluida la vuelta de
los contadores de 32 bits).

### v0.5.12: vértices por lotes (`ge_vdec_esp`, primera parte de 1.2)

- `ge_vdec_esp`: con la matemática rápida (MR), los vértices de una
  llamada sin índices (y el rango de una con índices repetidos) se
  decodifican y transforman todos de una vez, en orden, antes de
  ensamblar (`batch_range`, ge_vertex.c): un solo puntero a la RAM, sin
  `read_raw` ni cambios de fase del perfil por vértice. `read_vertex` es
  ahora un cuerpo en línea (`read_vertex_body`, con `fast` constante)
  que usan los dos caminos: mismas expresiones, mismas `fmadds`, mismos
  bits. Solo RAM (leer la VRAM puede bajar un framebuffer); through y
  morph siguen uno a uno. Con él, "leer vertices" incluye la
  transformación de esas llamadas.
- Verificación: tests/test_vdec.c, 3000 tandas de tres llamadas al azar
  (formatos, índices, huesos, morph, through, luces, niebla, generación
  de uv, descarte de caras, recorte de profundidad) con un renderizador
  que apunta todo: lo que llega a GX y los contadores, iguales bit a bit;
  detecta los fallos metidos a propósito. Pasa en x86 y en PowerPC bajo
  qemu (con `fmadds`). pspautotests A/B en MR: 0 diferencias (gpu/ con el
  binario PowerPC; gpu/complex varía entre corridas por las fechas de
  los archivos que crea, no por esto).
- Medición: en las corridas del autotest se medía cada vértice y cada
  primitiva con el reloj (`GE_PROF`, ~3 lecturas por vértice, solo para
  el `.stats`); quitado, lo dan el muestreo y `prof_pmc`. El `.stats`
  solo dice ya draw_prim y preparar.
- Siguiente (con los PMC de esta prueba): decodificadores por formato
  (los del censo de DBZ), psq_l para los componentes de 8 bits (los de 16
  son little-endian: hay que darles la vuelta antes), y el ensamblado y
  el envío a GX por triángulo.

### v0.5.13: decodificadores por formato y triángulos sin descarte de caras

Resultado de la 0.5.12 en el Wii (sin/con, con `prof_pmc`): 12 capturas
iguales; escena de 170,1 s a 151,5 s; peleas del 19,5-23 % al 23,3-27,5 %;
matrices reutilizadas el 100 % (comandos de ~7000 a ~2800 ciclos por
llamada); framebuffers en pelea del 4,3 % al 0,8 %; leer + transformar de
~1040 a ~860 ciclos por vértice. El GE va a IPC ~0,9 con casi cero fallos
de caché: el coste son instrucciones. DBZ usa casi solo `uv16 c4444 p16`
(~50 %) y `uv8 p16 w8x2/4/6/8` (~50 %), y **no activa el descarte de
caras**: cada triángulo se procesaba en los dos sentidos.

Perfil de instrucciones PowerPC por función con qemu (`-d
in_asm,exec,nochain` y un script que suma instrucciones por bloque
ejecutado; banco de tiras de 20 vértices como las de DBZ). Cambios:
- `ge_vdec_esp`, segunda parte: decodificadores con el formato como
  constante para los de DBZ (`decode_plain`, `BATCH` en `batch_range`);
  `fast_skin_plain` compartido con el camino viejo y desenrollado (la
  matriz mezclada en registros); sin `memset` del vértice (todos los
  campos se escriben); copia de vértices palabra a palabra (era un memcpy
  por vértice); `floor_to_int` (una conversión menos en `round_to_screen`).
- `ge_tri_rapido` (nuevo): sin descarte de caras, los descartes de un
  triángulo (que no dependen del orden) se hacen una vez y, si no hay que
  recortar, solo se envía el sentido con área positiva (el otro lo iba a
  tirar el rasterizador).
- Banco: por vértice, `uv16 c4444 p16` de 1521 a 950 instrucciones y
  `uv8 p16 w8x4` de 2156 a 1226.
- Verificación: tests/test_vdec.c compara ahora con los dos interruptores
  y apunta una huella de lo que sale con ellos apagados, que no cambió
  (x86 4D4A1D8B, PowerPC 0657B177): lo compartido sigue dando los mismos
  bits que la 0.5.12. Mutaciones detectadas. pspautotests A/B: 0
  diferencias (salvo el ruido conocido de malloc).

### v0.5.13.1: carpeta por sesión y verificación en el Wii

Resultado de la 0.5.13 (sin = `ge_vdec_esp` y `ge_tri_rapido` apagados):
**las capturas 2000, 2400 y 2800 salieron distintas** (las demás, iguales).
El juego no se separó: las 258 lecturas del mando son idénticas, con el
mismo tiempo emulado; es una diferencia de dibujo en escenas de menú con
modelos 3D (`uv16 c4444 p16`, sin luces ni morph) y tiras 2D. En el PC no
se reproduce: tests/test_vdec.c da 0 diferencias en x86, en PowerPC con
GCC 13 y también con ge_vertex.c compilado por el GCC 16 de devkitPPC
(enlazado con la libc de Linux y corrido en qemu: `-msdata=none -G0
-maix-struct-return` y quitando `.gnu.attributes`). Velocidad (con PMC):
peleas del 22,2-26,5 % al 26,9-28,9 %; leer 640 ciclos por vértice (antes
leer + transformar ~945), ensamblar ~410 por triángulo (antes ~670).

Para encontrarlo en el Wii:
- `ge_vdec_verificar` (apagado por defecto): cada lote de `ge_vdec_esp`
  se calcula también uno a uno y se compara campo a campo; las diferencias
  van al registro (`[VERIF]`, con los bits y los bytes del vértice) y
  `[TIEMPOS]` dice cuántos se compararon y cuántos salieron distintos.
- El autotest corre tres veces: sin, solo `ge_tri_rapido`, y los dos con
  la verificación.

Además, a petición del usuario: cada sesión escribe todo en su carpeta,
`sd:/wiisp/0001`, `0002`... (la primera libre): wiisp.log y, por cada
juego, registro, huellas, capturas y estadísticas (`session_open`,
`session_path` en wii/main.c). Las partidas y la configuración siguen
donde estaban.

### v0.5.13.2: misma partida en cada corrida (fechas de la base)

La diferencia de las capturas 2000-2800 de la 0.5.13 la encontró el
usuario: **otra partida** (otro nombre de usuario), no las optimizaciones.
En el registro, las dos corridas piden la lista (savedata modo 4) pero
"sin" carga ULUS10537DAT0 y "con" DAT1: `choose` elige la más reciente por
la fecha de la carpeta, y las partidas se copian de la base en cada
corrida con la fecha de la copia (si el segundo cambiaba entre DAT0 y
DAT1, ganaba DAT1). Arreglado: `io_host_stat` (io.c) da, para lo que está
en las partidas del guion, las fechas de la copia base (atime = mtime, la
de acceso cambia al copiar) y una fija (`HLE_EPOCH_BASE`) para lo que no
está en ella; lo usan Getstat, Dread y las listas de savedata. También
quita la diferencia de RAM entre reproducciones (las fechas que el HLE
copiaba a la memoria). Si preparar las partidas falla, ahora lo dice el
registro (`[GUION] no se pudieron preparar`). Prueba en tests/test_guion.c.

### v0.5.14: envío a GX, ensamblado y psq_l (`gx_envio_rapido`, `ge_ensamblar_rapido`, `ge_vdec_psq`)

Resultado de la 0.5.13.2 en el Wii (sin = `ge_vdec_esp` y `ge_tri_rapido`
apagados; con = encendidos y verificando cada vértice): 12 de 12 capturas
iguales, ~22 millones de vértices comparados sin diferencias; peleas del
22-26 % al 27-29 % (con la verificación puesta).

Medido con el banco de qemu (gx_ge.c y ge_vertex.c compilados con
devkitPPC, libogc de mentira, tiras de 20 vértices `uv16 c4444 p16`):
instrucciones por triángulo de **1694 a ~1150** (−32 %); `uv8 p16 w8x4`
(skinning) a ~1300. Tres interruptores nuevos, idénticos bit a bit:

- `gx_envio_rapido` (wii/gx_emit.h y `gx_triangle`): los valores de los
  tres vértices se calculan antes de escribir el FIFO (que es volatile: el
  compilador releía el estado tras cada escritura); x, y, z ya en float
  desde el GE (`fx`, `fy`, `fz` en GeVertex, iguales a `(float)x`...);
  k / w = 1 sin dividir para el vértice de la w mínima; uv finita con
  `fsel`; la niebla en 1 por sus bits; y sin llamar a `prepare(0)` si no
  hay nada que preparar (`prepare_idle`). De 348 a 280 instrucciones.
- `ge_ensamblar_rapido`: los descartes de cada triángulo miran banderas
  calculadas una vez por vértice (`clip_flags`, `GE_CF_*`, en
  `round_to_screen`; en una tira cada vértice se comprobaba en tres
  triángulos): de ~165 a ~25 por triángulo, más ~35 por vértice. La
  ventana de triángulos apunta a los vértices del caché de la llamada en
  vez de copiar 68 bytes por vértice (`vsource_ref`; al acabar, `data_`
  queda como antes para PRIM continuar y el modo inmediato). El área se
  calcula una vez (en double desde fx/fy si las coordenadas caben en 21
  bits, exacto; si no, en 64 bits) y el rasterizador no la repite
  (`ge_raster_triangle_ccw`).
- `ge_vdec_psq` (1.2b, solo Wii): los componentes de 8 bits se convierten
  al cargarlos con `psq_l`; los de 16 bits, que están en little-endian, en
  dos bytes sumados (el bajo sin signo por 2^-15 y el alto por 2^-7): todo
  exacto, sin pasar el entero por la memoria. libogc solo guarda GQR0 al
  cambiar de hilo: GQR2-4 se ponen antes de cada lote (nadie más los usa:
  revisado en libogc.a). ~35 instrucciones menos por vértice.
- Sin interruptor (mismos bits; las huellas no cambian): las sumas de
  productos con la fusión escrita (`ge_dot3_fused`, `GE_FMADD`): el
  compilador podía fusionar `a*b + c*d` distinto en dos copias en línea
  del mismo código, y GCC 13 lo hizo al desenrollar; ahora es la misma en
  todas. `lighting_process_fast` y `ge_shade_map_coord`, una sola copia
  (noinline). En el lote: bucles cortos desenrollados (sin `memset` ni pila
  para la matriz del skinning), `round_to_screen` en línea, la normal solo
  si se usa y el formato del caso dicho al compilador.
- `ge_vdec_verificar` pasa a `ge_verificar` y compara además, en el Wii,
  fx/fy/fz y las banderas de cada vértice, los descartes de cada triángulo
  (`[VERIF] descartes`) y cada triángulo enviado a GX (`[VERIF] GX`); los
  totales salen en `[TIEMPOS]`.

Encontrado de paso: un vértice fuera de pantalla (`GE_OUTSIDE`) que una
primitiva que continúa en through llega a dibujar tenía `fx` viejo; ahora
`ge_vertex_mark_outside` deja fx = (float)GE_OUTSIDE. Y `ge_tri_rapido`
cuenta como "fuera de pantalla" algunos triángulos que el camino viejo
contaba como "de espaldas" (solo la estadística).

Verificación: tests/test_gxemit.c (200000 triángulos al azar, rápido
contra referencia); tests/test_vdec.c compara además `ge_ensamblar_rapido`
solo (contadores incluidos), vértices en modo inmediato (`cull_xy` = 0),
el área con coordenadas enormes y 600 tandas con los formatos de
decodificador propio. En x86, en PowerPC (GCC 13) y con ge_vertex.c
compilado por devkitPPC en qemu, con `psq_l` emulado por SIGILL (438179
cargas): 0 diferencias, huellas sin cambios (x86 4D4A1D8B, PowerPC
0657B177, devkitPPC B3A2BCD3); las mutaciones a mano se detectan.
pspautotests A/B en MR con los tres interruptores: x86, 275 programas,
solo `malloc/malloc` (el ruido de siempre); PowerPC, los 97 de gpu/, solo
`gpu/complex` (la última región de la RAM cambia con la hora de cada
corrida; la imagen es igual, y dos corridas seguidas dan lo mismo con y
sin). Los programas de tests/guest pasan.

Autotest: tres corridas: sin (los tres nuevos apagados), con (encendidos)
y verif (encendidos y `ge_verificar`).

### Resultado de la 0.5.14 en el Wii (sin / con / verif, con `prof_pmc`)

- Capturas: 12 de 12 iguales entre sin y con.
- `ge_verificar`: 22,2 millones de vértices, 20,0 millones de descartes y
  9,15 millones de triángulos enviados a GX comparados: 0 distintos.
- Peleas del 27,9-30,0 % al 31,6-34,1 % (~+13 %); la escena entera de
  ~135,6 s a ~126,5 s reales. GE en pelea del 52-56 % al 45-50 % del tiempo.
- PMC del GE en pelea (ciclos): ensamblar de 421-439 a 240-246 por
  triángulo (−44 %), enviar de 525-543 a 392-432 por primitiva (−20 %),
  leer de 570 a 502 por vértice (−12 %, IPC 0,67: espera a fdivs y a las
  conversiones a entero por la memoria más que instrucciones).

### v0.5.14.1: censo del estado de cada llamada (`prof_estado`, solo mide)

Lo que dijeron los registros de la 0.5.14 (pelea, "con", cada 30 s): 537.115
llamadas de dibujo (~17.900 por segundo), **14,5 comandos por llamada**, ~16
triángulos por llamada, **0 % de vértices con luces**; comandos 6,7 % del
tiempo, ensamblar 10,1 %, enviar 7,5 %, estado 2,9 % y esperar GX 4,1 %.
PMC: comandos 2864 ciclos por llamada, estado 1212.

Medido antes con el banco de qemu (`tools/banco/bench_lista.c`, una lista
del GE de verdad por el bucle de `ge.c`, todo compilado con devkitPPC; tiras
de 20 vértices con el estado idéntico en cada llamada), en instrucciones:
- Cada comando: ~56 de leerlo (bucle 21 + `mem_valid` 19 + `mem_read32`
  16, palabra a palabra aunque la lista esté seguida) más 25-43 de
  `execute`. `LOADCLUT` +436 (copia la paleta de 16 en 16 bytes).
- Antes de cada dibujo (cuenta en "comandos"): `compute_transform_state`
  ~690 sin luces (163 propias + 290 de comparar las 3 matrices + 238 de
  copiarlas aunque no cambien), ~3100 con dos luces; `vformat_setup` +
  `setup_uv_prescale` ~120; el resto de `ge_draw_prim` ~210.
- `ge_raster_begin` ~600 (cuenta en "ensamblar"); `setup_targets` +
  `get_surface` ~560; `lookup_texture` ~350 (en "texturas"); `apply_gx` ~470
  propias y ~950 de libogc (contadas con objdump: en el banco libogc es de
  mentira) más el volcado del estado sucio en `GX_Begin`.
- `prepare_idle` + `drawn`: 67 por triángulo.
Si el estado no cambiara, ~4100 instrucciones por llamada se rehacen sin
necesidad (sin luces).

Esta versión solo cuenta (con `opt prof_estado 1`; apagado no cuesta nada
más que mirar el interruptor) y escribe tres líneas `[TIEMPOS] Censo de
estado`:
- Comandos por llamada; de los de estado, cuántos llegan con el valor que
  ya tenían; datos de matrices y de huesos iguales; `LOADCLUT` con los
  mismos bytes.
- Por grupo del estado: V (formato de vértice, uv, morph), T
  (transformación y luces) y R (píxel, muestreo y tijera de
  `ge_raster_begin`): "igual" = salió como en la llamada anterior;
  "limpio" = ningún comando que lo lee cambió de valor (la tabla
  `censo_grupo` de `ge.c`, por la que se marcan `ge_sucio`); "errores" =
  limpio pero distinto: **tiene que ser 0** (un comando que falta en la
  tabla). Es lo que usaría la versión que se lo salte.
- En GX: `setup_targets` con el mismo resultado (y si hubo avisos entre
  medias: VRAM escrita, búfer quitado, presentar), la misma textura, las
  entradas de `apply_gx` iguales (y si alguien más tocó GX: copias, vuelta
  a la CPU de las paletas, presentar) y, por primitiva, si `prepare_idle` y
  `drawn` no harían nada ("listas"), las primeras de su llamada y las que no
  están listas a mitad de llamada (p. ej. un framebuffer bajado al leer
  vértices de la VRAM).

Verificación: `tests/test_censo.c` cambia por el camino de las listas
(`ge_test_execute`) comandos al azar entre llamadas, uno por uno los 256 y
matrices, y comprueba 0 errores; quitando 6 comandos de la tabla a propósito
(TEXFUNC, ZTEST, LIGHTENABLE0, FOG1, MORPHWEIGHT0, TEXSCALEU) los errores
salen. Huellas de test_vdec sin cambios (x86 4D4A1D8B, PowerPC 0657B177,
devkitPPC B3A2BCD3). pspautotests 275/275; A/B con `prof_estado` en x86
(MR): solo el ruido de siempre (`malloc`, `gpu/complex`, que también cambia
en A/A, y unos de savedata que no se repiten). En el banco con el estado
fijo, todo sale "igual" y 17 de cada 18 triángulos "listos".

Autotest: la primera entrega llevaba sin/con/verif, pero en el Wii corrió
el autotest viejo de `sd:/wiisp/` (las tres con `prof_estado=0`; sirvió de
repetición de la 0.5.14: 12 de 12 capturas iguales, verificación sin
diferencias). Basta una corrida: `capturas 0`, `prof_pmc 1`,
`prof_estado 1`, `sufijo censo`.
Qué decidir con el resultado (pelea): si V/T/R y GX salen iguales en la
mayoría de llamadas, el plan de la 0.5.15 (`ge_estado_sucio`: no rehacer
V/T/R ni emitir `apply_gx` si sus entradas no cambiaron, y `tri_listo` en
vez de `prepare_idle` + `drawn` por triángulo); si alguno casi nunca se
repite, ese se queda fuera.

### Resultado del censo de la 0.5.14.1 (DBZ, una corrida con `prof_estado`)

Errores 0 en todos los bloques (la tabla de grupos está completa). El censo
cuesta: con él, comandos ~5900 ciclos por llamada (antes ~2900) y estado
~2800 (antes ~1200); apagado no cuesta nada.
- Pelea: estado de GX con las mismas entradas el 91-92 % de las llamadas;
  rasterizador igual 94-95 % (limpio 93 %); transformación igual 99 %,
  pero limpia solo el 71 % (DBZ alterna formatos con huesos y entonces
  cualquier cambio del tipo de vértice la marcaba); uv y morph 77 %;
  `setup_targets` y textura iguales 91-93 %; `LOADCLUT` con la misma paleta
  solo el 30 % (no compensa); primitivas "listas" solo el 44-54 %, con
  ~1,4 M por bloque no listas a mitad de llamada: era la condición prudente
  de `prepare_idle` con alfa por stencil o mezcla con el alfa del destino.
- Selección de personajes (~79 %): CPU 58 % (32 ciclos por instrucción de
  la PSP), GE 13,6 %, esperar GX 7,6 %, texturas 5,4 %, E/S 3,6 %. Ahí manda
  el intérprete (fase 2); lo de 1.4 apenas ayuda (~1-2 %).

### v0.5.15: el estado de cada llamada solo si cambió (`ge_estado_sucio`) y censo de tirones (`prof_tirones`)

`ge_estado_sucio` (encendido; 1.4, primera parte; juntar llamadas sigue
aparte, `ge_charolas`). Todo idéntico bit a bit:
- `ge.c`: cada comando que cambia de valor marca en `ge_sucio` los grupos
  que lo leen (`estado_grupo`, construida al arrancar; unas pocas
  instrucciones por comando, siempre); los datos de matrices marcan si
  cambia el valor (huesos, nada). El tipo de vértice no marca: cada grupo
  mira sus bits.
- `ge_vertex.c`: el formato se guarda por tipo de vértice; uv y morph (V)
  solo si V está sucio o cambia el bit de through; la transformación (T)
  si T está sucio o cambian through o "hay color" (lo único del formato
  que lee). Lo guardado no depende de ningún interruptor: solo del
  renderizador y de la matemática rápida (`estado_entorno`).
- `ge_raster.c`: el estado del rasterizador solo si R está sucio o cambia
  through; `ge_raster_gen` sube cada vez que se calcula.
- `gx_ge.c`: `apply_gx` no se emite si su clave (`GxKey`: `ge_raster_gen`,
  búferes, formato y alfa del destino, textura con los bytes de su objeto
  de GX, modo y tres comandos) es la de la última emisión y nadie más tocó
  GX (`gx_tocado`: blit, vuelta a la CPU de las paletas, `set_pf`,
  presentar, reiniciar). `setup_targets` sigue en cada llamada (sus
  efectos: bajadas, periodos de las texturas, censo). Por triángulo,
  `tri_listo` en vez de `prepare_idle` + `drawn` con una condición más fina
  (`estado_listo`: con stencil o mezcla con el alfa del destino no hay nada
  que hacer si el búfer ya tiene el alfa en la GPU); lo quitan el inicio de
  cada llamada, el gancho de la VRAM, las demás primitivas, presentar,
  reiniciar, la vuelta a la CPU de las paletas y `set_pf`.
- Con `ge_verificar`: V, T y R se recalculan y se comparan; el estado de GX
  se emite siempre, y cuando se habría saltado se compara el estado
  interno de libogc antes y después (`__gxregs`, 0x598 bytes, salvo las
  banderas sucias de la palabra 48; su dirección se lee de `lis`/`addi` de
  `GX_SetTevOrder`, comprobado con el `.dol`: 0x804C0D10). Las display
  lists no sirven para esto: `GX_EndDispList` devuelve el estado interno al
  de antes. Cada triángulo sin preparar comprueba `estado_listo()`.
  Líneas `[VERIF] estado`, `[VERIF] estado de GX`, `[VERIF] triangulo` y
  totales en `[TIEMPOS]`.

Banco de qemu (tiras de 20 vértices, estado repetido): de ~38.500 a ~36.000
instrucciones por llamada (−2540) en las tres escenas, sin contar libogc
(en el Wii, ~950 más cuando no se emite GX). Estimación para las peleas
(~15.600 llamadas/s, con los porcentajes del censo): ~6-7 % del Broadway.

`prof_tirones` (solo mide, apagado): tiempo real de cada vblank;
`[TIEMPOS] Tirones`: cuántos vblanks hasta 20, 33, 50, 100, 250 ms y más;
los de más de 50 ms, cuánto suman y en qué cubetas se fue su tiempo; el
peor, con su momento en el juego (para la selección de personajes).

Verificación: `tests/test_censo.c` también con la caché y `ge_verificar`
(~27.000 comparados, 0 distintos; las 6 mutaciones se ven); huellas de
test_vdec sin cambios (x86 4D4A1D8B, PowerPC 0657B177, devkitPPC B3A2BCD3);
pspautotests A/B con `ge_estado_sucio` en x86 (MR y exacto) y con el
renderizador de GX bajo qemu (`tools/banco/build_cli_gx.sh`: libogc de
mentira que guarda el estado, `stub_estado.c`, y una huella del estado de
GX en cada dibujo): solo el ruido de siempre (`malloc`, `gpu/complex`,
savedata que no se repite). Los 97 de `gpu/` con GX y `ge_verificar`: 0
distintos. `tests/guest/ge_vram_mid.c`: vértices leídos de la VRAM del
propio framebuffer a mitad de llamada; misma huella de GX con y sin, y
quitando a propósito la invalidación de `tri_listo` del gancho de la VRAM
la huella cambia y `ge_verificar` lo detecta (y se corrige).

Autotest: sin (`ge_estado_sucio` 0) y con (1), con capturas, `prof_pmc` y
`prof_tirones`; verif (1 y `ge_verificar`) sin capturas.

### Para el siguiente chat (empezar aquí)

Metodología acordada (detalle en `docs/HOJA_DE_RUTA.md`): primero medir
(pasos 1-5), después optimizar de una en una (pasos 6-11).

- Hechos: pasos 1, 3, 4 y 5 (guion de DBZ grabado en el Wii; registros del
  Broadway en wiisp.log; línea base r1-r8), `ge_tex_clut` (0.5.7.8),
  `ge_skin_rapido` (0.5.8), 2.1 `cpu_sin_extra` (0.5.9), las texturas de
  framebuffer (`ge_rtt_gpu`, `ge_fb_relleno`, `ge_fb_hueco`, 0.5.10),
  `ge_z_rango`, `ge_mtx_cache` y `registro_diferido` (0.5.11), `prof_pmc`
  (0.5.11.1), 1.2 `ge_vdec_esp` y `ge_tri_rapido` (0.5.12-0.5.13),
  `gx_envio_rapido`, `ge_ensamblar_rapido` y 1.2b `ge_vdec_psq` (0.5.14) y
  el censo `prof_estado` (0.5.14.1), `ge_estado_sucio` y `prof_tirones`
  (0.5.15).
- Herramientas para medir sin el Wii (banco de qemu con el código de
  devkitPPC, perfil por función, psq_l emulado, A/B): `tools/banco/LEEME.md`.
- **En curso: 0.5.15** (`ge_estado_sucio` y `prof_tirones`, arriba): falta
  el resultado del Wii (sin/con/verif). Juntar llamadas (`ge_charolas`,
  1.4) va aparte, después y medido. Con los tirones se decide qué atacar en
  la selección de personajes (probablemente la fase 2 del intérprete). Después: las esperas al leer vértices
  (`round_to_screen`, la división por w y tres conversiones a entero por la
  memoria; leer va a IPC 0,67). Otros candidatos medidos: leer los
  comandos con un puntero (~56 de las ~80-100 instrucciones por comando),
  saltarse `setup_targets`/`lookup_texture` cuando no cambien (~900), un
  solo `GX_Begin` por llamada, y lo de antes: el 4444 de 64x64 de DBZ, los
  índices CI8 con dcbt, el limitador de cuadros.

Pendiente de antes: la primera tanda del GE (skinning rápido, matrices al
cambiar, color constante; ver el carrito) entra en los pasos 6-11 con su
interruptor. Pendiente del usuario: probar la 0.5.4+ (warping, TN/TS y
MR/ME). Numeración: 0.5.6.x mientras se arreglaba DBZ, ahora 0.5.7.

### Carrito (para cuando el usuario vuelva a su Wii)

1. Verificar en el Wii la 0.5.4+ (warping: TN/TS y MR/ME) con el registro.
2. Skinning rápido en modo GX (aprobado): mezclar las matrices por peso en
   float y una sola multiplicación; saltar pesos 0; prueba de invitado
   con 1-8 huesos, pesos de 8/16 bits y float y morph, rápido contra
   exacto con tolerancia. Modo exacto sin cambios.
3. Después, por orden (ganancia estimada / riesgo en la conversación):
   matrices solo al cambiar, color constante sin luces, luces solo para
   los vértices de triángulos que se dibujan, paletas de GX (TLUT),
   framebuffers en la GPU, reemplazo de funciones por hash, T&L por GX.

## Siguientes pasos, por prioridad

1. **Que el usuario pruebe los controles de la v0.5.0** (sobre todo el
   sentido de la cruceta por movimiento) y, después, seguir con las
   optimizaciones del GE en el orden de arriba (sin frameskip ni dynarec
   por ahora, a petición del usuario). DBZ TTT: `sceKernelCheckThreadStack`
   (v0.5.5) y buzones Mbx (v0.5.6) hechos; falta que el usuario diga hasta
   dónde llega ahora.

2. Hecho (UMD).

3. Hecho (LwMutex).

4. Hecho (event flags). Antes decía: despertar en orden de llegada (`wait_seq`) o de
   prioridad, como PPSSPP, no en el orden de la tabla de hilos
   (`sceKernelSetEventFlag`). GTA usa un event flag `UmdStreamEventFlag`
   entre su hilo de lectura del UMD (prio 0x20) y el que pide los datos: si
   se pierde el bit de "terminado" (0x1), el juego espera para siempre (los
   ports recompilados de LCS/VCS tuvieron que parchear justo esto). Hace
   falta que `sceIoRead` ceda la CPU un tiempo realista (ya lo hace con
   `hle_delay_us`) y que despertar a un hilo de más prioridad lo ponga a
   correr en el acto (`make_ready` → `request_resched`; comprobarlo).

5. **Revisar cómo arranca PPSSPP un juego y compararlo con WIISP**
   (sugerencia del usuario). Archivos: `Core/PSPLoaders.cpp`
   (`Load_PSP_ISO`), `Core/HLE/sceKernelModule.cpp` (`__KernelLoadExec`,
   `KernelLoadModule`, cómo corre `module_start`, módulos falsos),
   `Core/HLE/HLE.cpp` (`g_moduleMeta`: los PRX del disco que PPSSPP no
   carga de verdad, p. ej. `sceMpeg_library`, `sceATRAC3plus_Library`,
   `sceSAScore`, `sceAvcodec_driver`, `sceAudiocodec`, `sceVideocodec`...),
   `Core/HLE/sceKernelThread.cpp` (hilo raíz y `user_main`: prioridad 0x20,
   pila, argumentos), `Core/HLE/sceKernelMemory.cpp` (particiones, memoria
   volátil 0x08400000 de 4 MB), y el estado inicial (UMD activado, Memory
   Stick insertada, versión de SDK). En WIISP: `src/frontend/app.c`,
   `src/hle/hle.c` (`hle_init`), `src/hle/kernel.c` (`kernel_init`),
   `src/hle/module.c`.

6. **Más requisitos conocidos de GTA LCS** (de los logs de PPSSPP y de los
   ports recompilados `elmasas/lcs-recomp` y `PSPRecomp`):
   - Interrupción de vblank: GTA registra la subinterrupción (30, 15); debe
     llamarse cada vblank con a0 = 15 y a1 = su argumento, y desde ella
     llama a `sceGeListEnQueue` (debe funcionar dentro de una interrupción).
   - Callbacks del GE (`sceGeSetCallback`): el de fin de lista debe correr.
   - `sceKernelVolatileMemTryLock` debe funcionar y dar 0x08400000/0x400000
     justo después de cerrar el diálogo de partidas (en WIISP siempre
     funciona).
   - El video de introducción: `sceMpegRingbufferPut` debe llamar al
     callback del juego y el video termina cuando `sceMpegGetAvcAu` /
     `GetAtracAu` devuelven 0x80618001. Después el juego llama a
     `sceKernelSuspendThread` sobre hilos ya borrados (debe fallar sin más).
   - `scePowerGetPllClockFrequencyFloat` = 222.0 (ya es así).
7. **Detalles menores** encontrados en la auditoría: `sceKernelWakeupThread`
   sobre el propio hilo → 0x80020197 (PPSSPP); `sceKernelRegisterExitCallback`
   con UID inválido solo da error si la versión de SDK ≥ 0x3090500; los
   avisos del sistema (UMD, energía, Memory Stick) no deberían forzar un
   cambio de hilo en el acto (PPSSPP los ejecuta en la siguiente
   replanificación).
8. Auditorías contra PPSSPP que no llegaron a terminar (límite de uso):
   módulos/memoria/interrupciones, display/GE/ctrl, audio/SAS/Atrac,
   Mpeg/Utility, red/varios e hilos/sincronización.

Pendientes de antes (menos urgentes): optimizar el núcleo del intérprete
(~114 ns por instrucción en Dolphin) y, más adelante, el dynarec; alarmas,
mutex normales, mailboxes, message pipes y VPL; sceMp3; planificación fiel
del GE; stencil en GX; salida de audio; frameskip.
