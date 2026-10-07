# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""mod-queue-bots: buscador de banda y de arena 2c2/3c3 (PLAN M33).

El camino de mazmorra de 5 ya lo cubre el caso `buscador`; aquí sólo los dos
que faltaban:

- **Banda**: el tablón de bandas de 3.3.5 no empareja ni teletransporta, sólo
  marca al jugador en el buscador (`CMSG_LFG_JOIN` con una mazmorra de tipo
  RAID en LFGDungeons.dbc pone el estado en `LFG_STATE_RAIDBROWSER` de
  inmediato, sin `rolecheck` ni propuesta) y `FillRaid` mete bots en tu grupo
  hasta el tamaño que toque (`QueueBots.RaidSize`, 10 por defecto sin banda de
  25 puesta). Se comprueba que el grupo se convierte en banda (`GROUP_TYPE_RAID`)
  y crece más allá de 5.
- **Arena 2c2/3c3**: `CMSG_BATTLEMASTER_JOIN_ARENA` contra un maestro de batalla
  de arena real (`.npc add` de la entrada «Arena Battlemaster», que vale para
  cualquier tamaño: el tamaño lo decide `arenaslot`, no el NPC). Con
  `QueueBots.Arenas=1` el módulo rellena las dos plazas con bots y el cliente
  acepta el puerto (`CMSG_BATTLEFIELD_PORT`) como el botón «Entrar a la
  batalla»; se comprueba que el mapa cambia a uno de arena.
