# Cómo seguir con WIISP (traspaso entre sesiones)

Este documento resume dónde se quedó el trabajo, para seguir en una sesión
nueva sin perder nada. Léelo junto con `README.md`, `ARQUITECTURA.md` y
`docs/OPTIMIZACION.md`.

## Reglas del proyecto

- Todo en español: respuestas, comentarios del código y mensajes de commit.
  El texto que se ve en la pantalla del Wii va **sin acentos** (la fuente no
  los tiene).
- Rama de trabajo: `claude/psp-wii-emulator-dt141p` hasta la v0.4.3; la
  v0.4.4 sigue encima en `claude/wiisp-psp-wii-emulator-gr0b83` (la rama
  que asigna cada sesión puede cambiar: partir siempre de la más reciente).
  No abrir pull requests si no se piden.
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
- Dolphin (opcional, lento de compilar) sirve para probar el .dol:
  `tools/dolphin_autotest.py` (necesita Xvfb).
- CLI de PC: `build-pc/wiisp-cli --run --frames N --log salida.log juego.iso`
  deja el mismo registro que `wiisp.log` en el Wii, y al llegar al límite de
  frames vuelca el estado de los hilos (`[DIAGNOSTICO]`).

**Paquete para el usuario**: un zip con `apps/wiisp/boot.dol` (copia de
`wiisp.dol`) y `apps/wiisp/meta.xml` (copia de `dist/apps/wiisp/meta.xml`,
subiendo su `<version>`). Se descomprime en la raíz de la SD.

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

## Siguientes pasos, por prioridad

1. **Que el usuario pruebe los controles de la v0.5.0** (sobre todo el
   sentido de la cruceta por movimiento) y, después, seguir con las
   optimizaciones del GE en el orden de arriba (sin frameskip ni dynarec
   por ahora, a petición del usuario). Pendiente aparte: DBZ TTT se para en
   `sceKernelCheckThreadStack` (no implementada; devuelve 0 y su libc se
   descarga sola). El usuario pidió no tocarlo todavía.

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
