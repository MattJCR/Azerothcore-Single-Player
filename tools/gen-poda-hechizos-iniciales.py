#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# Genera patches/arac/09-hechizos-iniciales-podados-por-nivel.sql: la lista de
# hechizos que hay que sacar de `playercreateinfo_spell_custom` para poder
# volver a encender `PlayerStart.CustomSpells` sin que un personaje de nivel 1
# aprenda el árbol de clase entero. Ver CHANGELOG.md (22/09/2026, E1i) y la
# cabecera del propio .sql para el porqué.
#
# Criterio: se CONSERVA la fila si su hechizo tiene, en Spell.dbc,
# baseLevel <= 1 y spellLevel <= 1. Todo lo demás se borra: lo aprenderá del
# instructor al subir de nivel, como debe ser.
#
# Uso (en la VM, que es donde viven el DBC y la BD):
#   mysql ... -N -B acore_world -e "SELECT DISTINCT Spell FROM playercreateinfo_spell_custom;" > /tmp/spells.txt
#   python3 tools/gen-poda-hechizos-iniciales.py \
#       --dbc /home/acore/azerothcore/env/dist/bin/dbc/Spell.dbc \
#       --spells /tmp/spells.txt > patches/arac/09-hechizos-iniciales-podados-por-nivel.sql
#
# Spell.dbc 3.3.5a: 234 campos; baseLevel = índice 38, spellLevel = índice 39.

import argparse
import struct
import sys

BASE_LEVEL = 38
SPELL_LEVEL = 39


def ids_de_dbc(ruta):
    """Devuelve el conjunto de IDs (columna 0) de un DBC."""
    with open(ruta, "rb") as fh:
        raw = fh.read()
    magic, nrows, nfields, recsize, _ = struct.unpack("<4siiii", raw[:20])
    if magic != b"WDBC":
        sys.exit("no es un DBC: %s" % ruta)
    out = set()
    off = 20
    for _ in range(nrows):
        out.add(struct.unpack("<i", raw[off:off + 4])[0])
        off += recsize
    return out

