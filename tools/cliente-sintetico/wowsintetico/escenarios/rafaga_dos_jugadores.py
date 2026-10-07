# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Ráfaga de comandos y misiones con dos jugadores reales a la vez (PLAN M52).

`g_pending` (la cola de eventos entre los hooks de jugador/mapa y `OnUpdate`)
de `mod-quest-mates`, `mod-party-here` y `mod-home-guild` no tiene límite
explícito. Aquí dos sesiones reales, solapadas en el tiempo (los comandos de
A y B se mandan sin esperar la respuesta del anterior, `comando(..., espera=0)`,
así los dos llegan al mundo antes de que corra el siguiente `OnUpdate`),
repiten varias veces seguidas: aceptar/abandonar la misma misión (Willem,
quest-mates) y `.grupo`/`.grupo fuera` (party-here); a mitad de la ráfaga B se
reconecta de verdad (logout/login), lo que dispara `REQ_LOGOUT`/`REQ_LOGIN` en
ambos módulos mientras A sigue mandando comandos.

La profundidad real de cada cola (`SlowTick::WarnIfDeep`, añadido en esta
misma tarea) y los avisos de "Tick lento" se leen aparte en Server.log; este
caso comprueba el estado FINAL tras la ráfaga: la hermandad de A (creada antes
de empezar) sigue existiendo con él como líder, ninguno de los dos queda en un
grupo huérfano, y ambas sesiones siguen respondiendo con normalidad.
"""
import time

from .. import actualizaciones as upd
from ..catalogo import Parametro, caso
from ..ejecutor import Bloqueo
from ..mundo import ErrorMundo
from . import PersonajeTemporal, nombre_aleatorio, sin_colores
from .misiones import AMENAZA_INTERIOR, AYUDANTE_WILLEM


def _companeros(m) -> dict:
    return {x["guid"]: x["nombre"] for x in (m.grupo or {}).get("miembros", []) if x["guid"] != m.guid}


def _ir_a_willem(m, inf, sec, quien):
    willem = next((c for c in m.criaturas() if c["entrada"] == AYUDANTE_WILLEM), None)
    if inf.comprobar(sec, willem is not None, "el Ayudante Willem (823) esta a la vista para %s" % quien,
                     "mapa %s" % m.mapa, origen="entorno"):
        m.ir_a(willem["pos"], 2)
    return willem


def _rafaga_mision(m, willem, vueltas):
    """Acepta/abandona la misma mision `vueltas` veces con el mínimo respiro
    posible: `abandonar_mision` mira el diario CACHEADO en el cliente
    (`self.misiones()`), así que sin un `bombear` de por medio el cliente
    todavía no se ha enterado de que acaba de aceptarla y el abandono no
    manda nada. Aun con este medio segundo por vuelta, mision+grupo de A y B
    intercalados siguen llegando de golpe al `g_pending` de quest-mates y
    party-here, que es lo que se quiere poner a prueba."""
    if not willem:
        return
    for _ in range(vueltas):
        m.aceptar_mision(willem["guid"], AMENAZA_INTERIOR)
        m.bombear(0.5)
        m.abandonar_mision(AMENAZA_INTERIOR)
        m.bombear(0.5)


def _rafaga_grupo(m, vueltas):
    """`.grupo`/`.grupo fuera` seguidos, sin esperar a que lleguen companeros:
    lo que le pasa a `g_pending` de party-here con alguien tecleando rapido."""
    for _ in range(vueltas):
        m.comando("grupo", espera=0)
        m.comando("grupo fuera", espera=0)


@caso(id="rafaga-dos-jugadores", titulo="Rafaga de comandos y misiones con dos jugadores (M52)",
      descripcion=__doc__,
      etiquetas=("quest-mates", "party-here", "home-guild", "dos-cuentas", "personaje"),
      acciones=("conectar", "leer", "personaje", "gm", "grupo"),
      parametros={
          "espera": Parametro(int, 60, "segundos dentro antes de la rafaga (entrada de bots)",
                               minimo=0, maximo=600),
          "vueltas": Parametro(int, 5, "repeticiones de cada rafaga por jugador", minimo=1, maximo=20),
          "espera_asiento": Parametro(int, 30, "segundos tras la rafaga para que drenen las colas",
                                       minimo=10, maximo=180),
      },
      duracion_max=900, protege=("mod-quest-mates", "mod-party-here", "mod-home-guild", "PLAN M52"),
      control="directo",
      observa=("PLAYER_GUILDID de A tras la rafaga", "SMSG_GROUP_LIST de A y B", "reconexion real de B"),
      no_cubre=("profundidad exacta de g_pending por pasada: se lee aparte en Server.log "
                "(SlowTick::WarnIfDeep \"quest-mates|party-here|home-guild\"/\"g_pending\")",
                "hermandad de B (sólo se funda una, para A)"),
      en_todo=False)
def ejecutar(ctx):
    if not ctx.entorno.tiene_cuenta("secundaria"):
        raise Bloqueo("falta cuentas_adicionales.secundaria en el perfil")
    inf, sec, p = ctx.inf, "rafaga-dos-jugadores", ctx.p
    nombre_h = nombre_aleatorio("Vs")
    a = ctx.nueva_sesion()
    estado = {"a_grupo": False, "b_grupo": False, "hermandad": False}

    def deshacer_a():
        if AMENAZA_INTERIOR in a.misiones():
            a.abandonar_mision(AMENAZA_INTERIOR)
        if estado["a_grupo"]:
            a.comando("grupo fuera", 3)
        if estado["hermandad"]:
            a.comando('guild delete "%s"' % nombre_h, 4)

    with PersonajeTemporal(ctx, a, 1, 1, sec, antes_de_salir=deshacer_a) as pa:
        ctx.anotar_servidor(a)
        inf.paso("esperando %d s a que los bots entren (DisabledWithoutRealPlayer)" % p["espera"])
        a.bombear(p["espera"])

        r = sin_colores(" ".join(a.comando('guild create %s "%s"' % (pa.nombre, nombre_h), 3)))
        estado["hermandad"] = a.valor_propio(upd.PLAYER_GUILDID) != 0
        inf.comprobar(sec, estado["hermandad"], "A funda una hermandad antes de la rafaga (`.guild create`)",
                      "PLAYER_GUILDID=%d (%s)" % (a.valor_propio(upd.PLAYER_GUILDID), r[:120]))

        b = ctx.nueva_sesion("secundaria")

        def deshacer_b():
            if AMENAZA_INTERIOR in b.misiones():
                b.abandonar_mision(AMENAZA_INTERIOR)
            if estado["b_grupo"]:
                b.comando("grupo fuera", 3)

        with PersonajeTemporal(ctx, b, 1, 4, sec, antes_de_salir=deshacer_b) as pb:
            ctx.anotar_servidor(b)
            b.bombear(5)

            willem_a = _ir_a_willem(a, inf, sec, "A")
            willem_b = _ir_a_willem(b, inf, sec, "B")

            # ── La rafaga en si: misiones y grupo intercalados, sin esperar
            # la respuesta del otro entre medias (comando(..., espera=0)). ──
            inf.paso("rafaga: %d vueltas de mision+grupo intercaladas entre A y B" % p["vueltas"])
            for i in range(p["vueltas"]):
                _rafaga_mision(a, willem_a, 1)
                _rafaga_mision(b, willem_b, 1)
                _rafaga_grupo(a, 1)
                _rafaga_grupo(b, 1)
                a.bombear(1)
                b.bombear(1)

                # A mitad de la rafaga, B se reconecta de verdad: REQ_LOGOUT +
                # REQ_LOGIN en quest-mates y party-here mientras A sigue mandando.
                # El logout falla con "código 1" si un companion de la rafaga
                # anterior sigue metiendo a B en combate (fauna de Villanorte
                # de por medio); `.grupo fuera` + un par de reintentos con
                # margen lo resuelve sin renunciar a que sea una reconexion real.
                if i == p["vueltas"] // 2:
                    guid_b = pb.guid
                    b.comando("grupo fuera", 2)
                    for intento in range(3):
                        try:
                            b.salir()
                            break
                        except ErrorMundo:
                            if intento == 2:
                                raise
                            b.bombear(5)
                    a.comando("grupo", espera=0)   # A sigue tecleando durante la reconexion de B
                    b.entrar(guid_b, 1, nombre=pb.nombre, espera_mundo=8)
                    inf.comprobar(sec, b.guid == guid_b, "B se reconecta de verdad a mitad de la rafaga",
                                  "guid tras reconectar: %s" % b.guid)

            inf.paso("asentando %d s tras la rafaga" % p["espera_asiento"])
            limite = time.time() + p["espera_asiento"]
            while time.time() < limite:
                a.bombear(3)
                b.bombear(3)

            # ── Estado final ──────────────────────────────────────────────
            guild_a = a.valor_propio(upd.PLAYER_GUILDID)
            inf.comprobar(sec, guild_a != 0, "la hermandad de A sigue en pie tras la rafaga",
                          "PLAYER_GUILDID=%d" % guild_a, esperado="!=0", observado=guild_a)

            a.comando("grupo fuera", 3)
            b.comando("grupo fuera", 3)
            a.bombear(3)
            b.bombear(3)
            comp_a, comp_b = _companeros(a), _companeros(b)
            estado["a_grupo"], estado["b_grupo"] = bool(comp_a), bool(comp_b)
            inf.comprobar(sec, not comp_a, "A no queda con companeros huerfanos tras la rafaga y el `.grupo fuera` final",
                          "%d companero(s): %s" % (len(comp_a), ", ".join(comp_a.values())),
                          esperado=0, observado=len(comp_a))
            inf.comprobar(sec, not comp_b, "B no queda con companeros huerfanos tras la rafaga y el `.grupo fuera` final",
                          "%d companero(s): %s" % (len(comp_b), ", ".join(comp_b.values())),
                          esperado=0, observado=len(comp_b))
            estado["a_grupo"], estado["b_grupo"] = False, False

            inf.comprobar(sec, AMENAZA_INTERIOR not in a.misiones() and AMENAZA_INTERIOR not in b.misiones(),
                          "ninguno de los dos se queda con la mision en el diario tras la rafaga",
                          "A: %s, B: %s" % (AMENAZA_INTERIOR in a.misiones(), AMENAZA_INTERIOR in b.misiones()))

            inf.comprobar(sec, not a.errores_lectura and not b.errores_lectura,
                          "paquetes de ambas sesiones leidos sin error tras la rafaga",
                          "; ".join((a.errores_lectura + b.errores_lectura)[:3]), origen="cliente")
