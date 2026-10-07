#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Añade a Spell.dbc la piedra de sede SP02, independiente de la piedra de hogar.

Clona el hechizo 8690 de la propia versión del DBC para conservar su animación
y sus diez segundos de lanzamiento. Sustituye sus efectos por uno ficticio que
atiende el script del módulo y le da un ID y un enfriamiento propios.
"""

import argparse
import os
import struct
import tempfile

HEARTHSTONE = 8690
GUILD_STONE = 600001
FIELDS = 234
NAME_BASE = 136
DESCRIPTION_BASE = 170


def parchear(raw: bytes) -> bytes:
    if len(raw) < 20:
        raise ValueError("Spell.dbc está incompleto")
    magic, count, fields, size, string_size = struct.unpack_from("<4s4I", raw)
    if magic != b"WDBC" or fields != FIELDS or size != FIELDS * 4:
        raise ValueError(f"Spell.dbc inesperado: {magic!r}, {fields} campos, {size} bytes/fila")
    if len(raw) != 20 + count * size + string_size:
        raise ValueError("La longitud de Spell.dbc no coincide con su cabecera")

    rows = {}
    for i in range(count):
        row = bytearray(raw[20 + i * size:20 + (i + 1) * size])
        rows[struct.unpack_from("<I", row)[0]] = row
    if HEARTHSTONE not in rows:
        raise ValueError("Falta el hechizo base 8690")

    strings = bytearray(raw[20 + count * size:])
    if not strings or strings[0] != 0:
        raise ValueError("Bloque de cadenas DBC inválido")
    string_offsets = {}
    cursor = 0
    while cursor < len(strings):
        end = strings.find(0, cursor)
        if end < 0:
            raise ValueError("Cadena DBC sin terminador")
        string_offsets.setdefault(bytes(strings[cursor:end]), cursor)
        cursor = end + 1

    def add_string(value: str) -> int:
        encoded = value.encode("utf-8")
        if encoded in string_offsets:
            return string_offsets[encoded]
        offset = len(strings)
        strings.extend(encoded + b"\x00")
        string_offsets[encoded] = offset
        return offset

    stone = bytearray(rows[HEARTHSTONE])
    for field, value in {
        0: GUILD_STONE, 1: 0,       # ID propio, sin categoría de piedra de hogar
        28: 7, 29: 1_800_000, 30: 0,  # 10 s de lanzamiento, 30 min de reutilización
        71: 3, 72: 0, 73: 0,        # efecto ficticio gestionado por SpellScript
        110: 0, 111: 0, 112: 0,     # ningún destino de teletransporte heredado
    }.items():
        struct.pack_into("<I", stone, field * 4, value)
    for language, name, description in (
        (0, "Guildhouse Stone", "Teleports you to your guild house."),
        (6, "Piedra de la sede", "Te transporta a la sede de tu hermandad."),
        (7, "Piedra de la sede", "Te transporta a la sede de tu hermandad."),
    ):
        struct.pack_into("<I", stone, (NAME_BASE + language) * 4, add_string(name))
        struct.pack_into("<I", stone, (DESCRIPTION_BASE + language) * 4, add_string(description))
    rows[GUILD_STONE] = stone

    output = bytearray(struct.pack("<4s4I", magic, len(rows), fields, size, len(strings)))
    for spell_id in sorted(rows):
        output.extend(rows[spell_id])
    output.extend(strings)
    return bytes(output)


def tiene_piedra(raw: bytes) -> bool:
    magic, count, fields, size, _strings = struct.unpack_from("<4s4I", raw)
    if magic != b"WDBC" or fields != FIELDS or size != FIELDS * 4:
        raise ValueError("Spell.dbc inesperado")
    return any(struct.unpack_from("<I", raw, 20 + i * size)[0] == GUILD_STONE for i in range(count))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", help="Spell.dbc base o ya parcheado")
    parser.add_argument("output", nargs="?", help="Spell.dbc de destino")
    parser.add_argument("--comprobar", action="store_true",
                        help="no escribe nada: sale con 0 si input ya lleva el hechizo 600001 y con 1 si no")
    args = parser.parse_args()
    if args.comprobar:
        with open(args.input, "rb") as source:
            raise SystemExit(0 if tiene_piedra(source.read()) else 1)
    if not args.output:
        parser.error("falta el Spell.dbc de destino")
    with open(args.input, "rb") as source:
        output = parchear(source.read())
    target = os.path.abspath(args.output)
    os.makedirs(os.path.dirname(target), exist_ok=True)
    fd, temp_path = tempfile.mkstemp(prefix=".Spell.dbc.sp02.", dir=os.path.dirname(target))
    try:
        with os.fdopen(fd, "wb") as dest:
            dest.write(output)
        os.replace(temp_path, target)
    finally:
        if os.path.exists(temp_path):
            os.unlink(temp_path)


if __name__ == "__main__":
    main()
