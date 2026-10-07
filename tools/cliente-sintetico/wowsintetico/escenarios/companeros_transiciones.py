# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""mod-party-here: el vínculo de compañero a través de transiciones SP (PLAN M28-M30, M50).

Un grupo manual `.grupo <n>` con más de 5 en total debe sobrevivir un cambio de
continente por GM (M28), una muerte real seguida de liberar espíritu y revivir
(M29a) y una reconexión con el mismo personaje (M29b). `.grupo fuera` debe
cancelar la intención de verdad: tras cancelarla, una reconexión posterior NO
debe recomponer el grupo solo (M30).

M50: hasta la corrección, `mod_party_here.cpp` sólo guardaba el tamaño para
recomponer tras un logout si `wantedSize > 5`, así que el grupo MÁS habitual
(`.grupo`/`.grupo mazmorra` a secas, tamaño 5) nunca sobrevivía a una
reconexión mientras que uno de 6 sí. Unificado: cualquier grupo manual
(`wantedSize > 1`) se recompone igual. Se comprueba aparte, al final, con un
`.grupo` de 5 sin tocar la cadena de comprobaciones de M28-M30 (grupo de 6).
"""
import time

from .. import actualizaciones as upd
from ..catalogo import Parametro, caso
from . import PersonajeTemporal, sin_colores
from .mazmorra import esperar_llegada as _esperar_llegada


def _formar_grupo_n(m, inf, sec, companeros, espera=240):
    """`.grupo <companeros>` (mod-party-here): pide un tamaño total > 5 a propósito
    para entrar en el camino de PersistMinutes (wantedSize > 5). Devuelve guid -> nombre."""
    m.comando("grupo %d" % companeros, 2)
    limite = time.time() + espera
    completo = False
    while time.time() < limite:
        m.bombear(2)
        miembros = [x for x in (m.grupo or {}).get("miembros", []) if x["guid"] != m.guid]
        completo = completo or any("grupo esta completo" in t for _, t in m.mensajes[-50:])
        if len(miembros) >= companeros and all(x["conectado"] for x in miembros):
            break
    miembros = {x["guid"]: x["nombre"] for x in (m.grupo or {}).get("miembros", []) if x["guid"] != m.guid}
    inf.comprobar(sec, len(miembros) == companeros, "party-here forma el grupo manual de %d companeros" % companeros,
                  "%d compañeros: %s%s" % (len(miembros), ", ".join(miembros.values()),
                                           "" if completo else " (sin «Tu grupo esta completo»)"),
                  esperado=companeros, observado=len(miembros))
    return miembros


def _miembros_actuales(m):
    return {x["guid"]: x["nombre"] for x in (m.grupo or {}).get("miembros", []) if x["guid"] != m.guid}


@caso(id="companeros-transiciones",
      titulo="Vínculo de compañero a través de continente, muerte y relogin",
      descripcion=__doc__,
      etiquetas=("party-here", "grupo", "transiciones", "personaje"),
      acciones=("conectar", "leer", "personaje", "gm", "grupo"),
      parametros={"nivel": Parametro(int, 25, "nivel del jugador", minimo=10, maximo=80)},
      duracion_max=1200, protege=("mod-party-here", "PLAN M28", "PLAN M29", "PLAN M30", "PLAN M50"), control="directo",
      observa=("SMSG_GROUP_LIST tras cada transición", "SMSG_PARTY_MEMBER_STATS (posición)",
               "mensaje «Se recompone tu grupo de antes»"),
      no_cubre=("combate real durante la transición (eso es PLAN M31/M32, caso companeros-combate)",
                 "mod-quest-mates (sólo se prueba mod-party-here aquí)"))
def ejecutar(ctx):
    inf, sec, nivel = ctx.inf, "party-here-transiciones", ctx.p["nivel"]
    m = ctx.nueva_sesion()
    estado = {"grupo": False}

    def deshacer():
        if estado["grupo"]:
            m.comando("grupo fuera", 3)

    with PersonajeTemporal(ctx, m, 1, 8, sec, antes_de_salir=deshacer) as pj:
        ctx.anotar_servidor(m)
        m.comando("levelup %d" % (nivel - 1), 3)
        inf.comprobar(sec, m.valor_propio(upd.UNIT_FIELD_LEVEL) == nivel, "nivel de prueba",
                      "nivel %d" % m.valor_propio(upd.UNIT_FIELD_LEVEL), origen="entorno")
        mapa_inicial = m.mapa
        inf.datos["mapa_inicial"] = mapa_inicial

        # Grupo manual de 6 companeros (size total 7, wantedSize > 5): el único
        # camino que PersistMinutes recuerda tras un logout (ver docstring).
        miembros = _formar_grupo_n(m, inf, sec, 6)
        estado["grupo"] = bool(miembros)
        if not miembros:
            return
        _esperar_llegada(m, sec, inf, miembros, "los 6 companeros llegan junto al jugador antes de viajar")

        # ── M28: cambio de continente válido por GM ─────────────────────────
        # Sólo Kalimdor <-> Reinos del Este (la otra opción que admite el PLAN):
        # Rasganorte está bloqueado por mod-individual-progression para un
        # personaje nuevo ("Progression Level Required = 13" al '.tele dalaran'
        # con la etapa de IP por defecto) y forzar la etapa de IP para saltárselo
        # mezclaría dos módulos en una sola comprobación.
        destinos = [("orgrimmar", 1)] if mapa_inicial != 1 else [("stormwind", 0)]
        for nombre_tele, mapa_esperado in destinos:
            m.comando("tele %s" % nombre_tele, 3)
            m.bombear(10)
            inf.comprobar(sec, m.mapa == mapa_esperado,
                          "M28: '.tele %s' cambia de continente" % nombre_tele,
                          "mapa %s tras el teletransporte" % m.mapa,
                          esperado=mapa_esperado, observado=m.mapa, origen="entorno")
            if m.mapa != mapa_esperado:
                continue
            restantes = _miembros_actuales(m)
            inf.comprobar(sec, len(restantes) == len(miembros),
                          "M28: el grupo sigue teniendo %d companeros tras el cambio de continente (%s)"
                          % (len(miembros), nombre_tele),
                          "%d compañeros: %s" % (len(restantes), ", ".join(restantes.values())),
                          esperado=len(miembros), observado=len(restantes))
            _esperar_llegada(m, sec, inf, restantes or miembros,
                             "M28: los companeros llegan junto al jugador tras el viaje a %s" % nombre_tele)

        # ── M29a: muerte real (.die GM), liberar espiritu, revivir ──────────
        antes_muerte = _miembros_actuales(m)
        m.comando("die", 3)
        m.bombear(2)
        vivo = m.valor_propio(upd.UNIT_FIELD_HEALTH) > 0
        inf.comprobar(sec, not vivo, "M29: '.die' mata de verdad al personaje (vida a 0)",
                      "vida %s" % m.valor_propio(upd.UNIT_FIELD_HEALTH), origen="entorno")
        tras_morir = _miembros_actuales(m)
        inf.comprobar(sec, len(tras_morir) == len(antes_muerte),
                      "M29: el grupo no se disuelve por la muerte del jugador",
                      "%d compañeros tras morir" % len(tras_morir), esperado=len(antes_muerte),
                      observado=len(tras_morir))
        m.liberar_espiritu()
        m.bombear(3)
        r = sin_colores(" ".join(m.comando("revive", 3)))
        m.bombear(3)
        vivo = m.valor_propio(upd.UNIT_FIELD_HEALTH) > 0
        inf.comprobar(sec, vivo, "M29: '.revive' devuelve al personaje a la vida",
                      "vida %s tras revive (%s)" % (m.valor_propio(upd.UNIT_FIELD_HEALTH), r[:120]),
                      origen="entorno")
        tras_revivir = _miembros_actuales(m)
        inf.comprobar(sec, len(tras_revivir) == len(antes_muerte),
                      "M29: el grupo sigue completo tras morir, liberar espiritu y revivir",
                      "%d compañeros" % len(tras_revivir), esperado=len(antes_muerte), observado=len(tras_revivir))
        _esperar_llegada(m, sec, inf, tras_revivir or antes_muerte,
                         "M29: los companeros retoman el viaje hacia el jugador tras revivir")

        # ── M29b: reconexión con el mismo personaje ─────────────────────────
        guid_pj = pj.guid
        m.salir()
        m.entrar(guid_pj, 1, nombre=pj.nombre, espera_mundo=8)
        limite = time.time() + 90
        recompuesto = _miembros_actuales(m)
        aviso_recompone = False
        while time.time() < limite and (len(recompuesto) < 6 or not aviso_recompone):
            m.bombear(3)
            recompuesto = _miembros_actuales(m)
            aviso_recompone = aviso_recompone or any(
                "recompone tu grupo" in sin_colores(t) for _, t in m.mensajes)
        inf.comprobar(sec, len(recompuesto) == 6,
                      "M29: el vínculo sobrevive la reconexión (grupo manual >5, PersistMinutes)",
                      "%d compañeros recompuestos tras el relogin (aviso de recomposición: %s)" % (
                          len(recompuesto), aviso_recompone),
                      esperado=6, observado=len(recompuesto))
        estado["grupo"] = bool(recompuesto)

        # ── M30: cancelar antes de una transición y comprobar que no vuelve ──
        m.comando("grupo fuera", 3)
        m.bombear(3)
        vacio_tras_cancelar = _miembros_actuales(m)
        estado["grupo"] = bool(vacio_tras_cancelar)
        inf.comprobar(sec, not vacio_tras_cancelar, "M30: '.grupo fuera' cancela el vínculo de verdad",
                      "%d compañeros tras cancelar" % len(vacio_tras_cancelar))
        if vacio_tras_cancelar:
            return
        # Repite la MISMA transición que en M29b (relogin) que sí recompuso el
        # grupo cuando no estaba cancelado: ahora no debe volver nadie solo.
        m.salir()
        m.entrar(guid_pj, 1, nombre=pj.nombre, espera_mundo=8)
        m.bombear(20)
        tras_cancelar_y_relogin = _miembros_actuales(m)
        estado["grupo"] = bool(tras_cancelar_y_relogin)
        inf.comprobar(sec, not tras_cancelar_y_relogin,
                      "M30: ninguna reserva/intención cancelada se restaura sola tras la reconexión",
                      "%d compañeros aparecieron sin pedirlos" % len(tras_cancelar_y_relogin),
                      esperado=0, observado=len(tras_cancelar_y_relogin))

        # ── M50: el grupo de 5 (el más habitual, ".grupo"/".grupo mazmorra")
        # también se recompone tras un relogin. Antes de la corrección
        # wantedSize > 5 lo excluía a propósito; aquí sólo hace falta el
        # tamaño de grupo (SMSG_GROUP_LIST) tras reconectar, no que los
        # companeros hayan llegado físicamente junto al jugador. ──────────
        miembros5 = _formar_grupo_n(m, inf, sec, 4, espera=90)
        estado["grupo"] = bool(miembros5)
        if miembros5:
            m.salir()
            m.entrar(guid_pj, 1, nombre=pj.nombre, espera_mundo=8)
            limite = time.time() + 90
            recompuesto5 = _miembros_actuales(m)
            aviso5 = False
            while time.time() < limite and (len(recompuesto5) < 4 or not aviso5):
                m.bombear(3)
                recompuesto5 = _miembros_actuales(m)
                aviso5 = aviso5 or any("recompone tu grupo" in sin_colores(t) for _, t in m.mensajes)
            inf.comprobar(sec, len(recompuesto5) == 4,
                          "M50: un grupo manual de 5 (wantedSize=5) tambien se recompone tras el relogin",
                          "%d compañero(s) recompuestos tras el relogin (aviso de recomposicion: %s)" % (
                              len(recompuesto5), aviso5),
                          esperado=4, observado=len(recompuesto5))
            estado["grupo"] = bool(recompuesto5)
            m.comando("grupo fuera", 3)
            m.bombear(3)
            estado["grupo"] = bool(_miembros_actuales(m))
