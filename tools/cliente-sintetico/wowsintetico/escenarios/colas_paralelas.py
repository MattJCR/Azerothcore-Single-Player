# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Dos colas LFG independientes con humanos simultáneos (M45)."""

import time

from .. import actualizaciones as upd
from .. import grupo as grp
from ..catalogo import Parametro, caso
from ..ejecutor import Bloqueo
from ..mundo import CMSG_LFG_PROPOSAL_RESULT, CMSG_LFG_SET_ROLES
from . import PersonajeTemporal
from .buscador import id_lfg
from .mazmorra import MAZMORRAS


@caso(id="colas-paralelas", titulo="Queue-bots rellena dos colas humanas a la vez",
      descripcion="""
Dos personajes de cuentas distintas se apuntan a mazmorras diferentes. Se
mantiene pendiente la primera propuesta hasta que llega la segunda: ambas
colas tienen que progresar mientras los dos humanos siguen encolados.
Después se aceptan las propuestas y se comprueba la entrada de ambos.
""",
      etiquetas=("buscador", "dos-cuentas", "queue-bots", "personaje"),
      acciones=("conectar", "leer", "personaje", "gm", "cola"), requiere=("dbc",),
      parametros={"espera": Parametro(int, 180, "segundos máximos para ambas propuestas",
                                     minimo=60, maximo=600)},
      duracion_max=360, protege=("M45", "mod-queue-bots"), control="directo",
      observa=("propuestas LFG simultáneas para dos jugadores", "entrada en mapas distintos"),
      no_cubre=("tiempo exacto de cada pasada interna de QueueBots.ScanIntervalMs",), en_todo=False)
def ejecutar(ctx):
    if not ctx.entorno.tiene_cuenta("secundaria"):
        raise Bloqueo("falta cuentas_adicionales.secundaria en el perfil")
    inf, sec = ctx.inf, "colas-paralelas"
    datos_a, datos_b = MAZMORRAS["rfc"], MAZMORRAS["wc"]
    ida, idb = id_lfg(ctx.dbc, datos_a["mapa"]), id_lfg(ctx.dbc, datos_b["mapa"])
    if not inf.comprobar(sec, ida > 0 and idb > 0, "ambas mazmorras figuran en LFGDungeons.dbc",
                        "ids %d, %d" % (ida, idb), origen="entorno"):
        return
    a, b = ctx.nueva_sesion(), ctx.nueva_sesion("secundaria")
    tiempos = {"a": None, "b": None}
    for clave, m in (("a", a), ("b", b)):
        def observar(_t, tipo, datos, clave=clave, m=m):
            if tipo == "lfg_roles":
                m.enviar(CMSG_LFG_SET_ROLES, grp.lfg_roles(grp.ROL_DANO))
            elif tipo == "lfg_propuesta" and tiempos[clave] is None and datos.get("estado") == 0:
                tiempos[clave] = _t
        m.oyentes.append(observar)

    def deshacer(m, mapa):
        m.lfg_salir()
        if (m.grupo or {}).get("miembros"):
            m.dejar_grupo()
        m.bombear(2)
        if m.mapa == mapa:
            m.comando("dc off", 1)
            m.comando("tele stormwind", 3)
            m.bombear(3)

    with PersonajeTemporal(ctx, a, 1, 8, sec, antes_de_salir=lambda: deshacer(a, datos_a["mapa"])):
        with PersonajeTemporal(ctx, b, 1, 8, sec, antes_de_salir=lambda: deshacer(b, datos_b["mapa"])):
            ctx.anotar_servidor(a)
            for m, nivel in ((a, datos_a["nivel"]), (b, datos_b["nivel"])):
                m.comando("gm off", 1)
                m.comando("levelup %d" % (nivel - 1), 2)
                inf.comprobar(sec, m.valor_propio(upd.UNIT_FIELD_LEVEL) == nivel,
                              "nivel %d de %s" % (nivel, m.usuario), origen="entorno")
            a.lfg_unirse(grp.ROL_DANO, [ida], auto_aceptar=False)
            b.lfg_unirse(grp.ROL_DANO, [idb], auto_aceptar=False)
            inicio = time.time()
            while time.time() - inicio < ctx.p["espera"] and not all(tiempos.values()):
                a.bombear(0.5)
                b.bombear(0.5)
            inf.datos["segundos_propuesta"] = {k: round(t - inicio, 1) if t else None
                                             for k, t in tiempos.items()}
            ambas = all(tiempos.values())
            if not inf.comprobar(sec, ambas, "ambas colas reciben propuesta mientras la otra sigue pendiente",
                                 str(inf.datos["segundos_propuesta"])):
                return
            for m in (a, b):
                propuesta = next((p for p in m.lfg["propuestas"] if p["estado"] == 0), None)
                if propuesta:
                    m.enviar(CMSG_LFG_PROPOSAL_RESULT, grp.lfg_respuesta_propuesta(propuesta["id"], True))
            limite = time.time() + 45
            while time.time() < limite and (a.mapa != datos_a["mapa"] or b.mapa != datos_b["mapa"]):
                a.bombear(0.5)
                b.bombear(0.5)
            inf.comprobar(sec, a.mapa == datos_a["mapa"] and b.mapa == datos_b["mapa"],
                          "ambos entran en sus mazmorras distintas",
                          "mapas %s y %s" % (a.mapa, b.mapa))
            inf.comprobar(sec, not a.errores_lectura and not b.errores_lectura,
                          "ambas sesiones leen paquetes sin errores",
                          "; ".join((a.errores_lectura + b.errores_lectura)[:3]), origen="cliente")
