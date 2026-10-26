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
- Desde la 0.5.17.2 (pedido del usuario): cada cambio en el código lleva
  el comentario `//Arreglo` y **ningún otro comentario**. Lo que haga falta
  explicar va en `docs/ARREGLOS.md` (versión y descripción breve) y en este
  documento.
- El autotest genera como mucho 4 registros (`.log`) y un `.capturas.txt`:
  el usuario no puede mandar más de 5 archivos en un mensaje.
- Nunca subir datos de juegos (ISO, EBOOT descifrados, volcados): son de
  Sony/Rockstar.
- PPSSPP (GPLv2+) es la referencia de comportamiento: ante una duda de cómo
  se comporta la PSP, se mira cómo lo hace PPSSPP y se porta.
- Antes de subir algo: `make -f Makefile.pc test`, pspautotests en x86 (y en
  PowerPC si se toca algo sensible al orden de bytes) y compilar para el Wii.
- Cada compilación que se entrega lleva su autotest. El .zip tiene dos
  carpetas: `apps/wiisp/` (`boot.dol` y `meta.xml`) y `wiisp/` con
  `autotest.txt` (con ese nombre, sin número de versión: WIISP lee primero
  `sd:/wiisp/autotest.txt`). Corridas: normalmente sin y con (con capturas);
  verif solo cuando hay un camino rápido nuevo, sin capturas; todas con
  `opt prof_pmc 1`.

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

### Resultado de la 0.5.15 en el Wii (DBZ, sin / con / verif)

Capturas iguales 12 de 12 entre sin y con; `ge_verificar`: 0 diferencias
en ~757.000 emisiones de GX y ~7,2 millones de triángulos; peleas +6,5 %.
Los tirones del registro venían de tres sitios: el fsync de wiisp.log
(200-320 ms), las lecturas del disco (330-365 ms) y la espera a la GPU al
presentar ("esperar GX", 12 % en los menús). En las peleas: GE 39 %, leer
vértices 18 % (484 ciclos por vértice, IPC 0,66), índices CI8 ~2,9 ms cada
uno, y el registro 12,6 %.

### v0.5.16: medidas arregladas, recarga, registro en otro hilo, CPU y GPU en paralelo

Medidas:
- `prof_tirones` por imagen y relativo (`core/tirones.c`): por cada imagen
  del juego, cuánto tiempo real tardó por segundo de juego, comparado con
  la mediana de las últimas (lo normal); un tirón es más del doble y más de
  20 ms perdidos. Una pantalla de carga no es un tirón (pasa tiempo de
  juego). `tests/test_tirones.c`.
- La línea de verificación del estado sucio dice lo que se habría saltado.
- Texturas 565: se pasan a un búfer de su tamaño (la memoria y la cuenta
  del presupuesto coinciden).
- Línea de memoria en `[TIEMPOS]` (montón, MEM1/MEM2 libres, texturas,
  búferes, lo que espera a la GPU para liberarse).
- `prof_es` (apagado, solo mide): las lecturas del disco, su tamaño,
  tiempo real, MB/s, si son seguidas, y lo que leer en otro hilo podría
  esconder como mucho (lo que tarda la emulación en llegar al final de la
  espera emulada, o en recoger una asíncrona). **El hilo de E/S queda para
  después, según lo que mida**: para que guiones y capturas se repitan, el
  tiempo emulado no puede depender del USB, así que un hilo solo esconde
  la espera emulada (tamaño/100 us: 1,7 MB son 17 ms emulados contra ~200
  ms de leerlos del USB); si nadie más trabaja en esa espera, no se gana
  nada. Con lo que mida se elige: el hilo, leer por adelantado o la caché
  de libfat.

Recarga (cargar otro programa sin salir: autotest sin/con/verif, volver
al menú). `--veces N` en el CLI y `tests/guest` carga cada programa dos
veces seguidas. Encontrado y corregido (lo que quedaba del programa
anterior):
- `misc.c`: el DMA en curso (`dmac_deadline`; el reloj vuelve a 0, así que
  `sceDmacTryMemcpy` daba "ocupado" para siempre). `misc_init`.
- `net.c`: el reloj de la CPU y del bus (`power_init`: 222/111).
- `kernel.c`: la generación de los UID de LwMutex y el orden de eventos.
- `ge.c`: la última uv y normal, la primitiva a medias y la inmediata
  (`ge_vertex_reset`, antes solo en las pruebas): la segunda carga podía
  dibujar con otro color. La instantánea de la autotextura
  (`ge_raster_reset`).
- `display.c`: los botones y el stick que quedaban pulsados al final del
  guion pasaban al cuadro 1 del siguiente.
- `io.c`: los avisos de ioctl, una vez por programa.
Comprobado (`tests/recarga.py`): las 572 pspautotests con ASan, tres veces seguidas: la salida
es la de una vez repetida, la misma imagen y la misma memoria tras
descargar (byte a byte), sin errores ni fugas. `tests/guest/recarga.c`
falla sin los arreglos (DMA ocupado, reloj 333, otro color).

`registro_hilo` (encendido; `core/registro.c`): wiisp.log lo escribe otro
hilo (LWP de prioridad 100 en el Wii, pthread en el PC; en Windows sigue
en el acto). La emulación solo copia el texto a una cola de 64 KB; el
escritor hace fwrite, fflush y fsync, y mientras espera a la tarjeta
(IOS) la emulación sigue. Lo que decide cuándo sincronizar no cambia; los
fallos (`[CPU]`, atascos, el final) esperan a la tarjeta como antes; el
informe de `[TIEMPOS]` y las estadísticas de cada minuto ya no esperan.
Línea nueva: `[TIEMPOS] registro en otro hilo` (KB, lotes, fsync y su
tiempo, esperas de la emulación). Comprobado: `tests/test_registro.c`
(colas de 1 byte a 64 KB, trozos más grandes que la cola, fsync por medio,
cambios de archivo; las mutaciones se ven), ThreadSanitizer sin avisos del
registro, y wiisp.log idéntico con y sin el hilo (también al recortarse).

`gx_paralelo` (encendido; 3.5): presentar no espera a la GPU. Detrás de
la copia al XFB va un testigo (`GX_SetDrawSync`, otro registro que el de
`GX_DrawDone`); la imagen se muestra, con su texto, al principio de la
presentación siguiente, cuando la GPU lo pasa (normalmente ya). Al pasar
se libera lo diferido antes de él (las texturas de framebuffer se rehacen
en cada imagen) y el anillo de TLUT. Tres XFB: se compone en el que lleva
más tiempo sin verse. La imagen sale un vblank después; las capturas no
cambian (salen de la memoria de la PSP). Si un testigo no llega en 200 ms
se espera como antes y el informe lo dice. Revisado: nada da por hecho que
la GPU esté parada al empezar el cuadro (las escrituras de la CPU en
memoria de la GPU van a búferes nuevos, detrás de un `wait_gpu` o en un
anillo).

Pruebas nuevas: `tests/guest/disco.c` se ejecuta desde una ISO y una CSO
que hace `tests/guest/mkiso.py` (lecturas, saltos, asíncrona, `sce_lbn`,
`umd0:`; bytes y tiempos emulados). El stub de libogc del banco numera los
punteros por orden de aparición de verdad (antes por la dirección: la
huella cambiaba si cambiaba el montón) y con `GX_TRAZA=1` escribe la
huella de cada grupo en cada dibujo.

Verificación: huellas de test_vdec sin cambios (x86 4D4A1D8B, PowerPC
0657B177, devkitPPC B3A2BCD3), 8750 comprobaciones; 275/275 pspautotests;
`tests/guest` 22/22; A/B de `registro_hilo` y `prof_es`: iguales (salvo
`malloc` y savedata en paralelo); el renderizador de GX contra la 0.5.15
bajo qemu: la misma huella de GX en los 260 programas (`gpu/exact/pixel`
lee memoria que la GPU de mentira no escribe: con el mismo relleno de
malloc sale igual).

Autotest: sin (`registro_hilo` 0 y `gx_paralelo` 0) y con (1 y 1), con
capturas, `prof_pmc`, `prof_tirones` y `prof_es`; sin verif (no hay camino
rápido nuevo del GE).

### Resultado de la 0.5.16 en el Wii (DBZ, sin / con) y arreglos para la siguiente

Capturas 12 de 12 iguales. Menús de 128 % a 140 %, "esperar GX" de 12,1 %
a 2,4 %; la escena entera de 124 s a 117 s; peleas +3 %. El escritor del
registro hizo los fsync en 30-56 ms sin que la emulación esperara. Los
tirones de las peleas (200-230 ms cada 30 s, cubeta "registro") eran el
informe de `[TIEMPOS]` escrito también en la consola de texto del Wii
(oculta, pero se dibuja y se desplaza; en los menús era el 3-6 % que
quedaba de "registro"). `prof_es`: 1 s de E/S cada 30 s en los menús,
6-7,7 MB/s, 98 % seguidas, como mucho ~300 ms escondibles: el hilo de E/S
no compensa. Peor tirón de la selección de personajes: 700-850 ms de pura
CPU (el intérprete).

Ya en el código (van con la siguiente versión): `hle_set_log_screen`, que
el Wii apaga mientras se juega y en el autotest (a la consola solo van las
líneas `[CPU]`; a wiisp.log todo igual), y la línea de memoria, que contaba
como en uso el hueco de direcciones entre MEM1 y MEM2 (decía 284 MB): ahora
es lo que el montón pidió a cada una menos lo libre.

Plan acordado para lo siguiente (detalle en la conversación de la 0.5.16):
2.2 `cpu_predec` (bloques de tarjetas de 8 bytes con computed goto, reloj
exacto por bloque, salidas a mitad de bloque reconstruidas, invalidación
por páginas de código con `cpu_predec_verif`, arena en MEM2 porque MEM1
solo tiene 156 KB libres), después leer vértices sin esperas (`psq_st` para
x, y, z, división antes, banderas sin saltos, paired singles con
`ge_verificar`), CI8 en la GPU (copia 565 como CI14 con una TLUT de 16K
entradas) y los comandos del GE con un puntero.

### v0.5.17: intérprete predecodificado (`cpu_predec`, 2.2)

`cpu/predec.c` (encendido). Cada trozo de código se traduce una vez a un
bloque de tarjetas y se ejecuta con computed goto:
- Bloque: desde una dirección hasta el primer salto con su delay slot, un
  syscall, una instrucción sin tarjeta o 64 instrucciones. Tarjeta de 8
  bytes en el Wii: la dirección de su código y los operandos ya preparados
  (registros como desplazamientos en bytes, inmediato con signo). Ninguna
  escribe en $zero: las que lo harían son nop o (cargas, sc) van al
  intérprete. Cada una carga el código de la siguiente al principio, así el
  `mtctr` va pronto y el `bctr` no espera.
- Reloj exacto: un bloque solo se ejecuta entero si caben sus n
  instrucciones en lo que queda de `cpu_run` (se descuentan al entrar); si
  no, lo hace el intérprete de una en una. Las salidas a mitad (memoria
  lenta, VFPU, mtic, parar, el bloque se reescribió) reconstruyen pc, npc
  y el reloj con la posición de la tarjeta, también en el delay slot; el
  branch likely no tomado no cuenta el delay slot. Lo que no tiene tarjeta
  (instrucciones que fallan, break, un salto en el delay slot, código fuera
  de la RAM) lo ejecuta el intérprete de siempre (`cpu_run_interp(1)`).
- Enlaces: cada bloque guarda el que lo sigue y el del destino del salto;
  la tabla (16K cubetas) solo se mira si el enlace no vale. Arena de 4 MB
  en MEM2 (MEM1 solo tiene 156 KB libres); si se llena se vacía entera.
- Código que cambia: zonas de 1 KB (`cpu_code_map`, 32 KB). Las escrituras
  de la CPU (también en `cpu_run_interp` y `cpu_run_ref`), de la VFPU
  (sv.s, sv.q), `mem_write*` y `mem_note_write` (archivos, DMA, módulos,
  memset y memcpy del kernel) tiran los bloques de las zonas tocadas; si
  cae en el bloque que corre, se sale al acabar la instrucción.
  `sceKernelIcacheInvalidate*` no hace nada (un juego que lo llame en cada
  cuadro vaciaría todo; las escrituras ya se siguen).
- `cpu_verificar` (apagado): cada bloque comprueba al entrar que sus
  instrucciones siguen en la RAM; si no, `[PREDEC] bloque viejo` en el
  registro (faltaría un aviso de escritura) y se rehace. En las 572
  pspautotests, ninguno.
- `[TIEMPOS] CPU predecodificada`: % de instrucciones en bloques, bloques
  hechos y tirados, escrituras en código, búsquedas, arena.

Arreglo de paso (los dos intérpretes): con dos NaN de entrada, la FPU
devolvía el que tocara según el orden en que el compilador pusiera los
operandos (x86 y PowerPC dan el del primero); ahora es siempre el de la
primera fuente que lo sea, silencioso.

Medido (banco de qemu, `tools/banco/medir_cpu.sh`, copias, llamadas y
ramas): de 28,7 a 17,4 instrucciones del Broadway por instrucción de la
PSP; addiu de 24 a 10, lw de 29 a 16 (devkitPPC). En el PC (fuera de
orden) de 342 a 441 MIPS. El paso de un bloque a otro cuesta ~20
instrucciones (bloques cortos: 3-4 por instrucción): eso y las fusiones son
la 2.3. La VFPU sigue por `vfpu_exec`.

Verificación: tests/test_interp.c compara los tres intérpretes (referencia,
el de antes y el predecodificado) en 5400 programas aleatorios cortados en
trozos aleatorios, 2000 de ellos que se reescriben a sí mismos mientras
corren (nops y basura), con el código cruzando una zona de 1 KB; el estado
entero tiene que coincidir, también el pc del último fallo. Casos dirigidos:
`cpu_verificar` con código cambiado sin aviso, enlaces a bloques de otra
zona reescritos (por el destino del salto y cayendo al siguiente) y
`bltzal ra` con ra negativo. Las 9 mutaciones probadas (inmediato sin
signo, una instrucción de más o de menos al salir, el pc del delay slot al
llamar fuera, no salir al reescribirse, ra antes de leerlo, el delay slot
del likely no tomado, enlaces sin mirar left o si siguen vivos) se ven.
Huellas de test_vdec sin cambios; pspautotests 275/275 y A/B con
`cpu_predec` (solo `malloc`); tests/guest 22/22; recarga con ASan.

Autotest: sin (`cpu_predec` 0) y con (1), con capturas, `prof_pmc` y
`prof_tirones`; verif (1 y `cpu_verificar`) sin capturas.

### Resultado de la 0.5.17 en el Wii (DBZ, sin / con / verif)

Capturas sin y con idénticas (12/12); verif: ningún bloque viejo. Pero la
velocidad apenas cambia (`cpu_predec` 0 → 1, misma versión):

| Escena | % de la PSP | instr. Broadway / instr. PSP | ciclos / instr. PSP | fallos L1D / 1000 instr |
|---|---|---|---|---|
| Menús | 139,6 → 142,7 | 30,1 → 22,0 | 28,2 → 26,1 | 1,0 → 6,2 |
| Pelea | 73,5 → 72,5 | 33,2 → 25,4 | 33,6 → 33,7 | 1,6 → 9,5 |
| Escena pesada | 33,4 → 32,1 | 39,7 → 32,0 | 40,2 → 43,8 | 1,5 → 5,9 |

Un 20-27 % menos de instrucciones, mismos ciclos. qemu no lo vio porque no
simula la caché. Una investigación aparte (manuales del 750CL/750CX, libogc,
GCC) concluyó: los fallos de L1D que aciertan en L2 (~5 ciclos) solo
explican 1-1,7 ciclos por instrucción; el resto tienen que ser fallos que
llegan a la memoria (crece con el peso del GE, que echa la arena de L2) y
dos cosas del 750: `mtctr` es serializado (espera a que acabe todo lo
anterior; cada tarjeta paga ~7-9 ciclos aunque todo esté en caché) y el
paso entre bloques cuesta ~24 ciclos. Además GCC unió los finales de varias
tarjetas (crossjumping): `sw` salta a una cola común con el `mtctr` al
final. Quedan 12-17 de los 33,7 ciclos de la pelea sin explicar (VFPU,
FPU, HLE, memoria lenta, búsquedas): hay que medirlos.

### v0.5.17.1: medir la CPU en el Wii y la memoria del predecodificado

Medidas nuevas (solo miden):
- `prof_pmc_l2`, `prof_pmc_ctr`, `prof_pmc_carga` (apagados): lo que
  cuentan PMC3 y PMC4 (`broadway.c`, tabla de eventos del 750): fallos de
  datos en L2 (7) y expulsiones de L2 (5); ciclos con la unidad de saltos
  esperando LR o CTR (17); ciclos de fallos de carga en L1D (15). PMC1 y
  PMC2 siguen con ciclos e instrucciones. Las líneas `[TIEMPOS] PMC` dicen
  qué cuentan y, en "PMC de la CPU", cuánto por instrucción de la PSP.
