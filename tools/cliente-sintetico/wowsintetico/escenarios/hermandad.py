# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""mod-home-guild: adopción de la hermandad, bots conectados (M11) y preferencia de party-here (M21)."""
import random
import re
import time

from .. import actualizaciones as upd
from ..catalogo import Parametro, caso
from . import PersonajeTemporal, sin_colores

ESTADO = re.compile(r"Hermandad de casa '([^']*)': (\d+) companeros bot, (\d+) conectados")
MIEMBRO = re.compile(r"^\s+(\S+) \(nivel (\d+), ")


def _estado(m) -> tuple:
    """`.hermandad estado` -> (texto, total, conectados, {nombre: nivel} de los conectados)."""
    lineas = [sin_colores(t) for t in m.comando("hermandad estado", 3)]
    texto = "\n".join(lineas)
    r = ESTADO.search(texto)
    miembros = {}
    for linea in lineas:
        for trozo in linea.split("\n"):
            x = MIEMBRO.match(trozo)
            if x:
                miembros[x.group(1)] = int(x.group(2))
    return texto, (int(r.group(2)) if r else None), (int(r.group(3)) if r else None), miembros


@caso(id="hermandad", titulo="home-guild adopta tu hermandad, conecta a sus bots y party-here los prefiere",
      descripcion="""
Un mago de nivel `nivel` funda una hermandad con `.guild create` (el mismo
GuildScript::OnCreate que el flujo normal): home-guild debe adoptarla, reclutar
bots de su tramo y mantenerlos conectados (KeepOnline, M11). Después
`.grupo 1` pide un compañero a party-here, que debe ser de la hermandad (M21).
Al terminar se desactiva la hermandad de casa (expulsa a los bots) y se borra.
""",
      etiquetas=("home-guild", "party-here", "grupo", "bots", "personaje"),
      acciones=("conectar", "leer", "personaje", "gm", "grupo"),
      parametros={"nivel": Parametro(int, 25, "nivel del jugador", minimo=10, maximo=80),
                  "espera": Parametro(int, 240, "segundos máximos para reclutar y conectar", minimo=60, maximo=900)},
      duracion_max=900, protege=("mod-home-guild", "mod-party-here", "PLAN M11", "PLAN M21", "BotWorldAge.h"),
      control="directo",
      observa=("`.hermandad estado` (compañeros y conectados)", "SMSG_GROUP_LIST"),
      no_cubre=("renivelado de miembros rezagados, `.hermandad fijar`/`excluir` y reclamación por dueño "
                "inactivo: cubierto por el caso `hermandad-avanzada` (PLAN M36)",
                "reconectar a un miembro que se desconecta: playerbots no deja sacar a un bot aleatorio "
                "(`.kick` no afecta a sesiones sin socket y rechaza «logout»); lo cubre la simulación de KeepOnline",
                "el orden de ObjectAccessor que hacía fallar M21: sólo se ve el resultado"))
def ejecutar(ctx):
    inf, sec, nivel = ctx.inf, "home-guild", ctx.p["nivel"]
    m = ctx.nueva_sesion()
    estado = {"hermandad": None, "grupo": False}
    nombre_h = "Vs" + "".join(random.choice("bcdfglmnprstvz" if i % 2 == 0 else "aeiou") for i in range(8))

    def deshacer():
        if estado["grupo"]:
            m.comando("grupo fuera", 3)
        if estado["hermandad"]:
            r = sin_colores(" ".join(m.comando("hermandad desactivar", 4)))
            inf.datos["desactivar"] = r[:200]
            m.comando('guild delete "%s"' % estado["hermandad"], 3)

    with PersonajeTemporal(ctx, m, 1, 8, sec, antes_de_salir=deshacer) as pj:
        ctx.anotar_servidor(m)
        m.comando("levelup %d" % (nivel - 1), 3)
        inf.comprobar(sec, m.valor_propio(upd.UNIT_FIELD_LEVEL) == nivel, "nivel de prueba",
                      "nivel %d" % m.valor_propio(upd.UNIT_FIELD_LEVEL), origen="entorno")

        r = sin_colores(" ".join(m.comando('guild create %s "%s"' % (pj.nombre, nombre_h), 4)))
        estado["hermandad"] = nombre_h
        inf.datos["guild_create"] = r[:200]

        # Adopción + reclutamiento (Care cada 30 s) + conexión por tandas (KeepOnline cada 15 s).
        limite = time.time() + ctx.p["espera"]
        historia, texto, total, conectados, miembros = [], "", None, None, {}
        t0 = time.time()
        while time.time() < limite:
            texto, total, conectados, miembros = _estado(m)
            historia.append((round(time.time() - t0), total, conectados))
            if total and conectados == total and total >= 5:
                break
            m.bombear(10)
        inf.datos["hermandad"] = {"nombre": nombre_h, "total": total, "conectados": conectados,
                                  "evolucion": historia, "miembros": miembros}
        inf.comprobar(sec, total is not None, "home-guild adopta la hermandad recién fundada", texto[:200])
        inf.comprobar(sec, bool(total), "recluta bots de tu tramo", "%s compañeros" % total,
                      esperado=">0", observado=total)
        inf.comprobar(sec, bool(total) and conectados == total,
                      "sus miembros están todos conectados (KeepOnline, M11)",
                      "evolución (s, total, conectados): %s" % historia[-8:],
                      esperado=total, observado=conectados)
        fuera = {n: v for n, v in miembros.items() if not (nivel - 3 <= v <= nivel + 2)}
        inf.comprobar(sec, not fuera, "los miembros son del tramo de la hermandad (-3/+2)",
                      ", ".join("%s %d" % x for x in fuera.items()), estado_si_no="AVISO")

        if miembros:
            m.comando("grupo 1", 2)
            estado["grupo"] = True
            limite = time.time() + 90
            companero = None
            while time.time() < limite and not companero:
                m.bombear(2)
                otros = [x for x in (m.grupo or {}).get("miembros", []) if x["guid"] != m.guid]
                companero = otros[0]["nombre"] if otros else None
            inf.datos["companero"] = companero
            inf.comprobar(sec, companero is not None, "`.grupo 1` trae un compañero", str(companero))
            inf.comprobar(sec, companero in miembros,
                          "party-here prefiere a los de tu hermandad (M21)",
                          "%s; hermandad conectada: %s" % (companero, ", ".join(sorted(miembros))[:200]))
            m.comando("grupo fuera", 3)
            m.bombear(4)
            estado["grupo"] = bool([x for x in (m.grupo or {}).get("miembros", []) if x["guid"] != m.guid])
