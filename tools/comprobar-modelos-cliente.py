#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""
comprobar-modelos-cliente.py — ¿por qué este objeto se ve como un cubo?

EL PROBLEMA QUE RESUELVE
Un objeto que aparece como un cubo blanco o azul PUESTO EN EL PERSONAJE, pero
con su icono y su tooltip correctos, no es un fallo del servidor. Cada cosa sale
de un sitio distinto:

    nombre y estadísticas  →  el servidor (item_template)
    icono e identificador  →  ItemDisplayInfo.dbc, dentro del MPQ de idioma
    MODELO 3D              →  ficheros .m2 dentro de los MPQ de arte

Si el icono se ve y el modelo no, lo que falta es el .m2: al cliente le falta
arte. No hay nada que tocar en el servidor.

Así se descubrió que el cliente de ChromieCraft venía sin `patch-2.MPQ` ni
`patch-3.MPQ` —los parches oficiales donde vive el arte de la Ciudadela de la
Corona de Hielo— y por eso TODOS los objetos de esa banda salían como cubos.
Ver CHANGELOG.md anexo A6 §9.8.

USO
    pip install mpyq

    # ¿por qué este displayid se ve mal?  (el displayid sale de la BD:
    #   SELECT displayid FROM item_template WHERE entry = 50734; )
    python comprobar-modelos-cliente.py --cliente "<cliente>" 64521

    # recuento general: cuánto arte le falta al cliente
    python comprobar-modelos-cliente.py --cliente "<cliente>" --todos

