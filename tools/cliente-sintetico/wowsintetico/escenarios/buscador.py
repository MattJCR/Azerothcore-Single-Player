# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Buscador de mazmorras con mod-queue-bots (CS01, caso del buscador).

El cliente se apunta al buscador como DPS (CMSG_LFG_JOIN) a una mazmorra
concreta; queue-bots debe completar el grupo con bots, la propuesta llega y el
cliente la acepta (CMSG_LFG_PROPOSAL_RESULT, igual que el botón «Aceptar»),
el servidor lo teletransporta dentro (SMSG_NEW_WORLD), el grupo es de buscador
y queue-bots activa dungeon-clear sin orden del agente. Con `recorrido`, el
jugador pasa a selfbot y se hace la mazmorra entera con la misma vigilancia
que el caso `mazmorra`.
"""
import time

from .. import actualizaciones as upd
from .. import grupo as grp
from ..areatriggers import Vigilante
from ..catalogo import Parametro, caso
from ..telemetria import Telemetria
from . import PersonajeTemporal
from .mazmorra import MAZMORRAS, Recorrido, selfbot


def id_lfg(dbc, mapa) -> int:
    for d in dbc.mazmorras_lfg():
        if d["mapa"] == mapa and d["dificultad"] == 0 and d["tipo"] == 1:
            return d["id"]
    return 0


@caso(id="buscador", titulo="Buscador de mazmorras con queue-bots y dungeon-clear automático",
      descripcion=__doc__,
      etiquetas=("buscador", "lfg", "queue-bots", "dungeon-clear", "personaje"),
      acciones=("conectar", "leer", "personaje", "gm", "cola", "mazmorra", "selfbot"), requiere=("dbc",),
      parametros={
          "mazmorra": Parametro(str, "bfd", "clave de la mazmorra", opciones=sorted(MAZMORRAS)),
          "nivel": Parametro(int, 0, "nivel (0 = el recomendado)", minimo=0, maximo=80),
          "espera_cola": Parametro(int, 240, "segundos máximos en cola", minimo=60, maximo=1200),
          "recorrido": Parametro(bool, False, "hacer la mazmorra entera en selfbot"),
          "duracion": Parametro(int, 5400, "segundos máximos del recorrido", minimo=300, maximo=14400),
      },
      duracion_max=15000, protege=("CS01", "mod-queue-bots (buscador, dc automático)", "LFGMgr"),
      control="directo",
      observa=("SMSG_LFG_* (unión, propuesta)", "SMSG_NEW_WORLD", "SMSG_GROUP_LIST (grupo de buscador)",
               "mensajes de addon DC"),
      no_cubre=("ventana del buscador en Wow.exe", "recompensa final (SMSG_LFG_PLAYER_REWARD sólo se anota)"),
      en_todo=False)
def ejecutar(ctx):
    inf, p = ctx.inf, ctx.p
    datos = MAZMORRAS[p["mazmorra"]]
    nivel = p["nivel"] or datos["nivel"]
    sec = "buscador %s" % datos["nombre"]
    ident = id_lfg(ctx.dbc, datos["mapa"])
    if not inf.comprobar(sec, ident > 0, "la mazmorra está en LFGDungeons.dbc", "id %d" % ident, origen="entorno"):
        return
    m = ctx.nueva_sesion()
    estado = {"selfbot": False, "cola": False}

    def deshacer():
        if estado["selfbot"]:
            selfbot(m, False)
        m.comando("dc off", 1)
        if estado["cola"]:
            m.lfg_salir()
        if (m.grupo or {}).get("miembros"):
            m.dejar_grupo()
            m.bombear(3)

    with PersonajeTemporal(ctx, m, 1, 8, sec, antes_de_salir=deshacer) as pj:
        ctx.anotar_servidor(m)
        m.comando("gm off", 1)
        m.comando("levelup %d" % (nivel - 1), 3)
        m.comando_hasta("playerbots bot initself=rare", r"initself|ERROR", 20)
        inf.comprobar(sec, m.valor_propio(upd.UNIT_FIELD_LEVEL) == nivel, "nivel de prueba",
                      "nivel %d" % m.valor_propio(upd.UNIT_FIELD_LEVEL), origen="entorno")
        m.armar_areatriggers(Vigilante(ctx.dbc.areatriggers()))
        tele = Telemetria(m, ctx.artefacto("linea-de-tiempo.jsonl"), mapa_objetivo=datos["mapa"], eco=inf._eco)

        m.lfg_unirse(grp.ROL_DANO, [ident], auto_aceptar=True)
        estado["cola"] = True
        inicio = time.time()
        while time.time() - inicio < p["espera_cola"] and m.mapa != datos["mapa"]:
            m.bombear(1)
            union = m.lfg.get("union")
            if union and union["resultado"] != 0:
                break
        union = m.lfg.get("union")
        inf.comprobar(sec, not union or union["resultado"] == 0, "el buscador acepta la inscripción",
                      "resultado %s" % (union or "sin rechazo"))
        propuestas = m.lfg["propuestas"]
        ids = sorted({x["id"] for x in propuestas})
        inf.comprobar(sec, bool(propuestas), "llega una propuesta de grupo",
                      "%d propuesta(s) (%d actualizaciones) en %.0f s" % (len(ids), len(propuestas), time.time() - inicio))
        inf.comprobar(sec, len(propuestas) <= 20 * max(1, len(ids)), "sin tormenta de actualizaciones de propuesta",
                      "%d actualizaciones para %d propuesta(s)" % (len(propuestas), len(ids)), origen="cliente")
        dentro = m.mapa == datos["mapa"]
        inf.comprobar(sec, dentro, "el buscador teletransporta al jugador a la mazmorra",
                      "mapa %s a los %.0f s" % (m.mapa, time.time() - inicio),
                      esperado=datos["mapa"], observado=m.mapa)
        if not dentro:
            return
        estado["cola"] = False
        m.bombear(5)
        g = m.grupo or {}
        miembros = {x["guid"]: x["nombre"] for x in g.get("miembros", []) if x["guid"] != m.guid}
        inf.comprobar(sec, bool(g.get("tipo", 0) & grp.GROUP_TYPE_LFG), "el grupo es de buscador (GROUP_TYPE_LFG)",
                      "tipo 0x%02X" % g.get("tipo", 0))
        inf.comprobar(sec, len(miembros) == 4, "queue-bots completa el grupo con cuatro bots",
                      ", ".join(miembros.values()), esperado=4, observado=len(miembros))
        tele.fijar_grupo(miembros)
        limite = time.time() + 75
        while time.time() < limite and not tele.dc["visto_activo"]:
            m.bombear(1)
        inf.comprobar(sec, tele.dc["visto_activo"], "queue-bots activa dungeon-clear solo",
                      "activo a los %s s" % tele.dc.get("t_activo") if tele.dc["visto_activo"] else "sin STATUS activo en 75 s")
        if p["recorrido"]:
            final, textos = selfbot(m, True)
            estado["selfbot"] = bool(final)
            if inf.comprobar(sec, final is True, "selfbot activo para el recorrido", " / ".join(textos)[:200]):
                rec = Recorrido(ctx, m, tele, miembros, datos)
                fin = rec.ejecutar(p["duracion"], 15, False, 600, parar_en_grave=True)
                resumen = tele.resumen()
                inf.datos["recorrido"] = {"fin": fin, "intervenciones": rec.intervenciones, **resumen}
                pendientes = [j["nombre"] for j in resumen["jefes"].values() if j["estado"] != "dead"]
                inf.comprobar(sec, resumen["jefes"] and not pendientes, "todos los jefes muertos",
                              "pendientes: %s" % ", ".join(pendientes))
                for a in resumen["anomalias"]:
                    if a["severidad"] == "grave":
                        inf.anotar(sec, "FALLO", "anomalía grave: %s" % a["tipo"], a["texto"])
        tele.cerrar()
        inf.datos["lfg"] = {"id": ident, "propuestas": propuestas, "estado_dc": dict(tele.dc),
                            "chat_dc": tele.chat_dc[-20:], "personaje": pj.nombre}
        inf.comprobar(sec, not m.errores_lectura, "paquetes leídos sin error", "; ".join(m.errores_lectura[:3]),
                      origen="cliente")
