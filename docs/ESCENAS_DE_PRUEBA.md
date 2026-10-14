# Escenas de prueba repetibles

Para medir una optimización hay que correr **exactamente la misma escena**
con y sin ella. WIISP lo hace con un **guion de botones**: un archivo de
texto que dice qué hay pulsado en cada cuadro emulado. Como todo el tiempo
de la PSP (incluida la hora del RTC) sale de los ciclos emulados y no del
reloj del Wii, los mismos botones en los mismos cuadros llevan siempre al
mismo sitio.

## 1. Grabar el guion en el Wii

1. Copia el juego a la SD, por ejemplo `sd:/ISO/DBZ.iso`.
2. (Opcional) Si quieres empezar con una partida guardada, copia su carpeta
   de `SAVEDATA` (p. ej. `ULUS10537...`) dentro de
   `sd:/ISO/DBZ.iso.partidas_base/`. Sin ella, el juego empieza como en una
   PSP nueva.
3. En WIISP abre el juego y, antes de pulsar A, pulsa **+** (Wiimote) o
   **R** (GameCube) hasta que diga `guion GRABAR`. Cada pulsación pasa a la
   siguiente opción: `no` → `GRABAR` → `REPRODUCIR` → `no`.
4. Juega normalmente desde el arranque hasta la escena que quieres medir y
   un rato dentro de ella (por ejemplo 60 s de pelea). Arriba se ve
   `GRABANDO`. No uses el turbo mientras grabas.
5. Sal con HOME. Queda `sd:/ISO/DBZ.iso.guion` (el anterior pasa a
   `.guion.bak`) y `DBZ.iso.guion.huellas`.

Mientras se graba:

- Las partidas salen de `DBZ.iso.partidas`, que se vacía y se rellena con
  `partidas_base` al empezar. Así, lo que el juego guarde durante la
  prueba no cambia la siguiente corrida.
- El teclado en pantalla no se abre (el juego recibe el texto que propone),
  porque lo que se escribiera no quedaría en el guion.
- El stick se redondea (zona muerta y pasos de 8) para que el guion no
  tenga una línea por cada temblor; el juego ya recibe el valor redondeado.
- Cada 400 cuadros se guarda una captura (`DBZ.iso.guion.c000400.bmp`...).

El guion se puede retocar a mano. Por ejemplo, para añadir capturas de
referencia o cambiar dónde termina:

```
# cuadro  botones (se mantienen hasta la siguiente línea)
600   START
606   -
900   CRUZ
1500  DERECHA stick 255 128
3000  captura
5400  fin
```

Botones: `SELECT START ARRIBA DERECHA ABAJO IZQUIERDA L R TRIANGULO CIRCULO
CRUZ CUADRADO`, `-` (nada) y `stick X Y` (0-255, 128 es el centro). El
formato completo está en `src/frontend/guion.h`.

## 2. Reproducirla desde el menú

1. Abre el juego y pulsa **+** / **R** hasta que diga `guion REPRODUCIR`.
2. Pulsa A. El juego se juega solo con el guion (arriba se ve `GUION`); el
   mando no hace nada salvo HOME, que corta la reproducción.
3. Al llegar a la línea `fin` se para y vuelve al menú. Las salidas quedan
   junto al juego como `DBZ.iso.r1.*`; la siguiente reproducción será
   `r2`, luego `r3`... (nunca se pisan, para poder compararlas).

| Archivo | Qué es |
|---|---|
| `<juego>.rN.log` | registro de esa corrida, con `[OPT]` y `[TIEMPOS]` cada 30 s |
| `<juego>.rN.huellas` | huella del estado cada 60 cuadros |
| `<juego>.rN.c<cuadro>.bmp` | una captura cada 400 cuadros y las que pida el guion |
| `<juego>.rN.capturas.txt` | cada captura con las opciones con que corría (GX/software, MR/ME, TN/TS, texturas, turbo, `[OPT]`) |

Si cambias MR/ME, TN/TS o el turbo durante la reproducción, las capturas
siguientes lo apuntan.

Las capturas son la imagen de la PSP (512x272), sin el texto de arriba.
Guardarlas en la SD tarda un poco, así que para **medir velocidad** conviene
quitarlas (`capturas 0` en `autotest.txt`, abajo).

## 3. Reproducir varias veces: `autotest.txt`

Para la línea base (`veces 3`) o varios juegos seguidos,
`sd:/wiisp/autotest.txt` (o junto al `boot.dol`) hace que WIISP corra la
lista sin mandos y **apague el Wii al terminar**. Ejemplo:

```
gx
veces 3
sd:/ISO/DBZ.iso
sd:/ISO/SFA3.iso
```

- Si existe `<juego>.guion`, se reproduce hasta su línea `fin`.
- `veces N` corre N veces cada juego que sigue; las salidas llevan `.v1`,
  `.v2`, ...
- `opt NOMBRE 0|1` cambia un interruptor de optimización para los juegos
  que siguen (nombres en `src/core/opt.h` y en `docs/HOJA_DE_RUTA.md`).
- `capturas N` toma una captura cada N cuadros (400 por defecto; `capturas
  0` para medir velocidad sin escribir en la SD).
- `gx` / `soft` eligen el renderizador; `frames N` es el límite para los
  programas sin guion.

**Al terminar, borra o renombra `autotest.txt`**: mientras exista, WIISP
corre las pruebas en cada arranque y apaga el Wii sin llegar al menú.

Por cada corrida quedan, junto al juego, los mismos archivos que arriba
pero con `.vN` en vez de `.rN`, y además:

| Archivo | Qué es |
|---|---|
| `<juego>.vN.gx.bmp` | la última imagen |
| `<juego>.vN.stats` | FPS, MIPS y tiempos del GE de la corrida |

## 4. Comprobar que la escena se repite

```sh
python3 tools/comparar_huellas.py DBZ.iso.r1.huellas DBZ.iso.r2.huellas
cmp DBZ.iso.r1.c000400.bmp DBZ.iso.r2.c000400.bmp     # o, para ver dónde:
python3 tests/bmpdiff.py DBZ.iso.r1.c000400.bmp DBZ.iso.r2.c000400.bmp diff.png
```

`IDENTICAS` quiere decir que la escena es repetible. Si no, el script dice
el primer cuadro en el que las corridas se separaron: ahí algo depende del
reloj del Wii o de algo que no se reinicia entre corridas. Encontrarlo
también es un resultado.

Las huellas de la grabación (`.guion.huellas`) también se pueden comparar
con las de una reproducción: deberían coincidir.

## 5. Medir

La velocidad se mide **solo en el Wii**, porque Dolphin no emula los
tiempos de caché del Broadway. Dolphin sirve para la exactitud (capturas).
Para cada optimización:

1. Línea base: `capturas 0` y `veces 3` con todo como está; guardar los
   logs. Las capturas de referencia, en otra corrida con capturas.
2. Activar la optimización (`opt nombre 1`) y repetir.
3. Comparar los `[TIEMPOS]` (segundos de juego por cada 30 s reales) y las
   capturas contra la base.

## En el PC

```sh
build-pc/wiisp-cli --run --guion juego.guion --salida salida/juego \
    --partidas salida/juego.partidas --opt ge_vdec_esp=1 --log salida/juego.log juego.iso
```

`--capturas-guion N` cambia la frecuencia de las capturas (0 = ninguna).

`--grabar FILE` graba desde el teclado del CLI de Windows.
