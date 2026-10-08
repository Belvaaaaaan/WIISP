# Cómo seguir con WIISP (traspaso entre sesiones)

Este documento resume dónde se quedó el trabajo, para seguir en una sesión
nueva sin perder nada. Léelo junto con `README.md`, `ARQUITECTURA.md` y
`docs/OPTIMIZACION.md`.

## Reglas del proyecto

- Todo en español: respuestas, comentarios del código y mensajes de commit.
  El texto que se ve en la pantalla del Wii va **sin acentos** (la fuente no
  los tiene).
- Rama de trabajo: `claude/psp-wii-emulator-dt141p`. No abrir pull requests
  si no se piden.
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

## Siguientes pasos, por prioridad

1. **Mandar v0.4.3 al usuario** (si no se hizo ya) y pedir el nuevo
   `wiisp.log`. Lo siguiente que haga GTA tras recorrer el disco dirá dónde
   se para ahora.

2. **UMD como en PPSSPP** (`Core/HLE/sceUmd.cpp`):
   - El UMD empieza **activado** (`umdActivated = true`): `umd_activated = 1`
     en `io_init` (con disco, `sceUmdGetDriveStat` da 0x32 desde el inicio).
   - `sceUmdWaitDriveStat`, `...WithTimer` y `...CB` deben **esperar de
     verdad**: si `(stat & 0x3B) == 0` → 0x80010016; en interrupción →
     0x80020064; sin dispatch → 0x800201A7; gastan 520 ciclos; si
     `stat & estado` ya se cumple devuelven 0 (WithTimer/CB además
     replanifican); si no, `kernel_wait_object_timeout(KWAIT_UMD, stat, t)`
     (t = 0: para siempre; t ≤ 209 µs → 240; en WithTimer, t ≤ 1 → 25) y al
     vencer devuelven 0x800201A8. Al activar/desactivar,
     `kernel_wake_object_mask(KWAIT_UMD, estado, 0)`. Registrar con
     `kernel_set_wait_recheck` una función que mire `stat & estado`.
   - `sceUmdCancelWaitDriveStat`: el NID 0x6AF9B50A sale como
     `sceUmd_6AF9B50A` en `src/hle/nid_names.c`; renombrarlo y despertar a
     todos con 0x800201A9.

3. **LwMutex como objetos del kernel** (portar `Core/HLE/sceKernelMutex.cpp`,
   líneas ~596-1010 de PPSSPP). Hoy WIISP no crea objetos: si la memoria del
   workarea se libera y se reutiliza, `sceKernelLockLwMutex` ve basura y
   espera para siempre; en la PSP da error 0x800201CA porque el UID ya no
   existe. Esto bloquea el marco de pruebas de los 20 pspautotests
   `threads/callbacks/*` (comprobado con `threads/callbacks/check`: el hilo
   se queda en `lwmutex 088421E0`). Con esto arreglado esas pruebas
   validarán los callbacks contra una PSP real. Detalles: tabla de objetos
   con UID en workarea+16; `Create` valida nombre, attr < 0x400, cuenta;
   `Delete` despierta a los que esperan con 0x800201B5 y deja el workarea
   con lockLevel 0, lockThread -1, uid -1; al ir a esperar o al tomar uno
   libre con lockThread ≠ 0, comprobar que el UID existe; despertar en
   orden FIFO o de prioridad (attr 0x100); `wait_index` = índice del
   objeto. GTA no usa LwMutex, pero las pruebas sí.

4. **Event flags**: despertar en orden de llegada (`wait_seq`) o de
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
