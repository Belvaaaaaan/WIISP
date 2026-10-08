#!/bin/sh
# WIISP - tools/setup_entorno.sh
# Prepara lo que hace falta para compilar y probar WIISP en una máquina
# Linux sin Docker (por ejemplo, una sesión nueva en la nube):
#
#   sh tools/setup_entorno.sh [carpeta]      (por defecto ../wiisp-entorno)
#
#   - pspautotests (en el commit que usa CI): pruebas grabadas en una PSP real
#   - PPSSPP (solo el código, como referencia de comportamiento)
#   - devkitPPC + libogc: se sacan de la imagen Docker oficial
#     devkitpro/devkitppc descargando sus capas del registro, sin Docker
#
# Al terminar imprime las variables que hay que exportar. Para las pruebas
# en PowerPC hacen falta además gcc-powerpc-linux-gnu y qemu-user (apt).
#
# SPDX-License-Identifier: GPL-2.0-or-later

set -e
DEST=${1:-../wiisp-entorno}
mkdir -p "$DEST"
DEST=$(cd "$DEST" && pwd)

if [ ! -d "$DEST/pspautotests" ]; then
	git clone -q https://github.com/hrydgard/pspautotests "$DEST/pspautotests"
	git -C "$DEST/pspautotests" checkout -q f93c29855718a587360976e793f5b41a88ef7e68
fi

if [ ! -d "$DEST/ppsspp" ]; then
	git clone -q --depth 1 --filter=blob:none https://github.com/hrydgard/ppsspp "$DEST/ppsspp"
fi

if [ ! -d "$DEST/devkitpro/devkitPPC" ]; then
	python3 - "$DEST" <<'EOF'
import json, os, sys, tarfile, urllib.request
dest = sys.argv[1]
repo = 'devkitpro/devkitppc'
tok = json.load(urllib.request.urlopen(
    f'https://auth.docker.io/token?service=registry.docker.io&scope=repository:{repo}:pull'))['token']
def get(url, accept):
    req = urllib.request.Request(url, headers={'Authorization': 'Bearer ' + tok, 'Accept': accept})
    return urllib.request.urlopen(req)
base = f'https://registry-1.docker.io/v2/{repo}'
acc = ','.join(['application/vnd.docker.distribution.manifest.list.v2+json',
                'application/vnd.oci.image.index.v1+json',
                'application/vnd.docker.distribution.manifest.v2+json',
                'application/vnd.oci.image.manifest.v1+json'])
man = json.load(get(base + '/manifests/latest', acc))
if 'manifests' in man:   # lista multiarquitectura: la de amd64
    d = next(m['digest'] for m in man['manifests'] if m.get('platform', {}).get('architecture') == 'amd64')
    man = json.load(get(base + '/manifests/' + d, acc))
os.makedirs(dest + '/capas', exist_ok=True)
for layer in man['layers']:
    path = dest + '/capas/' + layer['digest'].split(':')[1] + '.tgz'
    if not os.path.exists(path):
        print('descargando', layer['digest'][:19], layer.get('size', 0) // (1 << 20), 'MB')
        with get(base + '/blobs/' + layer['digest'], '*/*') as r, open(path, 'wb') as f:
            while True:
                b = r.read(1 << 20)
                if not b: break
                f.write(b)
    # Solo interesa /opt/devkitpro
    with tarfile.open(path) as t:
        members = [m for m in t.getmembers() if m.name.startswith('opt/devkitpro')]
        for m in members:
            m.name = m.name[len('opt/'):]
            if m.islnk() and m.linkname.startswith('opt/'):
                m.linkname = m.linkname[len('opt/'):]
        t.extractall(dest, members=members)
EOF
fi

cat <<EOF

Listo. Exporta esto (o ponlo en tu shell):
  export DEVKITPRO=$DEST/devkitpro
  export DEVKITPPC=$DEST/devkitpro/devkitPPC
Pruebas:
  make -f Makefile.pc test
  python3 tests/autotests.py $DEST/pspautotests --list tests/autotests_pass.txt
Referencia de PPSSPP: $DEST/ppsspp/Core/HLE
EOF
