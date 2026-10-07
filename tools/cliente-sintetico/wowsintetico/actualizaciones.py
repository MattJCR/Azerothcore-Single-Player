# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Lectura de SMSG_UPDATE_OBJECT / SMSG_COMPRESSED_UPDATE_OBJECT.

Espejo de Object::BuildMovementUpdate, Unit::BuildMovementPacket,
Object::BuildValuesUpdate, UpdateData::BuildPacket y
Movement::PacketBuilder::WriteCreate del core fijado. Sólo se guarda lo que
usan los escenarios: tipo, posición y los campos de valores por índice.
"""
import zlib

from .binario import Lector

# UpdateData.h
VALUES, MOVEMENT, CREATE, CREATE2, OUT_OF_RANGE, NEAR = range(6)

# ObjectGuid.h: TypeID
TYPEID_UNIT, TYPEID_PLAYER = 3, 4

# UpdateFields.h (3.3.5a)
OBJECT_FIELD_ENTRY = 0x0003
OBJECT_END = 0x0006
UNIT_FIELD_TARGET = OBJECT_END + 0x000C
UNIT_FIELD_HEALTH = OBJECT_END + 0x0012
UNIT_FIELD_MAXHEALTH = OBJECT_END + 0x001A
UNIT_FIELD_LEVEL = OBJECT_END + 0x0030
UNIT_FIELD_FLAGS = OBJECT_END + 0x0035
UNIT_NPC_FLAGS = OBJECT_END + 0x004C
UNIT_END = OBJECT_END + 0x008E
PLAYER_FLAGS = UNIT_END + 0x0002
PLAYER_GUILDID = UNIT_END + 0x0003
PLAYER_QUEST_LOG_1_1 = UNIT_END + 0x000A          # 25 ranuras x 5 campos
PLAYER_VISIBLE_ITEM_1_ENTRYID = UNIT_END + 0x0087   # 19 ranuras x (entrada, encantamiento)
PLAYER_FIELD_PACK_SLOT_1 = UNIT_END + 0x00DE  # 16 ranuras de la mochila x GUID empaquetado (2 campos)
PLAYER_FIELD_KEYRING_SLOT_1 = UNIT_END + 0x015C  # 32 ranuras del llavero (BagFamily=256) x GUID (2 campos)
PLAYER_SKILL_INFO_1_1 = UNIT_END + 0x01E8     # 128 habilidades x 3 campos
PLAYER_FIELD_COINAGE = UNIT_END + 0x03FE

# UnitDefines.h
UNIT_NPC_FLAG_TRAINER = 0x10
UNIT_NPC_FLAG_TRAINER_CLASS = 0x20
UNIT_NPC_FLAG_PETITIONER = 0x40000      # 0xC0000 con TABARDDESIGNER = vendedor de carta de hermandad
UNIT_NPC_FLAG_TABARDDESIGNER = 0x80000
UNIT_NPC_FLAG_BATTLEMASTER = 0x100000   # confirmado en vivo (.npc info) 25/09/2026

# MovementFlags / MovementFlags2 / UpdateFlags
_MF_ONTRANSPORT = 0x00000200
_MF_FALLING = 0x00001000
_MF_SWIMMING = 0x00200000
_MF_FLYING = 0x02000000
_MF_SPLINE_ELEVATION = 0x04000000
_MF_SPLINE_ENABLED = 0x08000000
_MF2_ALWAYS_ALLOW_PITCHING = 0x0020
_MF2_INTERPOLATED_MOVEMENT = 0x0400
_UF_TRANSPORT, _UF_HAS_TARGET, _UF_UNKNOWN, _UF_LOWGUID = 0x2, 0x4, 0x8, 0x10
_UF_LIVING, _UF_STATIONARY, _UF_VEHICLE, _UF_POSITION, _UF_ROTATION = 0x20, 0x40, 0x80, 0x100, 0x200
# MoveSplineFlag.h
_SF_FINAL_POINT, _SF_FINAL_TARGET, _SF_FINAL_ANGLE = 0x8000, 0x10000, 0x20000


def leer_info_movimiento(L: Lector) -> tuple:
    """Unit::BuildMovementPacket. Devuelve (x, y, z, o, flags)."""
    flags, flags2 = L.u32(), L.u16()
    L.u32()
    x, y, z, o = L.f32(), L.f32(), L.f32(), L.f32()
    if flags & _MF_ONTRANSPORT:
        L.guid_empaquetado()
        L.bytes(4 * 4 + 4 + 1)
        if flags2 & _MF2_INTERPOLATED_MOVEMENT:
            L.u32()
    if (flags & (_MF_SWIMMING | _MF_FLYING)) or (flags2 & _MF2_ALWAYS_ALLOW_PITCHING):
        L.f32()
    L.u32()
    if flags & _MF_FALLING:
        L.bytes(16)
    if flags & _MF_SPLINE_ELEVATION:
        L.f32()
    return x, y, z, o, flags


def _leer_movimiento(L: Lector) -> dict:
    uf = L.u16()
    pos = {}
    if uf & _UF_LIVING:
        x, y, z, o, mf = leer_info_movimiento(L)
        pos = {"x": x, "y": y, "z": z, "o": o}
        L.bytes(9 * 4)                               # velocidades
        if mf & _MF_SPLINE_ENABLED:
            sf = L.u32()
            if sf & _SF_FINAL_ANGLE:
                L.f32()
            elif sf & _SF_FINAL_TARGET:
                L.u64()
            elif sf & _SF_FINAL_POINT:
                L.bytes(12)
            L.bytes(4 * 3 + 4 * 2 + 4 + 4)            # timePassed, duración, id, mods, acel., efecto
            nodos = L.u32()
            L.bytes(12 * nodos)
            L.u8()
            L.bytes(12)
    elif uf & _UF_POSITION:
        L.guid_empaquetado()
        x, y, z = L.f32(), L.f32(), L.f32()
        L.bytes(12)
        o = L.f32()
        L.f32()
        pos = {"x": x, "y": y, "z": z, "o": o}
    elif uf & _UF_STATIONARY:
        x, y, z, o = L.f32(), L.f32(), L.f32(), L.f32()
        pos = {"x": x, "y": y, "z": z, "o": o}
    if uf & _UF_UNKNOWN:
        L.u32()
    if uf & _UF_LOWGUID:
        L.u32()
    if uf & _UF_HAS_TARGET:
        L.guid_empaquetado()
    if uf & _UF_TRANSPORT:
        L.u32()
    if uf & _UF_VEHICLE:
        L.u32(); L.f32()
    if uf & _UF_ROTATION:
        L.u64()
    return pos


def _leer_valores(L: Lector) -> dict:
    bloques = L.u8()
    mascaras = [L.u32() for _ in range(bloques)]
    valores = {}
    for b, m in enumerate(mascaras):
        for bit in range(32):
            if m & (1 << bit):
                valores[b * 32 + bit] = L.u32()
    return valores


def leer_paquete(datos: bytes, comprimido: bool = False) -> list:
    """Devuelve la lista de bloques: dicts con tipo, guid y lo leído."""
    if comprimido:
        datos = zlib.decompress(datos[4:])
    L = Lector(datos)
    bloques = []
    for _ in range(L.u32()):
        tipo = L.u8()
        if tipo in (OUT_OF_RANGE, NEAR):
            guids = [L.guid_empaquetado() for _ in range(L.u32())]
            bloques.append({"tipo": tipo, "guids": guids})
        elif tipo == VALUES:
            guid = L.guid_empaquetado()
            bloques.append({"tipo": tipo, "guid": guid, "valores": _leer_valores(L)})
        elif tipo == MOVEMENT:
            guid = L.guid_empaquetado()
            bloques.append({"tipo": tipo, "guid": guid, "pos": _leer_movimiento(L)})
        elif tipo in (CREATE, CREATE2):
            guid = L.guid_empaquetado()
            tid = L.u8()
            pos = _leer_movimiento(L)
            bloques.append({"tipo": tipo, "guid": guid, "typeid": tid, "pos": pos,
                            "valores": _leer_valores(L)})
        else:
            raise ValueError("tipo de bloque desconocido %d" % tipo)
    return bloques