CABECERA = """-- =============================================================================
--  ARAC 09 — Poda por nivel de `playercreateinfo_spell_custom`, para poder
--  volver a encender `PlayerStart.CustomSpells`
--
--  GENERADO por tools/gen-poda-hechizos-iniciales.py. No editar a mano.
--
--  Hallazgo del 22/09/2026 (E1i, ver CHANGELOG). Síntoma: **ningún** personaje
--  de jugador tiene raciales, idiomas, competencias de armadura ni los
--  hechizos "generales" (Ataque, Duelo, Atascado, Abrir/Cerrar, Quitar
--  insignia, Detectar...). Comprobado sobre la BD real: los cuatro personajes
--  del usuario dan 0/30 de la lista básica.
--
--  **No es cosa de mod-arac**, aunque lo pareciera: `Prieste` (guid 9001712,
--  elfo nocturno sacerdote) es una combinación NATIVA y está igual de pelado.
--  En AzerothCore esos hechizos llegan por un único camino,
--  `Player::LearnCustomSpells()` leyendo `playercreateinfo_spell_custom`, y
--  esta instalación tiene `PlayerStart.CustomSpells = 0`.
--
--  Por qué estaba a 0, y por qué no basta con volver a encenderlo: esa tabla
--  no trae sólo los hechizos iniciales, trae **el árbol de clase entero sin
--  filtrar por nivel**. Verificado contra el `Spell.dbc` real: la lista de
--  gnomo druida incluye 33745 (Laceración, nivel 66) y 40121 (Forma de vuelo
--  veloz, nivel 68). Con el flag a 1 y la tabla intacta, un personaje de
--  nivel 1 los aprendería todos — que es el fallo que nos hizo apagarlo
--  (CHANGELOG, "Domar Bestia rompía la progresión").
--
--  LA SOLUCIÓN: podar la tabla dejando sólo lo que un personaje de nivel 1
--  puede tener, y volver a encender el flag (lo hace la fase 5, no este SQL).
--  DOS criterios, los dos objetivos y reproducibles. Se conserva la fila si:
--    1) su hechizo tiene `Spell.dbc` `baseLevel <= 1` **y** `spellLevel <= 1`; y
--    2) su hechizo EXISTE en el `Spell.dbc` efectivo del cliente
--       (`patch-<idioma>-4.MPQ` ya construido, no el `patch-3` pelado).
--
--  El criterio 2 se añadió el 22/09/2026 después de meter la pata: la primera
--  versión de esta poda sólo miraba el nivel y dejaba pasar 42 hechizos que el
--  cliente NO conoce — entre ellos dos ids imposibles, `426884` y `427448`, que
--  vienen corruptos en la fila Elfo de la Noche/Pícaro de mod-arac. Mandar al
--  cliente 3.3.5a un hechizo que no está en su `Spell.dbc` lo cuelga con
--  ERROR #132. De aquellos 42 sólo llegó a repartirse uno (`25359`, a un
--  personaje, ningún bot), pero 367 filas los tenían listos para el siguiente
--  personaje que se creara. Verificado leyendo el MPQ real con `mpyq`, no el
--  DBC del servidor, que no es el mismo fichero.
--
--  Comprobaciones de que el criterio no deja fuera nada que haga falta:
--    - 668 Idioma común (baseLevel 0, spellLevel 1), 7340 Idioma gnomo,
--      20589/20591/20592/20593 (raciales de gnomo), 9077 Cuero, 9078 Tela,
--      6603 Ataque, 7355 Atascado, 22027 Quitar insignia, 203 Sin armas,
--      204 Defensa -> **se quedan**.
--    - 5176 Ira y 5185 Toque sanador (baseLevel 1) -> se quedan, y de paso
--      tapan un hueco real: el instructor 33 vende el rango 2 de las dos
--      cadenas pero NO el rango 1.
--    - 5487 Forma de oso y 6807 Zarpazo brutal (nivel 10) -> se van, y es
--      correcto: desde `08-druid-bear-form-y-requisitos-visibles.sql` los
--      vende el instructor.
--    - De los hechizos descartados, 51 no los vende ningún instructor.
--      Revisados: son pasivas de forma (1178 Bear Form Passive, 3025 Cat
--      Form, 5419 Travel Form, 9635 Dire Bear, 24905 Moonkin, 33948 Flight
--      Form, 34123 Tree of Life...), pasivas de postura de guerrero (7376,
--      7381), invocaciones de brujo por misión (691, 697, 712) y 45438
--      Bloque de hielo, que borramos nosotros a propósito por el gating de
--      talento. Ninguna se aprende por instructor **por diseño**: llegan con
--      el hechizo padre. No hay nada que reponer.
--
--  ESTE SQL NO REPARA LOS PERSONAJES YA CREADOS — sólo arregla las creaciones
--  futuras. Los existentes se reparan con
--  `10-reparar-hechizos-iniciales-existentes.sql`.
--
--  Orden: la fase 5 aplica primero el SQL de mod-arac (que rellena la tabla
--  entera) y DESPUÉS `patches/arac/`, así que esta poda siempre manda.
--
--  Idempotente: DELETE con lista explícita; relanzarlo no borra nada más.
-- ============================================================================="""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dbc", required=True, help="ruta a Spell.dbc")
    ap.add_argument("--spells", required=True,
                    help="fichero con los Spell distintos de playercreateinfo_spell_custom, uno por línea")
    ap.add_argument("--client-spells", required=True,
                    help="fichero con los IDs del Spell.dbc EFECTIVO del cliente "
                         "(patch-<idioma>-4.MPQ ya construido), uno por línea. "
                         "Obligatorio: un hechizo que el cliente no conoce lo CUELGA "
                         "con ERROR #132 en cuanto el servidor se lo manda.")
    args = ap.parse_args()

    with open(args.dbc, "rb") as fh:
        raw = fh.read()
    magic, nrows, nfields, recsize, _ = struct.unpack("<4siiii", raw[:20])
    if magic != b"WDBC":
        sys.exit("no es un DBC: %s" % args.dbc)

    niveles = {}
    off = 20
    for _ in range(nrows):
        rec = struct.unpack("<%di" % nfields, raw[off:off + recsize])
        off += recsize
        niveles[rec[0]] = (rec[BASE_LEVEL], rec[SPELL_LEVEL])

    presentes = [int(l) for l in open(args.spells) if l.strip()]
    conocidos = set(int(l) for l in open(args.client_spells) if l.strip())

    por_nivel = set(s for s in presentes
                    if s in niveles and (niveles[s][0] > 1 or niveles[s][1] > 1))
    # Segundo criterio, añadido el 22/09/2026 tras un fallo real: el hechizo
    # tiene que existir en el Spell.dbc EFECTIVO del cliente. Uno que no exista
    # cuelga el Wow.exe con ERROR #132 en cuanto el servidor se lo manda.
    fantasma = set(s for s in presentes if s not in conocidos)
    fuera = sorted(por_nivel | fantasma)

    print(CABECERA)
    print()
    print("-- %d hechizos distintos de %d se van: %d por nivel, %d por no existir"
          % (len(fuera), len(presentes), len(por_nivel), len(fantasma)))
    print("-- en el Spell.dbc del cliente (%d de ellos por los dos motivos a la vez)."
          % len(por_nivel & fantasma))
    print("-- Los que se van SOLO por no existir en el cliente:")
    solo_fantasma = sorted(fantasma - por_nivel)
    for i in range(0, len(solo_fantasma), 12):
        print("--   %s" % ", ".join(str(s) for s in solo_fantasma[i:i + 12]))
    print("DELETE FROM `playercreateinfo_spell_custom` WHERE `Spell` IN (")
    for i in range(0, len(fuera), 12):
        trozo = ", ".join(str(s) for s in fuera[i:i + 12])
        coma = "," if i + 12 < len(fuera) else ""
        print("    %s%s" % (trozo, coma))
    print(");")


if __name__ == "__main__":
    main()
