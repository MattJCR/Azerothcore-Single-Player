# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Escenario de bots con jugador real (DisabledWithoutRealPlayer) y población.

El cliente sintético cuenta como jugador real: entra con un personaje nuevo,
espera y mide con `.server info` cuántos jugadores conectados (sesiones con
socket: humanos) y cuántos personajes en el mundo (humanos + bots) hay. Los
bots son la diferencia: así no se confunde un humano más con un bot. Además
cuenta los jugadores vistos cerca y anota el estado de world-bots.

También hace un ida y vuelta de `.wbots samaritano off`/`on`:
el modo en sí sigue apagado por defecto en el servidor (WorldBots.Samaritan =
0, no se activa aquí ni se altera la dificultad elegida), pero el comando de
un jugador cualquiera debe responder y no romper nada aunque no haya ningún
ayudante activo que retirar.
"""
import re
import time

from .. import actualizaciones as upd
from ..catalogo import Parametro, caso
from . import PersonajeTemporal, sin_colores

RAZA, CLASE = 1, 1          # guerrero humano: combinación nativa, empieza en Villanorte


def poblacion(m) -> dict:
    """`.server info`: 'Connected players: N. Characters in world: M.' (o en español)."""
    texto = sin_colores(" ".join(m.comando_hasta("server info", r"(?:Characters in world|Personajes en el mundo)", 4)))
    humanos = re.search(r"(?:Connected players|Jugadores conectados)\D*(\d+)", texto)
    mundo = re.search(r"(?:Characters in world|Personajes en el mundo)\D*(\d+)", texto)
    return {"conectados": int(humanos.group(1)) if humanos else -1,
            "en_mundo": int(mundo.group(1)) if mundo else -1}


@caso(id="bots", titulo="Bots del mundo con un jugador dentro",
      descripcion="""
Con `AiPlayerbot.DisabledWithoutRealPlayer` los bots sólo entran si hay un
jugador real. El cliente entra, espera `espera` segundos y comprueba que los
personajes en el mundo superan a los jugadores conectados (bots = diferencia),
cuántos jugadores se ven alrededor y qué dice world-bots.
""",
      etiquetas=("bots", "playerbots", "world-bots", "personaje"),
      acciones=("conectar", "leer", "personaje"),
      parametros={"espera": Parametro(int, 180, "segundos dentro antes de contar", minimo=30, maximo=1800)},
      duracion_max=2000, protege=("mod-playerbots", "mod-world-bots", "BOTS_DISABLED_WITHOUT_PLAYER"),
      control="directo",
      observa=("`.server info`", "SMSG_UPDATE_OBJECT de jugadores cercanos", "`.bots estado`",
                "`.wbots samaritano off`/`on`"),
      no_cubre=("qué hace cada bot", "reparto por nivel/zona (usar `.bots estado` a mano)",
                 "el envío real de un ayudante (exige WorldBots.Samaritan=1 y un jugador en apuros: no se activa en la VM)"))
def ejecutar(ctx):
    inf, sec, espera = ctx.inf, "bots", ctx.p["espera"]
    m = ctx.nueva_sesion()
    with PersonajeTemporal(ctx, m, RAZA, CLASE, sec):
        ctx.anotar_servidor(m)
        inicio = poblacion(m)
        inf.comprobar(sec, inicio["en_mundo"] >= 1, "`.server info` responde",
                      "al entrar: %d conectados, %d en el mundo" % (inicio["conectados"], inicio["en_mundo"]))
        muestras, vistos = [], set()
        limite = time.time() + espera
        while time.time() < limite:
            m.bombear(min(30, max(1, limite - time.time())))
            vistos |= {g for g, o in m.objetos.items() if o["typeid"] == upd.TYPEID_PLAYER and g != m.guid}
            muestras.append(poblacion(m))
        final = muestras[-1] if muestras else inicio
        bots = max((s["en_mundo"] - max(s["conectados"], 1) for s in muestras), default=0)
        estado_wb = sin_colores(" ".join(m.comando("bots estado", 2)))
        inf.datos["bots"] = {"al_entrar": inicio, "muestras": muestras, "vistos_cerca": len(vistos),
                             "world_bots": estado_wb[:600]}
        inf.comprobar(sec, final["conectados"] >= 1, "el cliente sintético cuenta como jugador conectado",
                      "%d conectados" % final["conectados"])
        inf.comprobar(sec, bots > 0, "los bots se conectan con un jugador dentro",
                      "hasta %d bots (personajes en el mundo − conectados) en %d s" % (bots, espera),
                      esperado="> 0", observado=bots)
        inf.comprobar(sec, len(vistos) > 0, "hay jugadores (bots) visibles alrededor del jugador",
                      "%d distintos" % len(vistos), estado_si_no="AVISO")
        inf.comprobar(sec, bool(estado_wb), "`.bots estado` (world-bots) responde", estado_wb[:160],
                      estado_si_no="AVISO")

        # ida y vuelta del rechazo de ayuda samaritana. No
        # depende de que el modo esté activo en el servidor (lo normal es que
        # no lo esté) ni activa nada: sólo comprueba que el comando responde y
        # que retirar ayudantes activos (aquí, ninguno) no rompe nada.
        off = sin_colores(" ".join(m.comando("wbots samaritano off", 2)))
        on = sin_colores(" ".join(m.comando("wbots samaritano on", 2)))
        inf.datos["samaritano"] = {"off": off[:200], "on": on[:200]}
        inf.comprobar(sec, "samaritan" in off.lower(), "`.wbots samaritano off` responde",
                      off[:160], estado_si_no="AVISO")
        inf.comprobar(sec, "samaritan" in on.lower(), "`.wbots samaritano on` responde",
                      on[:160], estado_si_no="AVISO")

        inf.comprobar(sec, not m.errores_lectura, "paquetes del mundo leídos sin error",
                      "; ".join(m.errores_lectura[:3]), origen="cliente")