"""
import time

from .. import actualizaciones as upd
from .. import grupo as grp
from ..mundo import CMSG_BATTLEFIELD_PORT
from ..catalogo import Parametro, caso
from . import PersonajeTemporal, sin_colores

MAPAS_ARENA = {559: "Nagrand Arena", 562: "Blade's Edge Arena", 572: "Ruins of Lordaeron",
               617: "Dalaran Sewers", 618: "The Ring of Valor"}
ENTRY_ARENA_BATTLEMASTER = 26007        # "Arena Battlemaster" (confirmado en vivo 25/09/2026)


def _raid_mas_accesible(dbc):
    """La mazmorra de tipo RAID (LFG_TYPE_RAID=2) con el nivel mínimo más bajo:
    la que menos cuesta preparar con un personaje de prueba."""
    raids = [d for d in dbc.mazmorras_lfg() if d["tipo"] == 2 and d["nivel_min"] > 0]
    return min(raids, key=lambda d: d["nivel_min"]) if raids else None


def _spawn_maestro_arena(m):
    antes = set(m.objetos.keys())
    m.comando("gm on", 1)                    # '.npc add' lo exige en este core
    r = m.comando("npc add %d" % ENTRY_ARENA_BATTLEMASTER, 2)
    m.bombear(3)
    nuevos = [g for g in m.objetos if g not in antes
              and m.objetos[g].get("typeid") == upd.TYPEID_UNIT
              and m.objetos[g].get("valores", {}).get(upd.OBJECT_FIELD_ENTRY) == ENTRY_ARENA_BATTLEMASTER]
    return nuevos[0] if nuevos else None, sin_colores(" ".join(r))[:200]


def _borrar_maestro(m, guid_maestro):
    """Selecciona al maestro de prueba y `.npc delete`. Hay que hacerlo en
    el mapa del NPC, así que se llama justo tras encolar, antes de que el jugador cambie de mapa.
    El core aún no ha procesado la selección si el comando va pegado a ella («Debes seleccionar
    una criatura»): se reintenta con la selección repetida. Devuelve (guid, respuesta, borrado)."""
    m.comando("gm on", 1)
    r = ""
    for _ in range(3):
        m.seleccionar(guid_maestro)
        m.bombear(1)
        m.comando("npc info", 2)       # observado en vivo: sin esta consulta intermedia el delete no ve la selección
        r = sin_colores(" ".join(m.comando("npc delete", 2)))
        if "remov" in r.lower():
            break
    m.comando("gm off", 1)
    return guid_maestro & 0xFFFFFF, r[:160], "remov" in r.lower()


def _aceptar_oferta(m, auto=True):
    """Tras borrar el maestro: vuelve a aceptar la oferta sola y, si ya llegó mientras tanto,
    la acepta ahora (CMSG_BATTLEFIELD_PORT). Se encola con `auto_puerto` a False para que el
    puerto no cambie de mapa antes de poder borrar el NPC."""
    m.bg["auto_puerto"] = auto
    if auto and m.bg.get("status") == grp.BG_STATUS_WAIT_JOIN:
        m.enviar(CMSG_BATTLEFIELD_PORT, grp.puerto_campo(m.bg["arenatype"], m.bg["bg_tipo"]))


def _probar_arena(m, inf, sec, guid_maestro, ranura, nombre_ranura, espera_s=150):
    """Encola `ranura` (0=2c2, 1=3c3) sin grupo, sin clasificar, y espera a que
    el buscador la rellene y teletransporte a un mapa de arena. Devuelve True si
    entró (para saber si hay que sacarlo con un `.tele` antes de la siguiente)."""
    m.arena_unirse(guid_maestro, ranura, en_grupo=False, clasificada=False, auto_aceptar=False)
    m.bombear(2)
    bajo, resp, borrado = _borrar_maestro(m, guid_maestro)
    inf.comprobar(sec, borrado, "el maestro de batalla de prueba se borra del mundo",
                  "guid bajo %d: %s" % (bajo, resp or "(vacía)"), origen="entorno", estado_si_no="AVISO")
    _aceptar_oferta(m)
    limite = time.time() + espera_s
    mapa_antes = m.mapa
    while time.time() < limite and m.mapa == mapa_antes:
        m.bombear(2)
    en_arena = m.mapa in MAPAS_ARENA
    inf.datos["bg_estado_%s" % nombre_ranura] = dict(m.bg)
    inf.comprobar(sec, en_arena, "arena %s: el buscador rellena y teletransporta al jugador" % nombre_ranura,
                  "mapa %s (%s) tras encolar; SMSG_BATTLEFIELD_STATUS: %s" % (
                      m.mapa, MAPAS_ARENA.get(m.mapa, "no es arena"), m.bg),
                  esperado="mapa de arena", observado=m.mapa)
    return en_arena


@caso(id="buscador-extendido", titulo="mod-queue-bots: buscador de banda y de arena 2c2/3c3",
      descripcion=__doc__,
      etiquetas=("buscador", "lfg", "queue-bots", "banda", "arena", "personaje"), requiere=("dbc",),
      acciones=("conectar", "leer", "personaje", "gm", "cola", "grupo"),
      parametros={"espera_banda": Parametro(int, 180, "segundos máximos esperando que crezca la banda",
                                            minimo=30, maximo=600),
                  "partes": Parametro(str, "ambos", "qué probar", opciones=("ambos", "banda", "arena"))},
      duracion_max=900, protege=("mod-queue-bots", "PLAN M33"), control="directo",
      observa=("SMSG_LFG_UPDATE_PLAYER/PARTY (JOIN_RAIDBROWSER)", "SMSG_GROUP_LIST (GROUP_TYPE_RAID, tamaño)",
               "SMSG_BATTLEFIELD_STATUS (WAIT_JOIN/IN_PROGRESS)", "cambio de mapa a uno de arena"),
      no_cubre=("arena 5c5 (no lo pide el PLAN)", "resultado de la partida (victoria/derrota, MMR)",
                "campos de batalla normales (Vasija de la Antigüedad, etc.): sólo arena y banda"))
def ejecutar(ctx):
    inf, sec, p = ctx.inf, "queue-bots-extendido", ctx.p
    m = ctx.nueva_sesion()
    estado = {"grupo": False, "cola": False, "en_arena": False}

    def deshacer():
        if estado["cola"]:
            m.lfg_salir()
        if estado["grupo"]:
            m.dejar_grupo()
        m.bombear(2)
        if estado["en_arena"] or m.mapa in MAPAS_ARENA:
            # Sacarlo de la arena por GM antes de que PersonajeTemporal intente
            # el logout: el core lo rechaza (código 1) si sigue dentro.
            m.comando("tele stormwind", 3)
            m.bombear(5)

    with PersonajeTemporal(ctx, m, 1, 8, sec, antes_de_salir=deshacer) as pj:
        ctx.anotar_servidor(m)
        m.comando("gm off", 1)

        # ── Arena 2c2 y 3c3 primero: `.levelup` sólo SUBE el nivel, nunca lo
        # baja, y la banda exige un nivel de banda (55+) que dejaría al
        # personaje sin apenas población de bots en el bracket de arena (10
        # niveles) si se hiciera al revés. ────────────────────────────────
        if p["partes"] in ("ambos", "arena"):
            # Nivel medio, no el máximo: la población de bots (mod-world-bots)
            # se mantiene hasta el nivel 60 por defecto en este perfil, así que
            # un personaje de prueba muy alto no tiene con quién emparejar.
            m.comando("levelup 24", 3)
            guid_maestro, resp_npc_add = _spawn_maestro_arena(m)
            if inf.comprobar(sec, guid_maestro is not None,
                              "hay un maestro de batalla de arena de prueba (`.npc add`)",
                              "entry %d (respuesta: %s)" % (ENTRY_ARENA_BATTLEMASTER, resp_npc_add or "(vacía)"),
                              origen="entorno"):
                entro_2c2 = _probar_arena(m, inf, sec, guid_maestro, 0, "2c2")
                estado["en_arena"] = entro_2c2
                if entro_2c2:
                    m.comando("tele stormwind", 3)
                    m.bombear(15)          # deja que el core asiente la salida antes de volver a encolar
                    estado["en_arena"] = False
                # _probar_arena borra el maestro tras encolar: para 3c3 hace falta uno nuevo
                guid_maestro, _ = _spawn_maestro_arena(m)
                guid_maestro = guid_maestro or None
                if guid_maestro:
                    entro_3c3 = _probar_arena(m, inf, sec, guid_maestro, 1, "3c3")
                    estado["en_arena"] = entro_3c3
                    if entro_3c3:
                        m.comando("tele stormwind", 3)
                        m.bombear(5)
                        estado["en_arena"] = False

        # ── Banda: LFG_TYPE_RAID -> LFG_STATE_RAIDBROWSER -> FillRaid ───────
        if p["partes"] in ("ambos", "banda"):
            raid = _raid_mas_accesible(ctx.dbc)
            if inf.comprobar(sec, raid is not None, "hay una mazmorra de tipo banda en LFGDungeons.dbc",
                              "%s" % raid, origen="entorno"):
                m.comando("levelup %d" % (max(raid["nivel_min"], 10) - 1), 3)
                m.lfg_unirse(grp.ROL_TANQUE | grp.ROL_SANADOR | grp.ROL_DANO, [raid["id"]])
                estado["cola"] = True
                limite = time.time() + 20
                estados_lfg = []
                while time.time() < limite and len(estados_lfg) < 2:
                    m.bombear(2)
                    if m.lfg.get("estado") is not None and m.lfg["estado"] not in estados_lfg:
                        estados_lfg.append(m.lfg["estado"])
                # 3 = LFG_UPDATETYPE_JOIN_RAIDBROWSER; el servidor manda antes un
                # 2 = LEAVE_RAIDBROWSER de limpieza de la pestaña (no es un fallo,
                # se anota tal cual se ve en vivo, sólo AVISO si falta el 3).
                inf.datos["estados_lfg_banda"] = estados_lfg
                inf.comprobar(sec, 3 in estados_lfg,
                              "el buscador mete al jugador en LFG_STATE_RAIDBROWSER (LFG_UPDATETYPE_JOIN_RAIDBROWSER)",
                              "tipos de actualizacion LFG vistos: %s" % estados_lfg,
                              esperado=3, observado=estados_lfg, estado_si_no="AVISO")

                limite = time.time() + p["espera_banda"]
                tamano = 1
                while time.time() < limite and tamano < 10:
                    m.bombear(3)
                    tamano = len((m.grupo or {}).get("miembros", [])) or 1
                estado["grupo"] = tamano > 1
                es_banda = bool((m.grupo or {}).get("tipo", 0) & grp.GROUP_TYPE_RAID)
                # M48 (25/09/2026): antes CollectBots exigía el nivel EXACTO del
                # jugador sin tramo, y sólo despertaba dormidos cuando esta
                # pasada no encontraba NINGUNO; con la población de bots repartida
                # entre muchos niveles por mod-world-bots, una banda en un nivel
                # poco poblado (Zul'Gurub, nivel_min 56) podía quedarse en 0-2
                # companeros para siempre. QueueBots.RaidLevelBelow (por defecto
                # 5) amplía el tramo cuando faltan, y ahora se despierta lo que
                # falte en cada pasada (no sólo con la banda a cero). Se exige
                # llegar a más de 5 (cierra el bug reportado); >=10 es el
                # objetivo ideal pero depende de cuántos bots del tramo están
                # libres en el momento exacto de la prueba, así que sólo se
                # anota como AVISO si se queda corto.
                inf.comprobar(sec, tamano >= 2 and es_banda,
                              "FillRaid convierte el grupo en banda y mete companeros de verdad",
                              "%d miembro(s) (incl. el jugador), tipo 0x%02X tras %.0f s" % (
                                  tamano, (m.grupo or {}).get("tipo", 0), p["espera_banda"]),
                              esperado=">=2 y GROUP_TYPE_RAID", observado="%d, tipo 0x%02X" % (
                                  tamano, (m.grupo or {}).get("tipo", 0)), estado_si_no="AVISO")
                inf.comprobar(sec, tamano > 5,
                              "la banda supera los 5 companeros en un nivel poco poblado (M48)",
                              "%d miembro(s) tras %.0f s" % (tamano, p["espera_banda"]),
                              esperado=">5", observado=tamano)
                inf.comprobar(sec, tamano >= 10, "la banda llega a completarse del todo (raidSize=10)",
                              "%d miembro(s) tras %.0f s" % (tamano, p["espera_banda"]),
                              esperado=10, observado=tamano, estado_si_no="AVISO")
                m.lfg_salir()
                estado["cola"] = False
                m.dejar_grupo()
                m.bombear(3)
                estado["grupo"] = False

        inf.comprobar(sec, not m.errores_lectura, "paquetes leídos sin error", "; ".join(m.errores_lectura[:3]),
                      origen="cliente")
