# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Campo de batalla normal (Garganta Grito de Guerra) con el relleno de bots (UPD-C).

Encola al jugador de prueba para un campo de batalla por el camino del maestro de
batalla (`CMSG_BATTLEMASTER_JOIN`) y mide cuánto tarda el servidor en ofrecerle la
entrada (`STATUS_WAIT_JOIN`) y en dejarle dentro (`STATUS_IN_PROGRESS`, mapa de
campo). Sirve para comparar el relleno de `mod-queue-bots` con el de
`DungeonClear.BgQueueFill.*` con el mismo método: el caso no sabe cuál está activo;
quien lo lanza anota la configuración. Con `entrar=false` se queda en la oferta y
no acepta.
"""
import time

from .. import grupo as grp
from ..catalogo import Parametro, caso
from . import PersonajeTemporal
from .buscador_extendido import _aceptar_oferta, _borrar_maestro, _spawn_maestro_arena

MAPAS_CAMPO = {30: "Valle de Alterac", 489: "Garganta Grito de Guerra", 529: "Cuenca de Arathi",
               566: "Ojo de la Tormenta", 607: "Playa de los Ancestros", 628: "Isla de la Conquista"}
TIPOS = {"wsg": 2, "ab": 3, "av": 1}


@caso(id="campo-batalla", titulo="Campo de batalla normal: tiempo hasta la oferta y la entrada",
      descripcion=__doc__, etiquetas=("campo-batalla", "queue-bots", "dungeon-clear", "personaje"),
      acciones=("conectar", "leer", "personaje", "gm", "cola"),
      parametros={"campo": Parametro(str, "wsg", "campo de batalla", opciones=tuple(TIPOS)),
                  "nivel": Parametro(int, 24, "nivel del jugador de prueba", minimo=10, maximo=80),
                  "espera": Parametro(int, 240, "segundos máximos esperando la oferta", minimo=30, maximo=900),
                  "entrar": Parametro(bool, True, "aceptar la oferta y entrar en el campo")},
      duracion_max=900, en_todo=False, protege=("mod-queue-bots (campos de batalla)", "mod-dungeon-clear (BgQueueFill)", "PLAN UPD-C"),
      control="directo",
      observa=("SMSG_BATTLEFIELD_STATUS (WAIT_QUEUE/WAIT_JOIN/IN_PROGRESS)", "cambio de mapa a un campo"),
      no_cubre=("tamaño real de cada bando (el cliente no lo lee; sale de Server.log)", "resultado de la partida"))
def ejecutar(ctx):
    inf, sec, p = ctx.inf, "campo-batalla", ctx.p
    m = ctx.nueva_sesion()
    estado = {"cola": False}

    def deshacer():
        m.bombear(2)
        if m.mapa in MAPAS_CAMPO or estado["cola"]:
            m.comando("tele stormwind", 3)
            m.bombear(5)

    with PersonajeTemporal(ctx, m, 1, 8, sec, antes_de_salir=deshacer):
        ctx.anotar_servidor(m)
        m.comando("levelup %d" % p["nivel"], 3)
        guid_maestro, resp = _spawn_maestro_arena(m)
        if not inf.comprobar(sec, guid_maestro is not None, "hay un maestro de batalla de prueba (`.npc add`)",
                             resp or "(vacía)", origen="entorno"):
            return
        m.comando("gm off", 1)
        mapa_antes = m.mapa
        hitos = {}

        def oyente(t, evento, datos):
            # hora real de cada paquete (el bucle de abajo puede llegar tarde si el borrado tarda)
            if evento == "bg_estado" and datos.get("status") in (grp.BG_STATUS_WAIT_QUEUE, grp.BG_STATUS_WAIT_JOIN,
                                                                  grp.BG_STATUS_IN_PROGRESS):
                hitos.setdefault(datos["status"], t)
        m.oyentes.append(oyente)
        auto = bool(p["entrar"])
        quiere_entrar = auto
        auto = False       # no aceptar hasta borrar el maestro: si no, el puerto cambia de mapa antes
        t0 = time.time()
        m.campo_unirse(guid_maestro, TIPOS[p["campo"]], en_grupo=False, auto_aceptar=False)
        estado["cola"] = True
        m.bombear(2)
        bajo, resp, borrado = _borrar_maestro(m, guid_maestro)     # regla 9: ya no hace falta
        inf.comprobar(sec, borrado, "el maestro de batalla de prueba se borra del mundo",
                      "guid bajo %d: %s" % (bajo, resp or "(vacía)"), origen="entorno", estado_si_no="AVISO")
        auto = quiere_entrar
        _aceptar_oferta(m, auto)
        t_oferta = t_dentro = None
        while time.time() - t0 < p["espera"]:
            m.bombear(1)
            st = m.bg.get("status")
            if t_oferta is None and st in (grp.BG_STATUS_WAIT_JOIN, grp.BG_STATUS_IN_PROGRESS):
                t_oferta = hitos.get(grp.BG_STATUS_WAIT_JOIN, time.time()) - t0
                if not auto:
                    break
            if st == grp.BG_STATUS_IN_PROGRESS and m.mapa in MAPAS_CAMPO:
                t_dentro = hitos.get(grp.BG_STATUS_IN_PROGRESS, time.time()) - t0
                break
        inf.datos["campo"] = {"tipo": p["campo"], "nivel": p["nivel"], "segundos_hasta_oferta": t_oferta,
                              "segundos_hasta_dentro": t_dentro, "hitos_s": {k: round(v - t0, 1) for k, v in hitos.items()}, "mapa": m.mapa, "estado": dict(m.bg)}
        inf.comprobar(sec, t_oferta is not None, "el servidor ofrece la entrada al campo",
                      "oferta a los %s s; estado %s" % ("%.1f" % t_oferta if t_oferta is not None else "—", m.bg),
                      esperado="STATUS_WAIT_JOIN", observado=m.bg.get("status"), origen="desarrollo")
        if auto:
            inf.comprobar(sec, t_dentro is not None and m.mapa != mapa_antes,
                          "el jugador entra en un mapa de campo de batalla",
                          "mapa %s (%s)%s" % (m.mapa, MAPAS_CAMPO.get(m.mapa, "no es un campo"),
                                              " a los %.1f s" % t_dentro if t_dentro is not None else ""),
                          esperado="mapa de campo", observado=m.mapa)
        inf.comprobar(sec, not m.errores_lectura, "paquetes leídos sin error", "; ".join(m.errores_lectura[:3]),
                      origen="cliente")
