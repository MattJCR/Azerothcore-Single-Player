# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Conmutador selfbot sobre el propio personaje, sin grupo ni mazmorra.

Caso corto de humo para CS01: `.playerbots bot self` enciende la IA sobre el
personaje del cliente (mismo GUID, misma sesión), el cliente sigue recibiendo
paquetes, no aparece otro personaje en el mundo por ello y el segundo
`.playerbots bot self` la apaga. Comprueba también que con la IA apagada el
logout funciona (PlayerbotsSelfBotAfk sólo lo rechaza en AFK con selfbot).
"""
import time

from ..catalogo import caso
from .bots import poblacion
from .mazmorra import selfbot
from . import PersonajeTemporal


@caso(id="selfbot", titulo="`.playerbots bot self` activa y desactiva la IA del propio personaje",
      descripcion=__doc__,
      etiquetas=("selfbot", "playerbots", "rapido", "personaje"),
      acciones=("conectar", "leer", "personaje", "selfbot"), duracion_max=180,
      protege=("CS01", "mod-playerbots (SelfBotLevel)"), control="delegado",
      observa=("respuesta de `.playerbots bot self`", "`.server info`", "paquetes durante la delegación"),
      no_cubre=("combate y movimiento delegados (caso mazmorra)",))
def ejecutar(ctx):
    inf, sec = ctx.inf, "selfbot"
    m = ctx.nueva_sesion()
    estado = {"activo": False}

    def apagar():
        if estado["activo"]:
            selfbot(m, False)

    with PersonajeTemporal(ctx, m, 1, 8, sec, antes_de_salir=apagar) as pj:
        ctx.anotar_servidor(m)
        antes = poblacion(m)
        final, textos = selfbot(m, True)
        estado["activo"] = bool(final)
        if not inf.comprobar(sec, final is True, "se activa sobre el propio personaje", " / ".join(textos)):
            return
        guid = m.guid
        paquetes = len(m.errores_lectura)
        inicio = time.time()
        m.bombear(20)
        durante = poblacion(m)
        inf.comprobar(sec, m.guid == guid and durante["en_mundo"] >= 1,
                      "la sesión sigue viva y es el mismo GUID durante la delegación",
                      "guid %d, %.0f s" % (guid, time.time() - inicio))
        inf.comprobar(sec, durante["conectados"] == antes["conectados"],
                      "no aparece otra sesión por activar selfbot",
                      "conectados %d → %d" % (antes["conectados"], durante["conectados"]),
                      esperado=antes["conectados"], observado=durante["conectados"])
        final, textos = selfbot(m, False)
        estado["activo"] = bool(final)
        inf.comprobar(sec, final is False, "se desactiva con la segunda orden", " / ".join(textos))
        inf.comprobar(sec, len(m.errores_lectura) == paquetes, "paquetes leídos sin error durante la delegación",
                      "; ".join(m.errores_lectura[paquetes:paquetes + 3]), origen="cliente")
        inf.datos["selfbot"] = {"nombre": pj.nombre, "guid": guid, "antes": antes, "durante": durante}
