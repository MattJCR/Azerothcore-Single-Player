# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Area triggers del lado del cliente.

Wow.exe manda CMSG_AREATRIGGER al entrar en el volumen de un trigger; el
servidor no lo detecta solo. Con selfbot, playerbots no lo hace por el jugador
(ReachAreaTriggerAction: «Selfbots reach their own area triggers through the
client»). Este vigilante hace lo mismo que el cliente: con la posición estimada
detecta la entrada (flanco) y avisa una sola vez hasta salir.

Geometría igual que Player::IsInAreaTriggerRadius / Position::IsWithinBox.
"""
import math


def dentro(t: dict, x, y, z, margen: float = 0.0) -> bool:
    if t["radio"] > 0:
        return math.sqrt((x - t["x"]) ** 2 + (y - t["y"]) ** 2 + (z - t["z"]) ** 2) <= t["radio"] + margen
    rot = 2 * math.pi - t["o"]
    s, c = math.sin(rot), math.cos(rot)
    dx0, dy0 = x - t["x"], y - t["y"]
    dx = dx0 * c - dy0 * s
    dy = dy0 * c + dx0 * s
    return (abs(dx) <= t["largo"] / 2 + margen and abs(dy) <= t["ancho"] / 2 + margen
            and abs(z - t["z"]) <= t["alto"] / 2 + margen)


class Vigilante:
    """Guarda los triggers del mapa actual y devuelve los que se acaban de pisar."""

    def __init__(self, triggers: list):
        self.todos = triggers
        self.mapa = None
        self.activos = []
        self.dentro = set()
        self.enviados = []           # (id, mapa) en orden

    def cambiar_mapa(self, mapa):
        if mapa != self.mapa:
            self.mapa = mapa
            self.activos = [t for t in self.todos if t["mapa"] == mapa]
            self.dentro = set()

    def revisar(self, x, y, z) -> list:
        nuevos = []
        ahora = set()
        for t in self.activos:
            if dentro(t, x, y, z):
                ahora.add(t["id"])
                if t["id"] not in self.dentro:
                    nuevos.append(t)
        self.dentro = ahora
        self.enviados += [(t["id"], self.mapa) for t in nuevos]
        return nuevos

    def mas_cercano(self, x, y, z, filtro=None):
        cand = [t for t in self.activos if filtro is None or filtro(t)]
        if not cand:
            return None
        return min(cand, key=lambda t: (t["x"] - x) ** 2 + (t["y"] - y) ** 2 + (t["z"] - z) ** 2)
