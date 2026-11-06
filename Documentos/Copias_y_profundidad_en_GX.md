# Copias del EFB y profundidad en GX (después de la 0.6.8)

Resumen de lo investigado tras la 0.6.8: el coste de las copias de la
mezcla doble y el moteado ("z-fighting") del suelo de DBZ en modo GX.
Mezcla lo medido en el Wii, lo leído en nuestro código y la investigación
del chat de capacidad máxima, que llegó con fuentes (PPSSPP, Dolphin,
hwtests, libogc, patentes de Nintendo/ArtX) y dos programas de prueba.

Etiquetas: **[Wii]** medido en nuestro Wii; **[código]** leído en WIISP;
**[fuente]** dicho por el otro chat con fuentes; **[sin confirmar]** nadie
lo ha medido.

## 1. Las copias de la mezcla doble

### Lo que ya sabíamos (0.6.8, pelea de DBZ, ventana 4200-4800)

- Velocidad sin / con / una_copia: 49,8 / 27,7 / 46,1 %. Casi todo el coste
  de la 0.6.7 son las recopias por solape: 24.000-51.000 por ventana,
  ~6-7 por llamada.
- Cada copia es el búfer entero, más otra copia de los índices A8, más
  `GX_PixModeSync` y `GX_InvalidateTexAll`. En las recopias por solape solo
  había cambiado el ~8-13 % de las filas.
- La primera copia de cada llamada y las 13 etapas del TEV cuestan solo
  ~4-8 puntos (una_copia frente a sin). **El TEV no es el problema.**

### Lo que aporta la investigación

- **Coste de `GX_CopyTex`** [fuente, patente US 7.184.059]: 4 píxeles por
  ciclo, en tiles de 32×32. Una copia de 480×272 en RGBA8 son unos 32.640
  ciclos, ~134 µs a 243 MHz. Ocupa el ancho de banda de MEM1 mientras dura.
  Nuestra copia A8 recorre otra vez todos los píxeles, así que cada recopia
  cuesta casi el doble.
- **`GX_PixModeSync`** [fuente]: ≥ 90 ciclos. Basta uno por tanda de copias
  antes de la primera primitiva que las lea.
- **`GX_InvalidateTexAll`** [fuente]: ~512 ciclos, más volver a leer las
  texturas que tenía la caché.
- **`GX_InvalidateTexRegion`** [fuente]: 16 ciclos por cada 32 KB.
  - Ojo [código, libogc]: trabaja sobre zonas de la caché de texturas (TMEM),
    no sobre direcciones de memoria.
  - libogc reparte las texturas entre zonas, así que para usarlo habría que
    fijarle una zona propia a la copia.
  - Ahorro pequeño (~500 frente a ~35.000 ciclos): para más adelante.
- **Copiar solo un rectángulo dentro de la misma textura es válido**
  [fuente: documentación de `GX_SetTexCopyDst`, Dolphin]:
  - En RGBA8, x0, y0, ancho y alto tienen que ser múltiplos de 4 (tiles de
    4×4), redondeando hacia fuera.
  - El destino es `tex + (y0/4)·(w/4)·64 + (x0/4)·64`, con el paso de la
    textura completa (`GX_SetTexCopyDst(w, ...)` en cada copia).
  - El ahorro es proporcional al área. Que el coste sea lineal en
    rectángulos pequeños está **[sin confirmar]**.
- **Filtro de copia** [código]: antes de cada copia de textura ya ponemos el
  filtro identidad (`copy_filter_none`, `gx_ge.c`). El antiparpadeo solo se
  usa en la copia a pantalla y se quita justo después. La trampa del filtro
  compartido no nos afecta.
- **Exactitud** [código]: la copia parcial es exacta, porque:
  - la primera copia de cada llamada es completa;
  - los borrados y los blits fuerzan otra copia;
  - entre dos copias solo se dibujan primitivas de la lista `gm_*`.

  El rectángulo que cubre esa lista es todo lo que cambió.
- **Prueba de solape** [código]: `gm_separados` (`gx_mezcla.h`) trata como
  separados dos triángulos que solo comparten una arista. Las recopias son
  solapes reales, no aristas de la malla.

### Ideas descartadas o aplazadas

- **El truco de PPSSPP para la máscara (commit 361f473f):** no aplica.
  Nosotros hacemos la máscara con operación lógica (0.6.5), sin copias.
