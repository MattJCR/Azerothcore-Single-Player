# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""mod-party-here en combate real (PLAN M31-M32).

`TrySummon` aplaza el summon si `human->IsInCombat() || bot->IsInCombat()`
(razonado por lectura hasta ahora, sin caso que lo dispare de verdad): un
compañero que aún no ha llegado no debe aparecer junto al jugador mientras
dure un combate real, y debe retomar el viaje en cuanto termina (M31).
`Dismiss` marca `dismissPending` si `IsBusy(bot)` (en combate, de viaje o
fuera del mundo) y avisa «N companero(s) estan en combate o de viaje: se iran
en cuanto terminen.» en vez de sacarlos en el acto (M32).

Combate real: un NPC hostil de bajo nivel, de `.lookup creature`, con la vida
subida por GM (`.modify hp`) para que aguante el rato de la prueba; `.atacar`
(CMSG_ATTACKSWING) empieza el combate de verdad (Unit::Attack pone en combate
tanto al jugador como al objetivo) y `.npc delete` lo termina de forma
determinista (el objetivo desaparece: sin objetivo, sin combate).
"""
import re
import time

from .. import actualizaciones as upd
from ..catalogo import Parametro, caso
from . import PersonajeTemporal, sin_colores
from .mazmorra import cerca_de, esperar_llegada, formar_grupo


def _buscar_hostil(m, palabra="kobold vermin"):
    """`.lookup creature <palabra>`: primer entry (Hcreature_entry:N)."""
    textos = m.comando("lookup creature %s" % palabra, 3)
    for t in textos:
        mobj = re.search(r"Hcreature_entry:(\d+)\|h\[([^\]]+)\]", t)
        if mobj:
            return int(mobj.group(1)), mobj.group(2)
    return None, None


def _spawn_hostil(m, entry, vida=500000):
    """`.npc add <entry>` en la posición del jugador; sube la vida por GM para que
    aguante el rato de la prueba sin morir de las auto-atacadas. Devuelve el guid."""
    antes = set(m.objetos.keys())
    m.comando("npc add %d" % entry, 2)
    m.bombear(3)
    nuevos = [g for g in m.objetos if g not in antes
              and m.objetos[g].get("typeid") == upd.TYPEID_UNIT
              and m.objetos[g].get("valores", {}).get(upd.OBJECT_FIELD_ENTRY) == entry]
    if not nuevos:
        return None
    guid = nuevos[0]
    m.seleccionar(guid)
    m.bombear(1)
    m.comando("modify hp %d" % vida, 2)
    return guid


@caso(id="companeros-combate", titulo="mod-party-here en combate real: summon aplazado y despedida pendiente",
      descripcion=__doc__,
      etiquetas=("party-here", "grupo", "combate", "personaje"),
      acciones=("conectar", "leer", "personaje", "gm", "grupo"),
      parametros={"nivel": Parametro(int, 25, "nivel del jugador", minimo=10, maximo=80)},
      duracion_max=600, protege=("mod-party-here", "PLAN M31", "PLAN M32"), control="directo",
      observa=("mensajes de `.grupo fuera` (despedida inmediata vs pendiente)",
               "SMSG_UPDATE_OBJECT (compañeros visibles cerca)", "Server.log ([party-here] ... en combate)"),
      no_cubre=("combate del propio compañero de forma aislada (aquí se induce vía asistencia a la IA de "
                "playerbots cuando el jugador es atacado; si esa asistencia no se dispara, M32 queda como "
                "AVISO, no como éxito fabricado)",))
def ejecutar(ctx):
    inf, sec, nivel = ctx.inf, "party-here-combate", ctx.p["nivel"]
    m = ctx.nueva_sesion()
    estado = {"grupo": False, "mob": None}

    def deshacer():
        if estado["mob"]:
            m.seleccionar(estado["mob"])
            m.comando("npc delete", 2)
        if estado["grupo"]:
            m.comando("grupo fuera", 3)

    with PersonajeTemporal(ctx, m, 1, 8, sec, antes_de_salir=deshacer) as pj:
        ctx.anotar_servidor(m)
        m.comando("gm off", 1)
        m.comando("levelup %d" % (nivel - 1), 3)
        inf.comprobar(sec, m.valor_propio(upd.UNIT_FIELD_LEVEL) == nivel, "nivel de prueba",
                      "nivel %d" % m.valor_propio(upd.UNIT_FIELD_LEVEL), origen="entorno")

        entry, nombre_hostil = _buscar_hostil(m)
        if not inf.comprobar(sec, entry is not None, "hay un hostil de prueba en `.lookup creature`",
                              "kobold vermin -> %s (%s)" % (entry, nombre_hostil), origen="entorno"):
            return

        # ── M31: el grupo se pide y el combate empieza ANTES de que lleguen,
        # así hay compañeros de verdad lejos que TrySummon debe aplazar. ────
        # espera a que se UNAN al grupo (puede tardar si hay que despertar bots),
        # pero no a que LLEGUEN junto al jugador: eso es justo lo que se prueba.
        miembros = formar_grupo(m, inf, sec, tamano=5, espera=150)
        estado["grupo"] = bool(miembros)
        if not miembros:
            return

        mob = _spawn_hostil(m, entry)
        estado["mob"] = mob
        if not inf.comprobar(sec, mob is not None, "se consigue un hostil de prueba (`.npc add`)",
                              "entry %s (%s)" % (entry, nombre_hostil), origen="entorno"):
            return
        ya_cerca_al_empezar = cerca_de(m, miembros)
        m.comando("gm off", 1)          # '.npc add' puede reactivar el modo GM del personaje
        m.atacar(mob)
        m.bombear(2)

        if len(ya_cerca_al_empezar) == len(miembros):
            # Ya habían llegado todos antes de que empezara el combate (formar
            # el grupo tardó lo bastante): no hay nadie "pendiente" con quien
            # demostrar el aplazamiento. Se anota y se sigue con el resto.
            inf.anotar(sec, "AVISO",
                       "M31: los 4 companeros ya habian llegado antes de empezar el combate",
                       "nada que aplazar con este intento; probar de nuevo suele bastar")
        else:
            # Mientras dura el combate: quienes aún faltaban no deberían llegar
            # (TrySummon: SUMMON_LATER por human->IsInCombat()).
            limite = time.time() + 25
            llegados_en_combate = set()
            while time.time() < limite:
                m.bombear(3)
                llegados_en_combate |= (cerca_de(m, miembros) - ya_cerca_al_empezar)
            inf.comprobar(sec, not llegados_en_combate,
                          "M31: quien faltaba no llega junto al jugador mientras dura el combate real",
                          "ya estaban cerca: %s; llegaron EN combate: %s" % (
                              ", ".join(ya_cerca_al_empezar) or "ninguno",
                              ", ".join(llegados_en_combate) or "ninguno"),
                          esperado=set(), observado=llegados_en_combate, estado_si_no="FALLO")

        # Termina el combate quitando el objetivo (determinista: sin objetivo, sin combate).
        m.seleccionar(mob)
        m.comando("npc delete", 2)
        estado["mob"] = None
        m.bombear(3)
        esperar_llegada(m, sec, inf, miembros,
                        "M31: el summon se reanuda y llegan en cuanto termina el combate", limite_s=90)

        # ── M32: con los compañeros ya cerca, un segundo combate para ver si
        # la IA de playerbots asiste al jugador atacado (IsBusy exige que el
        # PROPIO bot esté en combate, no sólo el jugador). ──────────────────
        entry2, nombre2 = _buscar_hostil(m)
        mob2 = _spawn_hostil(m, entry2) if entry2 else None
        estado["mob"] = mob2
        if mob2:
            m.comando("gm off", 1)
            m.atacar(mob2)
            m.bombear(6)             # deja tiempo a que la IA de los companeros asista
            antes_msj = len(m.mensajes)
            m.comando("grupo fuera", 3)
            m.bombear(2)
            msj = sin_colores(" ".join(t for _, t in m.mensajes[antes_msj:]))
            inf.datos["mensaje_grupo_fuera_en_combate"] = msj[:300]
            pendiente = "en combate o de viaje" in msj
            inf.comprobar(sec, pendiente,
                          "M32: '.grupo fuera' avisa de despedida pendiente si el companero sigue en combate",
                          msj[:200] or "(sin mensaje)", estado_si_no="AVISO")
            # Termina el segundo combate y confirma que los pendientes se van solos.
            m.seleccionar(mob2)
            m.comando("npc delete", 2)
            estado["mob"] = None
            limite = time.time() + 30
            restantes = [x["nombre"] for x in (m.grupo or {}).get("miembros", []) if x["guid"] != m.guid]
            while time.time() < limite and restantes:
                m.bombear(3)
                restantes = [x["nombre"] for x in (m.grupo or {}).get("miembros", []) if x["guid"] != m.guid]
            estado["grupo"] = bool(restantes)
            inf.comprobar(sec, not restantes,
                          "M32: los pendientes se despiden solos en cuanto termina el combate",
                          "%d siguen en el grupo: %s" % (len(restantes), ", ".join(restantes)),
                          estado_si_no="AVISO")
        else:
            inf.anotar(sec, "AVISO", "M32: sin segundo hostil de prueba, no se pudo repetir el combate", "")
            m.comando("grupo fuera", 3)
            m.bombear(3)
            estado["grupo"] = False
