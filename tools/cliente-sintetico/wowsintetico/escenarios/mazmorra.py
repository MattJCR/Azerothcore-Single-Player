# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Mazmorra completa con el jugador sintético en selfbot y cuatro compañeros (CS01).

El personaje del cliente sintético es un DPS de verdad del grupo: tras la
preparación GM, `.playerbots bot self` pone la IA de playerbots sobre ESE mismo
personaje (mismo GUID, misma sesión) y el agente deja de dar órdenes de
movimiento o combate. Los compañeros los pone mod-party-here (`.grupo
mazmorra`: tanque, sanador y daño); al pisar la instancia queue-bots trae a los
rezagados, topa su equipo y lanza `.dc on` en nombre del humano, y
dungeon-clear lleva el recorrido con el tanque.

Lo que el cliente hace mientras tanto (responsabilidades que selfbot deja al
cliente): confirmar teletransportes cercanos y lejanos, mandar los area
triggers que pisa el personaje (ReachAreaTriggerAction los omite en selfbot),
TIME_SYNC y ping. Lo que observa: telemetria.Telemetria.

Intervenciones del agente (sólo si `intervenir`): reactivar dungeon-clear si se
apaga con jefes pendientes y saltar el objetivo tras `max_atasco` segundos sin
progreso. Cada una queda anotada y el caso no puede salir OK si hubo alguna.
"""
import os
import re
import time

from .. import actualizaciones as upd
from ..areatriggers import Vigilante
from ..catalogo import Parametro, caso
from ..ejecutor import Bloqueo
from ..telemetria import Telemetria
from . import CLASES, RAZAS, PersonajeTemporal, sin_colores

# Mazmorras con su entrada. `tele`: nombre en game_tele (se busca con .lookup tele);
# `dentro`: punto de arranque de `.dc test` (DcTestDungeonRegistry.cpp) por si el portal falla.
MAZMORRAS = {
    "bfd": {"nombre": "Cavernas de Brazanegra", "mapa": 48, "zona": 719, "nivel": 25, "tele": "blackfathom",
            "exterior": (1, 4249.99, 740.10, -25.67), "dentro": (48, -151.89, 106.96, -39.87)},
    "rfc": {"nombre": "Sima Ígnea", "mapa": 389, "zona": 2437, "nivel": 16, "tele": "ragefire",
            "exterior": None, "dentro": (389, 3.81, -14.82, -17.84)},
    "wc": {"nombre": "Cuevas de los Lamentos", "mapa": 43, "zona": 718, "nivel": 20, "tele": "wailing",
           "exterior": None, "dentro": (43, -163.49, 132.90, -73.66)},
    "deadmines": {"nombre": "Minas de la Muerte", "mapa": 36, "zona": 1581, "nivel": 20, "tele": "deadmines",
                  "exterior": None, "dentro": (36, -16.40, -383.07, 61.78)},
    "sfk": {"nombre": "Castillo de Colmillo Oscuro", "mapa": 33, "zona": 209, "nivel": 21, "tele": "shadowfang",
            "exterior": None, "dentro": (33, -229.13, 2109.18, 76.89)},
}
CLASE_JUGADOR = {"mago": (1, 8), "picaro": (1, 4), "guerrero": (1, 1), "cazador": (3, 3), "brujo": (1, 9)}


# ── utilidades compartidas con otros escenarios ─────────────────────────────
def selfbot(m, activar: bool) -> tuple:
    """`.playerbots bot self` es un conmutador: se envuelve comprobando la respuesta.
    Devuelve (estado_final, textos). No reintenta a ciegas: como mucho una segunda
    orden, y sólo si la primera dejó el estado contrario al pedido."""
    textos = []
    for _ in range(2):
        r = sin_colores(" ".join(m.comando_hasta("playerbots bot self", r"player botAI|SelfBot is now|Self-bot|permission", 5)))
        textos.append(r)
        # texto de playerbots hasta 037c014 ("Enable/Disable player botAI") y desde #2815 ("SelfBot is now ...")
        if "Enable player botAI" in r or "SelfBot is now active" in r:
            estado = True
        elif "Disable player botAI" in r or "SelfBot is now deactivated" in r:
            estado = False
        else:
            return None, textos                          # deshabilitado o sin permiso
        if estado == activar:
            return estado, textos
    return estado, textos


def formar_grupo(m, inf, sec, tamano=5, espera=240) -> dict:
    """`.grupo mazmorra` (mod-party-here) y espera a los miembros. Devuelve guid -> nombre."""
    m.comando("grupo mazmorra", 2)
    limite = time.time() + espera
    completo = False
    while time.time() < limite:
        m.bombear(2)
        miembros = [x for x in (m.grupo or {}).get("miembros", []) if x["guid"] != m.guid]
        completo = completo or any("grupo esta completo" in t for _, t in m.mensajes[-50:])
        if len(miembros) >= tamano - 1 and all(x["conectado"] for x in miembros):
            break
    miembros = {x["guid"]: x["nombre"] for x in (m.grupo or {}).get("miembros", []) if x["guid"] != m.guid}
    inf.comprobar(sec, len(miembros) == tamano - 1, "party-here forma el grupo de %d" % tamano,
                  "%d compañeros: %s%s" % (len(miembros), ", ".join(miembros.values()),
                                           "" if completo else " (sin «Tu grupo esta completo»)"),
                  esperado=tamano - 1, observado=len(miembros))
    return miembros


def cerca_de(m, miembros) -> set:
    """Nombres de `miembros` (guid -> nombre) visibles como objeto cercano
    (SMSG_UPDATE_OBJECT): señal fiable de que de verdad están junto al jugador.
    Las estadísticas de grupo (SMSG_PARTY_MEMBER_STATS) no sirven tras un
    cambio de mapa: se quedan con la última posición conocida EN EL OTRO MAPA
    hasta que el miembro vuelve a estar a la vista."""
    return {n for g, n in miembros.items() if g in m.objetos}


def esperar_llegada(m, sec, inf, miembros, texto, limite_s=90):
    """Espera a que todos los de `miembros` se vean cerca; anota AVISO si no
    (puede variar sin ser un error: reintentos de summon, mapa cargando...)."""
    limite = time.time() + limite_s
    cerca = cerca_de(m, miembros)
    while time.time() < limite and len(cerca) < len(miembros):
        m.bombear(3)
        cerca = cerca_de(m, miembros)
    faltan = set(miembros.values()) - cerca
    inf.comprobar(sec, not faltan, texto,
                  "%d/%d cerca (visibles); faltan: %s" % (len(cerca), len(miembros), ", ".join(faltan) or "ninguno"),
                  estado_si_no="AVISO")
    return not faltan


def roles_del_grupo(m) -> str:
    return sin_colores(" ".join(m.comando("grupo estado", 2)))


def roles(texto: str) -> dict:
    """`.grupo estado` de party-here: '  Nombre (nivel N, tanque|sanador|dano...)' -> {nombre: rol}."""
    return {n: r for n, r in re.findall(r"(\S+) \(nivel \d+, (tanque|sanador|dano)", texto)}


# Rol que falta -> (clase de `addclass`, plantilla de talentos de playerbots.conf)
REPUESTOS = {"tanque": ("warrior", "prot pve"), "sanador": ("priest", "holy pve")}


def reponer_rol(m, rol, dps: str, calidad: str, anotar) -> str:
    """Cambia un DPS de party-here por un bot `addclass` con la especialización del rol.
    Devuelve el nombre del nuevo o None. Es una intervención del agente: se anota fuera."""
    clase, plantilla = REPUESTOS[rol]
    m.comando("grupo fuera %s" % dps, 3)
    limite = time.time() + 30
    while time.time() < limite and any(x["nombre"] == dps for x in (m.grupo or {}).get("miembros", [])):
        m.bombear(1)
    antes = {x["guid"] for x in (m.grupo or {}).get("miembros", [])}
    r = sin_colores(" ".join(m.comando_hasta("playerbots bot addclass %s" % clase, r"[Aa]dd class|failed|permission", 8)))
    anotar("addclass %s: %s" % (clase, r[:80]))
    limite = time.time() + 60
    nuevo = None
    while time.time() < limite and nuevo is None:
        m.bombear(1)
        nuevo = next((x for x in (m.grupo or {}).get("miembros", []) if x["guid"] not in antes), None)
    if nuevo is None:
        return None
    m.bombear(5)
    r = sin_colores(" ".join(m.comando_hasta("playerbots bot init=%s %s" % (calidad, nuevo["nombre"]), r"ok|ERROR|not", 20)))
    anotar("init=%s %s: %s" % (calidad, nuevo["nombre"], r[:80]))
    m.susurrar(nuevo["nombre"], "talents spec %s" % plantilla)
    m.bombear(4)
    respuesta = [c["texto"] for c in m.chat[-10:] if c["emisor"] == nuevo["guid"]]
    anotar("talents spec %s a %s: %s" % (plantilla, nuevo["nombre"], " / ".join(respuesta)[:120]))
    return nuevo["nombre"]


def _buscar_tele(m, clave) -> str:
    texto = " ".join(m.comando("lookup tele %s" % clave, 2))
    nombres = re.findall(r"\[([^\]]+)\]", sin_colores(texto))
    return nombres[0] if nombres else None


def entrar_por_portal(m, ctx, datos, inf, sec, espera=25) -> bool:
    """Lleva al jugador al exterior y lo mete en el trigger de entrada más cercano,
    como si lo pisara andando: el vigilante manda CMSG_AREATRIGGER."""
    nombre = _buscar_tele(m, datos["tele"])
    if nombre:
        m.comando("tele %s" % nombre, 5)
    elif datos["exterior"]:
        mapa, x, y, z = datos["exterior"]
        m.ir_a({"x": x, "y": y, "z": z}, 5, mapa=mapa)
    else:
        return False
    inf.paso("en el exterior (%s): mapa %s, %.0f, %.0f, %.0f" % (nombre or "coordenadas", m.mapa,
                                                                 m.pos["x"], m.pos["y"], m.pos["z"]))
    p = m.posicion_actual()
    cercanos = sorted((t for t in m.vigilante.activos),
                      key=lambda t: (t["x"] - p["x"]) ** 2 + (t["y"] - p["y"]) ** 2 + (t["z"] - p["z"]) ** 2)[:4]
    for t in cercanos:
        if ((t["x"] - p["x"]) ** 2 + (t["y"] - p["y"]) ** 2) ** 0.5 > 250:
            break
        inf.paso("pisando el trigger %d (%.0f, %.0f, %.0f)" % (t["id"], t["x"], t["y"], t["z"]))
        m.ir_a({"x": t["x"], "y": t["y"], "z": t["z"]}, 1)
        limite = time.time() + espera
        while time.time() < limite and m.mapa != datos["mapa"]:
            m.bombear(1)
        if m.mapa == datos["mapa"]:
            ctx.inf.datos["trigger_entrada"] = t["id"]
            return True
    return False


def entrar_directo(m, datos):
    mapa, x, y, z = datos["dentro"]
    m.ir_a({"x": x, "y": y, "z": z}, 6, mapa=mapa)
    limite = time.time() + 20
    while time.time() < limite and m.mapa != mapa:
        m.bombear(1)
    return m.mapa == mapa


def esperar_miembros_dentro(m, miembros, zona, espera=150) -> list:
    """Miembros en la mazmorra: zona de SMSG_PARTY_MEMBER_STATS_FULL (pedida como el marco de
    grupo) o visibles en nuestro mapa tras el cambio de mapa (los objetos se vacían al cambiar)."""
    limite = time.time() + espera

    def dentro():
        return [g for g in miembros if m.miembros.get(g, {}).get("zona") == zona or g in m.objetos]

    while time.time() < limite:
        for g in miembros:
            m.pedir_estadisticas(g)
        m.bombear(3)
        if len(dentro()) == len(miembros):
            break
    return dentro()


# ── recorrido ───────────────────────────────────────────────────────────────
class Recorrido:
    def __init__(self, ctx, m, tele, miembros, datos):
        self.ctx, self.m, self.tele, self.miembros, self.datos = ctx, m, tele, miembros, datos
        self.intervenciones = []
        self.fin = None

    def intervenir(self, orden, motivo):
        self.intervenciones.append({"t": self.tele.rel(), "orden": orden, "motivo": motivo})
        self.tele.escribir("intervencion", orden=orden, motivo=motivo)
        self.ctx.inf.paso("INTERVENCIÓN: .%s (%s)" % (orden, motivo))
        self.m.comando(orden, 2)

    def muestrear(self):
        self.tele.muestra_gps(self.m.guid, self.m.gps())
        for g in self.miembros:
            self.m.pedir_estadisticas(g)
        self.m.bombear(0.5)
        for g in self.miembros:
            self.tele.muestra_miembro(g, self.m.posicion_de(g))

    def recortar(self):
        for lista in (self.m.mensajes, self.m.chat, self.m.addon):
            if len(lista) > 4000:
                del lista[:2000]

    def jefes_pendientes(self):
        return [j for j in self.tele.jefes.values() if j["estado"] not in ("dead", "skipped")]

    def ejecutar(self, duracion, intervalo_gps, intervenir, max_atasco, parar_en_grave=True):
        m, tele, p = self.m, self.tele, self.ctx.p
        limite = time.time() + duracion
        prox_gps = prox_jefes = 0
        apagado_desde = None
        muertos_desde = None
        parar = os.path.join(os.path.dirname(self.ctx.carpeta_artefactos.rstrip("/\\")) or ".", "PARAR")
        parar_local = os.path.join(self.ctx.carpeta_artefactos, "PARAR")
        while True:
            ahora = time.time()
            if ahora > limite:
                self.fin = "tiempo agotado (%d s)" % duracion
                break
            if os.path.exists(parar_local) or os.path.exists(parar):
                self.fin = "parado por el operador (fichero PARAR)"
                break
            m.bombear(1)
            tele.revisar()
            self.recortar()
            if ahora >= prox_gps:
                self.muestrear()
                prox_gps = time.time() + intervalo_gps
            if ahora >= prox_jefes:
                m.comando("dc bosses", 1)
                prox_jefes = time.time() + 120
            if tele.jefes and not self.jefes_pendientes():
                self.fin = "todos los jefes muertos o saltados"
                break
            if parar_en_grave:
                grave = next((a for a in tele.anomalias if a["severidad"] == "grave"), None)
                if grave:
                    self.fin = "parado en la primera anomalía grave: %s — %s" % (grave["tipo"], grave["texto"])
                    break
            # dungeon-clear apagado con jefes pendientes
            if tele.dc["visto_activo"] and tele.dc["activo"] is False and self.jefes_pendientes():
                apagado_desde = apagado_desde or ahora
                if ahora - apagado_desde > 90:
                    if intervenir and sum(1 for i in self.intervenciones if i["orden"] == "dc on") < 5:
                        self.intervenir("dc on", "dungeon-clear lleva 90 s apagado con jefes pendientes")
                        apagado_desde = None
                    else:
                        self.fin = "dungeon-clear apagado con jefes pendientes"
                        break
            else:
                apagado_desde = None
            # grupo entero muerto sin recuperación
            vivos = [g for g, mi in tele.miembros.items() if mi["estado"] == "vivo"]
            yo_vivo = m.valor_propio(upd.UNIT_FIELD_HEALTH, 1) > 0
            if not vivos and not yo_vivo:
                muertos_desde = muertos_desde or ahora
                if ahora - muertos_desde > 300:
                    self.fin = "wipe sin recuperación en 300 s"
                    break
            else:
                muertos_desde = None
            # atasco prolongado
            quieto = ahora - tele.ultimo_progreso
            if intervenir and quieto > max_atasco and tele.dc["activo"]:
                if sum(1 for i in self.intervenciones if i["orden"] == "dc skip") >= 6:
                    self.fin = "atascado tras 6 saltos de objetivo"
                    break
                self.intervenir("dc skip", "%.0f s sin progreso hacia %s (%s)" % (
                    quieto, tele.dc["jefe"], tele.dc["estado"]))
                tele.ultimo_progreso = time.time()
        m.comando("dc bosses", 3)
        m.bombear(2)
        return self.fin


@caso(id="mazmorra", titulo="Mazmorra completa con selfbot y cuatro compañeros (CS01)",
      descripcion=__doc__,
      etiquetas=("mazmorra", "selfbot", "dungeon-clear", "party-here", "queue-bots", "playerbots", "largo",
                 "personaje"),
      acciones=("conectar", "leer", "personaje", "gm", "selfbot", "grupo", "mazmorra"), requiere=("dbc",),
      parametros={
          "mazmorra": Parametro(str, "bfd", "clave de la mazmorra", opciones=sorted(MAZMORRAS)),
          "clase": Parametro(str, "mago", "clase del jugador sintético (DPS)", opciones=sorted(CLASE_JUGADOR)),
          "nivel": Parametro(int, 0, "nivel del jugador (0 = el recomendado de la mazmorra)", minimo=0, maximo=80),
          "calidad": Parametro(str, "rare", "equipo de `initself`", opciones=["uncommon", "rare", "epic"]),
          "entrada": Parametro(str, "portal", "portal (pisando el area trigger) o directa (.go dentro)",
                               opciones=["portal", "directa"]),
          "duracion": Parametro(int, 5400, "segundos máximos del recorrido", minimo=300, maximo=14400),
          "intervalo_gps": Parametro(int, 15, "segundos entre muestras .gps del grupo", minimo=5, maximo=120),
          "intervenir": Parametro(bool, False, "reactivar dc y saltar objetivos atascados (queda anotado)"),
          "max_atasco": Parametro(int, 600, "segundos sin progreso antes de `.dc skip`", minimo=120, maximo=3600),
          "respiracion_acuatica": Parametro(bool, False, "aura 11789 (Respiración acuática, sin fin) a los cinco "
                                                         "antes del selfbot: mitigación anotada como intervención"),
          "parar_en_grave": Parametro(bool, True, "cortar el recorrido en la primera anomalía grave y avisar "
                                                   "(no seguir ni mitigar para completarlo)"),
          "completar_roles": Parametro(bool, False, "si party-here no trae tanque o sanador, cambiar un DPS por "
                                                    "un bot addclass con esa especialización (intervención)"),
      },
      duracion_max=15000, protege=("CS01", "mod-playerbots (selfbot)", "mod-dungeon-clear", "mod-party-here",
                                   "mod-queue-bots (dc automático, rezagados, equipo)"),
      control="delegado",
      observa=("mensajes de addon DC (STATUS/BOSS/CHAT)", "SMSG_PARTY_MEMBER_STATS", "SMSG_MONSTER_MOVE propio",
               "SMSG_START_MIRROR_TIMER", "SMSG_ENVIRONMENTAL_DAMAGE_LOG", "teletransportes", "`.gps` del grupo"),
      no_cubre=("render, cámara y animaciones", "entidades fuera del rango visible salvo por `.gps`",
                "atravesar paredes (no se infiere de dos muestras)"), en_todo=False)
def ejecutar(ctx):
    inf, p = ctx.inf, ctx.p
    datos = MAZMORRAS[p["mazmorra"]]
    nivel = p["nivel"] or datos["nivel"]
    raza, clase = CLASE_JUGADOR[p["clase"]]
    sec = "%s (%s %s %d)" % (datos["nombre"], CLASES[clase], RAZAS[raza], nivel)
    m = ctx.nueva_sesion()
    estado = {"selfbot": False, "grupo": False, "addclass": []}

    def deshacer():
        for nombre in estado["addclass"]:
            m.comando("playerbots bot remove %s" % nombre, 2)
        if estado["selfbot"]:
            final, textos = selfbot(m, False)
            inf.comprobar(sec, final is False, "selfbot desactivado al terminar", " / ".join(textos)[:200],
                          estado_si_no="AVISO")
            estado["selfbot"] = bool(final)
        if estado["grupo"]:
            m.comando("dc off", 1)
            m.comando("grupo fuera", 3)
            m.bombear(3)
            inf.comprobar(sec, not (m.grupo or {}).get("miembros"), "los compañeros se van con `.grupo fuera`",
                          "%d miembros" % len((m.grupo or {}).get("miembros", [])), estado_si_no="AVISO")

    with PersonajeTemporal(ctx, m, raza, clase, sec, antes_de_salir=deshacer) as pj:
        ctx.anotar_servidor(m)
        # ── preparación GM (reglas de GM; se deja constancia) ──────────────
        m.comando("gm off", 1)
        m.comando("levelup %d" % (nivel - 1), 3)
        nivel_real = m.valor_propio(upd.UNIT_FIELD_LEVEL)
        if not inf.comprobar(sec, nivel_real == nivel, "nivel de prueba", "nivel %d" % nivel_real,
                             esperado=nivel, observado=nivel_real, origen="entorno"):
            return
        r = sin_colores(" ".join(m.comando_hasta("playerbots bot initself=%s" % p["calidad"], r"initself|ERROR", 20)))
        m.bombear(3)
        equipo = [e for e in m.equipo_visible() if e]
        inf.comprobar(sec, "initself ok" in r and len(equipo) >= 6, "playerbots equipa y prepara al jugador (initself)",
                      "%s; %d piezas visibles, %d hechizos" % (r[:80], len(equipo), len(m.hechizos)),
                      origen="entorno")
        inf.datos["preparacion"] = {"nivel": nivel_real, "equipo_visible": equipo, "hechizos": len(m.hechizos),
                                    "habilidades": len(m.habilidades()), "gm": "off"}

        vigilante = Vigilante(ctx.dbc.areatriggers())
        m.armar_areatriggers(vigilante)
        ruta = ctx.artefacto("linea-de-tiempo.jsonl")
        tele = Telemetria(m, ruta, mapa_objetivo=datos["mapa"], zona_objetivo=datos["zona"], eco=inf._eco)

        # ── grupo ─────────────────────────────────────────────────────────
        miembros = formar_grupo(m, inf, sec)
        estado["grupo"] = bool(miembros)
        if len(miembros) < 4:
            raise Bloqueo("party-here no reunió cuatro compañeros (%d)" % len(miembros))
        texto_roles = roles_del_grupo(m)
        inf.datos["grupo_estado"] = texto_roles[:800]
        rol = roles(texto_roles)
        inf.datos["roles_party_here"] = rol
        faltan = [r for r in ("tanque", "sanador") if r not in rol.values()]
        inf.comprobar(sec, not faltan, "party-here trae tanque y sanador (grupo de mazmorra)",
                      "roles: %s%s" % (", ".join("%s=%s" % x for x in rol.items()),
                                       "; faltan: " + ", ".join(faltan) if faltan else ""),
                      esperado="tanque, sanador y daño", observado=sorted(rol.values()))
        intervenciones_previas = []
        if faltan and not p["completar_roles"]:
            inf.paso("parado: el grupo no tiene %s (completar_roles=si para reponerlo con addclass)" % ", ".join(faltan))
            return
        if faltan and p["completar_roles"]:
            for r in faltan:
                dps = next((n for n, x in rol.items() if x == "dano"), None)
                if dps is None:
                    break
                rol.pop(dps)
                nuevo = reponer_rol(m, r, dps, p["calidad"], inf.paso)
                intervenciones_previas.append({"orden": "addclass %s" % REPUESTOS[r][0],
                                               "motivo": "party-here no trajo %s (sale %s)" % (r, dps),
                                               "resultado": nuevo})
                inf.paso("INTERVENCIÓN: %s por %s (%s)" % (nuevo, dps, r))
                if nuevo:
                    estado["addclass"].append(nuevo)
            miembros = {x["guid"]: x["nombre"] for x in (m.grupo or {}).get("miembros", []) if x["guid"] != m.guid}
            inf.comprobar(sec, len(miembros) == 4 and len(estado["addclass"]) == len(faltan),
                          "grupo recompuesto con los roles que faltaban", ", ".join(miembros.values()),
                          origen="entorno")
        tele.fijar_grupo(miembros)

        # ── entrada ───────────────────────────────────────────────────────
        dentro = False
        if p["entrada"] == "portal":
            dentro = entrar_por_portal(m, ctx, datos, inf, sec)
            inf.comprobar(sec, dentro, "entrar por el portal pisando su area trigger (CMSG_AREATRIGGER)",
                          "trigger %s → mapa %s" % (inf.datos.get("trigger_entrada"), m.mapa),
                          estado_si_no="AVISO")
        if not dentro:
            dentro = entrar_directo(m, datos)
            if p["entrada"] == "portal":
                inf.paso("entrada directa con .go tras fallar el portal")
        if not inf.comprobar(sec, dentro, "el jugador está en la mazmorra", "mapa %s" % m.mapa,
                             esperado=datos["mapa"], observado=m.mapa):
            return
        dentro_bots = esperar_miembros_dentro(m, miembros, datos["zona"])
        inf.comprobar(sec, len(dentro_bots) == len(miembros), "los cuatro compañeros llegan a la mazmorra",
                      "%d/%d dentro (zona %d)" % (len(dentro_bots), len(miembros), datos["zona"]),
                      esperado=len(miembros), observado=len(dentro_bots))

        if p["respiracion_acuatica"]:
            # Preparación GM, antes de delegar: seleccionar a cada uno y `.aura` (actúa sobre la selección).
            for g in list(miembros) + [m.guid]:
                m.seleccionar(g)
                m.bombear(0.3)
                m.comando("aura 11789", 1)
            m.seleccionar(0)
            for g in miembros:
                m.pedir_estadisticas(g)
            m.bombear(2)
            con_aura = [n for g, n in miembros.items() if 11789 in m.miembros.get(g, {}).get("auras", [])]
            inf.comprobar(sec, len(con_aura) == len(miembros), "los compañeros tienen Respiración acuática",
                          ", ".join(con_aura), estado_si_no="AVISO", origen="entorno")
            intervenciones_previas.append({"orden": "aura 11789 x5", "motivo": "mitigación del ahogamiento pedida "
                                                                                "por parámetro (respiracion_acuatica)"})
            inf.paso("INTERVENCIÓN: Respiración acuática (11789) a los cinco")

        # ── delegación ────────────────────────────────────────────────────
        final, textos = selfbot(m, True)
        estado["selfbot"] = bool(final)
        if not inf.comprobar(sec, final is True, "`.playerbots bot self` activa la IA sobre el propio personaje",
                             " / ".join(textos)[:200]):
            return
        inf.datos["selfbot"] = {"guid": m.guid, "nombre": pj.nombre, "respuestas": textos}
        tele.escribir("selfbot", activo=True, guid=m.guid)

        # dungeon-clear: primero lo que haga queue-bots solo
        limite = time.time() + 75
        while time.time() < limite and not tele.dc["visto_activo"]:
            m.bombear(1)
        auto = tele.dc["visto_activo"]
        inf.comprobar(sec, auto, "queue-bots activa dungeon-clear solo al entrar (sin orden del agente)",
                      "activo a los %s s" % tele.dc.get("t_activo") if auto else "no llegó STATUS activo en 75 s",
                      estado_si_no="AVISO")
        rec = Recorrido(ctx, m, tele, miembros, datos)
        rec.intervenciones += intervenciones_previas
        if not auto and not p["intervenir"]:
            inf.anotar(sec, "FALLO", "dungeon-clear no arrancó: se para y se avisa", "sin STATUS activo en 75 s")
            return
        if not auto:
            rec.intervenir("dc on", "queue-bots no lo activó en 75 s")
            m.bombear(5)
        tanque = tele.tanque
        inf.comprobar(sec, tanque in miembros, "hay un tanque bot que dirige (emisor de STATUS)",
                      tele.nombre(tanque) if tanque else "ningún STATUS de un miembro")
        m.comando("dc bosses", 3)

        # ── recorrido ─────────────────────────────────────────────────────
        inf.paso("recorrido en marcha: el agente sólo observa")
        fin = rec.ejecutar(p["duracion"], p["intervalo_gps"], p["intervenir"], p["max_atasco"], p["parar_en_grave"])
        resumen = tele.resumen()
        tele.cerrar()
        inf.paso("fin del recorrido: %s (%.0f s)" % (fin, resumen["duracion_s"]))
        inf.datos["recorrido"] = {"fin": fin, "intervenciones": rec.intervenciones, **resumen}

        # ── aserciones ────────────────────────────────────────────────────
        grupo_final = [x for x in (m.grupo or {}).get("miembros", []) if x["guid"] != m.guid]
        inf.comprobar(sec, len(grupo_final) == 4, "no aparece un sexto participante",
                      "%d compañeros al final" % len(grupo_final), esperado=4, observado=len(grupo_final))
        inf.comprobar(sec, resumen["movimientos_propios_del_servidor"] > 0,
                      "playerbots mueve a ESTE personaje (SMSG_MONSTER_MOVE con nuestro GUID)",
                      "%d trayectorias" % resumen["movimientos_propios_del_servidor"])
        jefes = resumen["jefes"]
        muertos = [j["nombre"] for j in jefes.values() if j["estado"] == "dead"]
        pendientes = ["%s (%s)" % (j["nombre"], j["estado"]) for j in jefes.values() if j["estado"] != "dead"]
        inf.comprobar(sec, bool(jefes), "dungeon-clear publica la lista de jefes", "%d jefes" % len(jefes))
        inf.comprobar(sec, jefes and not pendientes, "todos los jefes muertos",
                      "muertos: %s%s" % (", ".join(muertos) or "ninguno",
                                         "; pendientes: " + ", ".join(pendientes) if pendientes else ""),
                      esperado=len(jefes), observado=len(muertos))
        inf.comprobar(sec, not rec.intervenciones, "sin intervenciones del agente",
                      "; ".join("%s (%s)" % (i["orden"], i["motivo"]) for i in rec.intervenciones))
        graves = [a for a in resumen["anomalias"] if a["severidad"] == "grave"]
        por_tipo = {}
        for a in graves:
            por_tipo.setdefault(a["tipo"], []).append(a)
        for tipo, lista in sorted(por_tipo.items()):
            inf.anotar(sec, "FALLO", "anomalía grave: %s (%d)" % (tipo, len(lista)), lista[0]["texto"],
                       origen="cliente" if tipo == "protocolo" else "desarrollo")
        avisos = {}
        for a in resumen["anomalias"]:
            if a["severidad"] == "aviso":
                avisos.setdefault(a["tipo"], []).append(a)
        for tipo, lista in sorted(avisos.items()):
            inf.anotar(sec, "AVISO", "anomalía: %s (%d)" % (tipo, len(lista)), lista[0]["texto"])
        inf.comprobar(sec, not m.errores_lectura, "paquetes del mundo leídos sin error",
                      "; ".join(m.errores_lectura[:3]), origen="cliente")