- **Camino barato con alfa ≤ 127** (dejar la mezcla al mezclador de GX):
  - Recorrí las 8,4 millones de combinaciones con `error_mezcla_gx.py`. Lo
    mejor queda en ±1 (83 % exactas con el alfa ajustado), nunca exacto.
  - Ese cálculo no incluye el tramado. La PSP suma el tramado antes de
    recortar el resultado de la mezcla, y el mezclador de GX no puede sumar
    nada después de mezclar. Estas llamadas llevan tramado, así que el error
    sería mayor.
  - Además, el 80 % de los píxeles puede pasar de 127.
  - De paso confirma el suelo brillante de antes de la 0.6.7: el método de
    PPSSPP para todo el alfa da errores de hasta 254 con α ≥ 129.
- **Leer el EFB desde el TEV:** no se puede [fuente]. El EFB solo se lee
  después del TEV (mezcla, operación lógica y z).
- **Capas sin solape** (agrupar las primitivas de una llamada en capas y
  copiar solo entre capas): exacto, pero hay que medir primero cuántas capas
  salen por llamada.

## 2. El moteado del suelo (z-fighting)

### El suelo en el censo de la 0.6.7 [código]

- Todo el terreno usa la mezcla doble (modo 0x076), el mismo formato de
  vértices (0x11A) y tiras.
  - 1.362 dibujos con prueba GEQUAL que escriben z.
  - 213 (más 44 con textura de formato 5) que no la escriben: son las capas.
- WIISP manda cada triángulo a GX como `GX_TRIANGLES`, con los vértices en
  el orden en que los arma el GE. x e y van en 1/16 de píxel y z entera,
  como las deja el GE. El camino de un triángulo y el de lotes mandan lo
  mismo.
- La proyección (`load_pixel_projection`, `gx_ge.c`) usa un viewport de
  1024×1024 y lleva la z de la PSP (0..65535) a 0..65535·256, según el
  minz/maxz de cada dibujo.

### La z en la PSP [fuente: PPSSPP, pspautotests]

- Por vértice, la z sale entera (floor).
- Por píxel, la interpolación es lineal en pantalla, con un plano en punto
  fijo anclado por la geometría.
- La prueba de z compara enteros de 16 bits.
- Rotar los vértices de un triángulo no cambia la z [Wii de la PSP:
  `nearclip.expected`]. Según el modelo, reflejarlos tampoco.
- Con el modelo de PPSSPP:
  - rotar, reflejar, tira contra lista, abanico y partir en lotes dan 0
    diferencias;
  - triangular por la otra diagonal o partir el triángulo en el CPU sí
    cambia la z.

  El juego tiene que dibujar las capas con los mismos triángulos.

### La prueba en el Wii (`wiisp_ztest`, mapeo ajustado al de WIISP) [Wii]

Un suelo de 768 triángulos: la pasada A escribe z y la pasada B se dibuja
encima con GEQUAL y sin escribir z, como las capas de DBZ. Se cuentan los
píxeles que fallan, sobre 56.576.

| Prueba | Fallos | Máx. diferencia (Z24) |
|---|---|---|
| `identico` / `w_identico` (B = A) | 0 / 0 | 0 |
| `rot1` (v1, v2, v0) | 44.454 | 53 |
| `rot2` (v2, v0, v1) | 20.842 | 30 |
| `refl` (v0, v2, v1) | 0 | 0 |
| `tira_vs_lst` | 18.691 | 50 |
| `tira_vs_o01` (impares de la tira como s2, s1, s3) | 0 | 0 |
| `abanico`, `fmt_u16`, `fmt_idx16`, `lote_x_tri`, `x16_vs_x24` | 0 | 0 |
| `zfrz_id`, `zfrz_rot1` (`GX_SetCoPlanar`) | 0 | 0 |
| `rot1_ztarde` (zcomploc tardío) | igual que `rot1` | 53 |
| `w_rot1` / `w_refl` / `w_tira` (mapeo de WIISP) | 44.869 / 0 / 19.103 | 52 / 0 / 50 |
| `rot1/6`, `refl/6` (EFB en RGBA6) | igual que en RGB8 | — |
| `sesgo_rot1`, `sesgo_tira` | sesgo mínimo sin fallos: 64 | — |

Conclusiones:

1. **GX es determinista:** los mismos vértices en el mismo orden dan la
   misma z.
2. **En GX la z depende de qué vértice va primero.** Rotar cambia la z
   hasta ~53 unidades de Z24 (~0,2 de una unidad de la PSP); reflejar
   manteniendo el primer vértice no la cambia. GX ancla el plano en el
   primer vértice. Es bastante menos preciso que el modelo por software de
   Dolphin, que predecía como mucho 2 unidades.
3. **La PSP no depende del orden; GX sí.** Si el juego dibuja una capa con
   los mismos triángulos pero empezando por otro vértice, en la PSP empata
   y en GX aparece el moteado. Es la explicación más probable del
   z-fighting del suelo. Falta confirmarla con el juego (punto 2.4).
