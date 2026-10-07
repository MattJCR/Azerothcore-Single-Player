# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Trayectorias del servidor: SMSG_MONSTER_MOVE y posición estimada en cada instante.

Con `.playerbots bot self` el servidor mueve al propio jugador con splines
(Unit::UpdateSplinePosition actualiza también a los jugadores) y manda el
mismo SMSG_MONSTER_MOVE que para una criatura, incluido al cliente del jugador.
Wow.exe lo reproduce; aquí se interpola para saber dónde estamos (area
triggers, telemetría) sin mandar movimiento propio.

Espejo de Movement::PacketBuilder::WriteMonsterMove / WriteLinearPath /
WriteCatmullRomPath y ByteBuffer::appendPackXYZ del core fijado.
"""
import math
import time

from .binario import Lector

# MoveSplineFlag.h
SF_PARABOLIC = 0x00000800
SF_FLYING = 0x00002000
SF_CATMULLROM = 0x00040000
SF_CYCLIC = 0x00080000
SF_ANIMATION = 0x00200000
MASK_CATMULLROM = SF_FLYING | SF_CATMULLROM

# MonsterMoveType
MOVE_NORMAL, MOVE_STOP, MOVE_FACING_SPOT, MOVE_FACING_TARGET, MOVE_FACING_ANGLE = range(5)


def _con_signo(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def desempaquetar_xyz(p: int) -> tuple:
    return (_con_signo(p & 0x7FF, 11) * 0.25, _con_signo((p >> 11) & 0x7FF, 11) * 0.25,
            _con_signo((p >> 22) & 0x3FF, 10) * 0.25)


def empaquetar_xyz(x, y, z) -> int:
    """Igual que appendPackXYZ (sólo para pruebas)."""
    return (int(x / 0.25) & 0x7FF) | ((int(y / 0.25) & 0x7FF) << 11) | ((int(z / 0.25) & 0x3FF) << 22)


def leer_monster_move(datos: bytes, transporte: bool = False) -> dict:
    """Devuelve {"guid", "inicio": (x,y,z), "id", "tipo", "flags", "duracion_ms", "puntos": [...]}.
    Con tipo STOP, `puntos` es [inicio] y la duración 0."""
    L = Lector(datos)
    guid = L.guid_empaquetado()
    res = {"guid": guid}
    if transporte:
        res["transporte"] = L.guid_empaquetado()
        L.u8()
    L.u8()
    inicio = (L.f32(), L.f32(), L.f32())
    res.update(inicio=inicio, id=L.u32())
    tipo = L.u8()
    res["tipo"] = tipo
    if tipo == MOVE_STOP:
        res.update(flags=0, duracion_ms=0, puntos=[inicio])
        return res
    if tipo == MOVE_FACING_SPOT:
        L.bytes(12)
    elif tipo == MOVE_FACING_TARGET:
        L.u64()
    elif tipo == MOVE_FACING_ANGLE:
        L.f32()
    flags = L.u32()
    if flags & SF_ANIMATION:
        L.u8(); L.u32()
    duracion = L.u32()
    if flags & SF_PARABOLIC:
        L.f32(); L.u32()
    n = L.u32()
    puntos = [inicio]
    if flags & MASK_CATMULLROM:
        puntos += [(L.f32(), L.f32(), L.f32()) for _ in range(n)]
    else:
        destino = (L.f32(), L.f32(), L.f32())
        medio = tuple((a + b) / 2 for a, b in zip(inicio, destino))
        for _ in range(max(0, n - 1)):
            off = desempaquetar_xyz(L.u32())
            puntos.append(tuple(m - o for m, o in zip(medio, off)))
        puntos.append(destino)
    res.update(flags=flags, duracion_ms=duracion, puntos=puntos)
    return res


def _dist(a, b):
    return math.sqrt(sum((p - q) ** 2 for p, q in zip(a, b)))


class Trayectoria:
    """Un spline recibido: posición por tiempo, a velocidad constante por longitud."""

    def __init__(self, mov: dict, t0: float = None):
        self.t0 = t0 if t0 is not None else time.time()
        self.puntos = mov["puntos"]
        self.duracion = mov["duracion_ms"] / 1000.0
        self.ciclica = bool(mov["flags"] & SF_CYCLIC)
        self.tramos = [_dist(a, b) for a, b in zip(self.puntos, self.puntos[1:])]
        self.largo = sum(self.tramos)

    @property
    def destino(self):
        return self.puntos[-1]

    def terminada(self, t=None) -> bool:
        return not self.ciclica and ((t or time.time()) - self.t0) >= self.duracion

    def posicion(self, t=None) -> tuple:
        t = t or time.time()
        if self.duracion <= 0 or self.largo <= 0:
            return self.puntos[-1]
        f = (t - self.t0) / self.duracion
        f = f % 1.0 if self.ciclica else min(max(f, 0.0), 1.0)
        objetivo = f * self.largo
        for (a, b), largo in zip(zip(self.puntos, self.puntos[1:]), self.tramos):
            if objetivo <= largo and largo > 0:
                k = objetivo / largo
                return tuple(p + (q - p) * k for p, q in zip(a, b))
            objetivo -= largo
        return self.puntos[-1]
