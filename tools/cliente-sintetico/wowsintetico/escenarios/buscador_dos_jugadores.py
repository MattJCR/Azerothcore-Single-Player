# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Buscador de mazmorras con tanque y sanador de dos cuentas reales (M44)."""

import time

from .. import actualizaciones as upd
from .. import grupo as grp
from ..catalogo import Parametro, caso
from ..ejecutor import Bloqueo
from . import PersonajeTemporal
from .buscador import id_lfg
from .mazmorra import MAZMORRAS


@caso(id="buscador-dos-jugadores", titulo="Buscador con dos jugadores y roles distintos",
      descripcion="""
Invita a un segundo jugador real, entra en el buscador como tanque y responde
al control de roles desde la otra cuenta como sanador. Comprueba que ambos
reciben la propuesta y entran juntos en la mazmorra con tres bots.
""",
      etiquetas=("buscador", "dos-cuentas", "queue-bots", "personaje"),
      acciones=("conectar", "leer", "personaje", "gm", "grupo", "cola"), requiere=("dbc",),
      parametros={"espera": Parametro(int, 240, "segundos máximos hasta la propuesta",
                                     minimo=60, maximo=600)},
      duracion_max=420, protege=("M44", "mod-queue-bots"), control="directo",
      observa=("SMSG_GROUP_LIST", "SMSG_LFG_ROLE_CHECK_UPDATE", "SMSG_LFG_PROPOSAL_UPDATE",
               "SMSG_NEW_WORLD en ambas sesiones"),
      no_cubre=("recorrido completo de la mazmorra",), en_todo=False)
def ejecutar(ctx):
    if not ctx.entorno.tiene_cuenta("secundaria"):
        raise Bloqueo("falta cuentas_adicionales.secundaria en el perfil")
    inf, sec = ctx.inf, "buscador-dos-jugadores"
    datos = MAZMORRAS["rfc"]
    ident = id_lfg(ctx.dbc, datos["mapa"])
    if not inf.comprobar(sec, ident > 0, "Sima Ígnea figura en LFGDungeons.dbc", origen="entorno"):
        return
    a = ctx.nueva_sesion()
    b = ctx.nueva_sesion("secundaria")
    rolechecks = {"a": 0, "b": 0}
    a.oyentes.append(lambda _t, tipo, _datos: rolechecks.__setitem__("a", rolechecks["a"] + 1)
                     if tipo == "lfg_roles" else None)
    b.oyentes.append(lambda _t, tipo, _datos: rolechecks.__setitem__("b", rolechecks["b"] + 1)
                     if tipo == "lfg_roles" else None)

    def deshacer(m):
        m.lfg_salir()
        if (m.grupo or {}).get("miembros"):
            m.dejar_grupo()
        m.bombear(2)
        if m.mapa == datos["mapa"]:
            m.comando("dc off", 1)
            m.comando("tele stormwind", 3)
            m.bombear(3)

    with PersonajeTemporal(ctx, a, 1, 1, sec, antes_de_salir=lambda: deshacer(a)) as pa:
        with PersonajeTemporal(ctx, b, 1, 5, sec, antes_de_salir=lambda: deshacer(b)) as pb:
            ctx.anotar_servidor(a)
            for m in (a, b):
                m.comando("gm off", 1)
                m.comando("levelup 15", 2)
            inf.comprobar(sec, a.valor_propio(upd.UNIT_FIELD_LEVEL) == 16
                          and b.valor_propio(upd.UNIT_FIELD_LEVEL) == 16,
                          "tanque y sanador son de nivel 16")
            a.invitar_grupo(pb.nombre)
            b.bombear(1)
            b.aceptar_grupo()
            a.bombear(2)
            b.bombear(2)
            juntos = (any(x["guid"] == pb.guid for x in (a.grupo or {}).get("miembros", []))
                      and any(x["guid"] == pa.guid for x in (b.grupo or {}).get("miembros", [])))
            if not inf.comprobar(sec, juntos, "ambos jugadores reales comparten grupo"):
                return

            # Sólo el líder envía CMSG_LFG_JOIN; cada miembro marca su rol en
            # CMSG_LFG_SET_ROLES al recibir SMSG_LFG_ROLE_CHECK_UPDATE.
            b.lfg.update(auto_aceptar=True, roles=grp.ROL_SANADOR,
                         propuestas=[], aceptadas=set(), union=None)
            a.lfg_unirse(grp.ROL_TANQUE, [ident], auto_aceptar=True)
            limite = time.time() + ctx.p["espera"]
            while time.time() < limite and (a.mapa != datos["mapa"] or b.mapa != datos["mapa"]):
                a.bombear(0.5)
                b.bombear(0.5)
                if a.lfg.get("union") and a.lfg["union"]["resultado"] != 0:
                    break
            inf.datos["rolechecks"] = rolechecks
            inf.datos["propuestas"] = {"principal": len(a.lfg["propuestas"]),
                                      "secundaria": len(b.lfg["propuestas"])}
            inf.comprobar(sec, rolechecks["a"] > 0 and rolechecks["b"] > 0,
                          "el servidor pide roles a las dos cuentas", str(rolechecks))
            inf.comprobar(sec, bool(a.lfg["propuestas"]) and bool(b.lfg["propuestas"]),
                          "ambos reciben la propuesta LFG", str(inf.datos["propuestas"]))
            dentro = a.mapa == datos["mapa"] and b.mapa == datos["mapa"]
            inf.comprobar(sec, dentro, "tanque y sanador entran juntos en Sima Ígnea",
                          "mapas %s, %s" % (a.mapa, b.mapa))
            if dentro:
                a.bombear(2)
                b.bombear(2)
                grupo = a.grupo or {}
                miembros = {x["guid"]: x for x in grupo.get("miembros", [])}
                inf.comprobar(sec, pb.guid in miembros and len(miembros) == 4,
                              "los dos humanos comparten grupo LFG con tres bots",
                              "4 compañeros visibles: %d" % len(miembros))
                inf.comprobar(sec, bool(grupo.get("roles_propios", 0) & grp.ROL_TANQUE)
                              and bool(miembros.get(pb.guid, {}).get("roles", 0) & grp.ROL_SANADOR),
                              "el grupo LFG registra tanque humano y sanador humano",
                              "roles: A=%d, B=%d" % (grupo.get("roles_propios", 0),
                                                       miembros.get(pb.guid, {}).get("roles", 0)))
            inf.comprobar(sec, not a.errores_lectura and not b.errores_lectura,
                          "paquetes de ambas sesiones leídos sin error",
                          "; ".join((a.errores_lectura + b.errores_lectura)[:3]), origen="cliente")
