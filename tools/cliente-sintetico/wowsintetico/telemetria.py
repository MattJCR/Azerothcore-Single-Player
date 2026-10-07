# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Telemetría y detectores de anomalías de una ejecución con grupo.

Escucha los eventos de la sesión (mundo.Mundo.emitir), los vuelca a una línea
de tiempo JSONL y mantiene el estado del grupo, de dungeon-clear (mensajes de
addon `DC\\t...` que su tanque manda a los jugadores reales y a los selfbots del
grupo, DcStatusPublisher.cpp) y de los jefes. Las muestras `.gps` dan al
servidor la última palabra sobre mapa, instancia, suelo y líquido.

Cada anomalía lleva tipo, severidad (info / aviso / grave), quién, dónde y la
evidencia. Nada de esto decide por sí solo que un caso falla: el escenario
compara el resumen con sus criterios.

Limitaciones: sólo se ve lo que llega al cliente (paquetes del grupo y del
rango de visión) más lo que devuelve `.gps`. Una recta entre dos muestras no
prueba haber atravesado una pared; no se infiere.
"""
import json
import math
import re
import time

INVALIDO = -100000.0            # INVALID_HEIGHT del core
LIQUIDO_BAJO_AGUA = 0x08        # LIQUID_MAP_UNDER_WATER
LIQUIDO_EN_AGUA = 0x04
ESTADOS_COMBATE = {"fighting_boss", "fighting_trash", "pulling", "clearing_room"}
ESTADOS_ESPERA_LEGITIMA = ESTADOS_COMBATE | {"looting", "resting", "paused"}
ORDEN_SEVERIDAD = {"info": 0, "aviso": 1, "grave": 2}


def _d2(a, b):
    return math.hypot(a["x"] - b["x"], a["y"] - b["y"])


class Telemetria:
    def __init__(self, m, ruta_jsonl, mapa_objetivo=None, eco=print, umbrales=None, zona_objetivo=None):
        self.m, self.eco, self.mapa_objetivo, self.zona_objetivo = m, eco, mapa_objetivo, zona_objetivo
        self.t0 = time.time()
        self.fichero = open(ruta_jsonl, "w", encoding="utf-8") if ruta_jsonl else None
        self.u = {"sin_progreso_s": 240, "separacion_m": 70, "separacion_s": 60, "combate_largo_s": 300,
                  "salto_m": 45, "bajo_piso_m": 4.0, "caida_z_m": 25}
        self.u.update(umbrales or {})
        self.nombres = {}            # guid -> nombre
        self.miembros = {}           # guid -> estado vivo del miembro
        self.tanque = None
        self.dc = {"activo": None, "jefe": None, "jefe_entrada": 0, "estado": None, "detalle": "",
                   "atasco": "", "saltados": 0, "t_estado": self.t0, "visto_activo": False}
        self.estados_dc = {}         # estado -> segundos acumulados
        self.jefes = {}              # entrada -> dict
        self.chat_dc = []
        self.anomalias = []
        self._claves = {}            # dedupe: clave -> t
        self.muertes = []
        self.muertes_unidad = 0
        self.movimientos_propios = 0
        self.areatriggers = []
        self.teletransportes = []
        self.ultimo_progreso = self.t0
        self.ultimo_progreso_pos = None
        self.eventos_n = 0
        self._sumergido_quieto = None
        m.oyentes.append(self._evento)

    # ── utilidades ──────────────────────────────────────────────────────────
    def rel(self, t=None):
        return round((t or time.time()) - self.t0, 1)

    def escribir(self, _evento, **datos):
        self.eventos_n += 1
        if self.fichero:
            linea = {"t": self.rel(), "evento": _evento}
            linea.update(datos)
            self.fichero.write(json.dumps(linea, ensure_ascii=False, default=str) + "\n")
            self.fichero.flush()

    def nombre(self, guid):
        return self.nombres.get(guid) or ("yo" if guid == self.m.guid else "0x%X" % guid)

    def anomalia(self, tipo, severidad, texto, clave=None, ventana=120, **datos):
        clave = clave or tipo
        ahora = time.time()
        if clave in self._claves and ahora - self._claves[clave] < ventana:
            return
        self._claves[clave] = ahora
        a = {"t": self.rel(ahora), "tipo": tipo, "severidad": severidad, "texto": texto, **datos}
        self.anomalias.append(a)
        self.escribir("anomalia", **a)
        self.eco("  ! [%s] %s: %s" % (severidad, tipo, texto))

    def fijar_grupo(self, miembros: dict, tanque=None):
        """miembros: guid -> nombre (sin incluirnos)."""
        self.nombres.update(miembros)
        for g, n in miembros.items():
            self.miembros.setdefault(g, {"nombre": n, "estado": "vivo", "pos": None, "t_pos": 0,
                                         "lejos_desde": None, "muertes": 0, "gps": None})
        self.tanque = tanque

    def cerrar(self):
        if self._evento in self.m.oyentes:
            self.m.oyentes.remove(self._evento)
        if self.fichero:
            self.fichero.close()
            self.fichero = None

    # ── eventos de la sesión ────────────────────────────────────────────────
    def _evento(self, t, tipo, d):
        if tipo == "addon" and d.get("prefijo") == "DC":
            self._dc(d.get("emisor"), d.get("campos") or [])
            return
        if tipo == "stats_miembro":
            self._stats(d)
            return
        if tipo == "movimiento_propio":
            self.movimientos_propios += 1
            if self.movimientos_propios <= 3 or self.movimientos_propios % 50 == 0:
                self.escribir(tipo, **d)
            return
        if tipo == "movimiento_miembro":
            return
        self.escribir(tipo, **d)
        if tipo == "estado_miembro":
            self._estado_miembro(d)
        elif tipo == "vida_propia":
            if not d["vivo"]:
                self._muerte(self.m.guid, "yo")
        elif tipo == "temporizador":
            if d["tipo"] == "respiración" and d.get("escala", 0) < 0:
                self.anomalia("respiracion", "aviso", "el jugador está bajo el agua y pierde respiración "
                              "(%.0f s de %.0f)" % (d["valor"] / 1000, d["max"] / 1000), pos=self.m.posicion_actual())
            elif d["tipo"] == "fatiga" and d.get("escala", 0) < 0:
                self.anomalia("fatiga", "grave", "el jugador está en aguas profundas (fatiga)",
                              pos=self.m.posicion_actual())
        elif tipo == "dano_ambiental":
            grave = d["tipo"] in ("ahogamiento", "caída al vacío", "agotamiento")
            self.anomalia("dano_" + str(d["tipo"]).replace(" ", "_"), "grave" if grave else "aviso",
                          "%s recibe daño por %s (%d)" % (self.nombre(d["guid"]), d["tipo"], d["cantidad"]),
                          clave="dano-%s-%s" % (d["guid"], d["tipo"]), ventana=30, quien=self.nombre(d["guid"]))
        elif tipo == "teletransporte":
            self._teletransporte(d)
        elif tipo == "muerte_unidad":
            self.muertes_unidad += 1
            if d.get("entrada") in self.jefes:
                self._jefe_muerto(d["entrada"], "registro de muerte")
        elif tipo == "areatrigger":
            self.areatriggers.append((self.rel(t), d["id"], d["mapa"], d["automatico"]))
        elif tipo in ("cambio_mapa_abortado", "mensaje_trigger"):
            self.anomalia(tipo, "aviso", str(d), clave="%s-%s" % (tipo, d))
        elif tipo == "error_lectura":
            self.anomalia("protocolo", "aviso", "paquete 0x%03X ilegible: %s" % (d["opcode"], d["error"]),
                          clave="proto-%s" % d["opcode"], origen="cliente")
        elif tipo == "grupo_disuelto":
            self.anomalia("grupo_disuelto", "grave", "el grupo se ha disuelto durante la prueba")

    def _stats(self, d):
        g = d["guid"]
        mi = self.miembros.get(g)
        if mi is None:
            return
        zona_antes = mi.get("zona")
        if "vida" in d:
            mi["vida"] = d["vida"]
        if "vida_max" in d:
            mi["vida_max"] = d["vida_max"]
        if "zona" in d:
            mi["zona"] = d["zona"]
        if "x" in d:
            nueva = {"x": d["x"], "y": d["y"]}
            ahora = time.time()
            if mi["pos"] and ahora - mi["t_pos"] <= 3 and _d2(mi["pos"], nueva) > self.u["salto_m"] \
                    and mi["estado"] == "vivo" and (zona_antes is None or mi.get("zona") == zona_antes):
                self.anomalia("salto_posicion", "aviso", "%s aparece %.0f m más allá en %.1f s (teletransporte "
                              "o rescate)" % (mi["nombre"], _d2(mi["pos"], nueva), ahora - mi["t_pos"]),
                              clave="salto-%s" % g, ventana=20, quien=mi["nombre"], desde=mi["pos"], hasta=nueva)
            mi["pos"], mi["t_pos"] = nueva, ahora

    def _estado_miembro(self, d):
        mi = self.miembros.get(d["guid"])
        if mi is None:
            return
        antes, mi["estado"] = mi["estado"], d["estado"]
        if d["estado"] == "muerto" and antes != "muerto":
            self._muerte(d["guid"], mi["nombre"])
        if d["estado"] == "desconectado":
            self.anomalia("desconexion", "grave", "%s se ha desconectado" % mi["nombre"], clave="desc-%s" % d["guid"])
        if d["estado"] == "fantasma":
            self.anomalia("liberado", "aviso", "%s ha liberado el espíritu (fantasma)" % mi["nombre"],
                          clave="fant-%s" % d["guid"])

    def _muerte(self, guid, nombre):
        mi = self.miembros.get(guid)
        if mi is not None:
            mi["muertes"] += 1
        pos = self.m.posicion_actual() if guid == self.m.guid else (mi or {}).get("pos")
        self.muertes.append({"t": self.rel(), "quien": nombre, "pos": pos, "jefe": self.dc["jefe"],
                             "estado_dc": self.dc["estado"]})
        self.anomalia("muerte", "aviso", "%s muere (dc: %s, hacia %s)" % (nombre, self.dc["estado"], self.dc["jefe"]),
                      clave="muerte-%s-%d" % (guid, len(self.muertes)), quien=nombre, pos=pos)
        vivos = [g for g, mi in self.miembros.items() if mi["estado"] == "vivo"]
        yo_vivo = self.m.valor_propio(0x18, 1) > 0
        if not vivos and not yo_vivo:
            self.anomalia("wipe", "grave", "todo el grupo ha muerto", clave="wipe-%d" % len(self.muertes))

    def _teletransporte(self, d):
        self.teletransportes.append({"t": self.rel(), **d})
        if d.get("provocado"):
            return                                      # lo pidió el agente (.go, .tele): no es anomalía
        if d.get("hechizo"):
            return                                      # lo causó un hechizo propio (p. ej. Traslación, 1953)
        desde, hasta = d.get("desde"), d.get("hasta")
        dist = _d2(desde, hasta) if desde and hasta and not d.get("lejano") else None
        if d.get("lejano") and self.mapa_objetivo is not None and d.get("mapa_antes") == self.mapa_objetivo \
                and d.get("mapa") != self.mapa_objetivo:
            self.anomalia("salida_de_instancia", "grave", "el jugador sale del mapa %d al %d" % (
                self.mapa_objetivo, d.get("mapa")), desde=desde, hasta=hasta)
        elif dist is not None and dist > 5:
            self.anomalia("teletransporte", "aviso", "el servidor teletransporta al jugador %.0f m dentro del mapa "
                          "(rescate/recolocación)" % dist, clave="tp-%d" % len(self.teletransportes),
                          desde=desde, hasta=hasta)

    # ── dungeon-clear ───────────────────────────────────────────────────────
    def _fin_normal_dc(self, texto):
        return ("all remaining bosses are marked dead or skipped" in texto.lower()
                and bool(self.jefes) and all(j["estado"] == "dead" for j in self.jefes.values()))

    def _dc(self, emisor, campos):
        if not campos:
            return
        clase = campos[0]
        if clase == "STATUS" and len(campos) >= 8:
            activo = campos[1] == "1"
            entrada = int(campos[2] or 0)
            estado, detalle, atasco = campos[6], campos[7], campos[4]
            if emisor and emisor in self.miembros:
                self.tanque = self.tanque or emisor
            self._cambio_estado_dc(estado)
            cambio = (activo, entrada, estado, atasco) != (self.dc["activo"], self.dc["jefe_entrada"],
                                                           self.dc["estado"], self.dc["atasco"])
            if activo and not self.dc["visto_activo"]:
                self.dc["visto_activo"] = True
                self.dc["t_activo"] = self.rel()
            if self.dc["activo"] and not activo:
                vivos = [e for e, j in self.jefes.items() if j["estado"] not in ("dead", "skipped")]
                self.anomalia("dc_apagado", "info" if not vivos else "grave",
                              "dungeon-clear se desactiva%s" % (
                                  " con jefes pendientes: %s" % ", ".join(self.jefes[e]["nombre"] for e in vivos)
                                  if vivos else ""), clave="dcoff-%d" % len(self.anomalias))
            if entrada != self.dc["jefe_entrada"]:
                self._progreso("objetivo %s" % campos[3])
            self.dc.update(activo=activo, jefe_entrada=entrada, jefe=campos[3], estado=estado, detalle=detalle,
                           atasco=atasco, saltados=int(campos[5] or 0))
            if cambio:
                self.escribir("dc_estado", activo=activo, jefe=campos[3], estado=estado, detalle=detalle,
                              atasco=atasco)
            if atasco and not self._fin_normal_dc(atasco):
                self.anomalia("dc_atasco", "grave" if estado == "stalled" else "aviso",
                              "dungeon-clear atascado hacia %s: %s" % (campos[3], atasco),
                              clave="atasco-%s" % atasco, ventana=600, jefe=campos[3])
            if estado == "door_blocked":
                self.anomalia("dc_puerta", "aviso", "puerta cerrada en el camino a %s" % campos[3],
                              clave="puerta-%s" % entrada, ventana=600)
        elif clase == "BOSS" and len(campos) >= 5:
            try:
                entrada = int(campos[1])
            except ValueError:
                return
            j = self.jefes.setdefault(entrada, {"nombre": campos[3], "estado": None, "orden": campos[2]})
            if len(campos) >= 8:
                j["pos"] = [campos[5], campos[6], campos[7]]
            antes, j["estado"] = j["estado"], campos[4]
            if antes != j["estado"]:
                self.escribir("jefe", entrada=entrada, nombre=campos[3], estado=j["estado"], antes=antes)
                if j["estado"] == "dead":
                    self._jefe_muerto(entrada, "dungeon-clear")
                elif j["estado"] == "skipped":
                    self.anomalia("jefe_saltado", "grave", "dungeon-clear salta a %s" % campos[3],
                                  clave="skip-%d" % entrada, ventana=10 ** 6, jefe=campos[3])
                elif j["estado"] == "missing":
                    self.anomalia("jefe_desaparecido", "grave", "%s estaba vivo y ya no está (sin matarlo)"
                                  % campos[3], clave="miss-%d" % entrada, ventana=10 ** 6, jefe=campos[3])
        elif clase == "CHAT" and len(campos) >= 2:
            texto = campos[1]
            self.chat_dc.append((self.rel(), texto))
            self.escribir("dc_chat", texto=texto)
            bajo = re.sub(r"skipped: \d+\.?", "", texto.lower())
            if not self._fin_normal_dc(bajo) and any(p in bajo for p in (
                    "repathing", "nudging", "stuck", "unreachable", "can't", "cannot",
                    "no path", "giving up", "skipped")):
                self.anomalia("dc_aviso", "aviso", "dungeon-clear: %s" % texto, clave="dcchat-%s" % texto[:40],
                              ventana=300)

    def _jefe_muerto(self, entrada, fuente):
        j = self.jefes.get(entrada)
        if j is None or j.get("t_muerte") is not None:
            return
        j["t_muerte"] = self.rel()
        j["estado"] = "dead"
        self._progreso("jefe %s" % j["nombre"])
        self.eco("  ✓ %s muerto (%s) a los %.0f s" % (j["nombre"], fuente, j["t_muerte"]))

    def _progreso(self, motivo):
        self.ultimo_progreso = time.time()
        self.escribir("progreso", motivo=motivo)

    def _cambio_estado_dc(self, estado):
        ahora = time.time()
        if self.dc["estado"] is not None:
            self.estados_dc[self.dc["estado"]] = self.estados_dc.get(self.dc["estado"], 0) + (ahora - self.dc["t_estado"])
        if estado != self.dc["estado"]:
            self.dc["t_estado_inicio"] = ahora
        self.dc["t_estado"] = ahora

    # ── muestras de posición y detectores periódicos ───────────────────────
    def muestra_miembro(self, guid, pos3d):
        """Posición de un compañero: x/y/zona de SMSG_PARTY_MEMBER_STATS_FULL y, si está a la
        vista, la 3D de su trayectoria del servidor. Sin suelo: `.gps` sólo vale para uno mismo."""
        mi = self.miembros.get(guid)
        if mi is None:
            return
        e = self.m.miembros.get(guid, {})
        muestra = {"quien": mi["nombre"], "zona": e.get("zona"), "estado": mi["estado"], "vida": e.get("vida"),
                   "vida_max": e.get("vida_max")}
        fresca = "x" in e and time.time() - e.get("t", 0) < 5
        # La x/y de PARTY_MEMBER_STATS_FULL (pedida en cada muestra) manda; la del objeto puede
        # ser vieja (creado y sin trayectorias después). La z sólo se toma si casa con ella.
        if pos3d and fresca and math.hypot(pos3d["x"] - e["x"], pos3d["y"] - e["y"]) > 10:
            pos3d = None
        if pos3d:
            muestra.update(x=round(pos3d["x"], 1), y=round(pos3d["y"], 1), z=round(pos3d["z"], 1), vista=True)
            anterior = mi.get("pos3d")
            if anterior and anterior["z"] - pos3d["z"] > self.u["caida_z_m"]:
                # Sin .gps no se sabe si nada o cae: bucear también baja. La caída real la
                # delata SMSG_ENVIRONMENTAL_DAMAGE_LOG (dano_caída).
                self.anomalia("descenso", "info", "%s baja %.0f m de altura entre dos muestras" % (
                    mi["nombre"], anterior["z"] - pos3d["z"]), clave="caida-%s" % guid, ventana=60,
                    quien=mi["nombre"], desde=anterior, hasta=pos3d)
            mi["pos3d"] = dict(pos3d)
            mi["pos"], mi["t_pos"] = {"x": pos3d["x"], "y": pos3d["y"]}, time.time()
        elif fresca:
            muestra.update(x=e["x"], y=e["y"], vista=False)
            mi["pos"], mi["t_pos"] = {"x": e["x"], "y": e["y"]}, time.time()
        if self.zona_objetivo and e.get("zona") and e["zona"] != self.zona_objetivo and mi["estado"] == "vivo":
            self.anomalia("fuera_de_la_mazmorra", "grave", "%s está en la zona %s, no en la mazmorra" % (
                mi["nombre"], e["zona"]), clave="zona-%s-%s" % (guid, e["zona"]), ventana=600, quien=mi["nombre"])
        self.escribir("muestra", **muestra)

    def muestra_gps(self, guid, gps):
        if not gps:
            return
        quien = self.nombre(guid)
        self.escribir("gps", quien=quien, **gps)
        mi = self.miembros.get(guid)
        anterior = mi.get("gps") if mi is not None else getattr(self, "_gps_propio", None)
        if mi is not None:
            mi["gps"] = gps
            mi["pos"], mi["t_pos"] = {"x": gps["x"], "y": gps["y"]}, time.time()
        else:
            self._gps_propio = gps
        if self.mapa_objetivo is not None and gps["mapa"] != self.mapa_objetivo:
            self.anomalia("fuera_del_mapa", "grave", "%s está en el mapa %d, no en el %d" % (
                quien, gps["mapa"], self.mapa_objetivo), clave="mapa-%s-%d" % (guid, gps["mapa"]), ventana=600,
                quien=quien)
        suelo, piso = gps.get("suelo", INVALIDO), gps.get("piso", INVALIDO)
        liq = gps.get("liquido") or {}
        bajo_agua = bool(liq.get("estado", 0) & LIQUIDO_BAJO_AGUA)
        if piso <= INVALIDO + 1 and suelo <= INVALIDO + 1:
            self.anomalia("sin_suelo", "grave", "%s está donde el mapa no tiene altura (fuera de la geometría) "
                          "en %.1f, %.1f, %.1f" % (quien, gps["x"], gps["y"], gps["z"]),
                          clave="sinsuelo-%s" % guid, ventana=120, quien=quien, gps=gps)
        elif piso > INVALIDO + 1 and gps["z"] < piso - self.u["bajo_piso_m"] and not bajo_agua:
            self.anomalia("bajo_el_suelo", "grave", "%s está %.1f m por debajo del suelo del servidor "
                          "(z %.1f, suelo %.1f) en %.1f, %.1f" % (quien, piso - gps["z"], gps["z"], piso,
                                                                  gps["x"], gps["y"]),
                          clave="bajo-%s" % guid, ventana=120, quien=quien, gps=gps)
        if bajo_agua and mi is None:
            quieto = self._sumergido_quieto
            if quieto and math.hypot(gps["x"] - quieto["x"], gps["y"] - quieto["y"]) < 3:
                if time.time() - quieto["t"] > 45:
                    self.anomalia("quieto_bajo_el_agua", "aviso", "el jugador lleva %.0f s quieto y sumergido en "
                                  "%.0f, %.0f, %.0f (dc: %s)" % (time.time() - quieto["t"], gps["x"], gps["y"],
                                                                gps["z"], self.dc["estado"]),
                                  clave="quieto-agua-%d" % int(quieto["t"]), ventana=10 ** 6, gps=gps)
            else:
                self._sumergido_quieto = {"x": gps["x"], "y": gps["y"], "t": time.time()}
        elif mi is None:
            self._sumergido_quieto = None
        if bajo_agua:
            self.anomalia("bajo_el_agua", "info", "%s va sumergido en %.0f, %.0f, %.0f" % (
                quien, gps["x"], gps["y"], gps["z"]), clave="agua-%s" % guid, ventana=300, quien=quien)
        if anterior and anterior.get("mapa") == gps["mapa"] and anterior["z"] - gps["z"] > self.u["caida_z_m"]:
            self.anomalia("caida", "aviso", "%s baja %.0f m de altura entre dos muestras" % (
                quien, anterior["z"] - gps["z"]), clave="caida-%s" % guid, ventana=60, quien=quien,
                desde=anterior, hasta=gps)

    def revisar(self):
        """Detectores que dependen del tiempo. Llamar cada pocos segundos."""
        ahora = time.time()
        estado = self.dc["estado"]
        if self.dc["activo"]:
            en_estado = ahora - self.dc.get("t_estado_inicio", ahora)
            if estado in ESTADOS_COMBATE and en_estado > self.u["combate_largo_s"]:
                self.anomalia("combate_largo", "aviso", "%.0f s seguidos en %s (%s)" % (
                    en_estado, estado, self.dc["detalle"]), clave="combate-%d" % int(self.dc.get("t_estado_inicio", 0)),
                    ventana=10 ** 6)
            if estado == "recovering" and en_estado > 30:
                self.anomalia("dc_recuperando", "aviso", "el tanque lleva %.0f s atascado replanificando hacia %s" % (
                    en_estado, self.dc["jefe"]), clave="recov-%d" % int(self.dc.get("t_estado_inicio", 0)),
                    ventana=10 ** 6)
            tanque = self.miembros.get(self.tanque) if self.tanque else None
            pos_t = tanque["pos"] if tanque else None
            if pos_t:
                if self.ultimo_progreso_pos is None or _d2(self.ultimo_progreso_pos, pos_t) > 20:
                    self.ultimo_progreso_pos = dict(pos_t)
                    self.ultimo_progreso = max(self.ultimo_progreso, ahora)
            quieto = ahora - self.ultimo_progreso
            if quieto > self.u["sin_progreso_s"] and estado not in ESTADOS_ESPERA_LEGITIMA:
                self.anomalia("sin_progreso", "grave", "%.0f s sin matar jefe ni avanzar 20 m (dc: %s, %s)" % (
                    quieto, estado, self.dc["detalle"] or self.dc["atasco"]),
                    clave="quieto-%d" % int(self.ultimo_progreso), ventana=10 ** 6, pos_tanque=pos_t)
            if pos_t:
                for g, mi in self.miembros.items():
                    if g == self.tanque or mi["estado"] != "vivo" or not mi["pos"]:
                        continue
                    d = _d2(mi["pos"], pos_t)
                    if d > self.u["separacion_m"]:
                        mi["lejos_desde"] = mi["lejos_desde"] or ahora
                        if ahora - mi["lejos_desde"] > self.u["separacion_s"]:
                            self.anomalia("separado", "aviso", "%s lleva %.0f s a %.0f m del tanque" % (
                                mi["nombre"], ahora - mi["lejos_desde"], d), clave="sep-%s-%d" % (g, int(mi["lejos_desde"])),
                                ventana=10 ** 6, quien=mi["nombre"])
                    else:
                        mi["lejos_desde"] = None

    # ── resumen ─────────────────────────────────────────────────────────────
    def resumen(self) -> dict:
        self._cambio_estado_dc(self.dc["estado"])
        por_tipo = {}
        for a in self.anomalias:
            por_tipo[a["tipo"]] = por_tipo.get(a["tipo"], 0) + 1
        return {
            "duracion_s": self.rel(),
            "dc": {k: v for k, v in self.dc.items() if not k.startswith("t_")},
            "segundos_por_estado_dc": {k: round(v) for k, v in sorted(self.estados_dc.items(), key=lambda x: -x[1])},
            "jefes": {str(e): {k: v for k, v in j.items()} for e, j in self.jefes.items()},
            "muertes": self.muertes, "muertes_de_unidades_vistas": self.muertes_unidad,
            "anomalias_por_tipo": por_tipo,
            "anomalias": self.anomalias,
            "areatriggers_enviados": self.areatriggers,
            "teletransportes": self.teletransportes,
            "movimientos_propios_del_servidor": self.movimientos_propios,
            "forzados_sin_ack": dict(self.m.forzados_sin_ack),
            "errores_lectura": list(self.m.errores_lectura[:20]),
            "eventos_registrados": self.eventos_n,
        }
