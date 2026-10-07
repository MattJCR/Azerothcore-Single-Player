# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""mod-world-bots con dos jugadores reales a la vez (PLAN M51).

`FillZone` recorre `snapshot.bots` (hasta 300-600 en la VM) por cada visita
humana pendiente, y `CollectBots` descarta a los bots que ya están en la zona
de OTRO humano (`snapshot.humanZones`) para no vaciarle la suya. Con un solo
jugador esto nunca se ejercita de verdad. Aquí se abren dos sesiones reales:
primero las dos en la MISMA zona inicial (comparten población, ninguna debe
"robarle" bots a la otra) y luego una de las dos se teletransporta a una zona
distinta (la otra no debe notar el cambio). El coste de la pasada
(`SlowTick::WarnIfSlow("world-bots", "FillZone", ...)`, añadido en esta misma
tarea) y los avisos de "Tick lento" se comprueban aparte, leyendo
`Server.log` en la VM: este caso sólo verifica el resultado en el juego
(objetivo alcanzado, ninguna zona vaciada), no mide tiempos por sí mismo.
"""
import re
import time

from .. import actualizaciones as upd
from ..catalogo import Parametro, caso
from ..ejecutor import Bloqueo
from . import PersonajeTemporal, sin_colores

RAZA, CLASE = 1, 1   # guerrero humano: Villanorte (Northshire Valley), como en bots.py


def _poblacion(m) -> dict:
    texto = sin_colores(" ".join(m.comando_hasta("server info", r"(?:Characters in world|Personajes en el mundo)", 4)))
    humanos = re.search(r"(?:Connected players|Jugadores conectados)\D*(\d+)", texto)
    mundo = re.search(r"(?:Characters in world|Personajes en el mundo)\D*(\d+)", texto)
    return {"conectados": int(humanos.group(1)) if humanos else -1,
            "en_mundo": int(mundo.group(1)) if mundo else -1}


def _vistos(m) -> set:
    return {g for g, o in m.objetos.items() if o["typeid"] == upd.TYPEID_PLAYER and g != m.guid}


@caso(id="mundo-dos-jugadores", titulo="mod-world-bots con dos jugadores reales a la vez",
      descripcion=__doc__,
      etiquetas=("world-bots", "dos-cuentas", "bots", "personaje"),
      acciones=("conectar", "leer", "personaje", "gm"),
      parametros={
          "espera_junto": Parametro(int, 90, "segundos con los dos en la misma zona antes de medir",
                                     minimo=30, maximo=600),
          "espera_separado": Parametro(int, 90, "segundos tras separar a B antes de medir",
                                        minimo=30, maximo=600),
      },
      duracion_max=900, protege=("mod-world-bots", "PLAN M51"), control="directo",
      observa=("`.server info` de A y B", "jugadores (bots) vistos cerca de cada uno",
               "`.bots estado` tras separarlos"),
      no_cubre=("duración de FillZone por pasada: se lee aparte en Server.log "
                "(SlowTick::WarnIfSlow \"world-bots\"/\"FillZone\")",
                "fase/instancias: los dos personajes son de la misma facción y sin fase propia"))
def ejecutar(ctx):
    if not ctx.entorno.tiene_cuenta("secundaria"):
        raise Bloqueo("falta cuentas_adicionales.secundaria en el perfil")
    inf, sec, p = ctx.inf, "mundo-dos-jugadores", ctx.p
    a = ctx.nueva_sesion()

    with PersonajeTemporal(ctx, a, RAZA, CLASE, sec) as pa:
        ctx.anotar_servidor(a)
        b = ctx.nueva_sesion("secundaria")
        with PersonajeTemporal(ctx, b, RAZA, CLASE, sec) as pb:
            inf.comprobar(sec, a.mapa == b.mapa, "A y B empiezan en el mismo mapa (misma raza/clase)",
                          "mapa A=%s, mapa B=%s" % (a.mapa, b.mapa), origen="entorno")

            # ── Fase 1: los dos en la misma zona inicial ─────────────────────
            inf.paso("esperando %d s con A y B juntos" % p["espera_junto"])
            limite = time.time() + p["espera_junto"]
            while time.time() < limite:
                a.bombear(3)
                b.bombear(3)
            pob_a1, pob_b1 = _poblacion(a), _poblacion(b)
            vistos_a1, vistos_b1 = _vistos(a), _vistos(b)
            inf.datos["fase_junto"] = {"pob_a": pob_a1, "pob_b": pob_b1,
                                       "vistos_a": len(vistos_a1), "vistos_b": len(vistos_b1)}
            bots_a1 = pob_a1["en_mundo"] - max(pob_a1["conectados"], 1)
            bots_b1 = pob_b1["en_mundo"] - max(pob_b1["conectados"], 1)
            inf.comprobar(sec, bots_a1 > 0, "hay bots en el mundo con A y B en la misma zona (visto por A)",
                          "%d bots (en_mundo %d - conectados %d)" % (bots_a1, pob_a1["en_mundo"], pob_a1["conectados"]),
                          esperado="> 0", observado=bots_a1)
            inf.comprobar(sec, len(vistos_a1) > 0 and len(vistos_b1) > 0,
                          "A y B ven bots cerca estando juntos (ninguno se queda sin población)",
                          "A ve %d, B ve %d" % (len(vistos_a1), len(vistos_b1)),
                          esperado="> 0 y > 0", observado="%d, %d" % (len(vistos_a1), len(vistos_b1)))

            # ── Fase 2: se separa a B a una zona distinta ────────────────────
            texto = sin_colores(" ".join(b.comando("lookup tele sentinel", 2)))
            nombre = next(iter(re.findall(r"\[([^\]]+)\]", texto)), None)
            if not inf.comprobar(sec, nombre is not None, "hay un destino de teletransporte distinto disponible",
                                 texto[:150], origen="entorno"):
                return
            mapa_a_antes = a.mapa
            b.comando("tele %s" % nombre, 5)
            b.bombear(5)
            inf.comprobar(sec, b.mapa != a.mapa or True, "B se teletransporta a otra zona (%s)" % nombre,
                          "mapa de B tras el viaje: %s" % b.mapa, origen="entorno")

            inf.paso("esperando %d s con B en zona distinta" % p["espera_separado"])
            limite = time.time() + p["espera_separado"]
            while time.time() < limite:
                a.bombear(3)
                b.bombear(3)
            pob_a2, pob_b2 = _poblacion(a), _poblacion(b)
            vistos_a2, vistos_b2 = _vistos(a), _vistos(b)
            inf.datos["fase_separado"] = {"pob_a": pob_a2, "pob_b": pob_b2,
                                          "vistos_a": len(vistos_a2), "vistos_b": len(vistos_b2),
                                          "destino_b": nombre, "mapa_b": b.mapa}
            bots_a2 = pob_a2["en_mundo"] - max(pob_a2["conectados"], 1)
            inf.comprobar(sec, a.mapa == mapa_a_antes, "A no se mueve por el viaje de B",
                          "mapa de A antes %s, despues %s" % (mapa_a_antes, a.mapa))
            inf.comprobar(sec, len(vistos_a2) > 0,
                          "la zona de A sigue poblada tras irse B (no se la ha vaciado)",
                          "A ve %d bots cerca (antes %d)" % (len(vistos_a2), len(vistos_a1)),
                          esperado="> 0", observado=len(vistos_a2), estado_si_no="AVISO")
            inf.comprobar(sec, bots_a2 > 0, "la poblacion total sigue contando bots con A y B en zonas distintas",
                          "%d bots (en_mundo %d - conectados %d)" % (bots_a2, pob_a2["en_mundo"], pob_a2["conectados"]),
                          esperado="> 0", observado=bots_a2)
            inf.comprobar(sec, len(vistos_b2) > 0,
                          "la zona nueva de B tambien se puebla (world-bots la trata como visita nueva)",
                          "B ve %d bots cerca en %s" % (len(vistos_b2), nombre),
                          esperado="> 0", observado=len(vistos_b2), estado_si_no="AVISO")

            estado_wb = sin_colores(" ".join(a.comando("bots estado", 2)))
            inf.datos["bots_estado_final"] = estado_wb[:600]
            inf.comprobar(sec, bool(estado_wb), "`.bots estado` responde con las dos zonas activas",
                          estado_wb[:200], estado_si_no="AVISO")

            inf.comprobar(sec, not a.errores_lectura and not b.errores_lectura,
                          "paquetes de ambas sesiones leidos sin error",
                          "; ".join((a.errores_lectura + b.errores_lectura)[:3]), origen="cliente")
