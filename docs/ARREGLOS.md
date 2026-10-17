# Registro de arreglos

Cada cambio en el código lleva el comentario `//Arreglo` y nada más (desde
la 0.5.17.2; antes se marcaban con `//arreglo`). Aquí va, por versión, qué
se arregló o cambió y una descripción breve. El detalle está en
`docs/CONTINUAR.md`.

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
