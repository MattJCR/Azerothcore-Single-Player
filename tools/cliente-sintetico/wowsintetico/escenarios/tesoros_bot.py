# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Verifica con un bot real el veto de saqueo de los cofres SP03."""
import struct
import time
import math
import re

from .. import actualizaciones as upd
from ..catalogo import caso
from ..mundo import SMSG_LOOT_RESPONSE, _leer_respuesta_loot
from . import PersonajeTemporal, sin_colores
from .profesiones import _paquete_hechizo


@caso(id="tesoros-bot", titulo="SP03: un bot no puede saquear el cofre",
      descripcion="Un compañero bot intenta usar un cofre y el humano lo abre después con su botín intacto.",
      etiquetas=("tesoros", "sp03", "bots"),
      acciones=("conectar", "leer", "personaje", "gm", "grupo"),
      duracion_max=240, protege=("SP03", "mod-treasure"), control="directo",
      en_todo=False,
      observa=("intento de uso por bot real", "botín intacto para el humano", "limpieza"),
      no_cubre=("intentos automáticos de saqueo de bots ajenos al grupo",))
def ejecutar(ctx):
    inf, sec = ctx.inf, "tesoros-bot"
    m = ctx.nueva_sesion()
    with PersonajeTemporal(ctx, m, 1, 1, sec) as personaje:
        ctx.anotar_servidor(m)
        m.comando("gm on", 1)
        m.comando("go xyz -9751.66 184.723 55.732 0", 3)
        gps = m.gps()
        if not inf.comprobar(sec, gps is not None and gps.get("mapa") == 0,
                             "personaje en Elwynn", str(gps), origen="entorno"):
            return
        added_bot = None
        try:
            group_response = m.comando("grupo 2", 2)
            limite = time.monotonic() + 25
            bot = None
            while time.monotonic() < limite:
                m.bombear(2)
                candidatos = [x for x in (m.grupo or {}).get("miembros", [])
                              if x["guid"] != m.guid and x["guid"] in m.objetos]
                if candidatos:
                    bot = candidatos[0]
                    break
            fallback = []
            if bot is None:
                fallback = m.comando_hasta("playerbots bot addclass warrior",
                                           r"[Aa]dd class|failed|permission", timeout=10)
                limite = time.monotonic() + 60
                while time.monotonic() < limite:
                    m.bombear(2)
                    candidatos = [x for x in (m.grupo or {}).get("miembros", [])
                                  if x["guid"] != m.guid and x["guid"] in m.objetos]
                    if candidatos:
                        bot = candidatos[0]
                        added_bot = bot["nombre"]
                        break
            if not inf.comprobar(sec, bot is not None, "compañero bot real visible",
                                 "bot %s; grupo %s; addclass %s" % (bot, group_response, fallback),
                                 origen="entorno"):
                return
            cerca = False
            limite = time.monotonic() + 60
            while time.monotonic() < limite:
                obj = m.objetos.get(bot["guid"], {})
                pos = obj.get("pos", {})
                if pos and math.hypot(pos.get("x", 1e9) - m.pos["x"],
                                       pos.get("y", 1e9) - m.pos["y"]) <= 6:
                    cerca = True
                    break
                m.bombear(2)
            if not inf.comprobar(sec, cerca, "el bot llega a distancia de interacción",
                                 "posición %s; jugador %s" % (m.objetos.get(bot["guid"], {}).get("pos"), m.pos),
                                 origen="entorno"):
                return
            result = " ".join(map(sin_colores, m.comando_hasta(
                "tesoro prueba crear %s 1 12" % personaje.nombre,
                r"Cofre de prueba|No hay suelo", timeout=10)))
            if not inf.comprobar(sec, "Cofre de prueba" in result,
                                 "cofre de prueba creado", result):
                return
            low = int(re.search(r"Cofre de prueba (\d+)", result).group(1))
            m.bombear(2)
            chest = next((guid for guid, obj in m.objetos.items()
                          if obj.get("valores", {}).get(upd.OBJECT_FIELD_ENTRY) == 700037), None)
            if not inf.comprobar(sec, chest is not None, "cofre visible ante bot y humano",
                                 str(chest)):
                return
            before = len(m.chat)
            link = "|cFFFFFF00|Hfound:%d:700037:|h[Cofre de tesoro]|h|r" % chest
            m.susurrar(bot["nombre"], "u " + link)
            m.bombear(8)
            reply = [c.get("texto", "") for c in m.chat[before:]
                     if c.get("emisor") == bot["guid"]]
            estado = " ".join(map(sin_colores, m.comando("tesoro estado", 2)))
            veto = re.search(r"Ultimo veto a bot: interaccion GUID (\d+), botin GUID (\d+)", estado)
            denied = bool(veto and low in (int(veto.group(1)), int(veto.group(2))))
            if not inf.comprobar(sec, denied, "el servidor veta al bot en el GUID del cofre",
                                 "cofre %d; veto %s; chat %s" % (low, veto.groups() if veto else None,
                                                                  " / ".join(reply)[:200])):
                return
            m.enviar(0x12E, _paquete_hechizo(3365, chest))
            opcode, data = m.esperar({SMSG_LOOT_RESPONSE, 0x130, 0x133}, timeout=8)
            loot = _leer_respuesta_loot(data) if opcode == SMSG_LOOT_RESPONSE else {}
            item = struct.unpack_from("<I", data, 15)[0] if loot.get("abierto") and len(data) >= 19 else None
            inf.comprobar(sec, item == 2589, "botín intacto para el humano tras el intento del bot",
                          "objeto esperado 2589; recibido %s" % item)
            if loot.get("abierto"):
                m.liberar_loot(chest)
        finally:
            m.comando("tesoro prueba retirar %s" % personaje.nombre, 1)
            m.comando("grupo fuera", 2)
            if added_bot:
                m.comando("playerbots bot remove %s" % added_bot, 2)
