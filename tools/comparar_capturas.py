#!/usr/bin/env python3
"""
WIISP - comparar_capturas.py
Compara dos <prefijo>.capturas.txt (una línea por captura, con la huella de
la imagen) y dice en qué cuadros salieron imágenes distintas. Sirve para
comprobar que una optimización no cambia la imagen: el mismo guion con el
interruptor apagado y encendido.

    python3 tools/comparar_capturas.py DBZ.iso.v1.capturas.txt DBZ.iso.v2.capturas.txt

SPDX-License-Identifier: GPL-2.0-or-later
"""
import re, sys

def leer(path):
    caps = {}
    for line in open(path, encoding='utf-8', errors='replace'):
        # El nombre del archivo puede llevar espacios (el de la ISO): se
        # toma todo hasta la palabra "imagen".
        m = re.match(r'cuadro (\d+)\s+(.+?)\s+imagen ([0-9A-F]{8})\s+(.*)', line)
        if m:
            caps[int(m.group(1))] = (m.group(3), m.group(2), m.group(4).strip())
    return caps

def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    a, b = leer(sys.argv[1]), leer(sys.argv[2])
    comunes = sorted(set(a) & set(b))
    if not comunes:
        print('Sin capturas en común (¿índices de antes de la 0.5.10, sin huella de imagen?)')
        return 2
    distintas = [c for c in comunes if a[c][0] != b[c][0]]
    print(f'{len(comunes)} capturas en común, {len(distintas)} distintas')
    for c in distintas:
        print(f'  cuadro {c}: {a[c][1]} ({a[c][0]}) != {b[c][1]} ({b[c][0]})')
    if a[comunes[0]][2] != b[comunes[0]][2]:
        print('opciones:\n  ' + a[comunes[0]][2] + '\n  ' + b[comunes[0]][2])
    return 1 if distintas else 0

if __name__ == '__main__':
    sys.exit(main())