QUÉ HACER CON EL RESULTADO
Si el modelo no aparece en ningún MPQ, se copia el `patch-N.MPQ` que lo tenga
desde un cliente 3.3.5a completo a la carpeta `Data/` del tuyo. Van en su hueco
de siempre: por encima de `patch.MPQ` y por debajo de los parches propios del
servidor, así que no pisan nada de lo que ya funcione.
"""

import argparse
import os
import struct
import sys

try:
    import mpyq
except ImportError:
    sys.exit('Falta el lector de MPQ. Instálalo con:  pip install mpyq')

SEP = bytes([92])          # la barra invertida que usan las rutas de los MPQ

# Los DBC viven en los MPQ de idioma; manda el parche de número más alto.
LOCALE = ['patch-{loc}-3.MPQ', 'patch-{loc}-2.MPQ', 'patch-{loc}.MPQ', 'locale-{loc}.MPQ']

# El arte vive en los MPQ generales, y los parches propios del servidor mandan
# sobre los de Blizzard.
ART = ['patch-Zava-3.MPQ', 'patch-3.MPQ', 'patch-2.MPQ', 'patch.MPQ',
       'lichking.MPQ', 'expansion.MPQ', 'common-2.MPQ', 'common.MPQ']


def abrir(ruta):
    try:
        return mpyq.MPQArchive(ruta)
    except Exception:
        return None            # sin índice interno no se puede listar: se ignora


def buscar_locale(data, preferido=None):
    """Devuelve (nombre, bytes) del ItemDisplayInfo.dbc que manda de verdad."""
    carpetas = sorted(c for c in os.listdir(data) if os.path.isdir(os.path.join(data, c)) and len(c) == 4)
    if preferido and preferido in carpetas:
        carpetas.insert(0, carpetas.pop(carpetas.index(preferido)))

    for carpeta in carpetas:
        ruta = os.path.join(data, carpeta)
        for plantilla in LOCALE:
            mpq = os.path.join(ruta, plantilla.format(loc=carpeta))
            if not os.path.exists(mpq):
                continue
            archivo = abrir(mpq)
            if not archivo:
                continue
            try:
                dbc = archivo.read_file(r'DBFilesClient\ItemDisplayInfo.dbc')
            except Exception:
                dbc = None
            if dbc:
                return os.path.join(carpeta, plantilla.format(loc=carpeta)), dbc
    return None, None


def leer_dbc(dbc):
    """Saca del DBC {displayid: (modelo_izq, modelo_der)}."""
    _, registros, campos, tam, _ = struct.unpack('<4siiii', dbc[:20])
    textos = dbc[20 + registros * tam:]

    def texto(offset):
        return textos[offset:textos.find(b'\0', offset)].decode('latin-1')

    tabla = {}
    for i in range(registros):
        v = struct.unpack('<%di' % campos, dbc[20 + i * tam: 20 + (i + 1) * tam])
        tabla[v[0]] = (texto(v[1]), texto(v[2]))
    return tabla, registros


def indexar_arte(data):
    """{nombre_de_modelo_sin_extensión: mpq_donde_está}"""
    indice = {}
    ilegibles = []
    for nombre in ART:
        ruta = os.path.join(data, nombre)
        if not os.path.exists(ruta):
            continue
        archivo = abrir(ruta)
        if not archivo:
            ilegibles.append(nombre)
            continue
        for f in (archivo.files or []):
            bajo = f.lower()
            if bajo.endswith(b'.m2') or bajo.endswith(b'.mdx'):
                clave = bajo.rsplit(SEP, 1)[-1].rsplit(b'.', 1)[0].decode('latin-1')
                indice.setdefault(clave, nombre)
    return indice, ilegibles


def main():
    p = argparse.ArgumentParser(description='Comprueba si al cliente le falta el modelo de un objeto.')
    p.add_argument('displayid', nargs='?', type=int, help='displayid del objeto (de item_template)')
    p.add_argument('--cliente', required=True, help='carpeta del World of Warcraft')
    p.add_argument('--todos', action='store_true', help='recuento general de arte que falta')
    p.add_argument('--idioma', default='esES', help='carpeta de idioma a mirar (por defecto esES)')
    args = p.parse_args()

    data = os.path.join(args.cliente, 'Data')
    if not os.path.isdir(data):
        sys.exit('No encuentro la carpeta Data en %s' % args.cliente)

    origen, dbc = buscar_locale(data, args.idioma)
    if not dbc:
        sys.exit('No he podido leer ItemDisplayInfo.dbc de ningun MPQ de idioma.')

    tabla, registros = leer_dbc(dbc)
    print('ItemDisplayInfo.dbc  : %s (%d registros, displayid maximo %d)'
          % (origen, registros, max(tabla)))

    indice, ilegibles = indexar_arte(data)
    print('Modelos indexados    : %d' % len(indice))
    if ilegibles:
        print('Sin indice (no se pueden listar): %s' % ', '.join(ilegibles))

    if args.todos:
        piden = {m.rsplit('.', 1)[0].lower() for par in tabla.values() for m in par if m}
        faltan = sorted(m for m in piden if m not in indice)
        print('\nModelos que pide el DBC : %d' % len(piden))
        print('Modelos que faltan      : %d' % len(faltan))
        print('\nPor contenido:')
        for etiqueta, clave in [('Corona de Hielo', 'icecrownraid'),
                                ('Ulduar', 'ulduarraid'),
                                ('Coliseo de la Cruzada', 'argentraid'),
                                ('Naxxramas', 'naxxramas')]:
            total = sum(1 for m in piden if clave in m)
            if total:
                print('  %-24s faltan %3d de %3d' % (etiqueta, sum(1 for m in faltan if clave in m), total))
        print('\nOjo: siempre "faltan" unos cientos. Son referencias muertas del propio')
        print('DBC de Blizzard, que apuntan a ficheros que nunca llego a publicar.')
        return

    if args.displayid is None:
        p.error('dime un displayid, o usa --todos')

    print('\n--- displayid %d ---' % args.displayid)
    if args.displayid not in tabla:
        print('  Ese displayid NO esta en el DBC del cliente: le falta el DBC, no el arte.')
        return

    for etiqueta, modelo in zip(('modelo izquierda', 'modelo derecha '), tabla[args.displayid]):
        if not modelo:
            continue
        clave = modelo.rsplit('.', 1)[0].lower()
        donde = indice.get(clave)
        print('  %s: %-36s %s' % (etiqueta, modelo,
                                  ('esta en ' + donde) if donde else '*** NO ESTA: falta arte en el cliente ***'))


if __name__ == '__main__':
    main()
