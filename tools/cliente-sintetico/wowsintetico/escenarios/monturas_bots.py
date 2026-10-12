# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Los bots siguen montando tras mod-playerbots #2884 (monturas condicionadas a la Equitación).

Desde mod-playerbots 79bd428 un bot no usa una montura si su habilidad de Equitación es menor que la
que exige. Los bots aleatorios de la VM llevan la Equitación de su tramo (75/150/225/300), así que los de
nivel 20 o más deberían seguir montando. Este caso lo mira desde fuera: el personaje de prueba sube a un
nivel medio y se teletransporta a una ciudad grande, donde los bots se concentran, y durante `espera`
segundos anota de cada jugador visto su nivel y si alguna vez llevó una montura puesta
(`UNIT_FIELD_MOUNTDISPLAYID` distinto de 0).

Es una observación de población, no una garantía por bot: que un bot concreto no se vea montado no
prueba nada. Por eso, si no se ve ninguno montado, el resultado es AVISO y no FALLO.
"""
import time

from .. import actualizaciones as upd
from ..catalogo import Parametro, caso
from . import PersonajeTemporal

RAZA, CLASE = 1, 1                       # guerrero humano
UNIT_FIELD_MOUNTDISPLAYID = upd.OBJECT_END + 0x003F
NIVEL_PRUEBA = 45                        # tramo 40-49: Equitación 150 en los bots


@caso(id="monturas-bots", titulo="Bots de nivel 20 o más siguen montando (#2884, Equitación)",
      descripcion=__doc__,
      etiquetas=("bots", "playerbots", "monturas", "personaje"),
      acciones=("conectar", "gm", "leer", "personaje"),
      parametros={"espera": Parametro(int, 240, "segundos de observación en la ciudad", minimo=60, maximo=1800),
                  "destino": Parametro(str, "stormwind", "nombre de `.tele` donde observar")},
      duracion_max=2000, protege=("mod-playerbots #2884", "UPD-L"),
      control="directo",
      observa=("SMSG_UPDATE_OBJECT de jugadores cercanos: nivel y UNIT_FIELD_MOUNTDISPLAYID",),
      no_cubre=("qué Equitación tiene cada bot (se mira en SQL: character_skills, habilidad 762)",
                "bots de nivel 55 sin Equitación: no montan por diseño y este caso no los distingue",
                "otras zonas o niveles del personaje distintos del de prueba"))
def ejecutar(ctx):
    inf, sec = ctx.inf, "monturas-bots"
    m = ctx.nueva_sesion()
    with PersonajeTemporal(ctx, m, RAZA, CLASE, sec):
        ctx.anotar_servidor(m)
        m.comando("levelup %d" % (NIVEL_PRUEBA - 1), 3)
        m.bombear(2)
        m.comando("tele %s" % ctx.p["destino"], 3)
        m.bombear(5)
        vistos = {}                                        # guid -> {"nivel": n, "montado": bool}
        limite = time.time() + ctx.p["espera"]
        while time.time() < limite:
            m.bombear(min(10, max(1, limite - time.time())))
            for g, o in m.objetos.items():
                if o["typeid"] != upd.TYPEID_PLAYER or g == m.guid:
                    continue
                v = o.get("valores", {})
                nivel = v.get(upd.UNIT_FIELD_LEVEL)
                if nivel is None:
                    continue
                e = vistos.setdefault(g, {"nivel": nivel, "montado": False})
                e["nivel"] = nivel
                if v.get(UNIT_FIELD_MOUNTDISPLAYID):
                    e["montado"] = True
        con_equitacion = [e for e in vistos.values() if e["nivel"] >= 20]
        montados = [e for e in con_equitacion if e["montado"]]
        inf.datos["monturas"] = {"vistos": len(vistos), "nivel_20_o_mas": len(con_equitacion),
                                 "montados": len(montados),
                                 "niveles_montados": sorted(e["nivel"] for e in montados)}
        inf.comprobar(sec, len(con_equitacion) >= 5, "se ven bots de nivel 20 o más en %s" % ctx.p["destino"],
                      "%d jugadores vistos, %d de nivel 20 o más" % (len(vistos), len(con_equitacion)),
                      esperado=">= 5", observado=len(con_equitacion), estado_si_no="AVISO")
        inf.comprobar(sec, len(montados) >= 1, "al menos un bot de nivel 20 o más se ve montado",
                      "%d de %d montados alguna vez en %d s (niveles %s)" % (
                          len(montados), len(con_equitacion), ctx.p["espera"],
                          sorted({e['nivel'] for e in montados})),
                      esperado=">= 1", observado=len(montados), estado_si_no="AVISO")
        inf.comprobar(sec, not m.errores_lectura, "paquetes del mundo leídos sin error",
                      "; ".join(m.errores_lectura[:3]), origen="cliente")
