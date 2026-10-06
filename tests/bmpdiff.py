#!/usr/bin/env python3
"""
WIISP - bmpdiff.py
Compara dos capturas BMP de 512x272 (las de pspautotests) y genera un PNG
con tres paneles: referencia | WIISP | diferencias (en rojo), a escala 2x
si se pide. Sin dependencias externas.

    python3 tests/bmpdiff.py esperado.bmp obtenido.bmp salida.png [--zoom N] [--crop x,y,w,h]

SPDX-License-Identifier: GPL-2.0-or-later
"""
import struct, sys, zlib

def load(path):
    d = open(path, 'rb').read()[54:]
    img = []
    for y in range(272):
        row = d[(271 - y) * 2048:(272 - y) * 2048]   # el BMP va de abajo arriba
        img.append([(row[x * 4 + 2], row[x * 4 + 1], row[x * 4]) for x in range(512)])
    return img

def save_png(path, rows):
    h, w = len(rows), len(rows[0])
    raw = b''.join(b'\x00' + bytes(c for px in r for c in px) for r in rows)
    def chunk(t, b):
        return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))

def main():
    args = sys.argv[1:]
    zoom, crop = 1, (0, 0, 480, 272)
    if '--zoom' in args:
        i = args.index('--zoom'); zoom = int(args[i + 1]); del args[i:i + 2]
    if '--crop' in args:
        i = args.index('--crop'); crop = tuple(int(v) for v in args[i + 1].split(',')); del args[i:i + 2]
    a, b = load(args[0]), load(args[1])
    x0, y0, cw, ch = crop
    out = []
    bad = 0
    for y in range(y0, y0 + ch):
        row = []
        for panel in range(3):
            for x in range(x0, x0 + cw):
                pa, pb = a[y][x], b[y][x]
                if panel == 0: px = pa
                elif panel == 1: px = pb
                else:
                    px = (255, 0, 0) if pa != pb else (pa[0] // 4, pa[1] // 4, pa[2] // 4)
                    bad += pa != pb
                row.extend([px] * zoom)
            row.extend([(255, 255, 255)] * 2)
        for _ in range(zoom):
            out.append(row)
    save_png(args[2], out)
    print('%d píxeles distintos de %d' % (bad, cw * ch))

main()
