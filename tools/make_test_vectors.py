#!/usr/bin/env python3
"""
WIISP - make_test_vectors.py
Genera datos de prueba sin depender de archivos de juegos:

  - Cifra un ELF como un EBOOT.BIN de PSP ("~PSP", etiqueta de tipo 2) con
    las mismas claves que usa el descifrador (el camino inverso de
    decrypt_type_seed en src/loader/prx_decrypt.c).
  - Crea una imagen ISO 9660 de UMD a partir de una carpeta, y su CSO
    (deflate) o ZSO (LZ4).

    python3 tools/make_test_vectors.py prx entrada.elf salida.bin [--tag 0xD91609F0] [--gzip]
    python3 tools/make_test_vectors.py iso CARPETA salida.iso|.cso|.zso [--block 2048]
    python3 tools/make_test_vectors.py header tests/vectors.h   # vectores de las pruebas

Necesita el módulo cryptography de Python.

SPDX-License-Identifier: GPL-2.0-or-later
"""
import gzip, hashlib, os, re, struct, sys, zlib
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def parse_bytes(text):
    return bytes(int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})\b', text))


def load_keys():
    kirk = open(os.path.join(ROOT, 'src/loader/kirk_keys.h')).read()
    rows = re.findall(r'\{((?:\s*0x[0-9A-Fa-f]{2},?){16})\s*\}', kirk[kirk.index('kirk_keys['):kirk.index('kirk1_key')])
    vault = {0x40 + i: parse_bytes(r) for i, r in enumerate(rows)}
    kirk1 = parse_bytes(kirk[kirk.index('kirk1_key[16]'):].split(';')[0])
    prx = open(os.path.join(ROOT, 'src/loader/prx_keys.h')).read()
    named = {m.group(1): parse_bytes(m.group(2)) for m in
             re.finditer(r'static const u8 (\w+)\[\d*\]\s*=\s*\{([^}]*)\}', prx)}
    tags = {}
    tab = prx[prx.index('g_tagInfo2[]'):]
    for m in re.finditer(r'\{\s*(0x[0-9A-Fa-f]+),\s*(\w+),\s*(0x[0-9A-Fa-f]+)(?:,\s*(\d+))?', tab):
        tags[int(m.group(1), 16)] = (named[m.group(2)], int(m.group(3), 16), int(m.group(4) or 0))
    return vault, kirk1, tags


def cbc(key, data, enc):
    c = Cipher(algorithms.AES(key), modes.CBC(b'\0' * 16))
    o = c.encryptor() if enc else c.decryptor()
    return o.update(data) + o.finalize()


def encrypt(elf, tag=0xD91609F0, compress=False):
    vault, kirk1, tags = load_keys()
    seed, code, typ = tags[tag]
    assert typ == 2, 'solo etiquetas de tipo 2'
    body = gzip.compress(elf, mtime=0) if compress else elf
    data_size = len(body)
    padded = body + b'\0' * (-len(body) % 16)

    # xorbuf: la semilla expandida (9 bloques numerados, KIRK 7)
    blocks = b''.join(bytes([i]) + seed[1:16] for i in range(9))
    xorbuf = cbc(vault[code], blocks, False)

    # Cabecera ~PSP (0x80 bytes en claro)
    total = 0x150 + len(padded)
    hdr = bytearray(0x80)
    hdr[0:4] = b'~PSP'
    struct.pack_into('<HH', hdr, 4, 0x0800, 1 if compress else 0)   # mod_attribute, comp_attribute
    hdr[0x0A:0x0A + 7] = b'wiisp\0\0'
    struct.pack_into('<II', hdr, 0x28, len(elf), total)              # elf_size, psp_size
    hdr[0x7C] = 0

    # Bloque del KIRK 1: claves cifradas con la clave del comando 1
    data_key = hashlib.sha1(b'wiisp-test-key').digest()[:16]
    keys = data_key + b'\0' * 16
    H = cbc(kirk1, keys, True) + b'\0' * 0x20                       # 0x40 bytes en claro de la cabecera
    meta = struct.pack('<II', data_size, 0x80) + b'\0' * 8
    E = bytes(a ^ b for a, b in zip(cbc(vault[code], bytes(a ^ b for a, b in zip(H, xorbuf[0x50:0x90])), True),
                                    xorbuf[0x10:0x50]))
    id_plain = hashlib.sha1(b'wiisp-id').digest()[:16]
    tagb = struct.pack('<I', tag)
    sha = hashlib.sha1(tagb + xorbuf[:16] + b'\0' * 0x58 + id_plain + E + meta + bytes(hdr)).digest()
    R = id_plain + sha + E[:0x3C]
    Renc = cbc(vault[code], R, True)
    kirk_file = Renc[36:96] + E[0x3C:0x40]

    out = bytearray(total)
    out[0:0x80] = hdr
    out[0x80:0xB0] = kirk_file[0:0x30]
    out[0xB0:0xC0] = meta
    out[0xC0:0xD0] = kirk_file[0x30:0x40]
    out[0xD0:0xD4] = tagb
    out[0x12C:0x140] = Renc[16:36]
    out[0x140:0x150] = Renc[0:16]
    out[0x150:] = cbc(data_key, padded, True)
    return bytes(out)



