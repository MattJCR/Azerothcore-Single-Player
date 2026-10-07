# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Grupo y buscador: SMSG_GROUP_LIST, SMSG_PARTY_MEMBER_STATS(_FULL) y paquetes LFG.

Espejo de Group::SendUpdateToPlayer, WorldSession::BuildPartyMemberStatsChangedPacket,
HandleRequestPartyMemberStatsOpcode, LFGPackets.cpp (LFGJoin::Read) y
LFGHandler.cpp (SendLfgUpdateProposal, SendLfgRoleCheckUpdate) del core fijado.
"""
import struct

from .binario import Escritor, Lector

# Group.h
GROUP_TYPE_RAID, GROUP_TYPE_LFG = 0x02, 0x08
MEMBER_STATUS_ONLINE, MEMBER_STATUS_DEAD, MEMBER_STATUS_GHOST, MEMBER_STATUS_AFK = 0x01, 0x04, 0x08, 0x40
F_STATUS, F_CUR_HP, F_MAX_HP, F_POWER_TYPE = 0x1, 0x2, 0x4, 0x8
F_CUR_POWER, F_MAX_POWER, F_LEVEL, F_ZONE, F_POSITION, F_AURAS = 0x10, 0x20, 0x40, 0x80, 0x100, 0x200
F_PET_GUID, F_PET_NAME, F_PET_MODEL, F_PET_CUR_HP, F_PET_MAX_HP = 0x400, 0x800, 0x1000, 0x2000, 0x4000
F_PET_POWER_TYPE, F_PET_CUR_POWER, F_PET_MAX_POWER, F_PET_AURAS, F_VEHICLE = 0x8000, 0x10000, 0x20000, 0x40000, 0x80000

# LFG.h: roles
ROL_LIDER, ROL_TANQUE, ROL_SANADOR, ROL_DANO = 0x01, 0x02, 0x04, 0x08


def leer_lista_grupo(datos: bytes) -> dict:
    L = Lector(datos)
    tipo = L.u8()
    L.u8(); L.u8()
    roles_propios = L.u8()
    if tipo & GROUP_TYPE_LFG:
        L.u8(); L.u32()
    guid_grupo = L.u64()
    L.u32()
    miembros = []
    for _ in range(L.u32()):
        nombre = L.cadena()
        guid = L.u64()
        conectado, subgrupo, flags, roles = L.u8(), L.u8(), L.u8(), L.u8()
        miembros.append({"nombre": nombre, "guid": guid, "conectado": bool(conectado), "subgrupo": subgrupo,
                         "flags": flags, "roles": roles})
    lider = L.u64() if L.resto() >= 8 else 0
    return {"tipo": tipo, "guid": guid_grupo, "roles_propios": roles_propios, "miembros": miembros, "lider": lider}


def _i16(v):
    return v - 0x10000 if v & 0x8000 else v


def leer_estadisticas(datos: bytes, completo: bool = False) -> dict:
    """SMSG_PARTY_MEMBER_STATS[_FULL]. Las coordenadas viajan como uint16 del float (x, y)."""
    L = Lector(datos)
    if completo:
        L.u8()
    guid = L.guid_empaquetado()
    mascara = L.u32()
    e = {"guid": guid}
    if mascara & F_STATUS:
        e["estado"] = L.u16()
    if mascara & F_CUR_HP:
        e["vida"] = L.u32()
    if mascara & F_MAX_HP:
        e["vida_max"] = L.u32()
    if mascara & F_POWER_TYPE:
        e["tipo_poder"] = L.u8()
    if mascara & F_CUR_POWER:
        e["poder"] = L.u16()
    if mascara & F_MAX_POWER:
        e["poder_max"] = L.u16()
    if mascara & F_LEVEL:
        e["nivel"] = L.u16()
    if mascara & F_ZONE:
        e["zona"] = L.u16()
    if mascara & F_POSITION:
        e["x"], e["y"] = _i16(L.u16()), _i16(L.u16())
    if mascara & F_AURAS:
        m = L.u64()
        auras = []
        for i in range(64):
            if m & (1 << i):
                auras.append(L.u32())
                L.u8()
        e["auras"] = auras
    # Lo de la mascota no se usa; se deja de leer aquí (no hay campos propios detrás).
    return e


def estado_texto(estado: int) -> str:
    if not estado & MEMBER_STATUS_ONLINE:
        return "desconectado"
    if estado & MEMBER_STATUS_GHOST:
        return "fantasma"
    if estado & MEMBER_STATUS_DEAD:
        return "muerto"
    return "vivo"


# ── buscador ───────────────────────────────────────────────────────────────
def lfg_unirse(roles: int, mazmorras, comentario: str = "") -> bytes:
    e = Escritor().u32(roles).u8(0).u8(0).u8(len(mazmorras))
    for m in mazmorras:
        e.u32(m)
    e.u8(3).u8(0).u8(0).u8(0)
    return e.cadena(comentario).valor()


def lfg_roles(roles: int) -> bytes:
    return struct.pack("<B", roles)


def lfg_respuesta_propuesta(propuesta: int, aceptar: bool = True) -> bytes:
    return struct.pack("<IB", propuesta, 1 if aceptar else 0)


def leer_propuesta(datos: bytes) -> dict:
    L = Lector(datos)
    return {"mazmorra": L.u32() & 0x00FFFFFF, "estado": L.u8(), "id": L.u32(), "jefes_hechos": L.u32()}


def leer_resultado_union(datos: bytes) -> dict:
    L = Lector(datos)
    return {"resultado": L.u32(), "estado": L.u32()}


# ── campo de batalla / arena (BattlegroundMgr::BuildBattlegroundStatusPacket) ─
BG_STATUS_NONE, BG_STATUS_WAIT_QUEUE, BG_STATUS_WAIT_JOIN, BG_STATUS_IN_PROGRESS = 0, 1, 2, 3


def leer_estado_batalla(datos: bytes) -> dict:
    """SMSG_BATTLEFIELD_STATUS. Con `status` NONE (BuildBattlegroundStatusPacket,
    StatusID == STATUS_NONE) el paquete trae sólo la ranura y un u64(0) legado detrás
    (12 bytes en total); un paquete real necesita al menos otros 19 bytes (arenatype,
    es_arena, bg_tipo, la constante 0x1F90, nivel_min/max, instancia, clasificada,
    status) antes de que tenga sentido seguir leyendo."""
    L = Lector(datos)
    e = {"ranura": L.u32()}
    if L.resto() < 19:
        e["status"] = BG_STATUS_NONE
        return e
    e["arenatype"] = L.u8()
    e["es_arena"] = L.u8() != 0
    e["bg_tipo"] = L.u32()
    L.u16()                        # constante 0x1F90
    e["nivel_min"], e["nivel_max"] = L.u8(), L.u8()
    e["instancia"] = L.u32()
    e["clasificada"] = bool(L.u8())
    e["status"] = L.u32()
    if e["status"] == BG_STATUS_WAIT_QUEUE:
        e["espera_media_ms"], e["en_cola_ms"] = L.u32(), L.u32()
    elif e["status"] == BG_STATUS_WAIT_JOIN:
        e["mapa"] = L.u32()
        L.u64()
        e["quitar_cola_ms"] = L.u32()
    elif e["status"] == BG_STATUS_IN_PROGRESS:
        e["mapa"] = L.u32()
        L.u64()
        e["auto_salir_ms"], e["desde_inicio_ms"] = L.u32(), L.u32()
        e["faccion_arena"] = L.u8()
    return e


def puerto_campo(arenatype: int, bg_tipo: int, accion: int = 1) -> bytes:
    """CMSG_BATTLEFIELD_PORT (HandleBattleFieldPortOpcode): como el botón «Entrar»."""
    return Escritor().u8(arenatype).u8(0).u32(bg_tipo).u16(0x1F90).u8(accion).valor()
