#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""
construir-parche-cliente-items.py — MPQ de cliente para los objetos propios

EL PROBLEMA
Los objetos propios del servidor (`item_template`, `entry >= 600000`) no los
conoce el cliente 3.3.5a:

  · el ICONO lo saca de `Item.dbc`, que sólo llega al `entry` ~56806  → sale "?"
  · la línea verde "Uso: ..." la saca de `Spell.dbc` (la ganzúa usa el hechizo
    59403, el de la Llave esqueleto de titanio, y su texto dice "llave
    esqueleto")

El nombre y la descripción amarilla del objeto SÍ vienen del servidor y no hay
que tocarlos.

LA SOLUCIÓN
Reconstruir `Item.dbc` y `Spell.dbc` a partir de los que trae el cliente,
añadiendo/editando sólo lo propio, y empaquetarlos en un MPQ por idioma:

    cliente/Data/esES/patch-esES-4.MPQ
    cliente/Data/enUS/patch-enUS-4.MPQ

El nombre NO es libre: el Wow.exe carga `patch-<idioma>-4.MPQ` (comprobado en el
binario) y, al ser parche de idioma con sufijo -4, manda sobre
`patch-<idioma>-3.MPQ` —de donde salen hoy `Item.dbc` y `Spell.dbc`—. Un
`Data/patch-4.MPQ` genérico NO valdría: los parches de idioma mandan sobre él.
Se hace en los dos idiomas que trae el cliente (`enUS` y `esES`) porque el WoW
carga la cadena del idioma con el que arranca.

USO
    pip install mpyq
    python tools/construir-parche-cliente-items.py --cliente "<cliente>" --instalar

Genera los MPQ en `cliente/Data/<idioma>/`; con `--instalar` los copia también a
`<cliente>/Data/<idioma>/`. El instalador del cliente (`cliente/instalar-cliente.ps1`)
los lleva desde `cliente/Data/` vía `cliente/manifest.tsv`.

AL AÑADIR UN OBJETO PROPIO NUEVO
Se mete su fila en ITEMS (y, si usa un hechizo con texto molesto, en
SPELL_DESC) y se regenera. Si ChromieCraft actualiza sus DBC hay que regenerar:
si no, se pierden los objetos/hechizos que trajera ese parche (y quizá subir el
sufijo a -5).

mod-arac (razas y clases)
Si mod-arac está fijado en versions.lock, este mismo MPQ también lleva
CharBaseInfo.dbc, CharStartOutfit.dbc y SkillRaceClassInfo.dbc (los tres DBC
que habilitan cualquier combinación de raza/clase), extraídos y verificados
por SHA-256 desde `mirrors/mod-arac@<hash>.tar.gz`
(`patch-contents/DBFilesContent/`, la misma fuente cruda que ya usa el
instalador para el DBC del servidor). Antes se distribuían aparte como
`Data/Patch-Arac.MPQ`: el Wow.exe sólo carga `Data/patch-<UN carácter>.MPQ`
para los parches genéricos (comprobado por desensamblado) y
"Arac" no encaja, así que ese fichero nunca se cargaba — todos los hechizos
de cualquier combinación exclusiva de ARAC (p. ej. Paladín elfo nocturno)
salían bajo "General" en el libro de hechizos por no encontrarse en el índice
de CharBaseInfo, sin relación con SkillLineAbility. `patch-<idioma>-4.MPQ` sí
lo carga el cliente (confirmado en juego), así que ahí es donde va ahora, en
los dos idiomas. Reemplaza el DBC entero (no se fusiona fila a fila): son
ficheros completos, igual que el DBC que ya copia el servidor. Desactivar con
--sin-arac.
"""
import argparse
import hashlib
import os
import struct
import subprocess
import sys
import tarfile
import zlib

from piedra_sede_dbc import parchear as parchear_piedra_sede

try:
    import mpyq
except ImportError:
    mpyq = None  # sólo hace falta para leer un cliente (--cliente); con --insumos no

# Carpeta con los DBC ya extraídos del cliente (dbc/<idioma>/Item.dbc y Spell.dbc);
# la fija main() con --insumos. El panel los recibe del navegador del jugador.
INSUMOS = None

REPO_DIR = os.path.normpath(os.path.join(os.path.dirname(__file__), ".."))

IDIOMAS = ["esES", "enUS"]

# ── Contenido propio ───────────────────────────────────────────────────────
# Item.dbc 3.3.5a = 8 int32: id, class, subclass, sound_override(-1), material,
# displayid, inventorytype, sheathe
ITEMS = {
    # displayid 57272 = Ability_Rogue_TricksOftheTrade (Secretos del oficio).
    # Existe ya en el ItemDisplayInfo.dbc del cliente, así que basta con
    # apuntar el Item.dbc ahí. Mismo valor en
    # patches/custom-items/600000-ganzua-de-recompensa.sql.
    600000: (0, 8, -1, 2, 57272, 0, 0),   # Ganzúa de recompensa
    600001: (15, 0, -1, -1, 6418, 0, 0),  # Piedra de la sede; icono de la piedra de hogar
}

# Spell.dbc: se reescribe la Descripción. Sólo texto de cliente, no toca nada
# mecánico (el servidor usa su propio Spell.dbc, que no cambia).
# clave = spell id ; valor = {índice de idioma en el DBC: texto}
#   0 = enUS, 6 = esES  (en el bloque Description_Lang, offset de campo 170)
SPELL_DESC = {
    59403: {                                  # "Opening" de la Titanium Skeleton Key / ganzúa
        6: "Abre cualquier cajón o caja cerrada. Se consume al usarla.",
        0: "Opens any closed strongbox or lockbox. Consumed on use.",
    },
}

DESC_FIELD_BASE = 170     # Description_Lang[0] en Spell.dbc build 12340
CLIENTE_DIR = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "cliente"))
DBF = "DBFilesClient" + chr(92)


# ── Lectura de un DBC del cliente para un idioma ───────────────────────────
def _mpqs_idioma(data, idioma):
    """MPQ de la raíz de Data/ y de Data/<idioma>/, de menos a más prioridad.
    Se EXCLUYE nuestro propio patch-<idioma>-4.MPQ: hay que partir del DBC
    pristino del cliente, no de una generación anterior."""
    loc = idioma.lower()
    # nuestras propias salidas (de cualquier idioma, en cualquier carpeta): fuera
    propios = tuple("patch-%s-4.mpq" % i.lower() for i in IDIOMAS)
    cand = []
    for d in (data, os.path.join(data, idioma)):
        if os.path.isdir(d):
            for f in os.listdir(d):
                if f.lower().endswith(".mpq") and f.lower() not in propios:
                    cand.append(os.path.join(d, f))

    def prio(p):
        n = os.path.basename(p).lower()
        base = {"common.mpq": 0, "common-2.mpq": 1, "expansion.mpq": 2, "lichking.mpq": 3,
                "patch.mpq": 4, "patch-2.mpq": 5, "patch-3.mpq": 6}
        if n in base:
            return base[n]
        if n == "locale-%s.mpq" % loc:
            return 10
        if n == "patch-%s.mpq" % loc:
            return 20
        if n == "patch-%s-2.mpq" % loc:
            return 21
        if n == "patch-%s-3.mpq" % loc:
            return 22
        if n.startswith("patch-") and n.endswith(".mpq"):
            tag = n[6:-4]
            if tag.startswith("zava"):
                return 200
            if len(tag) == 1 and tag.isalnum():
                return 30 + ord(tag)
            return 100
        return 50

    return sorted(cand, key=prio)


def leer_dbc(data, idioma, nombre):
    if INSUMOS:
        ruta = os.path.join(INSUMOS, "dbc", idioma, nombre)
        if not os.path.isfile(ruta):
            sys.exit("Faltan los insumos de %s: %s" % (idioma, ruta))
        with open(ruta, "rb") as f:
            raw = f.read()
        print("    %-10s <- insumo recibido (%d bytes)" % (nombre, len(raw)))
        return raw
    if mpyq is None:
        sys.exit("Falta el lector de MPQ para leer un cliente:  pip install mpyq")
    ganador = None
    for p in _mpqs_idioma(data, idioma):
        try:
            raw = mpyq.MPQArchive(p).read_file(DBF + nombre)
        except Exception:
            raw = None
        if raw:
            ganador = (os.path.basename(p), raw)
    if not ganador:
        sys.exit("No pude leer %s de ningún MPQ (%s)." % (nombre, idioma))
    print("    %-10s <- %s (%d bytes)" % (nombre, ganador[0], len(ganador[1])))
    return ganador[1]


# ── Item.dbc: añadir filas ────────────────────────────────────────────────
def parchear_item_dbc(raw):
    magic, rec, fld, sz, ss = struct.unpack("<4siiii", raw[:20])
    if (fld, sz) != (8, 32):
        sys.exit("Item.dbc con formato inesperado: %d campos, %d bytes/fila" % (fld, sz))
    filas = {}
    for i in range(rec):
        v = struct.unpack("<8i", raw[20 + i * sz: 20 + (i + 1) * sz])
        filas[v[0]] = v
    for entry, campos in ITEMS.items():
        filas[entry] = (entry, *campos)
    ids = sorted(filas)
    out = bytearray(struct.pack("<4siiii", magic, len(ids), fld, sz, 2))
    for i in ids:
        out += struct.pack("<8i", *filas[i])
    out += b"\x00\x00"
    print("    Item.dbc  : %d filas (+%d propias)" % (len(ids), len(ITEMS)))
    return bytes(out)


# ── Spell.dbc: reescribir textos de Descripción ───────────────────────────
def parchear_spell_dbc(raw):
    magic, rec, fld, sz, ss = struct.unpack("<4siiii", raw[:20])
    buf = bytearray(raw)
    strings = bytearray(raw[20 + rec * sz:])
    nuevos = {}

    def offset_de(texto):
        if texto not in nuevos:
            nuevos[texto] = len(strings)
            strings.extend(texto.encode("utf-8") + b"\x00")
        return nuevos[texto]

    fila_de = {}
    for i in range(rec):
        sid = struct.unpack("<i", raw[20 + i * sz: 20 + i * sz + 4])[0]
        fila_de[sid] = i

    tocadas = 0
    for sid, por_idioma in SPELL_DESC.items():
        if sid not in fila_de:
            print("    aviso: hechizo %d no está en Spell.dbc; se ignora" % sid)
            continue
        base_rec = 20 + fila_de[sid] * sz
        for loc_idx, texto in por_idioma.items():
            off = base_rec + (DESC_FIELD_BASE + loc_idx) * 4
            struct.pack_into("<i", buf, off, offset_de(texto))
        tocadas += 1

    buf[0:20] = struct.pack("<4siiii", magic, rec, fld, sz, len(strings))
    salida = bytes(buf[:20 + rec * sz]) + bytes(strings)
    print("    Spell.dbc : %d hechizos con Descripción propia (+%d bytes de texto)"
          % (tocadas, len(strings) - ss))
    return salida


# ── Escritor MPQ v1 (single-unit, zlib) ───────────────────────────────────
def _crypt_table():
    t = [0] * 0x500
    seed = 0x00100001
    for i1 in range(0x100):
        idx = i1
        for _ in range(5):
            seed = (seed * 125 + 3) % 0x2AAAAB
            a = (seed & 0xFFFF) << 16
            seed = (seed * 125 + 3) % 0x2AAAAB
            t[idx] = (a | (seed & 0xFFFF)) & 0xFFFFFFFF
            idx += 0x100
    return t

_CT = _crypt_table()


def _hash(s, ht):
    s1, s2 = 0x7FED7FED, 0xEEEEEEEE
    for ch in s:
        c = ord(ch)
        if c == 0x2F:
            c = 0x5C
        if 0x61 <= c <= 0x7A:
            c -= 0x20
        s1 = (_CT[(ht << 8) + c] ^ ((s1 + s2) & 0xFFFFFFFF)) & 0xFFFFFFFF
        s2 = (c + s1 + s2 + ((s2 << 5) & 0xFFFFFFFF) + 3) & 0xFFFFFFFF
    return s1


def _encrypt(data, key):
    seed = 0xEEEEEEEE
    key &= 0xFFFFFFFF
    vals = struct.unpack("<%dI" % (len(data) // 4), data)
    res = []
    for v in vals:
        seed = (seed + _CT[0x400 + (key & 0xFF)]) & 0xFFFFFFFF
        res.append((v ^ ((key + seed) & 0xFFFFFFFF)) & 0xFFFFFFFF)
        key = (((~key & 0xFFFFFFFF) << 0x15) + 0x11111111 | (key >> 0x0B)) & 0xFFFFFFFF
        seed = (v + seed + ((seed << 5) & 0xFFFFFFFF) + 3) & 0xFFFFFFFF
    return struct.pack("<%dI" % len(res), *res)


def escribir_mpq(archivos, ruta, sector_shift=3):
    F_EXISTS, F_SINGLE, F_COMPRESS = 0x80000000, 0x01000000, 0x00000200
    nombres = list(archivos)
    listfile = ("\r\n".join(nombres) + "\r\n").encode("ascii")
    entradas = nombres + ["(listfile)"]
    crudos = [archivos[n] for n in nombres] + [listfile]

    ht = 16
    while ht < len(entradas) * 2:
        ht <<= 1

    bloques, trozos, pos = [], [], 32
    for raw in crudos:
        flags = F_EXISTS | F_SINGLE
        guardado = raw
        comp = b"\x02" + zlib.compress(raw, 9)
        if len(comp) < len(raw):
            guardado, flags = comp, flags | F_COMPRESS
        bloques.append((pos, len(guardado), len(raw), flags))
        trozos.append(guardado)
        pos += len(guardado)

    hpos = pos
    tabla = [[0xFFFFFFFF, 0xFFFFFFFF, 0xFFFF, 0xFFFF, 0xFFFFFFFF] for _ in range(ht)]
    for bidx, nombre in enumerate(entradas):
        i = _hash(nombre, 0) & (ht - 1)
        while tabla[i][4] != 0xFFFFFFFF:
            i = (i + 1) & (ht - 1)
        tabla[i] = [_hash(nombre, 1), _hash(nombre, 2), 0, 0, bidx]
    ht_enc = _encrypt(b"".join(struct.pack("<IIHHI", *e) for e in tabla), _hash("(hash table)", 3))

    bpos = hpos + len(ht_enc)
    bt_enc = _encrypt(b"".join(struct.pack("<IIII", *e) for e in bloques), _hash("(block table)", 3))
    total = bpos + len(bt_enc)

    os.makedirs(os.path.dirname(ruta), exist_ok=True)
    with open(ruta, "wb") as f:
        f.write(struct.pack("<4sIIHHIIII", b"MPQ\x1a", 32, total, 0, sector_shift,
                            hpos, bpos, ht, len(bloques)))
        for c in trozos:
            f.write(c)
        f.write(ht_enc)
        f.write(bt_enc)
    return total


# ── Utilidades de los espejos (mirrors/) ────────────────────────────────
def _commit_fijado(nombre):
    """Commit exacto de `nombre` en versions.lock, o None si no está fijado."""
    ruta = os.path.join(REPO_DIR, "versions.lock")
    with open(ruta, "r", encoding="utf-8") as f:
        for linea in f:
            if linea.startswith("#") or not linea.strip():
                continue
            campos = linea.rstrip("\n").split("\t")
            if campos and campos[0] == nombre and len(campos) >= 3:
                return campos[2]
    return None


# ── mod-arac: los tres DBC de raza/clase, tal cual, en patch-<idioma>-4.MPQ ─
ARAC_DBC_NOMBRES = ["CharBaseInfo.dbc", "CharStartOutfit.dbc", "SkillRaceClassInfo.dbc"]


def _hidratar_arac(commit):
    """Si falta el snapshot de mod-arac, lo reconstruye con `install.sh --hidratar`
    (asset o upstream exacto, siempre verificado por SHA-256)."""
    instalador = os.path.join(REPO_DIR, "install.sh")
    if not os.path.isfile(instalador):
        return
    print("[mod-arac] falta el snapshot; hidratando (./install.sh --hidratar mod-arac)...")
    subprocess.run(["bash", instalador, "--hidratar", "mod-arac"], check=False)


def _extraer_dbcs_arac(carpeta=None):
    """DBC de raza/clase de mod-arac: de `carpeta` (ya verificados por quien los
    preparó) o de mirrors/mod-arac@<commit>.tar.gz, comprobado contra MANIFEST.tsv
    (nunca se usa un tarball sin comprobar su SHA-256). Devuelve {nombre: bytes};
    None si mod-arac no está fijado en versions.lock."""
    if carpeta:
        dbcs = {}
        for nombre in ARAC_DBC_NOMBRES:
            ruta = os.path.join(carpeta, nombre)
            if not os.path.isfile(ruta):
                sys.exit("[mod-arac] falta %s en %s" % (nombre, carpeta))
            with open(ruta, "rb") as f:
                dbcs[nombre] = f.read()
        return dbcs

    commit = _commit_fijado("mod-arac")
    if not commit:
        return None

    corto = commit[:12]
    tarball = os.path.join(REPO_DIR, "mirrors", "mod-arac@%s.tar.gz" % corto)
    manifest = os.path.join(REPO_DIR, "mirrors", "MANIFEST.tsv")
    if not os.path.isfile(tarball):
        _hidratar_arac(commit)
    if not os.path.isfile(tarball) or not os.path.isfile(manifest):
        sys.exit("[mod-arac] falta %s o MANIFEST.tsv: './install.sh --hidratar mod-arac'."
                  % os.path.relpath(tarball, REPO_DIR))

    esperado = None
    with open(manifest, "r", encoding="utf-8") as f:
        for linea in f:
            campos = linea.rstrip("\n").split("\t")
            if len(campos) >= 5 and campos[0] == "mod-arac" and campos[1] == commit:
                esperado = campos[4]
                break
    if not esperado:
        sys.exit("[mod-arac] MANIFEST.tsv no tiene SHA-256 para el commit fijado (%s)." % corto)

    real = hashlib.sha256()
    with open(tarball, "rb") as f:
        for bloque in iter(lambda: f.read(1 << 20), b""):
            real.update(bloque)
    if real.hexdigest() != esperado:
        sys.exit("[mod-arac] %s no cuadra con su SHA-256 de MANIFEST.tsv. No se usa sin verificar."
                  % os.path.basename(tarball))

    dbcs = {}
    with tarfile.open(tarball, "r:gz") as archivo:
        for nombre in ARAC_DBC_NOMBRES:
            miembro = "patch-contents/DBFilesContent/%s" % nombre
            try:
                extraido = archivo.extractfile(miembro)
            except KeyError:
                extraido = None
            if extraido is None:
                sys.exit("[mod-arac] no encuentro %s dentro de %s" % (miembro, os.path.basename(tarball)))
            dbcs[nombre] = extraido.read()
    return dbcs


# ── main ──────────────────────────────────────────────────────────────────
def main():
    global INSUMOS
    ap = argparse.ArgumentParser()
    origen = ap.add_mutually_exclusive_group(required=True)
    origen.add_argument("--cliente", help="carpeta del World of Warcraft")
    origen.add_argument("--insumos", help="carpeta con dbc/<idioma>/Item.dbc y Spell.dbc ya extraídos (los recibe el panel)")
    ap.add_argument("--idioma", choices=IDIOMAS, action="append",
                    help="sólo este idioma (repetible); por defecto, todos")
    ap.add_argument("--instalar", action="store_true",
                    help="copiar también a <cliente>/Data/<idioma>/ tras generar (sólo con --cliente)")
    ap.add_argument("--sin-arac", action="store_true",
                    help="no meter los DBC de mod-arac (razas y clases) aunque esté fijado en versions.lock")
    ap.add_argument("--arac", help="carpeta con los tres DBC de mod-arac ya verificados (en vez del snapshot de mirrors/)")
    ap.add_argument("--salida", help="carpeta donde dejar <idioma>/patch-<idioma>-4.MPQ (por defecto cliente/Data)")
    args = ap.parse_args()

    salida_base = os.path.abspath(args.salida) if args.salida else os.path.join(CLIENTE_DIR, "Data")
    if args.insumos:
        INSUMOS = os.path.abspath(args.insumos)
        if args.instalar:
            sys.exit("--instalar sólo tiene sentido con --cliente")
        data = None
        idiomas = args.idioma or [i for i in IDIOMAS if os.path.isdir(os.path.join(INSUMOS, "dbc", i))]
        if not idiomas:
            sys.exit("Los insumos no traen ningún idioma de %s" % IDIOMAS)
    else:
        data = os.path.join(args.cliente, "Data")
        if not os.path.isdir(data):
            sys.exit("No encuentro %s" % data)
        idiomas = args.idioma or [i for i in IDIOMAS if os.path.isdir(os.path.join(data, i))]
        if not idiomas:
            sys.exit("El cliente no tiene ninguna carpeta de idioma de %s" % IDIOMAS)

    arac_dbcs = None
    if not args.sin_arac:
        arac_dbcs = _extraer_dbcs_arac(args.arac)
        if arac_dbcs is None:
            print("(mod-arac no está fijado en versions.lock; patch-<idioma>-4.MPQ sin CharBaseInfo/CharStartOutfit/SkillRaceClassInfo)")

    for idioma in idiomas:
        nombre_mpq = "patch-%s-4.MPQ" % idioma
        print("\n%s" % idioma)
        item_raw = leer_dbc(data, idioma, "Item.dbc")
        spell_raw = leer_dbc(data, idioma, "Spell.dbc")
        spell_raw = parchear_piedra_sede(parchear_spell_dbc(spell_raw))
        archivos = {DBF + "Item.dbc": parchear_item_dbc(item_raw),
                    DBF + "Spell.dbc": spell_raw}
        if arac_dbcs:
            for nombre, contenido in arac_dbcs.items():
                archivos[DBF + nombre] = contenido
            print("    +ARAC       : %s (%d bytes)" % (", ".join(sorted(arac_dbcs)), sum(len(v) for v in arac_dbcs.values())))
        salida = os.path.join(salida_base, idioma, nombre_mpq)
        total = escribir_mpq(archivos, salida)
        print("    -> %s (%d bytes)" % (salida, total))

        if args.instalar:
            destino = os.path.join(data, idioma, nombre_mpq)
            os.makedirs(os.path.dirname(destino), exist_ok=True)
            with open(salida, "rb") as s, open(destino, "wb") as d:
                d.write(s.read())
            print("    instalado en %s" % destino)

    print("\nListo. Si no usaste --instalar, pásalo o corre cliente/instalar-cliente.ps1.")
    print("En el cliente: cerrar WoW, borrar Cache/, volver a entrar.")


if __name__ == "__main__":
    main()