# --- Imágenes de disco --------------------------------------------------------

SECTOR = 2048


def both16(v): return struct.pack('<H', v) + struct.pack('>H', v)
def both32(v): return struct.pack('<I', v) + struct.pack('>I', v)


def dir_record(name, lba, size, is_dir):
    n = len(name)
    ln = 33 + n + (1 - n % 2)
    r = bytes([ln, 0]) + both32(lba) + both32(size) + bytes(7) + bytes([2 if is_dir else 0, 0, 0]) + both16(1) + bytes([n]) + name
    return r + bytes(ln - len(r))


def make_iso(tree):
    """tree: {nombre: bytes | dict}. Devuelve la imagen ISO 9660."""
    # Directorios en anchura con su sector; luego los archivos
    dirs, files, queue = [], [], [('', tree, None)]
    while queue:
        path, node, parent = queue.pop(0)
        dirs.append([path, node, parent, 0, 0])
        for name in sorted(node):
            if isinstance(node[name], dict):
                queue.append((path + '/' + name, node[name], len(dirs) - 1))
    lba = 18  # 16 = PVD, 17 = terminador
    for d in dirs:
        recs = 68 + sum(len(dir_record(n.encode() + (b'' if isinstance(v, dict) else b';1'), 0, 0, 0)) for n, v in d[1].items())
        d[3] = lba
        d[4] = ((recs + SECTOR - 1) // SECTOR) * SECTOR
        lba += d[4] // SECTOR
    where = {}
    for d in dirs:
        for name in sorted(d[1]):
            v = d[1][name]
            if not isinstance(v, dict):
                where[(d[0], name)] = lba
                files.append((lba, v))
                lba += max(1, (len(v) + SECTOR - 1) // SECTOR)
    img = bytearray(lba * SECTOR)
    for i, d in enumerate(dirs):
        par = dirs[d[2]] if d[2] is not None else d
        data = dir_record(b'\0', d[3], d[4], True) + dir_record(b'\1', par[3], par[4], True)
        for name in sorted(d[1]):
            v = d[1][name]
            if isinstance(v, dict):
                sub = next(x for x in dirs if x[0] == d[0] + '/' + name)
                rec = dir_record(name.encode(), sub[3], sub[4], True)
            else:
                rec = dir_record(name.encode() + b';1', where[(d[0], name)], len(v), False)
            if len(data) // SECTOR != (len(data) + len(rec) - 1) // SECTOR:
                data += bytes(SECTOR - len(data) % SECTOR)  # un registro no cruza sectores
            data += rec
        img[d[3] * SECTOR:d[3] * SECTOR + len(data)] = data
    for l, v in files:
        img[l * SECTOR:l * SECTOR + len(v)] = v
    pvd = bytearray(SECTOR)
    pvd[0:6] = b'\x01CD001'
    pvd[6] = 1
    pvd[40:72] = b'UMD_DATA'.ljust(32)
    pvd[80:88] = both32(lba)
    pvd[128:132] = both16(SECTOR)
    pvd[156:190] = dir_record(b'\0', dirs[0][3], dirs[0][4], True)
    img[16 * SECTOR:17 * SECTOR] = pvd
    img[17 * SECTOR:17 * SECTOR + 7] = b'\xffCD001\x01'
    return bytes(img)


def lz4_block(data):
    """Compresor LZ4 sencillo (voraz, tabla hash de 4 bytes)."""
    out, table, i, anchor, n = bytearray(), {}, 0, 0, len(data)
    def emit(lit, mlen, off):
        tok_l = min(len(lit), 15)
        tok_m = 0 if mlen is None else min(mlen - 4, 15)
        out.append(tok_l << 4 | tok_m)
        if len(lit) >= 15:
            k = len(lit) - 15
            while k >= 255: out.append(255); k -= 255
            out.append(k)
        out.extend(lit)
        if mlen is not None:
            out.extend(struct.pack('<H', off))
            if mlen - 4 >= 15:
                k = mlen - 4 - 15
                while k >= 255: out.append(255); k -= 255
                out.append(k)
    while i + 12 < n:
        key = data[i:i + 4]
        j = table.get(key)
        table[key] = i
        if j is not None and i - j < 65536:
            m = 4
            while i + m < n - 5 and data[j + m] == data[i + m]: m += 1
            emit(data[anchor:i], m, i - j)
            i += m
            anchor = i
        else:
            i += 1
    emit(data[anchor:], None, 0)
    return bytes(out)


def compress_image(img, kind, block=SECTOR):
    """CSO (deflate) o ZSO (LZ4) v1 con alineación 0."""
    frames = (len(img) + block - 1) // block
    hdr = (b'CISO' if kind == 'cso' else b'ZISO') + struct.pack('<IQIBBH', 24, len(img), block, 1, 0, 0)
    pos = len(hdr) + 4 * (frames + 1)
    index, body = [], bytearray()
    for f in range(frames):
        raw = img[f * block:(f + 1) * block].ljust(block, b'\0')
        if kind == 'cso':
            c = zlib.compressobj(9, zlib.DEFLATED, -15)
            comp = c.compress(raw) + c.flush()
        else:
            comp = lz4_block(raw)
        if len(comp) >= block:
            index.append((pos + len(body)) | 0x80000000)
            body += raw
        else:
            index.append(pos + len(body))
            body += comp
    index.append(pos + len(body))
    return hdr + b''.join(struct.pack('<I', x) for x in index) + bytes(body)


def read_tree(root):
    t = {}
    for name in os.listdir(root):
        p = os.path.join(root, name)
        t[name.upper()] = read_tree(p) if os.path.isdir(p) else open(p, 'rb').read()
    return t


def test_tree():
    elf = bytes((i * 13 + 5) & 255 for i in range(5000))
    return {'PSP_GAME': {'PARAM.SFO': b'\0PSF' + bytes(60), 'SYSDIR': {'EBOOT.BIN': elf, 'BOOT.BIN': bytes(16)},
                         'USRDIR': {'DATA': {'A.TXT': b'hola disco\n', 'GRANDE.BIN': bytes((i * 7) & 255 for i in range(9000))},
                                    'VACIO': {}}},
            'UMD_DATA.BIN': b'ULUS99999|0001|0001|G'}

def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        sys.exit(2)
    cmd = a.pop(0)
    if cmd == 'header':
        plain = bytes((i * 37 + 11) & 255 for i in range(1000))   # pisa la cabecera al descifrar en su sitio
        enc = encrypt(plain)
        encz = encrypt(plain, compress=True)
        # Deflate: texto repetitivo (copias largas) y bytes pseudoaleatorios,
        # con bloques guardados (nivel 0), fijos y dinámicos
        words = [b'psp', b'wii', b'emula', b'la', b'en', b'el', b'gx', b'kirk', b'vfpu', b'dynarec', b'ge']
        text = (b'WIISP emula la PSP en el Wii. ' * 40) + b' '.join(words[(i * i * 7 + i // 3) % 11] for i in range(900)) \
               + bytes((i * i * 7 + i // 3) & 255 for i in range(1500)) \
               + bytes(b'aaaaaaaaeeeeeeiiiooouuu bcdfg'[(i * 7919 + (i * i) // 13) % 29] for i in range(5000))
        def raw(level, strategy=zlib.Z_DEFAULT_STRATEGY):
            c = zlib.compressobj(level, zlib.DEFLATED, -15, 9, strategy)
            return c.compress(text) + c.flush()
        vectors = [('prx_plain', plain), ('prx_enc', enc), ('prx_enc_gz', encz), ('defl_plain', text),
                   ('defl_stored', raw(0)), ('defl_fixed', raw(9, zlib.Z_FIXED)), ('defl_dynamic', raw(9)),
                   ('defl_gzip', gzip.compress(text, mtime=0))]
        iso = make_iso(test_tree())
        vectors += [('iso_gz', gzip.compress(iso, mtime=0)), ('iso_cso', compress_image(iso, 'cso')),
                    ('iso_zso', compress_image(iso, 'zso', 4096))]
        with open(a[0], 'w') as f:
            f.write('/* Generado por tools/make_test_vectors.py header: no editar */\n')
            for name, d in vectors:
                f.write('static const unsigned char %s[%d] = {\n' % (name, len(d)))
                for i in range(0, len(d), 16):
                    f.write('\t' + ', '.join('0x%02X' % b for b in d[i:i + 16]) + ',\n')
                f.write('};\n')
        return
    if cmd == 'iso':
        block = SECTOR
        if '--block' in a:
            block = int(a[a.index('--block') + 1])
            del a[a.index('--block'):a.index('--block') + 2]
        img = make_iso(read_tree(a[0]))
        if a[1].lower().endswith(('.cso', '.zso')):
            img = compress_image(img, a[1][-3:].lower(), block)
        open(a[1], 'wb').write(img)
        return
    tag, gz, files = 0xD91609F0, False, []
    while a:
        x = a.pop(0)
        if x == '--tag': tag = int(a.pop(0), 16)
        elif x == '--gzip': gz = True
        else: files.append(x)
    open(files[1], 'wb').write(encrypt(open(files[0], 'rb').read(), tag, gz))


main()
