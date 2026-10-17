#!/usr/bin/env python3
"""
WIISP - tests/guest/mkiso.py
Una imagen de UMD mínima (ISO 9660, o CSO con --cso) para las pruebas de
tests/guest que leen del disco: el programa va en PSP_GAME/SYSDIR/BOOT.BIN
y los demás archivos donde se diga. Los datos de prueba no se guardan en el
repositorio: "patron:N" son N bytes de la fórmula de patron() (la misma que
comprueba el programa).

    python3 mkiso.py salida.iso [--cso] ruta/en/el/disco=archivo|patron:N ...

SPDX-License-Identifier: GPL-2.0-or-later
"""
import struct, sys, zlib

SECTOR = 2048


def patron(n):
    """byte i = (i ^ i>>8 ^ i>>16) * 13 + 7: cada sitio del archivo tiene su valor"""
    return bytes((((i ^ (i >> 8) ^ (i >> 16)) * 13 + 7) & 0xFF) for i in range(n))


def both16(v):
    return struct.pack('<H', v) + struct.pack('>H', v)


def both32(v):
    return struct.pack('<I', v) + struct.pack('>I', v)


def record(name, lba, size, is_dir):
    """Registro de directorio (nombre ya en bytes; \\0 y \\1 son . y ..)"""
    n = len(name)
    length = 33 + n + (1 if n % 2 == 0 else 0)
    r = bytes([length, 0]) + both32(lba) + both32(size)
    r += bytes([126, 1, 1, 0, 0, 0, 0])   # 2026-01-01 00:00:00
    r += bytes([2 if is_dir else 0, 0, 0]) + both16(1) + bytes([n]) + name
    return r + b'\0' * (length - len(r))


def build(entries):
    """entries: {ruta: bytes}. Devuelve la imagen."""
    # Árbol de carpetas
    tree = {'': {}}
    for path in sorted(entries):
        parts = path.upper().split('/')
        for k in range(1, len(parts)):
            tree.setdefault('/'.join(parts[:k]), {})
        tree.setdefault('/'.join(parts[:-1]), {})[parts[-1]] = path
    for d in list(tree):
        if d:
            parent = '/'.join(d.split('/')[:-1])
            tree[parent][d.split('/')[-1]] = None   # subcarpeta
    dirs = sorted(tree, key=lambda d: (d.count('/') if d else -1, d))
    # Sitio: 16 de sistema, PVD, fin, tabla de rutas (vacía), carpetas (un
    # sector cada una), archivos
    lba = 20
    dir_lba = {}
    for d in dirs:
        dir_lba[d] = lba
        lba += 1
    file_lba = {}
    for path in sorted(entries):
        file_lba[path] = lba
        lba += (len(entries[path]) + SECTOR - 1) // SECTOR or 1
    total = lba
    img = bytearray(total * SECTOR)
    for d in dirs:
        parent = '/'.join(d.split('/')[:-1]) if d else ''
        data = record(b'\0', dir_lba[d], SECTOR, True) + record(b'\1', dir_lba[parent], SECTOR, True)
        for name in sorted(tree[d]):
            target = tree[d][name]
            if target is None:
                sub = (d + '/' if d else '') + name
                data += record(name.encode(), dir_lba[sub], SECTOR, True)
            else:
                data += record((name + ';1').encode(), file_lba[target], len(entries[target]), False)
        assert len(data) <= SECTOR, 'carpeta demasiado grande para un sector'
        img[dir_lba[d] * SECTOR:dir_lba[d] * SECTOR + len(data)] = data
    for path, data in entries.items():
        img[file_lba[path] * SECTOR:file_lba[path] * SECTOR + len(data)] = data
    pvd = bytearray(SECTOR)
    pvd[0:7] = b'\x01CD001\x01'
    pvd[8:40] = b'WIISP'.ljust(32)
    pvd[40:72] = b'WIISP_PRUEBA'.ljust(32)
    pvd[80:88] = both32(total)
    pvd[120:124] = both16(1)
    pvd[124:128] = both16(1)
    pvd[128:132] = both16(SECTOR)
    pvd[132:140] = both32(10)
    pvd[140:144] = struct.pack('<I', 18)
    pvd[148:152] = struct.pack('>I', 19)
    pvd[156:190] = record(b'\0', dir_lba[''], SECTOR, True)
    pvd[881] = 1
    img[16 * SECTOR:17 * SECTOR] = pvd
    img[17 * SECTOR:17 * SECTOR + 7] = b'\xffCD001\x01'
    # Tablas de rutas mínimas (solo la raíz; el emulador no las usa)
    img[18 * SECTOR:18 * SECTOR + 10] = bytes([1, 0]) + struct.pack('<I', dir_lba['']) + struct.pack('<H', 1) + b'\0\0'
    img[19 * SECTOR:19 * SECTOR + 10] = bytes([1, 0]) + struct.pack('>I', dir_lba['']) + struct.pack('>H', 1) + b'\0\0'
    return bytes(img)


def cso(img):
    """CISO v1: bloques de un sector; los que no encogen van sin comprimir"""
    n = len(img) // SECTOR
    header = b'CISO' + struct.pack('<IQIBB', 24, len(img), SECTOR, 1, 0) + b'\0\0'
    pos = 24 + (n + 1) * 4
    index, blocks = [], []
    for k in range(n):
        raw = img[k * SECTOR:(k + 1) * SECTOR]
        c = zlib.compressobj(9, zlib.DEFLATED, -15)
        comp = c.compress(raw) + c.flush()
        if len(comp) < SECTOR:
            index.append(pos)
            blocks.append(comp)
        else:
            index.append(pos | 0x80000000)
            blocks.append(raw)
        pos += len(blocks[-1])
    index.append(pos)
    return header + b''.join(struct.pack('<I', v) for v in index) + b''.join(blocks)


def main():
    args = sys.argv[1:]
    out = args.pop(0)
    use_cso = '--cso' in args
    args = [a for a in args if a != '--cso']
    entries = {}
    for a in args:
        path, src = a.split('=', 1)
        entries[path] = patron(int(src[7:])) if src.startswith('patron:') else open(src, 'rb').read()
    img = build(entries)
    open(out, 'wb').write(cso(img) if use_cso else img)


if __name__ == '__main__':
    main()