- `[TIEMPOS] CPU por dentro`: muestreo de la cubeta CPU por fases
  (`prof_cpu_phase`: tarjetas, VFPU, FPU, memoria lenta, buscar y
  traducir, intérprete de siempre) con cuántas hubo y los ns de cada una; y
  dónde está cada cosa (bloques, lo frío, tabla, mapa, RAM de la PSP:
  0x80... es MEM1, 0x90... MEM2).

Cambios, cada uno con su interruptor (marcados `//arreglo` en el código):
- `predec_compacto` (1): el bloque en dos partes. Lo que se lee al
  ejecutar (pc, end, target, succ[2], n, alive, branch: 24 bytes en el Wii
  y las tarjetas) alineado a 32, así la cabecera y la primera tarjeta
  llenan una línea; lo frío (listas de la tabla y de zonas, zonas e
  instrucciones) en su propia arena de 1,5 MB (la de los bloques, 3 MB;
  antes 4 MB para todo); la tarjeta de cierre dice
  dónde (`c[n].a.w`). Las tarjetas de FPU y VFPU llevan la instrucción en
  `a.w` (no leen lo frío). Con 0, lo frío va detrás de las tarjetas,
  alineado a 8: los mismos bytes que en la 0.5.17.
- `predec_sin_unir` (1): el bucle (`cpu/predec_bucle.h`) se compila dos
  veces, la segunda con `-fno-crossjumping` (atributo `optimize`): cada
  tarjeta acaba en su propio `mtctr`/`bctr`. `-fno-gcse` también se probó:
  cada tarjeta recalculaba direcciones, 21,2 en vez de 17,5 instrucciones
  en el banco; descartado. Cambiar de copia vacía los bloques.
- `predec_mem1` (1): el Wii reserva 3 MB al principio de MEM1 al arrancar
  (`reserva_mem1`, como la RAM de la PSP en MEM2; el montón sigue detrás);
  con el interruptor los bloques van ahí, si no con `malloc`.

Banco (`tools/banco/medir_cache.sh`, nuevo): cachegrind sobre el CLI del PC
con programas de N funciones (`cpu_grande.py`), caché del Broadway con el
doble de bytes (punteros y tarjetas miden el doble en el PC). Fallos por
instrucción de la PSP, 256 / 1024 funciones: intérprete 0,0006 / 0,023 de
L1D y 0 de L2; predecodificado 0.5.17 0,160 / 0,192 de L1D y 0,0023 / 0,122
de L2; con `predec_compacto` 0,128 / 0,164 de L1D y 0,0002 / 0,096 de L2
(−15-20 % en L1D, −21 % en L2). Lo que más falla: la cabecera del bloque
siguiente al enlazar, las tarjetas y las búsquedas en la tabla (los `jr` de
vuelta casi nunca aciertan el enlace guardado). De fondo: una instrucción
ocupa ~16 bytes en vez de 4. qemu (`medir_cpu.sh`): 17,5 instrucciones con
cualquier combinación (17,4 en la 0.5.17).

Verificación: test_interp compara el predecodificado con las cuatro
combinaciones de `predec_compacto` y `predec_sin_unir`, la mitad de los
programas con arenas diminutas (34921 vaciados; en una de cada cuatro se
llena antes la de lo frío) y un caso dirigido que cambia de copia y de
reparto sin tocar el código (`cpu_predec_tarjetas_ok`: todas las tarjetas
son de la copia que corre). Mutaciones nuevas detectadas: el desplazamiento
de lo frío, la lista de zona cambiada, cambiar de copia o de reparto sin
vaciar, vaciar sin la arena fría, la FPU con otra palabra, no mirar si cabe
lo frío. Huellas de test_vdec sin cambios (x86, PowerPC, devkitPPC);
pspautotests 275/275 y A/B con `predec_compacto,predec_sin_unir` y con
`cpu_predec` (solo savedata en paralelo, idéntico de una en una);
tests/guest 22/22; recarga con ASan en las 572 sin problemas (en una
corrida anterior `gpu/clipping/guardband` acabó con unos cientos de bytes
distintos entre vueltas; no se repitió en 7 intentos ni en la corrida
entera siguiente); `cpu_verificar` en las 572: ningún bloque viejo.

Autotest (8 corridas, todas con `prof_pmc` y `prof_tirones`): viejo (los
tres a 0), compacto, sinunir, mem1 (uno más cada vez), con capturas; l2viejo
y l2 (`prof_pmc_l2`, los tres a 0 y a 1), ctr (`prof_pmc_ctr`) y verif, sin
capturas.

### Resultado de la 0.5.17.1 en el Wii (DBZ, 8 corridas) y plan acordado

Las 8 corridas ejecutan exactamente lo mismo (8206 `[LLAMADA]` idénticas,
mismas `[STATS]`); verif: ningún bloque viejo. Ciclos por instrucción de la
PSP en la corrida completa (análisis aparte, con los registros):

| Cambio | Ciclos/instr | Notas |
|---|---|---|
| l2viejo → compacto (bloques en MEM2 en las dos) | 34,50 → 33,15 (−3,9 %) | fallos L1D −14 %, vaciados 6 → 2 |
| compacto → sinunir | 33,15 → 33,42 (+0,8 %) | +0,67 instr, +5,6 % saltos mal: empeora |
| sinunir → mem1 | 33,42 → 29,48 (−11,8 %) | lo que gana es MEM1 |
| los tres (l2viejo → l2) | 34,50 → 29,47 (−14,6 %) | velocidad completa 70,8 → 77,3 % |

- Sin `predec_mem1` los bloques caen donde los ponga malloc (MEM1 en viejo,
  MEM2 en compacto); MEM1 queda con 4 KB libres. La 0.5.17 los tenía en
  MEM2. Con bloques en MEM1, la 0.5.17.1 llega al mismo punto del juego un
  4-6 % antes que la 0.5.17. Comparar por tiempo real hasta un mismo
  punto del juego, no por ventanas de 30 s reales (cubren tramos distintos).
- Fallos de datos en L2: 0,065 por instrucción en la pelea, 0,089-0,099
  en la escena pesada; un fallo a MEM2 cuesta ~66 ciclos más que a MEM1.
- Espera del CTR: 8,2-8,3 % de los ciclos de la CPU (2,0-3,0 por instr).
- CPU por dentro (mem1): tarjetas 60-80 % (20-25 ciclos por instrucción,
  la meta de 2.2); VFPU 11-33 % de la CPU con el 2-9 % de las
  instrucciones (~127 ciclos cada una); FPU 4-6 % (~50-60 ciclos);
  intérprete de siempre 1-3 % (~270-390 ciclos); buscar 1,4-1,8 %.
- Pelea: CPU 51 % del tiempo (tarjetas 34 %, VFPU 12 %); escena pesada: GE
  51 % (leer vértices 24 %, ensamblar + enviar 16 %), VFPU 12 %.
- El "registro" más caro en viejo/compacto/sinunir/mem1 son las capturas
  (`capturas 400`; las otras cuatro corridas iban con 0).
- Las 1-2 instrucciones del Broadway de más por instrucción en todas las
  configuraciones vienen seguramente de las medidas nuevas en las tarjetas
  de FPU/VFPU (contador de 64 bits, fase del muestreo, `compacto ?`).

Plan acordado (una cosa por versión, cada una con su interruptor):
1. **0.5.17.2** (pequeña): por defecto `predec_sin_unir` 0 y `predec_mem1`
   1; ventanas por tiempo de juego (`prof_ventana_juego`, N vblanks, que
   reporten segundos reales); instrucciones por bloque ejecutado y qué cae
   al intérprete (`prof_bloques`); los contadores de las tarjetas solo con
   las medidas; autotest midiendo sin capturas y una corrida con capturas
   solo para comprobar que salen iguales.
2. **0.5.18**: tarjetas propias para la VFPU (`cpu_vfpu_tarjetas`), paso a:
   lv.s, lv.q, sv.s, sv.q, mtv/mfv (43 % de las VFPU, solo bits,
   verificables en el PC contra vfpu_exec). Después b) aritmética simple en
   C (fmuls, verificable en qemu) y c) paired singles con su interruptor y
   verificación en el Wii (qemu no las emula).
3. **0.5.19**: enlace directo entre bloques y mtctr temprano (2.3a,
   `cpu_enlace`) si `prof_bloques` da bloques de 5-6 instrucciones; si no,
   fusiones.
4. Después: leer vértices midiendo antes por partes (~330 instrucciones por
   vértice; el problema no son las esperas), CI8 en la CPU, comandos del GE
   con computed goto, HLE (2.4a/b, con histograma de syscalls y perfil por
   PC). Tiras directas a GX (3.1b) cambian lo que se dibuja: A/B de
   capturas. Mapa fijo de MEM1 (2.9) midiendo antes qué ocupa MEM1. 2.5
   junto con el dynarec.

### v0.5.17.2: medidas por tiempo de juego y bloques (punto 1 del plan)

Cambios (todos con `//Arreglo`; lista en `docs/ARREGLOS.md`):
- Por defecto `predec_sin_unir` 0 y `predec_mem1` 1.
- Las tarjetas solo se cuentan al medir: el bucle se compila cuatro veces
  (unido o sin unir, con o sin medidas, `PREDEC_MEDIR`). Las copias
  normales no suman `st.fpu`/`st.vfpu` ni escriben `prof_cpu_phase`
  (tampoco al buscar, en el intérprete de siempre ni en la memoria lenta).
  La copia de medida se usa con `prof_cpu` (nuevo, apagado: el muestreo
  "CPU por dentro") o con `prof_bloques`. Cambiar de copia vacía los
  bloques, como antes. Sin `prof_cpu`, la línea "CPU por dentro" solo dice
  dónde está cada cosa en la memoria.
- `prof_bloques` (apagado, solo en la copia de medida): línea
  `[TIEMPOS] Bloques`: bloques ejecutados, instrucciones por bloque (las
  del bloque entero, aunque se salga a mitad), reparto por tamaño (1, 2,
  3-4, 5-8, 9-16, 17-32, 33-64), y cuántas veces se pasa al bloque
  siguiente o al destino del salto por el enlace guardado y cuántas sin él
  (y de esas, cuántas acaban en `jr`/`jalr`). Línea `[TIEMPOS] Interprete
  de siempre`: cuántas instrucciones y por qué (delay slot suelto, sin
  tarjeta, no cabe en lo que queda del reloj, fuera de la RAM) y las 10
  instrucciones que más caen. En un programa de prueba casi todo son los
  stubs de las importaciones: `jr ra` con el `syscall` en el delay slot
  (el bloque no puede acabar en un syscall dentro de un delay slot), dos
  instrucciones del intérprete de siempre por cada llamada al HLE.
- `prof_ventana_juego` (apagado): el informe `[TIEMPOS]` se corta cada N
  vblanks de juego en vez de cada 30 s reales, en múltiplos exactos de N
  (vblank N, 2N...): todas las corridas cortan en los mismos puntos del
  juego. La primera ventana empieza en el vblank 1. La cabecera dice
  "ventana de juego: vblanks A a B" con los segundos reales. N: línea
  `ventana N` del autotest (600 por defecto, ~10 s de juego) o
  `--ventana N` en el CLI.

Medido (banco de qemu, `medir_cpu.sh`): 17,5 → 17,3 instrucciones del
Broadway por instrucción de la PSP por defecto (copia unida sin medidas;
la 0.5.17.1 daba 17,5 unida o sin unir); con `prof_cpu` 17,5; con
`prof_bloques` 25,9 (cuenta cada bloque con contadores de 64 bits: esa
corrida no sirve para medir velocidad). cachegrind (`medir_cache.sh`, 256 /
1024 funciones): fallos de L1D 0,1285 / 0,1638 → 0,1284 / 0,1637, L2 sin
cambios; con `prof_cpu`, igual que antes. El banco casi no tiene VFPU ni
FPU, así que el ahorro en el Wii puede ser mayor que este.

Verificación: test_interp compara el predecodificado con las ocho
combinaciones de `predec_compacto`, `predec_sin_unir` y medir (con
`prof_cpu`, `prof_bloques` o los dos); caso dirigido con los contadores de
`prof_bloques` (bloques, instrucciones, tamaño, enlaces, caídas por no
caber, sin tarjeta y delay slot suelto, y nada con el interruptor
apagado); el cambio de copia también con y sin medidas, y
`cpu_predec_tarjetas_ok` mira además que `h_tab` sea de la copia que toca.
Mutaciones detectadas: dos copias con el mismo número, el reloj cambiado
solo en la copia de medida, el tamaño del bloque mal, la caída mal
clasificada y la copia de medida sin elegir con `prof_bloques`. Huellas de
test_vdec sin cambios (x86 4D4A1D8B, PowerPC 0657B177, devkitPPC
B3A2BCD3); `build_tests_dkp.sh` sin fallos; pspautotests 275/275; A/B con
`prof_cpu,prof_bloques`, `predec_sin_unir` y `prof_ventana_juego`: solo
`gpu/complex` y `malloc/malloc` (cambian entre corridas) y savedata en
paralelo (idénticas de una en una); tests/guest 22/22; recarga con ASan
en las 572 sin problemas (una corrida con la máquina cargada, con el
banco y compilaciones a la vez, dio `gpu/clipping/guardband` y
`gpu/texcolors/dxt5` con unos cientos de bytes de memoria distintos e
`io/file/rename` con otra salida; solas, esas tres salen bien con y sin
los cambios).

Autotest (DBZ, todas con `prof_pmc`, `prof_tirones`, `prof_ventana_juego`
y `ventana 600`): base (valores por defecto, sin capturas), cpu
(`prof_cpu`, sin capturas: la diferencia con base es lo que cuestan los
contadores), bloques (`prof_cpu` y `prof_bloques`, sin capturas: solo para
los recuentos) y capturas (como base, con `capturas 400`, para comparar
las imágenes con las de la 0.5.17.1).

### Resultado de la 0.5.17.2 en el Wii (DBZ, base / cpu / bloques / capturas)

Las cuatro corridas ejecutan lo mismo que la 0.5.17.1 (mismas `[LLAMADA]`,
`[MANDO]`, `[IO]` y `[STATS]`: 819.031.925 instrucciones del guion,
20.417.174 de VFPU; 1343,1 M en total). Faltó comparar las huellas de las
capturas con las de la 0.5.17.1 (pedir su `.capturas.txt`).

| Corrida | Real | Velocidad | Ciclos / instr. PSP | Instr. Broadway / instr. PSP |
|---|---|---|---|---|
| 0.5.17.1 l2 (la mejor) | ~108,9 s | 77,3 % | 29,47 | ~28 |
| base | 106,2 s | 79,3 % | 28,20 | 26,3 |
| cpu (`prof_cpu`) | 108,6 s | 77,5 % | 29,41 | 27,3 |
| bloques | 119,1 s | 70,7 % | 34,75 | 34,3 |
| capturas | 109,8 s | 76,7 % | 28,13 | 26,3 |

- Los contadores de las tarjetas costaban 1,2 ciclos y 1,0 instrucción del
  Broadway por instrucción de la PSP (~4 % de la CPU): eran las 1-2
  instrucciones de más de la 0.5.17.1. `predec_sin_unir` a 0 casi no
  cambia (cpu 29,41 frente a l2 29,47, los dos con contadores).
- Tirones: el de 60,2 s de 971 a 930 ms; el de 35,1 s de ~770 a 736 ms.
- Escenas por ventana de juego: 0-30 s menús y 2D (227-341 %); 30-50 s
  82-93 %, CPU 74-78 % del tiempo; 50-60 s carga (tirón de 448 ms de E/S a
  los 56,9 s); 60-84 s escena pesada 36-38 %: GE 44-50 % (16.000 vértices
  por vblank, 44 % con huesos, 496 ciclos por vértice; 873 llamadas por
  vblank, 2785 ciclos de comandos cada una) y CPU 37-40 %, un tercio VFPU
  (9,4 % de las instrucciones). Ahí la CPU hace 33 instrucciones del
  Broadway por instrucción de la PSP (22 en las 2D): cada instrucción VFPU
  son ~120 instrucciones del Broadway; el costo es de instrucciones.
- Reparto de la corrida (base, 106 s): CPU 51,9 s (tarjetas ~36, VFPU
  ~11,7 con 69,9 M instrucciones a ~175 ns, FPU 2,3, intérprete de siempre
  1,1, buscar 0,7); GE 35,0 s (leer vértices 16,0, ensamblar 6,1, comandos
  5,8, enviar 4,9, estado 1,9); texturas 6,0; syscalls 3,3; esperar GX 2,2;
  E/S 2,2.
- `prof_bloques`: 5,1 instrucciones por bloque ejecutado (4,3-5,9 según la
  ventana); 23-35 % de los bloques son de 2. 262 M pasos entre bloques (a
  ~24 ciclos, ~8-9 s de los 36 de tarjetas): **la 2.3 empieza por
  `cpu_enlace`, no por fusiones**. Enlaces: al siguiente 100 %, al destino
  90-96 %; 10,9 M fallos (uno cada 123 instrucciones), el 89 % `jr`/`jalr`
  (retornos), ~0,7 s.
- Intérprete de siempre: 2.426.211 instrucciones, exactamente dos por
  syscall (1.186.131): los stubs de las importaciones (`jr ra` con el
  `syscall` en el delay slot). ~1,1 s; una tarjeta para ese par lo quita.