4. No cambian la z: el formato de las posiciones (F32, U16, indexado),
   partir en lotes, zcomploc, el formato del EFB ni nuestro mapeo en float.
5. **`GX_SetCoPlanar` corrige el orden**, pero necesita un triángulo de
   referencia y duplica el trabajo de preparación.
6. **En una tira, GX arma los impares como (s2, s1, s3).**
7. **Calibración:** la z guardada no es exactamente z·256.
   - Con los mapeos exactos queda +1 por debajo de la mitad del rango.
   - Con el nuestro, entre −1 y +2 (+1 casi siempre, +2 en z=255, −1 cerca
     del máximo).

   Es igual en todas las pasadas, así que no causa el moteado.
8. **Curiosidad:** en `gb_rot1` (triángulos enormes dentro de la banda de
   guarda) la prueba de z falló mucho menos de lo que predice la z leída
   (1.174 frente a 30.674). No afecta al suelo.
9. **Partir un triángulo en el CPU** (`split4`) cambia la z hasta ~64
   unidades. Mientras el suelo y sus capas pasen por el mismo recorte, no
   nos afecta.

### Plan para el z-fighting

1. **Medir en el juego.** Por cada triángulo de los dibujos con prueba de
   z, dos huellas: una de la geometría sin importar el orden y otra del
   orden en que se manda. También el minz/maxz de cada dibujo.
   - Si una capa coincide con el suelo en geometría pero no en orden,
     queda confirmado.
2. **Arreglo propuesto, con su interruptor:** primer vértice canónico.
   - Antes de mandar cada triángulo a GX, rotarlo para que empiece por un
     vértice elegido con una regla solo geométrica (por ejemplo, el de
     menor y, y después menor x).
   - Rotar no cambia el sentido de giro, y el color plano ya se pasa
     aparte.
   - Por `rot1`, `rot2` y `refl`, eso deja a GX tan invariante como la PSP
     para triángulos con la misma geometría.
3. **Alternativas descartadas:** `GX_SetCoPlanar` (caro), y un sesgo de 64
   unidades en las pasadas sin escritura (no es exacto).

## 3. Orden acordado

1. **0.6.9 (hecha):** copia parcial del rectángulo cambiado en las
   recopias por solape (interruptor `gx_mezcla_parcial`), y sin la copia A8
   en todas las copias de la mezcla doble (`gx_mezcla_sin_a8`; lo que lee
   el búfer con paleta lo vuelve a copiar entero al cambiar de destino).
   - Comprobación en `prueba_gx` de que la copia parcial es idéntica byte a
     byte a la completa, con tiempos de copia.
   - Medida de las huellas de profundidad.
   - Autotest de 4 corridas: con, parcial, parcial + sin A8, una_copia.
2. **0.6.10 (hecha):** la 0.6.9 lo confirmó (en la pelea, entre 45.000 y
   132.000 triángulos de las capas por tramo llegaban con otro primer
   vértice); el primer vértice canónico con su interruptor
   (`gx_vertice_canonico`).
3. **Para más adelante (velocidad):**
   - Mandar las posiciones en 16 bits con fracción: `fmt_u16` da resultado
     idéntico.
   - Mandar tiras en vez de triángulos sueltos. Choca con el vértice
     canónico, porque la tira impone el orden.
   - `GX_InvalidateTexRegion` con una zona propia, y un solo
     `GX_PixModeSync` por tanda.
   - Capas sin solape, si la medida lo justifica.

## Fuentes citadas por la investigación

- PPSSPP: `GPU/Software/GEMath.h`, `Rasterizer.cpp`, `TransformUnit.cpp`,
  `Clipper.cpp`, `DrawPixel.cpp`; `GPU/Common/GPUStateUtils.cpp`,
  `VertexShaderGenerator.cpp`, `DrawEngineCommon.cpp`; `assets/compat.ini`;
  commit 361f473f.
- pspautotests: `gpu/depth/transformprecision`, `gpu/depth/precision`,
  `gpu/exact/planes`, `gpu/exact/nearclip`, `gpu/clipping/xycull`.
- Dolphin: `VideoBackends/Software/Rasterizer.cpp`, `SWEfbInterface.cpp`;
  `VideoCommon/BPMemory.h`, `BPStructs.cpp`, `PixelShaderGen.cpp`,
  `TextureCacheBase.cpp`, `TextureConversionShader.cpp`; PR #1812
  (zfreeze).
- hwtests: `gxtest/clipping.cpp`, `gxtest/rasterization.cpp`,
  `gxtest/copyfilter.cpp`.
- libogc: `gc/ogc/gx.h`, `libogc/gx.c`.
- Patentes: US 7.184.059 (copias), US 6.636.214 (rasterizador),
  US 7.034.828 (TEV).
