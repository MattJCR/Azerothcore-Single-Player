# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""mod-world-bots: guerra de mundo real de principio a fin (PLAN M34) y modo
samaritano (PLAN M35).

M34. `.wpvp iniciar <nombre>` arranca `StartEvent` directamente sobre el punto
pedido: a diferencia de la selección automática (`PickHotspot`), no exige que
el jugador esté en la zona (`OnlyWithPlayerInZone` sólo filtra el sorteo
periódico) -- sólo bots libres del tramo del punto. Se prueba cada punto
caliente activo (de menor a mayor nivel, más población) hasta que uno arranca,
se confirma con `.wpvp estado` y se para con `.wpvp parar <id>` sobre ESE id
concreto (no "todos": eso ya lo cubre el caso `mantenimiento`), comprobando
que el evento deja de listarse.

M35. Samaritano (`WorldBots.Samaritan`), EXPERIMENTAL y en 0 por defecto en la
VM: esta parte sólo tiene sentido con `WorldBots.Samaritan=1` puesto a mano
(SSH + `.reload config`) antes de ejecutar el caso, y devuelto a 0 al acabar
-- el caso no toca la config, sólo la ejercita. Un jugador en combate real por
debajo de `SamaritanHpPercent` de vida debe recibir hasta `SamaritanMaxHelpers`
ayudantes libres y cercanos (dentro de `SamaritanRadius` yardas: no son
teletransportados desde lejos, tienen que estar ya rondando cerca), avisados
con el mensaje «Un aventurero acude a ayudarte.» (`WorldBots.Announce`,
activado por defecto). Que vuelvan solos al terminar el combate sólo se puede
confirmar con `Server.log` («... deja de ayudar»): el cliente no recibe ningún
mensaje de despedida.
"""
import re
import time

from .. import actualizaciones as upd
from ..catalogo import Parametro, caso
from . import PersonajeTemporal, sin_colores
from .companeros_combate import _buscar_hostil, _spawn_hostil

LINEA_HOTSPOT = re.compile(r"#(\d+)\s+(\S+)\s+\[[^\]]*\]\s+(\S+):.*?nivel (\d+)-(\d+)")
LINEA_EVENTO = re.compile(r"#(\d+)\s+(\S+)\s+\((escaramuza|duelos)\)")


def _listar_hotspots(m):
    """`.wpvp lista` -> lista de puntos activos y sin enfriar, ordenada por nivel mínimo
    (los de nivel bajo tienen más población libre: se prueban primero)."""
    textos = [sin_colores(t) for t in m.comando("wpvp lista", 4)]
    out = []
    for t in textos:
        mobj = LINEA_HOTSPOT.search(t)
        if not mobj:
            continue
        out.append({"id": int(mobj.group(1)), "name": mobj.group(2), "estado": mobj.group(3),
                    "min": int(mobj.group(4)), "max": int(mobj.group(5)), "enfriando": "enfriando" in t})
    out.sort(key=lambda h: h["min"])
    return out


def _eventos_activos(m):
    """`.wpvp estado` -> {id: línea} de los eventos en marcha ahora mismo."""
    textos = [sin_colores(t) for t in m.comando("wpvp estado", 4)]
    out = {}
    for t in textos:
        mobj = LINEA_EVENTO.search(t)
        if mobj:
            out[int(mobj.group(1))] = t
    return out


def _probar_pvp_real(m, inf, sec, espera_poblacion):
    # El servidor puede llevar un rato sin ningún jugador real conectado
    # (mod-standby, DisabledWithoutRealPlayer de playerbots): la población de
    # bots aleatorios tarda en subir tras el primer login. Sin esta espera,
    # el primer intento encuentra el mundo casi vacío.
    m.bombear(espera_poblacion)

    hotspots = _listar_hotspots(m)
    candidatos = [h for h in hotspots if h["estado"] == "on" and not h["enfriando"]]
    inf.datos["wpvp_lista"] = hotspots
    if not inf.comprobar(sec, bool(candidatos), "`.wpvp lista` ofrece al menos un punto caliente activo y sin enfriar",
                          "%d de %d puntos" % (len(candidatos), len(hotspots)), origen="entorno"):
        return

    id_evento, nombre_spot, resp = None, None, ""
    intentos = []
    for intento in range(2):    # 2ª vuelta con más margen de población si la 1ª falla entera
        for h in candidatos:
            resp = sin_colores(" ".join(m.comando("wpvp iniciar %s" % h["name"], 3)))
            intentos.append({"spot": h["name"], "respuesta": resp[:200]})
            mobj = re.search(r"Evento #(\d+) arrancado", resp)
            if mobj:
                id_evento, nombre_spot = int(mobj.group(1)), h["name"]
                break
        if id_evento is not None:
            break
        m.bombear(45)
    inf.datos["wpvp_intento"] = {"spot": nombre_spot, "respuesta": resp[:300], "intentos": intentos[-6:]}
    if not inf.comprobar(sec, id_evento is not None,
                          "M34: `.wpvp iniciar` arranca un evento real con bots libres del tramo",
                          resp[:200] or "(sin respuesta)", estado_si_no="AVISO"):
        inf.anotar(sec, "AVISO", "M34: ningun punto caliente probado tenia bots libres suficientes",
                   "%d puntos probados de %d candidatos" % (len(candidatos), len(candidatos)))
        return

    activos = _eventos_activos(m)
    inf.comprobar(sec, id_evento in activos, "M34: `.wpvp estado` lista el evento recien arrancado",
                  activos.get(id_evento, "no aparece"), esperado="presente", observado=sorted(activos))

    m.bombear(15)   # deja que avance/controle un poco antes de pararlo: no es un parar-al-vuelo

    resp_parar = sin_colores(" ".join(m.comando("wpvp parar %d" % id_evento, 3)))
    inf.datos["wpvp_parar"] = resp_parar[:200]
    mobj = re.search(r"(\d+) evento\(s\) terminando", resp_parar)
    parados = int(mobj.group(1)) if mobj else 0
    inf.comprobar(sec, parados >= 1,
                  "M34: `.wpvp parar <id>` sobre el evento real (no 'todos') lo manda a terminar",
                  resp_parar[:200], esperado=">=1", observado=parados)

    limite = time.time() + 90
    sigue = id_evento in _eventos_activos(m)
    while time.time() < limite and sigue:
        m.bombear(5)
        sigue = id_evento in _eventos_activos(m)
    inf.comprobar(sec, not sigue,
                  "M34: el evento parado desaparece de `.wpvp estado` (los bots vuelven a casa)",
                  "sigue listado 90 s despues de pararlo" if sigue else "ya no aparece en `.wpvp estado`",
                  estado_si_no="AVISO")


def _bot_mas_cercano(m, nivel_min=1, nivel_max=80, excluir=()):
    """GUID y distancia (yardas) del jugador visible más cercano (no uno mismo,
    no en `excluir`) con nivel dentro de [nivel_min, nivel_max]: el samaritano
    sólo recluta a quien YA está a SamaritanRadius yardas y en el tramo del
    jugador, así que acercarse a uno de verdad (en vez de esperar a que pase
    por casualidad) es lo que hace fiable la prueba. `excluir` deja probar con
    otro candidato distinto en cada reintento, para no insistir siempre con el
    mismo bot si resulta que no está libre (en combate, reservado...)."""
    p = m.posicion_actual()
    if not p:
        return None, None
    mejor, mejor_d2 = None, None
    for guid, obj in m.objetos.items():
        if guid == m.guid or guid in excluir or obj.get("typeid") != upd.TYPEID_PLAYER:
            continue
        nivel = obj.get("valores", {}).get(upd.UNIT_FIELD_LEVEL)
        if nivel and not (nivel_min <= nivel <= nivel_max):
            continue
        pos = obj.get("pos")
        if not pos:
            continue
        d2 = (pos["x"] - p["x"]) ** 2 + (pos["y"] - p["y"]) ** 2
        if mejor_d2 is None or d2 < mejor_d2:
            mejor, mejor_d2 = guid, d2
    return mejor, (mejor_d2 ** 0.5 if mejor_d2 is not None else None)


def _probar_samaritano(m, inf, sec, estado, espera_poblacion, espera_ayuda):
    m.comando("levelup 24", 3)   # nivel 25: banda de ayudantes ni muy bajo ni muy alto
    nivel = m.valor_propio(upd.UNIT_FIELD_LEVEL)
    inf.comprobar(sec, nivel == 25, "nivel de prueba para el samaritano", "nivel %d" % nivel, origen="entorno")

    # La zona de partida (nivel 1) no tiene puntos de caza de nivel 25: world-bots
    # no reparte población de ese tramo ahí. Lakeshire es una de las zonas del
    # propio PLAN M34 (contestada, nivel 20-35) con caza de verdad de las dos
    # facciones.
    m.comando("tele Lakeshire", 5)
    m.bombear(3)

    # Deja que world-bots asiente poblacion en la zona (bots reales cerca, no
    # teletransportados: el samaritano sólo recluta a quien YA está a
    # SamaritanRadius yardas).
    m.bombear(espera_poblacion)

    entry, nombre_hostil = _buscar_hostil(m)
    if not inf.comprobar(sec, entry is not None, "hay un hostil de prueba en `.lookup creature`",
                          "kobold vermin -> %s (%s)" % (entry, nombre_hostil), origen="entorno"):
        return
    m.comando("gm off", 1)
    mob = _spawn_hostil(m, entry, vida=2000000)
    estado["mob"] = mob
    if not inf.comprobar(sec, mob is not None, "se consigue un hostil de prueba (`.npc add`)",
                          "entry %s (%s)" % (entry, nombre_hostil), origen="entorno"):
        return

    maxhp = m.valor_propio(upd.UNIT_FIELD_MAXHEALTH)
    if not inf.comprobar(sec, maxhp > 0, "vida máxima propia legible", "maxhp %d" % maxhp, origen="entorno"):
        return
    objetivo_hp = max(1, maxhp * 25 // 100)     # 25 % < SamaritanHpPercent (35 % por defecto)

    # El bot libre más cercano puede estar ocupado (en combate con un bicho:
    # BotEligibility::IsAvailable lo descarta) o alejarse antes de que llegue
    # la pasada del samaritano (cada WorldBots.ScanSeconds). En vez de fijar
    # la posición una sola vez, se reintenta unas cuantas veces: recolocarse
    # junto al candidato más cercano DEL TRAMO, entrar en combate real y
    # mantener la vida baja unos segundos antes de probar con otro.
    antes_msj = len(m.mensajes)
    avisos, intentos_reposicion, probados = [], [], set()
    limite_total = time.time() + espera_ayuda
    while time.time() < limite_total and not avisos:
        # Un candidato ya probado dos veces sin suerte se descarta un rato:
        # puede que ese bot en concreto no esté libre (en combate con un
        # bicho, reservado por otro módulo) y no lo vaya a estar pronto.
        excluir = probados if len(intentos_reposicion) >= 2 else ()
        guid_cercano, distancia = _bot_mas_cercano(m, nivel - 3, nivel + 8, excluir=excluir)
        if guid_cercano is None and excluir:
            probados.clear()   # se agotaron los candidatos nuevos: reintentar con todos otra vez
            guid_cercano, distancia = _bot_mas_cercano(m, nivel - 3, nivel + 8)
        if guid_cercano is not None:
            probados.add(guid_cercano)
        if guid_cercano is not None and distancia is not None and distancia > 30.0:
            pos_bot = m.objetos[guid_cercano]["pos"]
            m.comando("gm on", 1)
            m.comando("go xyz %.2f %.2f %.2f" % (pos_bot["x"] + 3.0, pos_bot["y"], pos_bot["z"]), 5)
            m.bombear(2)
            m.comando("gm off", 1)
            _, distancia = _bot_mas_cercano(m, nivel - 3, nivel + 8, excluir=set(probados) - {guid_cercano})
        intentos_reposicion.append({"guid": guid_cercano, "distancia": distancia})

        m.seleccionar(mob)
        m.atacar(mob)
        m.bombear(1)
        m.seleccionar(m.guid)    # '.modify hp' actúa sobre la selección: sin esto bajaría la vida AL MOB
        m.bombear(1)
        m.comando("modify hp %d" % objetivo_hp, 2)

        ciclo_limite = time.time() + min(20, max(1, limite_total - time.time()))
        while time.time() < ciclo_limite and not avisos:
            m.bombear(3)
            avisos = [sin_colores(t) for _, t in m.mensajes[antes_msj:] if "acude a ayudarte" in sin_colores(t)]
    inf.datos["samaritano_bot_cercano"] = intentos_reposicion[0] if intentos_reposicion else None
    inf.datos["samaritano_reposiciones"] = intentos_reposicion
    inf.comprobar(sec, any(r["distancia"] is not None and r["distancia"] <= 45.0 for r in intentos_reposicion),
                  "hubo al menos un jugador (bot) del tramo a menos de SamaritanRadius (45) yardas",
                  "%d intento(s): %s" % (len(intentos_reposicion), intentos_reposicion[:3]),
                  estado_si_no="AVISO")
    inf.datos["samaritano_aviso"] = avisos[:3]
    inf.comprobar(sec, bool(avisos),
                  "M35: con vida baja en combate real, un bot libre cercano acude a ayudar",
                  " | ".join(avisos[:2]) if avisos else "sin aviso en %d s (revisar WorldBots.Samaritan=1 en la VM "
                  "y que haya bots libres a menos de SamaritanRadius yardas)" % espera_ayuda,
                  estado_si_no="AVISO")

    # Termina el combate de forma determinista: sin objetivo, sin combate: el
    # samaritano debe recoger al ayudante en la siguiente pasada (no hay aviso
    # de vuelta al cliente: se confirma sólo por Server.log, fuera de este caso).
    m.seleccionar(mob)
    m.comando("npc delete", 2)
    estado["mob"] = None
    m.seleccionar(m.guid)
    m.comando("modify hp %d" % maxhp, 2)
    m.bombear(3)


@caso(id="wpvp-samaritano", titulo="mod-world-bots: guerra de mundo real y samaritano",
      descripcion=__doc__,
      etiquetas=("world-bots", "pvp", "samaritano", "personaje"),
      acciones=("conectar", "leer", "personaje", "gm"),
      parametros={"partes": Parametro(str, "ambos", "qué probar", opciones=("ambos", "pvp", "samaritano")),
                  "espera_poblacion": Parametro(int, 90, "segundos dejando asentar población de bots "
                                                "(tras standby/sin jugador) antes de intentar la guerra o el "
                                                "samaritano", minimo=0, maximo=300),
                  "espera_ayuda": Parametro(int, 60, "segundos máximos esperando al ayudante samaritano",
                                            minimo=10, maximo=300)},
      duracion_max=900, protege=("mod-world-bots", "PLAN M34", "PLAN M35"), control="directo",
      observa=("`.wpvp estado`/`lista`", "mensaje «Un aventurero acude a ayudarte.»",
               "Server.log ([world-bots] ... acude a ayudar / deja de ayudar)"),
      no_cubre=("selección automática de punto caliente (PickHotspot/OnlyWithPlayerInZone): éste sólo prueba "
                "`.wpvp iniciar` directo", "que el ayudante samaritano vuelva a casa (sin aviso al cliente, "
                "sólo Server.log)", "duelos (IsDuelSpot) como evento forzado: se prueba lo que `.wpvp lista` ofrezca, "
                "puede tocar un punto de escaramuza o de duelo según la población libre"))
def ejecutar(ctx):
    inf, sec, p = ctx.inf, "world-bots-pvp-samaritano", ctx.p
    m = ctx.nueva_sesion()
    estado = {"mob": None}

    def deshacer():
        if estado["mob"]:
            m.seleccionar(estado["mob"])
            m.comando("npc delete", 2)

    # Humano/guerrero (Alianza): Lakeshire (Redridge), la zona del samaritano,
    # es territorio de la Alianza -- un bot cercano de la Horda no cuenta
    # (bot->GetTeamId() == human->GetTeamId() en SamaritanPass). La parte de
    # guerra de mundo (M34) no depende de la facción del propio GM.
    with PersonajeTemporal(ctx, m, 1, 1, sec, antes_de_salir=deshacer) as pj:
        ctx.anotar_servidor(m)
        m.comando("gm off", 1)

        if p["partes"] in ("ambos", "pvp"):
            _probar_pvp_real(m, inf, sec, p["espera_poblacion"])

        if p["partes"] in ("ambos", "samaritano"):
            _probar_samaritano(m, inf, sec, estado, p["espera_poblacion"], p["espera_ayuda"])

        inf.comprobar(sec, not m.errores_lectura, "paquetes leídos sin error", "; ".join(m.errores_lectura[:3]),
                      origen="cliente")
