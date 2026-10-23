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

`campo-batalla-ordenes` (UPD-R) hace lo mismo y, ya dentro del campo, manda `debug state` y
`debug position` por el chat de campo de batalla (`CHAT_MSG_BATTLEGROUND`) y cuenta cuántos
bots de su bando contestan. Qué mide y qué no: un bot aleatorio de un
campo no tiene maestro y comparte banda con todo su bando, así que la comprobación de grupo
de `PlayerbotSecurity::LevelFor` deniega a todos; `Playerbots.AllowBattlegroundCommands` abre
esa puerta a los jugadores reales de su bando. Pero la seguridad da ALLOW_ALL antes a cualquier
cuenta con el permiso RBAC de GM, y la cuenta de pruebas lo tiene (como la de juego), así que
el caso mide lo que consigue un GM, no el efecto de la opción: quitar el permiso en caliente
(`rbac account deny`) se guarda pero la sesión conectada no lo pierde hasta volver a entrar.
Exige al menos una respuesta y que quien contestó siga en el mismo mapa de campo (los bots
siguen jugando la partida).
"""
import time

from .. import grupo as grp
from ..catalogo import Parametro, caso
from . import PersonajeTemporal
from .buscador_extendido import _aceptar_oferta, _borrar_maestro, _spawn_maestro_arena

MAPAS_CAMPO = {30: "Valle de Alterac", 489: "Garganta Grito de Guerra", 529: "Cuenca de Arathi",
               566: "Ojo de la Tormenta", 607: "Playa de los Ancestros", 628: "Isla de la Conquista"}
TIPOS = {"wsg": 2, "ab": 3, "av": 1}
CHAT_MSG_BATTLEGROUND = 0x2C
RESPUESTAS_DEBUG_STATE = ("combat", "non-combat", "dead", "unknown")


def _respuestas_bg(m, desde):
    """Mensajes del chat de campo de batalla recibidos desde `desde` (índice de `m.chat`) que no son
    del jugador: por este camino `PlayerbotAI::HandleCommand` contesta a `debug` con un mensaje del
    mismo tipo que la orden (no con un mensaje de addon), con el bot como emisor."""
    return [(c["emisor"], c["texto"]) for c in m.chat[desde:]
            if c["tipo"] == CHAT_MSG_BATTLEGROUND and c["emisor"] != m.guid]


def _probar_ordenes(m, inf, sec, espera=10):
    """Manda `debug state` y `debug position` por el chat de campo de batalla y cuenta quién contesta."""
    m.bombear(4)                                   # los bots acaban de cruzar el portal
    antes = len(m.chat)
    m.decir("debug state", CHAT_MSG_BATTLEGROUND)
    limite = time.time() + espera
    while time.time() < limite:
        m.bombear(1)
    estados = {e: t for e, t in _respuestas_bg(m, antes) if t in RESPUESTAS_DEBUG_STATE}
    inf.datos["ordenes"] = {"bots_que_contestan": len(estados), "estados": sorted(set(estados.values()))}
    if not inf.comprobar(sec, len(estados) >= 1,
                         "los bots del bando contestan a una orden por el chat de campo de batalla",
                         "%d bot(s) contestan a `debug state`: %s; mensajes de campo recibidos: %s" % (
                             len(estados), sorted(set(estados.values())), _respuestas_bg(m, antes)[:4]),
                         esperado=">=1", observado=len(estados), origen="desarrollo"):
        return
    antes = len(m.chat)
    m.decir("debug position", CHAT_MSG_BATTLEGROUND)
    limite = time.time() + espera
    while time.time() < limite:
        m.bombear(1)
    mapas = {}
    for e, t in _respuestas_bg(m, antes):
        partes = t.split()
        if len(partes) >= 4 and partes[3].isdigit():
            mapas[e] = int(partes[3])
    inf.comprobar(sec, bool(mapas) and all(v == m.mapa for v in mapas.values()),
                  "los bots que contestan siguen en el campo de batalla (no cambian de sitio ni dejan la partida)",
                  "%d respuesta(s) de posición; mapas %s; el jugador está en %s" % (len(mapas), sorted(set(mapas.values())), m.mapa),
                  esperado=m.mapa, observado=sorted(set(mapas.values())))


def _campo(ctx, sec, ordenes=False):
    """Flujo común de `campo-batalla` y `campo-batalla-ordenes`: encola, entra y mide."""
    inf, p = ctx.inf, ctx.p
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
        auto = bool(p.get("entrar", True))
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
        if ordenes and auto and t_dentro is not None:
            _probar_ordenes(m, inf, sec)
        inf.comprobar(sec, not m.errores_lectura, "paquetes leídos sin error", "; ".join(m.errores_lectura[:3]),
                      origen="cliente")


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
    _campo(ctx, "campo-batalla")


@caso(id="campo-batalla-ordenes", titulo="Órdenes del jugador a los bots dentro de un campo de batalla (UPD-R)",
      descripcion=__doc__, etiquetas=("campo-batalla", "queue-bots", "mod-playerbots", "personaje"),
      acciones=("conectar", "leer", "personaje", "gm", "cola"),
      parametros={"campo": Parametro(str, "wsg", "campo de batalla", opciones=tuple(TIPOS)),
                  "nivel": Parametro(int, 15, "nivel del jugador de prueba (el tramo 10-19 es el que tiene bots en línea)",
                                     minimo=10, maximo=80),
                  "espera": Parametro(int, 240, "segundos máximos esperando la oferta", minimo=30, maximo=900)},
      duracion_max=900, en_todo=False, protege=("mod-playerbots (seguridad de órdenes en campos de batalla)", "PLAN UPD-R"),
      control="directo",
      observa=("SMSG_MESSAGECHAT (tipo campo de batalla) con la respuesta de `debug state` y `debug position` de cada bot",),
      no_cubre=("el efecto de `Playerbots.AllowBattlegroundCommands` en sí: la cuenta de pruebas es GM y la seguridad "
                "la deja pasar antes de mirarla",
                "órdenes con efecto en la partida (`attack`, `flee`...: sólo se comprueba el acceso)",
                "bots de otro bando (la seguridad los rechaza antes por `IsOpposing`)"))
def ejecutar_ordenes(ctx):
    _campo(ctx, "campo-batalla-ordenes", ordenes=True)