- Orden que sale de aquí: 0.5.18 tarjetas VFPU (a: lv/sv/mtv/mfv, ~43 % de
  la VFPU, ~4 s); 0.5.19 `cpu_enlace` más la tarjeta de los stubs y quizá
  una pila de retornos; medir leer vértices por partes (`prof_vertices`,
  16 s); histograma de syscalls (`prof_hle`).
- Huellas de las capturas de la corrida "capturas" (cuadros 400 a 4800, de
  400 en 400): 9E8BC705 BCED373F 01FB6F73 59DF52DF 42533F39 8F6D7AF0
  B0BE5CCA 5219B693 0432AB0C 8B0AF11D 1C1E13BE E9392291.

### v0.5.18: tarjetas propias para la VFPU, parte a (`cpu_vfpu_tarjetas`, 2.6)

Antes, cada instrucción VFPU del predecodificado ponía al día el estado
global (pc, reloj, contadores de 64 bits) y llamaba a `vfpu_exec`: contexto
cargado, decodificar, contar, `op_svq` (no en línea), `mem_ptr_r`,
`write_vector` con la tabla `vidx` y la máscara del prefijo D; al volver,
¿parar?, ¿bloque vivo? En el banco de qemu, 164 instrucciones del Broadway
por instrucción VFPU (~120-130 ciclos en el Wii).

Ahora `lv.s`, `sv.s`, `lv.q`, `sv.q`, `mtv` y `mfv` tienen tarjeta propia
(`cpu_vfpu_tarjetas`, encendido):
- Operandos ya preparados: registro base, desplazamiento con signo y el
  índice en `vfpu.vi` (lv.s/sv.s), o el de la primera componente
  (lv.q/sv.q: en una cuádrupla el registro siempre empieza en la fila 0,
  así que las componentes van seguidas en una columna y de 16 en 16 bytes
  en una fila; dos tarjetas, `LVQ`/`SVQ` y `LVQT`/`SVQT`). `mtv`/`mfv`
  con el desplazamiento en bytes de los dos registros.
- Camino rápido: contexto de la VFPU cargado, RAM, dirección alineada (4
  o 16) y, en lv.q, sin máscara del prefijo D pendiente. Si no, la misma
  llamada a `vfpu_exec` de antes (con la instrucción de lo frío), con sus
  fallos, la carga perezosa del contexto, la VRAM y el scratchpad. `mfv`
  a `$zero` (barrera) y `mfvc`/`mtvc` siguen por `vfpu_exec`.
- Copian los 32 bits tal cual (no pasan por `float`). `sv.s`/`sv.q` avisan
  al mapa de código como `sw` y salen si tiraron su bloque. Cuentan en
  `vfpu_stat_now` igual que `vfpu_exec`: `[STATS]` no cambia.
- `cpu_vfpu_verificar` (apagado, para el Wii): esas instrucciones van por
  una tarjeta que hace la cuenta con la tarjeta, la deshace, la repite con
  `vfpu_exec` y compara registros, VFPU entera y memoria; si algo difiere,
  `[PREDEC] tarjeta VFPU distinta de vfpu_exec en PC (instrucción)` (16
  como mucho) y la línea `[TIEMPOS] cpu_vfpu_verificar: N tarjetas ...`.
- Con `prof_cpu`, "CPU por dentro" dice cuántas fueron por tarjeta propia;
  las que van por `vfpu_exec` siguen contando como VFPU.

Medido: banco de qemu con un programa nuevo de la VFPU
(`BANCO=vfpu_banco.c tools/banco/medir_cpu.sh`): de 164 a 45 instrucciones
del Broadway por instrucción VFPU (la primera versión, con el bucle de 4
componentes y la tabla de índices, daba 52: el bucle usaba el CTR del
salto a la siguiente tarjeta). En el PC el mismo programa pasa de 142 a
391 MIPS. `cpu_banco` sigue en 17,3; cachegrind sin cambios (no tiene
VFPU). El bucle unido crece de 12,0 a 14,4 KB.

Verificación: test_interp con lv.q/sv.q (alineadas y, en los programas
con fallos, desalineadas), mtv/mfv/mfvc/mtvc (también a `$zero`), prefijos
D con máscara, `sv.s`/`sv.q` sobre el propio código y el contexto de la
VFPU sin cargar en uno de cada cinco programas; cada programa con
tarjetas, sin ellas y con `cpu_vfpu_verificar` (202.860 verificadas,
ninguna distinta); se comparan también los recuentos de la VFPU y
`vfpu_live`. Casos dirigidos: `sv.q` y `sv.s` que reescriben su propio
bloque, máscara del prefijo D (cae a `vfpu_exec`) y contexto sin cargar.
Mutaciones detectadas (13 de 15; las otras dos son equivalentes): sin
mirar la máscara, sin avisar al mapa de código (sv.q y sv.s), índice de
lv.s sin `VFPU_INDEX`, bit alto de lv.q mal, alineación de 4 en vez de
16, `mfv` a `$zero` con tarjeta, recuento en otra casilla, sin mirar el
contexto cargado, filas con paso de columna, columna y fila cambiadas,
paso de sv.q cambiado, cambiar `cpu_vfpu_tarjetas` sin vaciar; la
verificación misma ve los índices mal (decenas de miles de diferencias).
Huellas de test_vdec sin cambios (x86 4D4A1D8B, PowerPC 0657B177,
devkitPPC B3A2BCD3); `build_tests_dkp.sh` sin fallos; pspautotests
275/275; A/B de `cpu_vfpu_tarjetas` en las 572 (solo `malloc` y savedata
en paralelo; de una en una, iguales); `cpu_vfpu_verificar` en las 572:
ninguna diferencia (26 usan la VFPU); tests/guest 22/22; recarga con ASan.

Autotest (DBZ, todas con `prof_pmc`, `prof_tirones`, `prof_ventana_juego`
y `ventana 600`, sin capturas salvo la última): sin (`cpu_vfpu_tarjetas`
0), con (1), cpu (1 y `prof_cpu`: cuántas van por tarjeta), verif (1 y
`cpu_vfpu_verificar`) y capturas (1 con `capturas 400`: tienen que dar las
huellas de la 0.5.17.2 de arriba).

### Resultado de la 0.5.18 en el Wii (DBZ, sin / con / cpu / verif / capturas)

Las cinco corridas ejecutan lo mismo, y lo mismo que la 0.5.17.2 (mismas
`[LLAMADA]`, `[MANDO]`, `[IO]` y `[STATS]`). verif: 30.166.472 tarjetas de
la VFPU comparadas con `vfpu_exec` en el Wii, todas iguales. Falta
comparar las huellas de las capturas con las de la 0.5.17.2 (pedir el
`.capturas.txt`).

| Corrida | Real | Velocidad | Ciclos / instr. PSP | Instr. Broadway / instr. PSP | CPU |
|---|---|---|---|---|---|
| sin (`cpu_vfpu_tarjetas` 0) | 106,3 s | 79,2 % | 28,16 | 26,28 | 51,8 s |
| con | 103,9 s | 81,0 % | 26,67 | 24,32 | 49,2 s |
| con + `prof_cpu` | 105,9 s | 79,5 % | 27,91 | 25,71 | 51,3 s |
| verif | 193,0 s | 43,6 % | 75,15 | 90,19 | 138,3 s |
| capturas | 106,2 s | 79,3 % | 26,59 | 24,32 | 48,9 s |

- −2,4 s en la corrida (−2,3 %), −5,3 % de ciclos por instrucción; en la
  escena pesada (60-84 s) −7,8 % de ciclos (34,7 → 32,0) y de 36,5 a
  37,6 % de velocidad. "sin" calca la base de la 0.5.17.2 (106,2 s, 28,20).
- Se estimaban ~4 s y fueron ~2,5: lv/sv/mtv/mfv eran más baratas que la
  media de la VFPU (la media de 178 ns incluía la aritmética). En la escena
  pesada van por tarjeta 8,6 M de las 20,2 M instrucciones VFPU de cada
  ventana (42 %); las 11,6 M que siguen por `vfpu_exec` cuestan ~225-233 ns
  cada una (~165 ciclos): ~2,65 s de cada ventana de 26,6 s (10 %).
- Lo que queda de la VFPU ([STATS]): conversiones 7,6 %, vmin/vmax 5,5,
  vmul 5,2, vpfx 4,4, vf2i 4,0, vi2f 3,9, vmov 3,8, vsub 2,8, vtfm 2,3,
  vscl 1,9, vadd 1,8 (4,4 % con prefijos). `vfpu_exec` ya tiene caminos
  rápidos en C sin prefijos (`vecdo3_fast`...): la parte b es llevarlos a
  tarjetas sin la llamada.

Las 12 huellas de las capturas de la 0.5.18 son las de la 0.5.17.2.

### v0.5.18.1: tarjetas VFPU, parte b (`cpu_vfpu_arit`)

`vadd`, `vsub`, `vmul` (cualquier tamaño), `vmov`, `vabs`, `vneg`,
`vmin`, `vmax`, `vscl`, `vi2f`, `vf2i` (los cuatro redondeos) y `vpfx`
(S, T y D) con tarjeta propia (`cpu_vfpu_arit`, encendido; solo con
`cpu_vfpu_tarjetas`). La tarjeta lleva la instrucción entera y los índices
salen de la tabla `vfpu_vidx` (en pares y tríos la fila puede dar la
vuelta, así que no hay un paso fijo como en lv.q).
- Solo sin prefijos pendientes (S y T 0xE4, D 0) y con el contexto
  cargado, como los caminos rápidos de `vfpu_exec` (que en ese caso no
  consumen prefijos); si no, `vfpu_exec`. `vpfx` solo escribe su registro
  de control.
- La cuenta es la misma en los dos sitios: `flush`, `canon`, la elección
  de vmin/vmax por componente y la conversión de vf2i pasaron a `vfpu.h`
  (`vfpu_flush`, `vfpu_canon`, `vfpu_minmax_bits`, `vfpu_f2i_lane`) y las
  usan `vfpu.c` y las tarjetas. vi2f y vf2i no tenían camino rápido: con
  prefijos neutros el general no hace nada más que la cuenta.
- Fuera, a propósito: `vdot`, `vtfm`, `vhdp`, `vmmul`... GCC une `a * b +
  c` en `fmadds` en el PowerPC (redondea una vez) y podría hacerlo en un
  sitio y no en el otro. vdiv (raro) y las conversiones de enteros cortos
  (`vs2i`, `vi2s`...) también quedan para después.

Medido (banco de qemu, `BANCO=vfpu_arit_banco.c`): de 263 a 150
instrucciones del Broadway por instrucción VFPU (la cuenta misma pesa:
denormales, NaN canónico, el `double` de vf2i). `vfpu_banco` sigue en 45,
`cpu_banco` en 17,3; cachegrind igual. El bucle unido crece de 14,4 a 18,9
KB (solo se cargan en la caché las líneas de las tarjetas que se usan).

Verificación: test_interp con esas instrucciones en los programas
aleatorios (y prefijos S, T y D con datos de 20 bits), cada programa con y
sin `cpu_vfpu_arit` y con `cpu_vfpu_verificar` (507.504 verificadas,
ninguna distinta); caso dirigido con las doce, un `vpfxs` pendiente (la
siguiente cae a `vfpu_exec`) y el recuento. 12 mutaciones detectadas (sin
flush, sin canon, sin mirar el prefijo D, modo de vf2i, factor de vi2f,
min y max cambiados, vpfxd con 20 bits, índice de vscl, vmov y vabs
cambiados, recuento en otra casilla, vmax con otro código, cambiar el
interruptor sin vaciar). Huellas de test_vdec sin cambios;
`build_tests_dkp.sh` sin fallos; pspautotests 275/275; A/B de
`cpu_vfpu_arit` en las 572 (solo `gpu/complex`, `malloc` y savedata en
paralelo; de una en una, iguales); `cpu_vfpu_verificar` en las 572 sin
diferencias; tests/guest 22/22; recarga con ASan: en una corrida
`cpu/fpu/fpu` dio 304 bytes de más en la tercera vuelta; sola, 5 de 5
bien (como `guardband` y `dxt5` en la 0.5.17.2: intermitente).

Autotest (como mucho 4 registros y un `.capturas.txt`, pedido del
usuario): sin (`cpu_vfpu_arit` 0), con, verif (`cpu_vfpu_verificar`) y
capturas (con `capturas 400` y `prof_cpu`, para contar las tarjetas);
todas con `prof_pmc`, `prof_tirones` y ventanas de 600 vblanks.

### Resultado de la 0.5.18.1 en el Wii (DBZ, sin / con / verif / capturas)

Las cuatro corridas ejecutan lo mismo que las anteriores; verif:
51.172.850 tarjetas de la VFPU comparadas con `vfpu_exec`, todas iguales;
las 12 huellas de las capturas, las de siempre.

| Corrida | Real | Velocidad | Ciclos / instr. PSP | Instr. Broadway / instr. PSP | CPU |
|---|---|---|---|---|---|
| sin (`cpu_vfpu_arit` 0) | 104,3 s | 80,7 % | 27,09 | 24,69 | 49,9 s |
| con | 102,7 s | 82,0 % | 26,21 | 23,66 | 48,2 s |
| verif | 255,3 s | 33,0 % | 109,04 | 135,59 | 200,9 s |
| capturas (+ `prof_cpu`) | 106,3 s | 79,2 % | 26,76 | 24,90 | 49,2 s |

- −1,6 s frente a "sin" (−3,2 % de ciclos); escena pesada de 32,4 a 31,0
  ciclos, de 37,5 a 38,1 %. Desde la 0.5.17.2 (106,2 s) van −3,5 s.
- "sin" (lo mismo que "con" de la 0.5.18) sale 0,4 ciclos peor que en la
  0.5.18 (27,09 frente a 26,67; escena pesada 32,4 frente a 32,0): o es
  ruido o el bucle más grande (18,9 KB) cuesta algo en la caché de
  instrucciones. Vigilarlo.
- VFPU por `vfpu_exec`: de 39,7 M instrucciones (9,0 s) en la 0.5.18 a
  18,7 M (4,4 s); en la escena pesada va por tarjeta el 69 % (14,0 de 20,2
  M por ventana) y lo que queda cuesta ~250 ns por instrucción (vtfm,
  vdot, conversiones, con prefijos...).
- En la compilación del Wii (devkitPPC, `-ffp-contract=fast` por defecto)
  no hay ni una `fmadds` en `vfpu.o` ni en `predec.o`: llevar vdot, vtfm,
  vhdp, vmmul a tarjetas es posible, compilando los dos con
  `-ffp-contract=off` para que siga así.
- Paired singles (parte c): poca ganancia esperada (lo caro de las tarjetas
  aritméticas son las cargas por la tabla de índices, `flush` y `canon` por
  componente, no la operación), solo se verifica en el Wii; se aparca.

### v0.5.19: enlace directo entre bloques (`cpu_enlace`) y syscall en el delay slot (`cpu_syscall_slot`)

Diseño: lo propuso otra conversación (con un prototipo y `test_enlace.c`)
a partir de esta rama; aquí se comprobó (su prototipo pasaba la suite y
bajaba el banco de 17,3 a 15,4) y se adaptó: dos interruptores, sin
comentarios salvo `//Arreglo`, `cpu_verificar` en el cierre nuevo y más
pruebas.

- `cpu_enlace` (encendido): al construir, un bloque que acaba en un salto
  con destino fijo (b*, j, jal, bc1, bv) lleva el cierre `BENDD`; con
  `jr`/`jalr`, el `BEND` de siempre. La tarjeta del salto elige el sucesor
  (`nbx = succ[1]` si salta, `succ[0]` si no; con `if`, no con índice, para
  que no se calcule el booleano antes del `mtctr`). `BENDD` lee del elegido
  `h`, `n` y `alive` (la misma línea de 32 bytes) y entra si está vivo y
  cabe en el reloj; si no, por `link` como antes. Un enlace sin hacer
  apunta a un bloque nulo (alive 0; una unión con sitio para `c[0]`). Si
  el delay slot es un nop, su tarjeta hace de cierre. Sin listas de
  enlaces de entrada: `alive` sigue siendo la red (cuesta casi nada).
  En devkitPPC el cierre son ~14 instrucciones con una carga dependiente
  (antes 26 y ~7 eslabones). Las tarjetas de salto cargan el sucesor
  siempre, también con el interruptor apagado (sin copias nuevas del
  bucle: MEM1 tiene 84 KB libres).
- `cpu_syscall_slot` (encendido): un `syscall` en el delay slot ya no corta
  el bloque (SYNC_AT ya pone pc = destino del salto, como el intérprete):
  los stubs `jr ra` + `syscall` (dos instrucciones por llamada al HLE,
  2,4 M por corrida de DBZ) dejan de ir por el intérprete de siempre.
- `cpu_verificar`: el cierre nuevo comprueba el bloque al que va (si está
  viejo, lo tira y sigue por la búsqueda); antes los enlaces se apagaban al
  verificar y la verificación del Wii no pasaba por ellos.

Medido (banco de qemu, `medir_cpu.sh`): 17,3 (0.5.18.1) → 17,5 con los dos
apagados → 15,4 con `cpu_enlace` (−11 %); `cpu_syscall_slot` no cambia el
banco (no llama al HLE). En `guion_eco` (tests/guest) lo que va por el
intérprete de siempre pasa de 2402 instrucciones a 0.

Verificación: `test_enlace` (3000 programas, cada uno con y sin
`cpu_enlace`, arenas normales y diminutas, con y sin `predec_compacto` y a
veces con `cpu_verificar`); test_interp con saltos con `syscall` en el
delay slot, las dos opciones en las combinaciones y `cpu_predec_enlaces_ok`
(cada sucesor guardado de un cierre directo es el bloque nulo o un bloque
de la arena con el pc que toca); caso dirigido de un enlace a un bloque
cambiado sin aviso con `cpu_verificar`. 165.340 comprobaciones, 0 fallos
(x86 con ASan, PowerPC y devkitPPC; huellas de test_vdec sin cambios). 11
mutaciones, todas detectadas (`BENDD` sin `alive`, J sin `nbx`, sucesores
cambiados, likely tomado sin `nbx`, SYNC_AT del delay slot con pc = end,
sin la comprobación de `cpu_verificar`, cualquier delay slot como cierre,
enlace guardado en el otro sucesor, `syscall` sin `llbit = 0`, `jr` con
`BENDD`, bv sin `nbx`). pspautotests 275/275; A/B de `cpu_enlace` y de
`cpu_syscall_slot` en las 572: solo `malloc`, savedata e
`io/file/file`+`utility/savedata/idlist` (esta pareja también cambia con
`prof_es`, que no toca la emulación: es de las pruebas); `cpu_verificar`
en las 572: ningún bloque viejo; tests/guest 22/22. Recarga con ASan: 2-3
pruebas por corrida con 304 bytes de más en la tercera vuelta (otras cada
vez); la 0.5.18.1 también lo hace (0-3 por corrida con las mismas tres
pruebas en paralelo): es de antes y depende de los tiempos; pendiente de
investigar.

Autotest (4 registros y un `.capturas.txt`): sin (los dos a 0), enlace
(solo `cpu_enlace`), con (los dos) y verif (los dos, `cpu_verificar`,
`prof_bloques` y `capturas 400`).

### Resultado de la 0.5.19 en el Wii (DBZ, sin / enlace / con / verif)

Las cuatro corridas ejecutan lo mismo que siempre; verif: ningún bloque
viejo (con el cierre nuevo comprobado); las 12 huellas de las capturas,
las de siempre.

| Corrida | Real | Velocidad | Ciclos / instr. PSP | Instr. Broadway / instr. PSP | CPU |
|---|---|---|---|---|---|
| sin (los dos a 0) | 102,5 s | 82,1 % | 26,17 | 23,60 | 48,1 s |
| enlace | 98,7 s | 85,3 % | 24,12 | 22,18 | 44,4 s |
| con | 99,1 s | 84,9 % | 23,84 | 21,90 | 43,8 s |
| verif (+ `prof_bloques`, capturas) | 143,7 s | 58,6 % | 46,78 | 46,28 | 86,1 s |

- `cpu_enlace`: −3,8 s (−7,8 % de ciclos); escena pesada de 31,0 a 29,2
  ciclos; menús y 2D de 253 a 267 % y de 84 a 94 % (30-40 s).
- `cpu_syscall_slot`: −0,6 s de CPU y lo que va por el intérprete de
  siempre baja de 2,43 M instrucciones a 54.000 (quedan "no cabe en el
  reloj"). El total sale 0,4 s peor, pero no por la CPU: las texturas
  hicieron lo mismo con menos IPC (0,74 frente a 0,95) y el montón estaba
  repartido de otra forma (MEM2 libre 12,9 frente a 14,3 MB). Las corridas
  van seguidas en el mismo arranque y los búferes pueden caer en otra
  memoria: comparar la cubeta CPU y los ciclos, no solo el total (y tenerlo
  en cuenta para el mapa fijo de MEM1, 2.9). MEM1 libre: 48 KB.
- Desde la 0.5.17.2 (106,2 s): −7 s; velocidad de 79 a 85 %.
- Los aciertos de los enlaces de la corrida verif no valen: con
  `cpu_verificar` los cierres `BEND`/`END` no usan el enlace (cuentan como
  fallos). Para medir la pila de retornos, `prof_bloques` sin
  `cpu_verificar`.

### v0.5.19.1: sin `bdnz` en el bucle y dos medidas (pila de retornos, `prof_vertices`)

- Optimización (sin interruptor posible): las cuatro copias del bucle con
  `optimize("no-branch-count-reg")`. En devkitPPC, de 25 `bdnz` a 0 en
  `run_unido` y de 185 a 169 `mtctr`; las tarjetas siguen en línea. Banco
  de qemu (instrucciones, no ciclos): `cpu_banco` 15,4 (igual); VFPU de
  carga y guardado 45 → 44; aritmética de la VFPU 152 → 160 (los bucles por
  componente cambian `bdnz` por suma, comparación y salto). En el Wii el
  CTR es serializante: lo que cuenta son los ciclos, compararlos con la
  corrida "con" de la 0.5.19 (23,84 ciclos por instrucción).
- Medida: pila de retornos simulada con `prof_bloques` (solo en la copia de
  medida del bucle): cuántos `jr ra` acertaría frente al enlace de hoy, y
  en los stubs `jr ra` + `syscall`.
- Medida: `prof_vertices` (apagado): en el camino por lotes (el de DBZ),
  "leer vertices" incluye decodificar, los huesos y `read_vertex_body`
  (transformar, pasar a pantalla, luces, uv, niebla): por eso "transformar"
  salía casi a 0 %. El muestreo separa esas partes. Con el interruptor
  apagado cuesta +0,25-0,5 % de instrucciones por vértice en el banco del
  GE (2767 → 2781 sin huesos, 3263 → 3271 con huesos): cuatro marcas por
  vértice, con el interruptor leído una vez por tanda (duplicar
  `batch_range`, 70 KB, no cabe en MEM1).

Verificación: test, test-ppc y devkitPPC 165.407 comprobaciones sin
fallos, huellas de test_vdec sin cambios; pspautotests 275/275; A/B de
`prof_vertices` y `prof_bloques` (con matemática rápida) en las 572: solo
savedata, intermitente también con `prof_es`; tests/guest 22/22; recarga
con ASan: 1 con los +304 bytes de siempre.

Autotest (4 registros y un `.capturas.txt`): con (todo por defecto, para
comparar con "con" de la 0.5.19), bloques (`prof_bloques`, sin
`cpu_verificar`), vertices (`prof_vertices`) y capturas.

### Resultado de la 0.5.19.1 en el Wii (DBZ, con / bloques / vertices / capturas)

Las cuatro ejecutan lo mismo que siempre (la primera del arranque da su
huella de siempre, 620a7fd8; las demás af4c4503); las 12 huellas de las
capturas, las de siempre.

| Corrida | Real | Velocidad | Ciclos / instr. PSP | Instr. Broadway / instr. PSP | CPU |
|---|---|---|---|---|---|
| con | 98,9 s | 85,1 % | 23,84 | 21,87 | 43,9 s |
| bloques (`prof_bloques`) | 117,4 s | 71,7 % | 33,84 | 33,56 | 62,4 s |
| vertices (`prof_vertices`) | 99,2 s | 84,9 % | 23,85 | 21,87 | 44,0 s |
| capturas | 101,8 s | 82,7 % | 23,87 | 21,89 | 43,9 s |

- Sin `bdnz`: neutro. 23,84 ciclos igual que la 0.5.19 y lo mismo ventana
  por ventana (±0,1); el CTR ya no se comparte, pero no se notaba. Se
  queda (no empeora y deja el CTR solo para el salto a la tarjeta).
- Pila de retornos simulada: 27,5 M `jr ra`; el enlace de hoy acierta el
  67,8 % (64,8-79 % según la ventana) y la pila el 99,7 %; la pila
  acertaría 8,82 M de los 8,84 M que falla el enlace. Cada fallo cuesta
  una búsqueda (53-81 ns medidos con `prof_cpu` en la 0.5.18.1): unos
  0,5-0,7 s en toda la corrida, 0,1 s (0,4 %) por ventana de la pelea, menos
  lo que cueste apilar en cada `jal`. Los stubs `jr ra` + `syscall`: 100 %.
  Ganancia pequeña: no es lo siguiente.
- Intérprete de siempre: 2-14 mil instrucciones por ventana (casi todo "no
  cabe en el reloj"); en 2400-3000, 5.460 `beql` sin tarjeta.
- Leer vértices por dentro, en la pelea (3600-fin, % del tiempo total):
  preparar 0,8, decodificar 3,0, huesos 3,9, transformar 7,1, pantalla
  11,2, luces uv y niebla 0,8, genérico 0,3 (27,1 en total). El reparto
  entre transformar y pantalla no es exacto (el compilador mueve cuentas a
  través de la marca y la división por w llega tarde), pero juntas son el
  18 % de la pelea, unos 4,7 s por ventana de 26 s. PMC del GE: leer 511
  ciclos por vértice, IPC 0,69 (unas 350 instrucciones) y L1D 1,4 por
  1000 instrucciones: no es la memoria, son las cuentas.
- Banco del GE (`bench_gx`, 0x11A uv16 c4444 p16, el 54 % de la pelea) con
  `qlines.py`: `batch_range` 346 instrucciones por vértice (lo mismo que el
  Wii). Pasar a pantalla, unas 180: `round_to_screen` 35, tres
  `ge_floorf` 48, `floor_to_int` 20, `ge_f2i` de z 19, `offset_float` 15
  (mira la clave en cada vértice), `ge_clip_flags_xy` 30 y 15 más. La
  matriz (`ge_dot3_fused`, la recarga en cada vértice) 62; decodificar
  ~40; uv y color 27; el resto del bucle ~30.

### v0.5.20: pasar a pantalla más barato en el camino por lotes (`ge_pantalla_rapida`, 1.2c)

Lo que mostró la 0.5.19.1: "pantalla" y "transformar" son el 18 % de la
pelea; en el banco del GE, de las 346 instrucciones por vértice de 0x11A
unas 180 eran pasar a pantalla (tres suelos con dos comprobaciones de
rango cada uno, tres conversiones a entero, la clave del offset en cada
vértice, las banderas mirando un rango que ya se sabía).

- `ge_pantalla_rapida` (encendido), solo en `batch_range` (el camino de
  uno en uno, `read_vertex`, no cambia: es la referencia de `test_vdec` y
  de `ge_verificar`). Si x e y están en [0, 4096 − 1/32) y z en
  [0, 65536), nada sale fuera ni se acota en ninguno de los dos modos de
  profundidad; para un valor ≥ 0 truncar es el suelo, así que cada
  coordenada lleva una conversión (`fctiwz` + `stfiwx` + `lwz`) y el suelo
  en float con el truco de 1,5 * 2^23 sin comprobar más; z en float es su
  suelo (también para 0: nunca −0); x e y siempre caben en 21 bits
  (`GE_CF_SMALL`). El offset en float se calcula una vez por tanda. Fuera
  de ese rango (o NaN), `round_to_screen` como siempre.
- Banco del GE (`batch_range` por vértice): 0x11A de 346,7 a 252,2;
  0x4301 de 414,9 a 324,5; 0xC301 de 473,8 a 383,2; 0x1C301 de 593,4 a
  502,7. Con el interruptor a 0, ~+5 (la comprobación de más). El código de
  `batch_range` crece 14 KB (DOL +15 KB): el montón toma 15 KB menos de
  MEM1 y los toma de MEM2 (17 MB libres).
- Probado y descartado: la matriz y la escala de pantalla copiadas a una
  estructura local para que no se recarguen en cada vértice: 249,9 frente
  a 252,2 y peor con huesos (el compilador las vuelve a cargar por falta de
  registros; además no se podría apagar con un interruptor).
- Arreglo de medida: con `prof_bloques`, los bloques de 65 instrucciones
  (salto en la 63 + delay slot) se contaban en `bq_tam[7]`, fuera de la
  tabla, y sumaban en `bq_enlace[0][0]` ("al siguiente por el enlace"):
  ahora van al último grupo, "33-65". Lo vio UBSan en `make test`.

Verificación: test, test-ppc y devkitPPC 165.478 comprobaciones sin
fallos, huellas de test_vdec sin cambios (4D4A1D8B, 0657B177, B3A2BCD3).
`test_vdec` corre además cada caso por lotes sin el interruptor, y una
prueba de los bordes (x, y alrededor de 0 y de 4096, z de 0 y de 65536,
−0, NaN, infinitos, w negativa y de varias potencias de 2, offsets de 0 a
0xFFFF; ~23.000 vértices por el camino nuevo y ~22.000 por el de siempre)
con `ge_verificar`. Mutaciones del camino nuevo: 11 de 12 detectadas (la
que no, `w >= 0` en las banderas, no puede pasar: con w = 0 x sale
infinita o NaN). pspautotests 275/275; A/B de `ge_pantalla_rapida` con
matemática rápida en las 572: solo `malloc/malloc` y
`utility/savedata/deleteemptyfilename`, que salen iguales o distintas
según la corrida (también con `prof_es`); tests/guest 22/22; recarga con
ASan: 572 pruebas, 0 con problemas (esta vez sin los +304 bytes).

Autotest (4 registros y un `.capturas.txt`): sin (`ge_pantalla_rapida`
0), con (todo por defecto), verif (`ge_verificar`: cada tanda también uno
a uno, comparado campo a campo; tiene que dar 0 distintos) y capturas
(con `prof_vertices`, para ver el reparto nuevo de leer vértices).

### Resultado de la 0.5.20 en el Wii (DBZ: sin / con / verif / capturas)

- Capturas: las 12 iguales a las de la 0.5.17.2 a la 0.5.19.1.
- `ge_verificar`: 22,17 millones de vértices, 20,0 millones de descartes,
  9,15 millones de triángulos enviados a GX y 11.414 emisiones de estado
  comparados: 0 distintos.
- Peleas (las tres últimas ventanas): del 38,6 / 39,9 / 37,6 % al
  40,2 / 41,2 / 38,9 % (+3,3 a +4,1 %); la escena de 99,1 a 96,8 s
  reales. Leer vértices de 516-522 a 418-435 ciclos por vértice (−87 a
  −98, lo que daba el banco) y del 26,8 % al 22,3 % del tiempo.
- `prof_vertices` (ciclos por vértice, 0.5.19.1 → 0.5.20): pantalla
  206-211 → 145-155, transformar 128-134 → 106-111 (el suelo de z ya no
  está ahí), huesos 61-89, decodificar 47-67, sin cambios. Pantalla sigue
  siendo la fase más grande (~90 instrucciones con IPC ~0,6: tres
  `fctiwz` + `stfiwx` + `lwz` y diez `fcmpu` con su salto).
- verif va a la mitad en las peleas (18,9-19,9 %, 162,9 s la escena):
  es lo que cuesta comprobar. Las mismas instrucciones de la PSP y los
  mismos segundos de CPU (8,5 s por ventana); el GE pasa de 12,6 a
  40,5 s: leer 418 → 1292 ciclos por vértice (cada vértice dos veces y
  comparado), ensamblar 208 → 664 por triángulo, enviar 352 → 1036 por
  primitiva y estado 720 → 7876 por llamada (con `ge_verificar` el estado
  de GX se emite siempre y se compara con libogc). Las ventanas de menús
  no cambian (allí no hay vértices por lotes).
- El peor tirón (771 ms a los 60,2 s de juego, 604 ms de CPU) es el
  mismo en las cuatro: no tiene que ver.

### v0.5.21: censo de cajas (`prof_bbox`, para 1.3) y comandos leídos con un puntero (`ge_cmd_puntero`)

Paso 10 de la hoja de ruta, empezando por 1.3 `ge_bbox`. Lo que había: el
GE **ya emula BBOX y BJUMP** (la prueba aproximada de
`DrawEngineCommon::TestBoundingBox` de PPSSPP, `test_bounding_box` en
`ge_vertex.c`): si el juego manda la caja de un modelo y cae fuera, el
BJUMP se salta sus llamadas, como en la PSP. Lo que se puede ganar con las
cajas del juego depende de cuántas mande DBZ, de lo que cueste probarlas y
de cuántos modelos pasan la caja pero no dibujan nada; eso solo se puede
medir en el Wii con DBZ. Antes de programar `ge_bbox`, el censo:

- `prof_bbox` (apagado, solo mide). Tres líneas `[TIEMPOS] Censo de
  cajas`:
  1. BBOX: cuántos, vértices de caja probados, milisegundos de la prueba,
     cuántos con huesos en el tipo de vértice, visibles, fuera, con 0
     vértices (fuera sin probar) y los que pasan sin probar (más de 1024
     vértices, la caja sale del espacio de 4096 o memoria no válida).
     BJUMP: cuántos (y sin BBOX antes), tomados, y las llamadas y vértices
     del tramo que se saltan (recorrido lineal del tramo, contando PRIM y el
     tipo de vértice; los tramos con saltos o llamadas dentro y los raros
     aparte).
  2. Objetos que pasan la caja (de un BJUMP no tomado a su destino, con la
     misma lista y profundidad de llamadas o más): llamadas, vértices y
     milisegundos; cuántos **no dibujan nada** (ni un triángulo ni un
     sprite), de ellos enteros fuera de pantalla y con huesos. Los vacíos
     (sin llamadas) aparte.
  3. Llamadas fuera de objetos: cuántas, vértices y milisegundos; cuántas
     no dibujan nada (con huesos aparte) y cuántas están enteras fuera de
     pantalla. Es el techo de cualquier forma de saltarse llamadas enteras
     (cajas propias), para comparar.
  El tiempo es el de `ge_draw_prim` de cada llamada (dos lecturas del reloj
  por llamada con el censo encendido).
- Qué decidir con el censo (en la pelea): si DBZ no manda BBOX (o casi),
  `ge_bbox` con las cajas del juego no sirve para DBZ: pasar a 1.4. Si
  manda muchas y la prueba cuesta (ms de BBOX), hacerla más barata e
  idéntica (la matriz combinada de `ge_mtx_cache`, decodificar solo la
  posición). Si hay muchos objetos sin huesos que pasan la caja y no
  dibujan nada, con muchos vértices, eso es lo que una prueba más fina
  podría saltarse (con verificación: dibujar igual y comprobar que no
  sale nada). Con huesos no: los vértices pueden salirse de la caja.

Para que la vuelta al Wii también gane velocidad, una optimización de las
medidas ("leer los comandos con un puntero", ~56 de las ~80-100
instrucciones por comando):

- `ge_cmd_puntero` (encendido), en `ge.c`: el bucle de comandos de una
  lista (`run_commands_ptr`) lee cada comando de la RAM con un puntero en
  vez de `mem_valid` + `mem_read32` (fuera de la RAM, el camino de siempre,
  con los ganchos de la VRAM), y los 229 comandos sin `case` propio en
  `execute` (los de estado: solo guardan su valor y marcan `ge_sucio`) se
  aplican en el propio bucle, sin la llamada, el prólogo de `execute` (7
  registros) ni la tabla de saltos (`mtctr`). Los 27 con `case` (y todos con
  `prof_estado`) van por `execute`. La tabla `cmd_con_caso` está junto a
  `execute`: un `case` nuevo tiene que ir también a la tabla
  (`test_listas` comprueba que hay 27).
- Con `ge_verificar`: cada comando sin `case` pasa por `execute` y se
  compara con lo que habría hecho el bucle (valor, `ge_sucio`, pc,
  direcciones, offset, CLUT, cuentas, estado de la lista y ciclos del GE), y
  lo leído con el puntero con `mem_read32`. Línea `[TIEMPOS] Verificacion
  de los comandos`; tiene que dar 0 distintos.
- Banco de qemu (`bench_lista`, que ahora usa el mismo bucle que
  `process_dl_queue_body`), instrucciones por llamada de leer y ejecutar
  los comandos: escena 1 (14 comandos, textura con paleta) de 1469 a 679
  (−54 %); escena 0 (4 comandos) 399 → 264; escena 2 (4 huesos y mundo, 81
  comandos) 9392 → 7107; escena 3 (8 huesos) 15856 → 12375 (los datos de
  matrices siguen por `execute`). Leer un comando baja de ~52 a ~18
  instrucciones. Estimación para la pelea (873 llamadas por vblank, 14,5
  comandos cada una): ~2 % del Broadway. DOL +7,5 KB.

Verificación: test, test-ppc y devkitPPC 165.657 comprobaciones sin
fallos; huellas de test_vdec sin cambios (4D4A1D8B, 0657B177, B3A2BCD3);
`gevert.sh` igual que la 0.5.20 (252,2 / 324,5 / 383,2 / 502,7 / 268,3).
`tests/test_listas.c`: 600 listas al azar en la RAM (también pegadas a su
final), la VRAM con sus espejos, la scratchpad y alias sin caché o de
kernel, con JUMP, CALL/RET, BJUMP (tramos saltados con FINISH y SIGNAL
dentro), ORIGIN y OFFSETADDR entre zonas: 69.615 comandos, mismo estado
final con y sin el interruptor; con `ge_verificar`, 17.838 comandos
comparados, 0 distintos; cada uno de los 229 comandos sin `case`, 8 veces.
Mutaciones detectadas: sin el camino lento fuera de la RAM, pc fijo tras un
salto, sin mirar el stall, sin `ge_sucio`, `TEXFLUSH` u `ORIGIN` fuera de
la tabla (este también lo ve `ge_verificar`: 770 distintos).
`tests/test_bbox.c`: cada cuenta del censo en una lista con objetos que
dibujan, saltados, que pasan sin dibujar (con y sin huesos), vacíos y
llamadas sin caja. pspautotests 275/275; A/B en las 572 de
`ge_cmd_puntero` (exacto y matemática rápida) y de `prof_bbox`: solo
`io/file/rename`, `io/io/io`, `io/stat/stat`, `malloc/malloc` y savedata,
que salen distintas también en un A/A (un interruptor que no hace nada) y
no usan el GE; A/B con el renderizador de GX bajo qemu
(`build_cli_gx.sh`) en las 110 de `gpu/`: misma huella del estado de GX
(solo `gpu/complex` cambia de huellas, como siempre); con `ge_verificar`
en las 110 de `gpu/`: 206.675 comandos comparados, 0 distintos;
tests/guest 22/22;
recarga con ASan: 572, solo `io/file/rename` (sola, bien: choca con los
archivos que deja la propia prueba).

Autotest (4 registros y un `.capturas.txt`; todas con `prof_pmc`,
`prof_tirones`, `prof_ventana_juego` y `ventana 600`): sin
(`ge_cmd_puntero` 0), con (todo por defecto), verif (`ge_verificar`: 0
distintos en vértices, estado de GX y comandos) y censo (`prof_bbox` con
`capturas 400`: las 12 huellas tienen que ser las de siempre).

### Resultado de la 0.5.21 en el Wii (DBZ: sin / con / verif / censo)

- Las 12 capturas de `censo`, las de siempre (9E8BC705 ... E9392291); las
  cuatro corridas ejecutan las mismas 1343,1 M instrucciones de la PSP.
- `ge_verificar`: 0 distintos en todo (vértices 9,6 M, descartes 8,6 M,
  estado de GX 322.636 emisiones, envío a GX 4 M triángulos y 938.494
  comandos por el camino nuevo en la pelea).
- `ge_cmd_puntero`: comandos de 2791 a 2609 ciclos por llamada (−6,5 %), de
  207 a 194 por comando; peleas de 40,5 / 41,6 % a 40,7 / 41,8 %; la escena
  de 96,5 a 96,2 s. Mucho menos de lo estimado (~2 %): de los 7,05 M
  comandos de la pelea solo 938 mil (13 %) son de los sin `case`; el 87 %
  sigue por `execute` (por las cuentas, sobre todo datos de matrices).
- Censo de cajas, en la pelea (por ventana de 600 vblanks):
  - DBZ manda ~108.000 BBOX (~180 por vblank), de 8 vértices, ninguna con
    huesos ni que pase sin probar; el 76-81 % caen fuera y el BJUMP salta
    331-379 mil llamadas y 5,2-6,4 M vértices (más que lo que se dibuja).
  - Probarlas cuesta 615-624 ms por ventana, ~2,5 % de la pelea: ~5,7 µs
    (~4150 ciclos) por caja de 8 vértices. Son ~860 de los 2609 ciclos de
    "comandos" de cada llamada.
  - Objetos que pasan la caja y no dibujan nada: 2.373-3.164 por ventana,
    todos enteros fuera de pantalla y ninguno con huesos: 17-49 mil
    llamadas, 0,4-1,05 M vértices, 290-832 ms (1,2-3,4 %).
  - Llamadas fuera de objetos (los personajes, con huesos) que no dibujan
    nada: <1 %. Cajas propias no compensan.
- Fallo latente encontrado al leer el código: `ge_bounding_box` ponía
  `uv_prescale = 0` sin marcar sucio el grupo V; con `ge_estado_sucio`, una
  llamada después de una BBOX con uv del modo 3 la habría usado mal (DBZ no
  usa ese modo: la verificación dio 0).
- Siguiente (aprobado por el usuario, 0.5.22): 1.3 a (BBOX más barata e
  idéntica), 1.3 b (saltarse las llamadas de cajas enteras fuera, exacto),
  el atajo de los datos de matrices, el histograma de comandos y el arreglo
  de `uv_prescale`.

### v0.5.22: cajas más baratas (`ge_bbox_rapido`), llamadas de cajas enteras fuera (`ge_bbox`) y datos de matrices en el bucle (`ge_cmd_matrices`)

Lo acordado al leer los registros de la 0.5.21: 1.3 a (la prueba de las
BBOX más barata e idéntica), 1.3 b (saltarse las llamadas de cajas enteras
fuera, exacto), el atajo de los datos de matrices, el histograma de
comandos y el arreglo de `uv_prescale`. Cada cosa con su interruptor, la
misma imagen y comprobable en el Wii con `ge_verificar`.

- **1.3 a, `ge_bbox_rapido`** (encendido). La prueba de las BBOX (la
  aproximada de PPSSPP, la que decide los BJUMP) da el mismo resultado,
  más barata:
  - `vista * proyección` y `mundo * (vista * proyección)` se guardan
    mientras no cambien las matrices (se comparan con `memcmp` en cada
    BBOX);
  - de cada vértice de la caja solo se lee la posición (sin índices, con un
    puntero: s8, s16 o float con los NaN e infinitos limpiados como
    siempre);
  - el formato se guarda por tipo de vértice, y los pesos de morph solo se
    cargan si hay morph;
  - el conteo por planos para en cuanto cada plano tiene un vértice dentro
    (el resultado solo depende de eso).
  Las cuentas que deciden el resultado (`bb_mult4`, `bb_contar`) son las
  mismas funciones en los dos caminos, con las `fma` escritas en el orden
  en que devkitPPC compilaba la 0.5.21: en el Wii el resultado es el mismo
  bit a bit (arnés de devkitPPC con 40.000 cajas al azar: la prueba de la
  0.5.21, la de siempre y la rápida dan lo mismo). Con `ge_verificar`, cada
  BBOX se prueba también por el camino de siempre: línea `[TIEMPOS]
  Verificacion de las cajas`, tiene que dar 0 distintas.
- **1.3 b, `ge_bbox`** (encendido). Después de cada BBOX de hasta 64
  vértices sin huesos ni morph que la prueba de siempre da por visible (las
  otras ya las salta el BJUMP: en DBZ, el 76-81 %), se mira si la caja
  (ampliada 1/256 de su tamaño y 2^-20 de sus coordenadas) está entera
  fuera de un mismo plano de recorte del GE (x, y o z fuera de ±w, o w
  negativa: los planos de `ge_clip_flags`), con un margen de 2^-9 de la
  magnitud de los términos de cada coordenada (el error de las cuentas del
  GE en modo exacto es menor de 2^-11). Si lo está, las llamadas
  siguientes con las mismas matrices de mundo, vista y proyección y todos
  sus vértices dentro de la caja no se calculan: cada triángulo suyo
  tendría los tres vértices fuera del mismo plano y el GE lo descartaría
  con las reglas de siempre.
  - Solo triángulos, tiras y abanicos, sin huesos, morph, índices ni modo
    through; lo demás, como siempre.
  - Lo que la llamada deja a la siguiente se calcula igual: direcciones,
    primitiva, índice de la ventana, última uv y normal y, si la siguiente
    primitiva puede continuarla (o no se sabe), los vértices de la ventana
    (`data_`, con el decodificador de siempre).
  - Para saberlo, `ge_prim_datos_sobran` mira la lista por delante (hasta
    48 comandos y 4 saltos): sigue el tipo de vértice, las direcciones,
    BASE, OFFSETADDR, ORIGIN, los JUMP y los dos lados de los BJUMP (los
    saltos, solo en listas sin stall). Si la siguiente primitiva es nueva,
    con posición y direcciones válidas, nadie lee la ventana: la llamada es
    "ligera" (solo la última uv y normal). Un PRIM de 0 vértices, uno sin
    posición o con una dirección mala no tocan la ventana: se sigue
    mirando o se calcula.
  - Con `ge_verificar`, las llamadas que se saltarían se dibujan igual y se
    comprueba que no sale ninguna primitiva y que lo que dejan es lo
    calculado: línea `[TIEMPOS] Verificacion de ge_bbox`.
  - Línea `[TIEMPOS] Cajas exactas (ge_bbox)`: cajas probadas, enteras
    fuera de un plano (de ellas detrás de la cámara), llamadas después de
    una caja fuera, saltadas (ligeras y vértices) y por qué no se saltan
    las demás (formato o primitiva, matrices cambiadas, algún vértice fuera
    de la caja). `ventanas.py` saca las llamadas y los vértices saltados por
    ventana.
  - Qué se puede esperar: la prueba de PPSSPP sin depth clamp no mira los
    planos cercano y lejano, y con w negativa puede dar "visible"; lo que
    deja pasar y está detrás de la cámara o más allá del plano lejano lo
    salta `ge_bbox`. Lo que sale de la pantalla en diagonal (cada esquina
    fuera de un plano distinto) no se puede saltar así. En el censo de la
    0.5.21, los objetos que pasan la caja y no dibujan nada eran 290-832 ms
    por ventana (1,2-3,4 %), todos sin huesos: es el techo.
- **`ge_cmd_matrices`** (encendido). En el bucle de comandos, los datos y
  números de las matrices (mundo, vista, proyección, textura y huesos: 10
  comandos) se aplican sin pasar por `execute`, con las mismas cuentas
  (`mat_calcular` y `mat_aplicar`; la tabla `mat_desc` está junto a
  `execute`). En la pelea de la 0.5.21, el 87 % de los comandos iban por
  `execute`, sobre todo datos de matrices. Con `ge_verificar`, cada uno
  pasa por `execute` y se compara: línea `[TIEMPOS] Verificacion de las
  matrices`. Lo que decide el camino lento del bucle (`prof_estado`,
  `ge_verificar`, `prof_comandos`) se lee una vez por lista.
- **`prof_comandos`** (apagado, solo mide): cuántos comandos de cada tipo
  ejecuta el bucle; línea `[TIEMPOS] Comandos por tipo` con los 16 más
  frecuentes (número del comando en hexadecimal y %). Para elegir los
  siguientes atajos del bucle.
- **Arreglo de `uv_prescale`**: `ge_bounding_box` ya no lo pone a 0 (la
  caja no lo usa); con `ge_estado_sucio`, una llamada con uv del modo 3
  después de una BBOX se quedaba con la uv sin escalar. Sin interruptor.

Banco de qemu (`bench_lista`, código de devkitPPC, instrucciones por
llamada sin contar el psq_l emulado):
- leer y ejecutar los comandos con y sin `ge_cmd_matrices`: escena 2 (4
  huesos y mundo, 81 comandos) de 7371 a 4686 (−36 %), escena 3 (8 huesos)
  de 12535 a 7890 (−37 %); las escenas 0 y 1 (sin matrices), 256 y 645
  (0.5.21: 264 y 679: los interruptores se leen una vez por lista);
- escena 4 (nueva: objetos de 4 llamadas con su caja, BBOX y BJUMP, y su
  matriz de mundo; las cajas se ven): la llamada entera 20988 sin los dos
  interruptores, 20686 con `ge_bbox_rapido` (~1200 menos por caja, con la
  matriz de mundo distinta en cada objeto, como en DBZ) y 20787 con los
  dos (la prueba exacta de una caja que se ve sale en el primer vértice:
  ~400 por caja, casi todo la segunda comparación de las matrices);
- escena 5 (nueva: los mismos objetos más allá del plano lejano, que la
  prueba de siempre deja pasar): 14256, 13955 y 7167 con `ge_bbox` (las
  llamadas no se calculan; la prueba exacta de una caja entera fuera
  cuesta ~4600);
- escena 6 (nueva: fuera de la pantalla por un lado; el BJUMP salta las
  llamadas, como el 76-81 % de las cajas de DBZ): 1270, 988 y 988. La
  prueba exacta solo corre en las cajas que la de siempre da por
  visibles: probar también estas costaba 2290 por llamada (~5200 más por
  caja; cambiado antes de entregar).
- Estimación para la pelea: probar las cajas eran 615-624 ms por ventana
  (~2,5 %) y `ge_bbox_rapido` le quita ~40 % (~1 %); `ge_bbox`, lo que
  salte de los objetos que pasan la caja sin dibujar nada (techo 1,2-3,4
  %); `ge_cmd_matrices`, parte de los "comandos" (8 %, sobre todo datos de
  matrices).

Verificación:
- test (x86 con ASan y UBSan), test-ppc y devkitPPC: 165.894
  comprobaciones sin fallos; huellas de test_vdec sin cambios (4D4A1D8B,
  0657B177, B3A2BCD3); `gevert.sh` igual que la 0.5.21 (252,2 / 324,5 /
  383,2 / 502,7 / 268,3).
- `tests/test_bbox.c`:
  - 8.000 BBOX al azar (todos los formatos, índices, huesos, morph, tramos
    de más de 0x100, matrices raras, cambiando matrices entre medias) con
    la prueba de siempre y la rápida: 0 distintas; con `ge_verificar`
    4.000 comparadas, 0 distintas. En el arnés de devkitPPC, 40.000 cajas:
    la prueba de la 0.5.21, la de siempre y la rápida dan lo mismo.
  - 240 listas con objetos alrededor de la cámara (detrás, también grandes
    que la prueba de siempre deja pasar; más allá del plano lejano; a
    caballo de los bordes, también finos en la franja del borde; cajas con
    las esquinas en cualquier orden o que no contienen sus vértices;
    continuaciones, PRIM vacíos o sin posición entre medias, uv
    arrastradas), dibujadas por el rasterizador por software en exacto y
    MR, con y sin `ge_bbox` y con `ge_verificar`: 446 cajas enteras fuera
    (112 detrás de la cámara), 382 llamadas saltadas, la VRAM entera
    igual; con `ge_verificar` 382 comprobadas, 0 distintas.
  - Una llamada saltada seguida de una continuación con ocho cosas entre
    medias (nada, PRIM de 0 vértices, sin posición, con la dirección mala,
    una llamada nueva, con índices buenos y malos): misma imagen y las
    llamadas ligeras que tocan, en exacto y MR.
  - 34 casos de lo que mira `ge_prim_datos_sobran`, con y sin stall.
- `tests/test_listas.c`: 600 listas, 69.615 comandos, 0 distintas; con
  `ge_verificar`, 17.838 comandos y 6.475 de matrices comparados con
  `execute`, 0 distintos; los 10 comandos de matrices por el atajo, 64
  veces cada uno, 0 distintos.
- Mutaciones (cada una tiene que hacer fallar alguna prueba), todas
  detectadas. En la prueba exacta: `bb_contiene` que siempre dice que sí,
  el plano x con el signo cambiado, el plano x en 0,9 w, "detrás" con w <
  5, las esquinas cortadas en la cuarta y sin comparar la matriz de mundo
  antes de saltar. En `ge_prim_datos_sobran` (15): siempre ligera, PRIM
  de 0 vértices tomado como nuevo, sin mirar la posición, la dirección,
  los índices, la BBOX, el tope de saltos, el stall, el otro lado del
  BJUMP, el tope de 48 comandos, ORIGIN, OFFSETADDR, CALL, VSC ni
  CONTINUE. Sin efecto en estas pruebas: el margen de w con el signo
  cambiado y "detrás" con w < 1 (con el plano cercano de las pruebas en w
  = 1, lo que queda entre 0 y 1 está también entero fuera de él). Las de
  PRIM de 0 vértices, el plano x en 0,9 w y "detrás" con w < 5 no las veía
  ninguna prueba: de ahí las pruebas dirigidas, los objetos finos en los
  bordes, las esquinas en cualquier orden y los objetos grandes detrás de
  la cámara.
- pspautotests 275/275. A/B en las 572 de cada interruptor (`ge_bbox`,
  `ge_bbox_rapido`, `ge_cmd_matrices`; exacto y MR): solo `malloc` y
  savedata, que cambian también entre corridas del mismo binario. A/B de
  la 0.5.21 contra la 0.5.22 con todo encendido: lo mismo más
  `gpu/complex` e `io/file` (escriben archivos con la hora del PC: el
  mismo binario da otra RAM en cada corrida). A/B del renderizador de GX
  bajo qemu (`build_cli_gx.sh`) en las 110 de `gpu/` con los tres
  interruptores: misma huella del estado de GX (solo `gpu/complex`, como
  siempre). Con `ge_verificar` en las 110 de `gpu/` (y el informe forzado
  también en las que duran menos de medio segundo, en una copia aparte):
  116 cajas, 33.178 datos de matrices y 348.649 comandos comparados, 0
  distintos y ninguna línea `[VERIF]`, en exacto y MR. En las
  pspautotests no se salta ninguna llamada (las cajas de `gpu/bounding`
  no llevan dibujos detrás): `ge_bbox` se comprueba con las pruebas de
  arriba y en el Wii con `ge_verificar`.
- tests/guest 22/22; recarga con ASan (`tests/recarga.py`, las 572 tres
  veces seguidas en el mismo proceso): 0 con problemas.

Autotest (4 registros y un `.capturas.txt`; todas con `prof_pmc`,
`prof_tirones`, `prof_ventana_juego` y `ventana 600`): sin (`ge_bbox`,
`ge_bbox_rapido` y `ge_cmd_matrices` a 0, como la 0.5.21), con (los tres a
1), verif (`ge_verificar`: 0 distintos en cajas, `ge_bbox`, matrices,
comandos, vértices y estado de GX) y censo (`prof_bbox` y `prof_comandos`
con `capturas 400`: las 12 huellas tienen que ser las de siempre; los ms
de las BBOX y de los objetos que pasan la caja sin dibujar, para comparar
con la 0.5.21, y el histograma de comandos).

### v0.5.23: cajas fuera de la pantalla (`ge_bbox_pantalla`), la caja de cada llamada (`ge_bbox_llamada`) y censo de los huesos (`prof_huesos`)

Lo acordado al leer los registros de la 0.5.22. `ge_bbox` solo saltó 3
llamadas: los objetos que pasan la caja sin dibujar nada (290-840 ms por
ventana) no están fuera de un plano ±w, los descarta el scissor. El punto
1, de lleno: rescatar esos objetos sin cambiar ni un píxel. Y el censo de
los huesos (`BONEMATRIXDATA` era el 57 % de los comandos). Ponytail y
Graphify, para más adelante.

Análisis (qué dibuja el GE de una caja, en el código del emulador):
- Todas las primitivas (triángulos, rectángulos, líneas y puntos, por
  software y por GX) pasan por `range_of`. Una se descarta por la
  izquierda si `(max x | 15) < sc_x1`, es decir, si todo vértice tiene
  `x = floor(sx·16) − OFFSETX < SCISSOR1·16`. Por la derecha, si
  `x ≥ 16·(min(SCISSOR2, REGION2) + 1)`. Igual en y. El umbral en
  pantalla es exacto: `L = (16·SCISSOR1 + OFFSETX)/16`.
- Con w > 0, `sx < L` equivale a `xs·x + (xc − L)·w < 0`: es lineal en
  coordenadas de recorte. Sobre una caja (afín) el máximo está en las
  esquinas y, si se corta por un plano, en los cruces de las aristas con
  él.
- Qué parte de la caja puede llegar a dibujarse:
  - Sin recorte de profundidad, un triángulo con algún vértice de w ≤ 0
    se descarta: cuenta Q = caja ∩ {w ≥ −τ}.
  - Con recorte (DEPTHCLAMPENABLE), lo que está detrás del plano cercano
    se recorta, y los vértices nuevos caen en el plano: cuenta
    Q = caja ∩ {z + w ≥ −τ}. Pero ahí un vértice con w < 0 y z + w ≥ 0
    se dibuja dado la vuelta, así que w tiene que ser claramente positiva
    en Q (`minw > s3/256`); si no, no se salta.
- Márgenes:
  - 2^-9 de la magnitud de los términos (`|xs|·s0 + |xc − L|·s3`);
  - 2^-11 de `|xs|·s0 + |xc|·s3`, por el redondeo del float24 en
    pantalla, que con el centro en 2048 es de ~1/32 de píxel;
  - `τ = (s2 + s3)/256` con recorte, `s3/256` sin él.
  Las mutaciones dicen que el margen tiene entre 32 y 128 veces de holgura
  sobre el error del emulador: con `mg/128` ya falla la prueba de los
  bordes, con `mg/32` no.
- Lo que ya hacía la prueba de siempre: comprueba los cuatro bordes del
  scissor con ±1 px, linealmente y sin mirar el signo de w. Una caja
  entera fuera con w > 0 ya la salta el BJUMP. Lo que queda por rescatar
  son las cajas que cruzan el plano de la cámara o el cercano, y las
  llamadas fuera dentro de cajas que se ven. De ahí las dos partes.

Lo hecho:
- **1.3 c, `ge_bbox_pantalla`** (encendido). La prueba exacta de
  `ge_bbox` (`bb_fuera`) mira también la pantalla con el programa lineal
  de arriba. Fuera de la pantalla, la caja se salta igual que una entera
  fuera de un plano.
  - La caja guarda los 10 comandos de la pantalla (escala y centro x/y,
    SCISSOR1/2, REGION2, OFFSETX/Y, DEPTHCLAMPENABLE) y se deja de saltar
    si cambia alguno.
  - Para que una caja que se ve no cueste, sale enseguida (es exacto: con
    márgenes ≥ 2^9 veces el redondeo, la prueba entera daría NO):
    - con el primer vértice de la BBOX dentro de los 6 planos, con w > 0
      y en la pantalla (sin márgenes);
    - con alguna esquina así;
    - sin márgenes, ningún plano con las 8 esquinas fuera y, en cada
      borde, una esquina de Q al lado de dentro.
    Solo si no sale se calculan |M| (perezoso), los márgenes, los planos
    con margen y el programa lineal con las aristas.
  - "Dentro" (las 8 esquinas dentro) solo se calcula con `prof_bbox`,
    para el censo. Las llamadas que se saltan son las mismas con censo y
    sin él.
- **1.3 d, `ge_bbox_llamada`** (encendido). Tras una caja que no está
  entera fuera, o si cambian sus matrices o su pantalla, cada llamada se
  prueba con su propia caja. Solo triángulos, tiras y abanicos sin
  huesos, morph, índices ni through, con los vértices en la RAM.
  - Primero su primer vértice: si se ve, se dibuja sin más.
  - Si no, la caja de sus posiciones (s8, s16 o float con NaN e infinitos
    limpiados como al decodificar) con la misma prueba.
  - Fuera, se salta como en `ge_bbox`: lo que deja a la siguiente llamada
    se calcula igual.
- **`ge_sucio` con dos grupos más:**
  - B: mundo, vista o proyección cambiadas; marcado donde ya se marca T,
    en `execute` y en el atajo de `ge_cmd_matrices`.
  - P: los 10 comandos de la pantalla.
  - `GE_SUCIO_TODO` pasa de 7 a 31.
  La prueba de cada llamada no compara las matrices (40 floats) ni la
  pantalla si el bit está limpio. La prueba de siempre, la que decide el
  BJUMP, sigue con `memcmp`, así que lo que hace el BJUMP no depende del
  bit. Con `ge_verificar` se compara siempre y se cuenta si el bit decía
  "limpio" con algo cambiado (línea `[TIEMPOS] Verificacion de ge_bbox`,
  "matrices y pantalla guardadas ... distintas de las de ahora"; tiene
  que dar 0). Las pruebas que escriben `ge.cmd` o las matrices a mano
  tienen que marcar `ge_sucio` (como ya hacían para `ge_estado_sucio`).
- **`prof_huesos`** (apagado, solo mide). En el camino lento del bucle de
  comandos, como `prof_comandos` (con él, `ge_cmd_matrices` no se usa):
  - datos de matrices de huesos, y cuántos traen el valor que ya había;
  - matrices completas (12 datos seguidos desde un múltiplo de 12), y
    cuántas iguales a la anterior;
  - rachas de 0x2A/0x2B seguidos (media, y cuántas de 12 o más);
  - llamadas con huesos (y datos por llamada).
  Línea `[TIEMPOS] Huesos (prof_huesos)`. Es para decidir si vale la pena
  un atajo para las rachas (pocos valores iguales: el coste es leerlas;
  muchos: no rehacer lo que dependa de ellas).
- **Censo de cajas:** cuarta línea, los objetos que no dibujan nada por la
  clase de su caja (sin prueba exacta, fuera, dentro, dudosa, dudosa que
  cruza el plano de la cámara) y los ms de probar la caja de cada llamada.
- **Arreglo de `ge_setup_recip`:** con `abs_det ≥ 2^48` leía la tabla con
  un índice negativo (lo encontró UBSan con posiciones de pantalla
  enormes). En los demás casos da lo mismo: comprobado en 150 millones de
  valores.

Fallo latente apuntado, sin arreglar porque cambiaría resultados y hay que
saber primero qué hace la PSP: un vértice con la posición de pantalla NaN
no se marca como fuera de rango en `round_to_screen` (las comparaciones
con NaN dan falso), y `floor_to_int(NaN)` da un entero cualquiera. Pasa
con w = 0 y x = 0, o con matrices con NaN o infinitos de los comandos. Las
pruebas de cajas no saltan esas llamadas: sus cajas no son finitas o dan
NO.

Banco de qemu (`bench_lista`, código de devkitPPC, instrucciones por
llamada sin el psq_l emulado; 4 llamadas de 20 vértices por objeto). Cada
celda dice: solo `ge_bbox` / con `ge_bbox_pantalla` / con
`ge_bbox_llamada` / con los dos.

| Escena | Qué hay | Instrucciones por llamada |
|---|---|---|
| 4 | Cajas y llamadas que se ven | 29262 / 29198 / 29457 / 29520 |
| 5 | Más allá del plano lejano | 8316 / 8088 / 7896 / 8088 |
| 6 | Fuera por un lado: las salta el BJUMP | 1838 en las cuatro |
| 7 (nueva) | Cajas finas que cruzan la cámara, fuera de la pantalla, con recorte | 144723 / 8374 / 145799 / 8374 |
| 8 (nueva) | Caja grande que se ve, con las llamadas fuera de la pantalla y dentro de ±w | 22165 / 22602 / 22850 / 11088 |
| 9 (nueva) | La misma, con las llamadas dentro y la caja de cada una por fuera | 29203 / 29640 / 29399 / 30382 |

Coste aproximado de cada prueba:
- caja que se ve por su primer vértice: unas −64 por llamada (sale antes
  de la segunda comparación de matrices);
- prueba de una llamada que se ve por su primer vértice: unas 260;
- caja "dudosa" grande: unas 1750 por caja;
- llamada que se ve sin que se vea su primer vértice: caja de sus 20
  vértices + prueba, unas 1500.
`gevert.sh` sin cambios (252,2 / 324,5 / 383,2 / 502,7 / 268,3).

Verificación (a mitad y al final):
- `test` (x86 con ASan y UBSan), `test-ppc` y devkitPPC: 166.181
  comprobaciones sin fallos; huellas de test_vdec sin cambios (4D4A1D8B,
  0657B177, B3A2BCD3).
- `tests/test_bbox.c`, las pruebas nuevas, todas comparando la VRAM entera
  y las cuentas de primitivas sin los interruptores, con ellos y con
  `ge_verificar` (0 distintas en todas):
  - 1.200 listas al azar alrededor de la pantalla y de la cámara (viewport
    ×1-×3 con signo, centros y offsets al azar, scissor y región,
    perspectiva y ortográfica, con y sin recorte, las 4 combinaciones):
    2.522 llamadas saltadas, 1.877 por su caja;
  - un vértice a −3..+10 px (en 1/16) de cada borde, también con la caja
    puesta por la traslación del mundo;
  - umbral exacto, con el centro del viewport junto al borde (margen
    < 1/32 px): a 1/32 px fuera se salta el 100 %, por dentro nunca;
    cambiando entre la caja y la llamada el offset, el scissor, el centro,
    la escala o el recorte de profundidad;
  - 800 cajas finas en diagonal que pasan por la cámara;
  - cajas que cruzan el plano cercano y solo se ven por detrás de él, con
    la z de pantalla ancha (sin recorte se dibujan), también quitando el
    recorte entre la caja y la llamada;
  - proyecciones con w negativa delante del plano cercano (lo de detrás
    de la cámara cortado por el plano cae en la pantalla): 894 de 900
    dibujan y no se salta ninguna;
  - el censo de cajas con los dos interruptores.
  `tests/test_listas.c`: el censo de los huesos con cuentas exactas.
- Mutaciones (51, con un arnés que recompila solo el archivo mutado):
  - **Detectadas:** el umbral ±1/16 en cada borde, el signo de los bordes,
    el offset, el programa lineal sin aristas o sin las de z, Q con o sin
    el plano cercano equivocado, `minw` (sin él o sin los cruces), no
    mirar la pantalla al saltar o no guardar el recorte o la escala x, la
    caja de cada llamada (s8, s16, float), las esquinas o `s` sin
    traslación, |M| sin valor absoluto o sin rehacer, la caja más
    pequeña, saltar con las matrices cambiadas, las cachés B y P
    ignoradas o sin marcar (mundo por `execute` y por el atajo; offset,
    scissor, escala, recorte), el camino lento sin mundo, Q' sin recorte,
    las salidas tempranas sin la pantalla, y `mg` a 0 o a 1/128.
  - **No detectadas, con su porqué:**
    - conservadoras: el scissor sin la región o al revés, no ampliar la
      caja;
    - márgenes de redondeo: τ = 0, `mg/8` y `mg/32` (la holgura de
      arriba), |M| con signo en el camino lento;
    - inalcanzables tras las salidas tempranas: el margen del plano
      lejano y la comprobación final del margen;
    - redundantes: el bit de w < 0 de la máscara sin margen (con w < 0
      siempre se cumple `c > w` o `−c > w`) y la guarda de 1e30 (la de
      `s` la cubre; se quitó).
  - La clase "antes del plano cercano" era código muerto (los planos con
    el margen de 2^-9 la atrapan antes): quitada.
- pspautotests 275/275; tests/guest 22/22.
- A/B en las 572 (exacto y MR) de `ge_bbox_pantalla`, de
  `ge_bbox_llamada` y de los dos: solo `malloc` y savedata, que cambian
  igual con el mismo binario y las mismas opciones (comprobado tres
  veces).
- A/B del renderizador de GX bajo qemu en las 110 de `gpu/` (exacto y MR):
  solo `gpu/complex`, como siempre.
- La 0.5.22 contra la 0.5.23 con todo encendido (dos `wiisp-cli-gx`):
  555 programas, 0 distintos.
- `ge_verificar` en las 110 de `gpu/`, en exacto y MR: 0 líneas `[VERIF]`
  y 0 distintos en cajas, `ge_bbox`, matrices, comandos y cachés.
- Recarga con ASan (`tests/recarga.py`, las 572 tres veces seguidas en el
  mismo proceso): 0 con problemas.

Autotest (4 registros y un `.capturas.txt`; todas con `prof_pmc`,
`prof_tirones`, `prof_ventana_juego` y `ventana 600`):
- sin: `ge_bbox_pantalla` y `ge_bbox_llamada` a 0, como la 0.5.22;
- con: los dos a 1;
- verif: con `ge_verificar`;
- censo: `prof_bbox`, `prof_comandos` y `prof_huesos`, con
  `capturas 400`.

Qué mirar:
- con frente a sin (`ventanas.py`): velocidad, y las columnas de cajas
  fuera de la pantalla y de llamadas probadas y saltadas por su caja;
- verif: 0 distintos en todo, también en "matrices y pantalla guardadas";
- censo: las 12 capturas de siempre; cuánto de los 290-840 ms de objetos
  que no dibujan nada queda y de qué clase; los ms de probar la caja de
  cada llamada; la línea de los huesos.

### Resultado de la 0.5.23 en el Wii (DBZ: sin / con / verif / censo)

- Las 12 capturas de `censo`, las de siempre; `ge_verificar`: 0 distintos
  en todo, también en "matrices y pantalla guardadas".
- Escena entera de 95,9 a 93,0 s (−3,0 %); peleas −4,5 %, −5,4 % y
  −3,8 %.
- En la ventana 4200-4800: 171.296 de las 236.011 llamadas probadas por
  su caja, saltadas (3,17 M vértices); ciclos de 17.429 a 16.496 M y GE
  de 8.575 a 7.605 M.
- Lo que funcionó no es la pantalla: solo 8 llamadas y 2-18 cajas por
  ventana caen fuera de la pantalla y no de un plano. La ganancia es de
  `ge_bbox_llamada` con los planos ±w, llamada a llamada.
- Probar la caja de cada llamada cuesta ~1.030 ms por ventana (~3.200
  ciclos por llamada, ~0,75 G ciclos) frente a ~1,7 G ciclos de ahorro
  bruto.
- Quedan 3.164 objetos sin dibujar nada (1,05 M vértices, 438 ms), casi
  todos de cajas dudosas que cruzan el plano de la cámara.
- Huesos: 4,27 M datos por ventana (57,3 % de los comandos), 10 %
  repetidos, 7,5 % de matrices repetidas, rachas de 36 datos de media.

Plan acordado con el usuario, una versión por punto:
1. 0.5.23.1: la prueba de cada llamada más barata.
2. 0.5.23.2: rescatar las llamadas que cruzan el plano de la cámara (la
   banda de guarda de 4096 y, sin recorte, el rango z de pantalla como
   restricciones lineales de Q).
3. 0.5.23.3: las rachas de huesos (0x2B) más baratas en el bucle de
   comandos.
4. 0.5.23.4: 1.4 `ge_charolas` y el diseño del IR.

### v0.5.23.1: la prueba de cada llamada más barata (`ge_bbox_orden`, `ge_bbox_aabb`)

Punto 1 del plan. En DBZ las llamadas se saltan por los planos ±w (casi
nunca por la pantalla), y la 0.5.23 calculaba siempre los bordes de la
pantalla y las máscaras de los 7 planos en las 8 esquinas.

- Banco: escena 10, como DBZ. Es la 8 con el viewport ×1 y una matriz de
  mundo que deja las llamadas enteras fuera del plano x− (formato 0x11A,
  20 vértices por llamada). Perfil de la 0.5.23 por llamada: `bb_fuera`
  2.705 instrucciones (1.768 sin `ge_bbox_pantalla`), la caja de las
  posiciones ~1.100 (55 por vértice: mínimos y máximos en la pila).
- `ge_bbox_orden`: el mismo resultado, en este orden:
  1. Las esquinas, sumando x + y una vez para las dos z (mismo redondeo:
     `(x + y) + z`).
  2. La máscara sin margen, completa solo hasta que algún plano deja fuera
     todas las esquinas vistas. Después, cada esquina solo se compara con
     esos planos: si sigue fuera de alguno no puede estar dentro, y si
     ya no, se hace la máscara completa.
  3. Los bordes de la pantalla, solo para las esquinas sin plano (las que
     pueden estar dentro). Las demás, solo si hace falta el programa
     lineal.
  4. Con un plano sin margen que deja todo fuera, los planos con margen
     antes que la pantalla, y solo esos (el margen es ≥ 0, así que un
     plano con margen implica el mismo sin margen). Sin ninguno, los
     planos con margen no se miran: no pueden dar nada.
  `bb_fuera_lento` es el de la 0.5.23, sin tocar.
- `ge_bbox_aabb`: la caja de las posiciones s8 y s16 con los mínimos y
  máximos en registros, empezando por el primer vértice (con `else if`).
  Float va por `bb_aabb_lento`, como antes.
- Censo (`prof_bbox`): la línea de las cajas añade lo que cuesta leer las
  posiciones de cada llamada, y cuántas llamadas y vértices se leen. Así
  se reparte en el Wii el coste de la prueba entre la caja y `bb_fuera`.

Banco (instrucciones por llamada, sin el psq_l emulado; sin / con los dos
interruptores):

| Escena | Sin | Con |
|---|---|---|
| 10, como DBZ | 10.371 | 8.388 (−19 %) |
| 8, llamadas fuera de la pantalla | 10.925 | 10.159 |
| 9, llamadas dentro | 23.215 | 23.047 |
| 7 | 8.130 | 8.059 |
| 4 | 22.313 | 22.313 |

En la 10, `bb_fuera` pasa de 2.705 a 1.210 instrucciones y la caja de
~1.100 a ~510. La prueba entera, de ~3.900 a ~1.850.

Verificación:
- `make -f Makefile.pc test` (x86), `test-ppc` y las pruebas con
  devkitPPC bajo qemu: 166.348 comprobaciones, 0 fallos. Huellas de
  test_vdec las de siempre: 4D4A1D8B, 0657B177 y B3A2BCD3.
- `tests/test_bbox.c`: en las pruebas al azar (pantalla, fino, cercano,
  rara y la nueva), cada ronda se corre una vez más con los dos
  interruptores al revés, y `ge_stats` entera y la VRAM tienen que salir
  iguales: 0 rondas distintas. Prueba nueva: cajas detrás de la cámara a
  menos del margen, con traslaciones grandes que se cancelan (400 cajas,
  286 probadas).
- Mutaciones del código nuevo (23):
  - **Detectadas, 22:** el signo de −c y el bit de w < 0 en la máscara
    parcial; los bordes sin calcular (tras los planos y sin ellos); los
    márgenes a 0 (planos, −c, detrás; este último, por la prueba nueva);
    detrás como plano; −c sin signo; sin los planos z; "dentro" sin un
    borde; x + y o z cambiados; sin `cruza`; `dentro` siempre; y en la
    caja, sin el máximo z, sin el último vértice, y leída del offset de
    x, s8 /256, sin el mínimo y, y sin signo.
  - **No detectada, 1:** quitar `q0`, que solo es un atajo. El programa
    lineal de después da lo mismo (equivalente).
- pspautotests 275/275; tests/guest 22/22.
- A/B en las 572 (exacto y MR) de `ge_bbox_orden`, de `ge_bbox_aabb`, de
  los dos y de los dos sin `ge_bbox_pantalla`. Salen distintos `malloc`,
  savedata, `io/file`, `io/stat` y `gpu/complex`. Esos cinco también
  cambian con el mismo binario y las mismas opciones (comprobado tres
  veces en exacto y en MR): no son de esta versión.
- A/B del renderizador de GX bajo qemu en las 110 de `gpu/` (exacto y MR):
  solo `gpu/complex`, como siempre.
- La 0.5.23 contra la 0.5.23.1 con todo encendido (dos `wiisp-cli-gx`):
  555 programas, 0 distintos.
- `ge_verificar` en las 110 de `gpu/`, en exacto y MR: 0 líneas `[VERIF]`.
- Recarga con ASan (las 572 tres veces seguidas en el mismo proceso):
  la primera vez, 1 de 572 con 336 bytes más en la tercera vuelta
  (`threads/scheduling/syscallkinds`, justo cuando se llenó el disco del
  entorno); sola, 5 de 5 bien, y la recarga entera otra vez, 0 con
  problemas.
- Compilación para el Wii: `wiisp.dol` 0.5.23.1.

Autotest (4 registros y un `.capturas.txt`; todas con `prof_pmc`,
`prof_tirones`, `prof_ventana_juego`, `ventana 600` y `ge_bbox_pantalla`
y `ge_bbox_llamada` encendidos):
- sin: `ge_bbox_orden` y `ge_bbox_aabb` a 0 (como la 0.5.23);
- con: los dos a 1;
- nopant: con, y `ge_bbox_pantalla` a 0 (¿aporta algo en DBZ?);
- censo: con, y `prof_bbox` y `capturas 400`.
Sin verif: el resultado es el mismo bit a bit que el de la 0.5.23 (lo
comprueban las pruebas ronda a ronda), y la 0.5.23 dio 0 distintos con
`ge_verificar`.

Qué mirar:
- con frente a sin (`ventanas.py`): velocidad de las peleas (la prueba
  costaba ~1.030 ms por ventana);
- nopant frente a con: si `ge_bbox_pantalla` no gana nada, se puede
  apagar;
- censo: las 12 capturas de siempre, los ms de probar la caja de cada
  llamada y cuánto de eso es leer las posiciones.

### Resultado de la 0.5.23.1 en el Wii (DBZ: sin / con / nopant / censo)

- Las 12 capturas de `censo`, las de siempre; las cuatro corridas ejecutan
  las mismas 1343,1 M instrucciones, y sin y con saltan las mismas
  llamadas en cada ventana (171.415 en la 4200-4800).
- Escena de 93,2 a 92,4 s (−0,9 %); peleas −1,7 %, −1,3 % y −1,0 %. En la
  4200-4800: ciclos de 16.466 a 16.248 M (−1,3 %), leer vértices de 559 a
  519 ciclos por vértice, GE de 46,3 a 45,4 %.
- Censo: probar la caja de cada llamada, de 1.031 a 739 ms por ventana
  (−28 %; en el banco bajaba a la mitad). La mitad, 367 ms, es leer las
  posiciones: 185.504 llamadas con 3,64 M vértices, ~73 ciclos por vértice
  (en qemu ~25 instrucciones). Es la memoria: los vértices de una llamada
  que se salta no se han leído nunca, y cada llamada (~240 bytes, 8
  líneas) viene de la memoria a ~180 ciclos por línea. El resto de la
  prueba, ~1.150 ciclos por llamada.
- nopant: igual o un pelo más rápido que con (16.205 M ciclos) y salta
  casi lo mismo (171.388). `ge_bbox_pantalla` no aporta nada en DBZ; se
  queda encendido (cuesta muy poco; otros juegos pueden usarlo).
- Quedan 3.164 objetos sin dibujar nada en la pelea; 2.617 son cajas
  dudosas que cruzan el plano de la cámara (1,02 M vértices, ~355 ms por
  ventana): el punto 2.
- Idea para más adelante: pedir con `dcbt` las líneas siguientes mientras
  se leen las posiciones de cada llamada.

### v0.5.23.2: banda de guarda (`ge_bbox_guarda`) y repaso por triángulos (`ge_bbox_triangulos`)

Punto 2 del plan: rescatar las llamadas que cruzan el plano de la cámara.

Análisis (en el código del emulador, `round_to_screen` y
`triangle_rejected`). Un triángulo solo se dibuja si:
- **sin recorte de profundidad:** sus tres vértices tienen w > 0,
  0 ≤ sx ≤ 4095,97, 0 ≤ sy < 4095,97 y 0 ≤ sz < 65536;
- **con recorte:** los vértices delante del plano cercano están en la
  banda en x e y, y los puntos del corte con el plano también;
- **en los dos casos:** no todos fuera del mismo plano, no todos con
  w < 0, y luego `range_of`.
El camino por lotes (`pantalla_lote`) solo se usa con el vértice dentro
del rango; si no, va por `round_to_screen`, que lo marca fuera. Con w > 0
las condiciones de banda y z son lineales en coordenadas de recorte, como
los bordes de la pantalla.

1. `ge_bbox_guarda`: el programa lineal de la caja con esas restricciones
   (ver ARREGLOS). Lo que aprendí al probarlo:
   - La proyección de la parte visible de la caja es convexa y la pantalla
     está dentro de la banda. Así que la banda en x/y solo cambia algo si
     esa proyección sale muy lejos (más de 2048 px) y "da la vuelta". La
     prueba de objetos largos (desde 3000-9000 px fuera hasta cerca de la
     pantalla) lo tiene: 212 y 173 rescates donde antes 5 y 1.
   - Con la escala z de siempre, el rango z solo añade un ~5 % detrás del
     plano cercano. Con una escala que no cubre el frustum, quita lo más
     cercano o lo más lejano de él. Para eso las salidas tempranas (primer
     vértice visto, una esquina dentro) también miran el rango z.
   - En escenas al azar alrededor de la cámara rescata muy poco (0-9 de
     cientos). Lo que mata a la mayoría de esas llamadas es que un solo
     vértice fuera de la banda, o detrás de la cámara sin recorte, descarta
     el triángulo entero, y eso la caja no lo puede ver.
2. Por eso `ge_bbox_triangulos`: el repaso de las posiciones triángulo a
   triángulo, con las mismas reglas y márgenes (ver ARREGLOS). En la
   prueba al azar alrededor de la cámara, el censo decía que en 79 de 90
   llamadas que no dibujaban nada todos sus triángulos se descartaban; el
   repaso se queda con las que puede probar con margen. Se intenta también
   en las llamadas vistas por su primer vértice (en una visible para en el
   primer triángulo).
3. Censo nuevo (`prof_bbox`), para saber qué pasa en DBZ: lo que cuesta el
   repaso y, para las llamadas probadas por su caja que no dibujan nada,
   con qué salida se quedaron, si tienen recorte, cómo son sus vértices y
   sus triángulos, y cuántas tienen todos los triángulos descartados.

Banco (instrucciones por llamada sin el psq_l emulado, sin / con los dos;
el banco no tiene llamadas que crucen la cámara, solo mide el coste):
4: 22.341 / 23.161; 7: 8.111 / 8.130; 8: 10.432 / 10.659; 9: 23.134 /
24.331; 10: 8.485 / 8.515. La banda casi no cuesta (solo hace algo cuando
la prueba de la caja ya falló, y las salidas tempranas miran la banda solo
de los testigos). El repaso cuesta ~800-1.200 en las llamadas visibles (3-7
vértices de media): lo dirá el censo.

Verificación:
- Pruebas (x86, PowerPC y devkitPPC bajo qemu): 166.533 comprobaciones,
  0 fallos. Huellas de test_vdec las de siempre: 4D4A1D8B, 0657B177 y
  B3A2BCD3.
- `tests/test_bbox.c`:
  - las pruebas al azar tienen un modo con los dos interruptores (imagen y
    primitivas iguales a las de referencia);
  - la igualdad con y sin `ge_bbox_orden` se mira con los dos apagados;
  - pruebas nuevas, todas con 0 cuentas e imágenes distintas:
    - llamadas que cruzan la cámara a su alrededor;
    - objetos largos desde muy fuera de la banda, con mínimos de rescate
      por cada lado;
    - un vértice junto al límite de la banda, del rango z (por los dos
      lados), del plano cercano o de w = 0, y uno en pantalla justo detrás
      del plano cercano sin recorte;
    - una escala z que deja fuera lo más cercano o lo más lejano del
      frustum, cambiada entre la caja y las llamadas (0 saltos de las que
      dibujan).
- Mutaciones del código nuevo (36):
  - **Detectadas, 25:**
    - en la banda: el límite x a 2048, los signos de x ≤ límite, y ≥ 0 y
      z ≥ 0, z < 32768;
    - en el certificado: λ < 0, sin valor absoluto, la caja a la mitad;
    - los testigos siempre (los tres sitios);
    - sin comparar la escala z guardada, las dos salidas tempranas sin el
      rango z;
    - en el repaso: banda, borde, z y traslación del margen; banda sin
      "delante"; plano de un solo vértice; reglas sin recorte siempre;
      tira, abanico y lista mal.
  - **No detectadas:**
    - de margen de redondeo, 7: x ≥ 0 y h sin margen, el certificado sin
      holgura, la caja sin agrandar, w < 0, "cerca" y "delante" sin margen
      en el repaso (los márgenes tienen de 32 a 128 veces de holgura);
    - equivalente, 1: todas las restricciones activas;
    - solo quitan poder, 2: h sin z con recorte (Q más grande) y el atajo
      del repaso sin mirar los bordes (dentro del frustum y fuera del
      scissor, que la caja ya salta).
  - **Adoptada como mejora:** "los tres más allá del mismo borde" no
    necesita que estén delante del plano cercano.
- pspautotests 275/275; tests/guest 22/22.
- A/B en las 572 (exacto y MR) de `ge_bbox_guarda`, de
  `ge_bbox_triangulos` y de los dos: solo `malloc`, `io/file`,
  savedata y una vez `gpu/complex`, que cambian también con el mismo
  binario.
- A/B del renderizador de GX bajo qemu en las 110 de `gpu/` (exacto y MR):
  0 distintos.
- La 0.5.23.1 contra la 0.5.23.2 con todo encendido: primero salió
  `gpu/exact/pixel` distinto, también con `ge_bbox=0`. El stub de GX no
  escribe en los destinos de las copias y se leía basura de la memoria
  (determinista con cada binario, distinta entre binarios). Con
  `MALLOC_PERTURB_` (ahora siempre en `ab_versiones.py`): 555 programas, 0
  distintos.
- `ge_verificar` en las 110 de `gpu/` (exacto y MR): 0 líneas `[VERIF]`.
- Recarga con ASan: la primera vez, 3 de 572 con 336 bytes más en la
  tercera vuelta (con la máquina cargada; ya pasó en la 0.5.23.1 con otro
  programa). Solos, 12 de 12 bien, y la recarga entera otra vez, 0.
- Compilación para el Wii: `wiisp.dol` 0.5.23.2.

Autotest (4 registros y un `.capturas.txt`; todas con `prof_pmc`,
`prof_tirones`, `prof_ventana_juego` y `ventana 600`):
- sin: `ge_bbox_guarda` y `ge_bbox_triangulos` a 0 (como la 0.5.23.1);
- con: los dos a 1;
- verif: con `ge_verificar` (camino nuevo que salta llamadas);
- censo: `prof_bbox` y `capturas 400`.

Qué mirar:
- con frente a sin (`ventanas.py`): velocidad de las peleas; llamadas
  fuera por la banda y por sus triángulos (línea `Cajas exactas`);
- verif: 0 distintos en "Verificacion de ge_bbox";
- censo: las 12 capturas; los ms de repasar los triángulos; y, de las
  llamadas probadas que no dibujan nada, la salida de la prueba, el
  recorte, cómo son sus vértices y triángulos, y cuántas tienen todos sus
  triángulos descartados (lo que el repaso podría saltar todavía).

### Resultado de la 0.5.23.2 en el Wii (DBZ: sin / con / verif / censo)

- Las 12 capturas de `censo`, las de siempre (9E8BC705 ... E9392291); las
  cuatro corridas ejecutan las mismas 1343,1 M instrucciones.
- verif: "Verificacion de ge_bbox" 0 distintas (175.234 llamadas que se
  habrían saltado, dibujadas y comparadas en la 4200-4800; 162.892 en la
  3600-4200, 57.313 al final), cajas rápidas 0 distintas, todo lo demás 0.
- `ge_bbox_guarda`: no hace nada en DBZ (53 cajas probadas por ventana, 0
  fuera). Cuesta casi nada; se queda encendido.
- `ge_bbox_triangulos`: en la 4200-4800 repasa 64.715 llamadas (todas las
  que la caja no salta, también las 50.507 vistas por su primer vértice) y
  salta 3.819 más (175.234 frente a 171.415; +181 mil vértices). Cuesta
  207 ms por ventana en el censo (293 en la 3600-4200), ~2.300 ciclos por
  llamada repasada, más de lo que ahorra: ciclos (PMC) de con frente a sin,
  +0,8 % (3600-4200), +0,5 % (4200-4800) y +1,0 % (final); leer vértices
  de 522 a 551 ciclos por vértice. Escena 92,4 frente a 92,6 s.
  **Se apaga por defecto en la 0.5.23.3** (el camino sigue, con su
  interruptor). Limitarlo a las llamadas que no se ven por su primer
  vértice queda como idea: haría falta separar en el censo lo que cuesta
  cada clase.
- Censo, lo que queda: las llamadas probadas por su caja que no dibujan
  nada son solo 536 (9.268 vértices), 505 con todos sus triángulos
  descartados. Por la salida: esquina dentro 401, vértices de Q en la banda
  134. Sus vértices: 7.822 fuera de la pantalla, 1.106 detrás de la
  cámara, 320 fuera de la banda. Lo que el repaso puede saltar ya está
  casi todo saltado.
- Siguen sin dibujar nada 15.483 llamadas fuera de objetos (sin BBOX) con
  267 mil vértices (218 ms), 13.546 con huesos: no tienen caja que probar.
  Idea para más adelante (con los huesos, la caja de las posiciones no
  vale).

### v0.5.23.3: rachas de huesos (`ge_cmd_huesos`) y mutaciones rápidas

Punto 3 del plan. En la 0.5.23 el censo `prof_huesos` daba, en la pelea,
4,27 M datos de hueso por ventana (57 % de los comandos) en rachas de 36
datos de media (3 matrices; cada una con su 0x2A delante). Con
`ge_cmd_matrices` cada dato todavía pasaba por el bucle general: leer,
mirar la tabla de casos, la de matrices, el descriptor, y guardar número,
comando, `ge_sucio`, `cur_cmd`, la cuenta y `l->pc`. Banco (escena 3, 8
huesos = 104 comandos de hueso por llamada): `run_commands_ptr` 7.911
instrucciones por llamada frente a 412 sin huesos (escena 1): ~62 por
comando.

`ge_cmd_huesos`: al llegar un 0x2A o 0x2B, `racha_huesos` ejecuta los
siguientes 0x2A/0x2B en un bucle propio con el número en un registro,
hasta otro comando, la parada (stall) o el final de la RAM (detalle en
ARREGLOS). La parada: el bucle normal mira `pc == stall` antes de cada
comando; como el pc sube de 4 en 4, la racha para en `(stall − pc) / 4`
si la diferencia es múltiplo de 4 (si no, el pc nunca llega a la parada).
Primera versión, 22 instrucciones por dato: GCC leía la palabra con `lwz`
para mirar el byte del comando y daba la vuelta al dato con tres
rotaciones. Leyendo el byte del comando aparte (`p[3]`) usa `lwbrx`; con
la máscara de 24 bits al final y sin la bandera de "hubo datos" (se sabe
contando los 0x2A), 15.

Banco, instrucciones por llamada (total sin el psq_l emulado; bucle de
comandos + racha), sin / con `ge_cmd_huesos`:
- escena 2 (4 huesos, 52 comandos de hueso): total 62.845 / 60.437;
  comandos 4.603 / 2.195;
- escena 3 (8 huesos, 104): total 68.557 / 63.605 (−7,2 %); comandos
  7.911 / 2.959.
En el Wii, con 4,27 M comandos de hueso por ventana a ~47 instrucciones
menos cada uno: ~200 M instrucciones, ~1,5 % de los ciclos de la pelea.

Además:
- `ge_bbox_triangulos` apagado por defecto (resultado de la 0.5.23.2,
  arriba).
- Mutaciones rápidas (`tools/banco/mutar.py`, pedido del usuario): antes
  cada mutación recompilaba todo con ASan (~1 min) y corría todas las
  pruebas (~6,8 min con ASan): ~8 min por mutación. Ahora los objetos se
  compilan una vez a -O2 sin ASan, cada mutación recompila solo su
  archivo y corre las suites que tocan el código (`--solo`, `--parar`), 4
  a la vez; las que sobreviven se repasan con todas las suites (y con
  ASan, `--asan`, o en PowerPC, `--ppc`). Mismo resultado: una mutación
  detectada lo está con cualquier suite; las que sobreviven pasan por todo
  como antes. Todas las pruebas sin ASan a -O2: 39 s de compilar y 96 s de
  correr (con ASan, ~60 s y ~407 s).

Verificación:
- Pruebas (x86 con ASan, PowerPC y devkitPPC bajo qemu): 166.624
  comprobaciones, 0 fallos en las tres; huellas de test_vdec las de
  siempre: 4D4A1D8B, 0657B177 y B3A2BCD3 (en qemu, ~30 min cada una).
  `tests-dkp` compila ahora también `ge.c`
  con devkitPPC (antes solo `ge_vertex.c` y los intérpretes): la racha se
  prueba con el compilador de la Wii.
- `tests/test_listas.c`, prueba nueva: 3000 listas con 0x2A y 0x2B
  mezclados (números al azar, por encima de 96 y junto al final de los 24
  bits), la parada dentro de la racha (750), la racha hasta el borde de la
  RAM con la parada nunca alcanzada o desalineada (750), en la copia sin
  caché; con el atajo, sin él y por `execute`: 0 distintas (2.270 rachas
  con 96.842 comandos; con `ge_verificar`, 1.119 comparadas, 0 distintas).
  Mismas listas en x86, PowerPC y devkitPPC (cada número al azar se saca
  aparte: el orden de los argumentos de una llamada cambia entre
  compiladores). Con el atajo estropeado a propósito
  (`ge_test_romper_racha`: un hueso, el número o el comando actual), la
  verificación lo ve las tres veces. Las listas al azar de antes cuentan
  también las rachas verificadas (96, 0 distintas).
- Mutaciones (27, `tools/banco/mutaciones/0.5.23.3.txt`, `mutar.py
  --solo listas`, 250 s; con el método de antes, ~3,5 horas):
  - **detectadas, 25:** 19 por las pruebas (índice ≤ 96 o < 95, índice o
    número con 8 bits, dato sin desplazar, contador de dos en dos, sin
    contar los 0x2A, 0x2B a 0 siempre o nunca, sin la máscara de 24 bits,
    comando actual fijo, comandos de menos, también fuera de la RAM, el
    atajo con `ge_verificar` o con los censos, y en el verificador: sin
    comparar huesos, número o comando actual, un comando menos) y 6 que
    dejan la lista dando vueltas (byte del comando equivocado, uno de más
    al final, sin la parada, la parada un comando después o sin mirar la
    alineación, el pc un comando antes);
  - **solo con ASan, 1:** leer un comando más allá del final de la RAM
    (en el repaso con ASan, 546 s);
  - **equivalente, 1:** el verificador sin deshacer los huesos (escribe
    dos veces lo mismo).
- pspautotests 275/275.
- A/B de `ge_cmd_huesos` en las 572 (exacto y MR, 4 a la vez): solo
  `io/`, `malloc` y savedata; de uno en uno quedan `io/file/rename` e
  `io/io/io`, que salen igual de distintos con `prof_vertices` (que solo
  mide): son de las carpetas de las pruebas, no del atajo.
- La 0.5.23.2 contra la 0.5.23.3 (`ab_versiones.py`, con los
  interruptores por defecto de cada una: también `ge_bbox_triangulos` 1
  frente a 0): 555 programas, solo `io/file/rename` distinto (0 dibujos,
  misma huella de GX).
- `ge_verificar` en las 110 de `gpu/` (exacto y MR): 0 líneas `[VERIF]`.
- Compilación para el Wii: `wiisp.dol` 0.5.23.3.

Autotest (4 registros y un `.capturas.txt`; todas con `prof_pmc`,
`prof_tirones`, `prof_ventana_juego` y `ventana 600`, y
`ge_bbox_triangulos 0`):
- sin: `ge_cmd_huesos 0`;
- con: `ge_cmd_huesos 1`;
- verif: con `ge_verificar` (camino nuevo);
- imagenes: como con, con `capturas 400` (las 12 de siempre).

Qué mirar:
- con frente a sin (`ventanas.py`): velocidad de las peleas, M ciclos,
  ciclos por comando y por llamada (comandos); rachas de huesos;
- verif: 0 distintas en "Verificacion de las rachas de huesos";
- imagenes: las 12 capturas de siempre.

### Resultado de la 0.5.23.3 en el Wii (DBZ: sin / con / verif / imagenes)

- Las 12 capturas de `imagenes`, las de siempre (9E8BC705 ... E9392291);
  las cuatro corridas ejecutan las mismas 1343,1 M instrucciones.
- verif: "Verificacion de las rachas de huesos" 0 distintas (109.725
  rachas con 4,18 M comandos en la 3600-4200, 117.450 con 4,45 M en la
  4200-4800, 29.884 con 1,13 M al final); todo lo demás, 0.
- con frente a sin (ciclos del PMC): 3600-4200 de 16.965 a 16.546 M
  (−2,5 %), 4200-4800 de 16.321 a 15.846 M (−2,9 %), final de 7.313 a
  7.145 M (−2,3 %). Comandos por llamada de 3.402 a 2.931 ciclos (−14 %),
  134 ciclos por comando (antes 156). Escena de 92,6 a 91,2 s (−1,5 %).
  Más de lo previsto (~1,5 %): las rachas (~38 comandos, como decía el
  censo de la 0.5.23) se quedan enteras en el bucle propio.

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
  (0.5.15), la recarga, `registro_hilo`, `gx_paralelo` y `prof_es`
  (0.5.16), 2.2 `cpu_predec` (0.5.17), la memoria del predecodificado y
  las medidas de la CPU (0.5.17.1), medidas por tiempo de juego, `prof_cpu`
  y `prof_bloques` (0.5.17.2), tarjetas VFPU parte a (0.5.18) y b
  (0.5.18.1), `cpu_enlace` y `cpu_syscall_slot` (0.5.19), la pila de
  retornos simulada y `prof_vertices` (0.5.19.1), 1.2c
  `ge_pantalla_rapida` (0.5.20), el censo `prof_bbox` y `ge_cmd_puntero`
  (0.5.21), 1.3 a `ge_bbox_rapido`, 1.3 b `ge_bbox`, `ge_cmd_matrices` y
  el censo `prof_comandos` (0.5.22), 1.3 c `ge_bbox_pantalla`, 1.3 d
  `ge_bbox_llamada` y el censo `prof_huesos` (0.5.23) y la prueba de cada
  llamada más barata (`ge_bbox_orden`, `ge_bbox_aabb`, 0.5.23.1) y la
  banda de guarda y el repaso por triángulos (`ge_bbox_guarda`,
  `ge_bbox_triangulos`, 0.5.23.2; el repaso, apagado desde la 0.5.23.3) y
  las rachas de huesos (`ge_cmd_huesos`, 0.5.23.3).
- Herramientas para medir sin el Wii (banco de qemu con el código de
  devkitPPC, perfil por función, psq_l emulado, A/B) y para leer los
  registros del Wii (`ventanas.py`): `tools/banco/LEEME.md`. El entorno
  (devkitPPC, qemu, pspautotests, PPSSPP): `tools/setup_entorno.sh`.
- **Siguiente:** leer los registros de la 0.5.23.3 (`ventanas.py sin=...
  con=... verif=... imagenes=...`):
  - con frente a sin: velocidad de las peleas, "M ciclos (PMC)", "ciclos
    por comando" y "ciclos por llamada (comandos)" (deberían bajar), y las
    rachas de huesos (filas nuevas; en la pelea, ~120 mil rachas y ~4,6 M
    comandos, datos y números, por ventana si el censo de la 0.5.23 sigue
    valiendo);
  - verif: "Verificacion de las rachas de huesos" con 0 distintas (y todo
    lo demás a 0);
  - imagenes: las 12 capturas de siempre (9E8BC705 ... E9392291).
  Después, el punto 4 (0.5.23.4, 1.4 `ge_charolas` y el diseño del IR).
  Para las mutaciones, `tools/banco/mutar.py` (con `--solo` de las suites
  del código tocado y `--asan` para el repaso de las que sobreviven).
  Lo que sigue es de antes de la 0.5.23:
  Si la prueba de cada llamada cuesta más de lo que salta, se apaga
  `ge_bbox_llamada` (su interruptor) o se limita a las cajas dudosas
  grandes. Después, 1.4 `ge_charolas` (juntar llamadas: lo que queda
  por llamada es `setup_targets`/`lookup_texture`, ~900 instrucciones, y
  la preparación de `ge_draw_prim`), y el paso 11 (diseño del IR, 2.3
  `cpu_fusion`, 2.5 `mem_bat`). Medido en la 0.5.20, en la pelea: CPU
  34 %, leer vértices 22 % (pantalla ~150 ciclos por vértice, transformar
  ~110, huesos ~76, decodificar ~54), ensamblar 10 %, enviar a GX 8 %,
  comandos 8 %. Otros candidatos: pantalla otra vez con paired singles
  (~3 %), transformar (la división por w), un solo `GX_Begin` por llamada,
  la parte b2 de la VFPU, la pila de retornos real (~0,5 %) y, en
  `ge_bbox`, no comparar otra vez las matrices en la prueba exacta (~350
  instrucciones por caja que se ve).
- Autotest: como mucho 4 registros y un `.capturas.txt` (el usuario no
  puede mandar más de 5 archivos en un mensaje). Siguiente también: las peleas (leer vértices: `round_to_screen`, la división por
  w y tres conversiones a entero por la memoria, IPC 0,66; los índices CI8
  con dcbt) y, con lo que mida `prof_es`, las lecturas del disco. Juntar
  llamadas (`ge_charolas`, 1.4) sigue aparte. Otros candidatos medidos:
  saltarse `setup_targets`/`lookup_texture` cuando no cambien (~900; va
  con 1.4), un solo `GX_Begin` por llamada, el 4444 de 64x64 de DBZ, el
  limitador de cuadros (leer los comandos con un puntero: hecho en la
  0.5.21, `ge_cmd_puntero`).

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
