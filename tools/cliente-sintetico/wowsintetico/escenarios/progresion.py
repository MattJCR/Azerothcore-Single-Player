# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Escenario de progresión (mod-individual-progression + Cronista de las Eras).

Personaje nuevo en etapa 0; `.ip get` lo confirma. Va al Cronista (NPC
600200, mod-progression-skip; aparición de Gadgetzan), recorre su menú como un
jugador, confirma «Saltar Vanilla» y comprueba que la etapa pasa a 8 y que el
menú ofrece ya el salto siguiente. Después prueba la vía de IP sin Cronista
(`.ip set`), que es la que usan el avance normal y los GM. El personaje se
borra al final.
"""
import re

from .. import actualizaciones as upd
from ..catalogo import caso
from . import PersonajeTemporal, sin_colores

RAZA, CLASE = 1, 1
CRONISTA = 600200
# Aparición de Gadgetzan (mod-progression-skip, guid 8000110): la de Vanilla.
# `.go creature id` elige Dalaran, e IP bloquea ese salto en la etapa 0
# ("Progression Level Required = 13"), igual que se lo bloquearía a un jugador.
CRONISTA_GADGETZAN = 8000110
SKIP_VANILLA = 8


def etapa_ip(m, nombre) -> int:
    texto = sin_colores(" ".join(m.comando_hasta("ip get %s" % nombre, r"Progression Level", 4)))
    cifra = re.search(r"Progression Level for \S+ = (\d+)", texto)
    return int(cifra.group(1)) if cifra else -1


def _opcion(opciones, empieza):
    return next((o for o in opciones if o["texto"].startswith(empieza)), None)


@caso(id="progresion", titulo="Individual Progression y Cronista de las Eras",
      descripcion="""
Personaje nuevo en etapa 0 de IP; recorre el menú del Cronista (600200) en
Gadgetzan como un jugador, confirma «Saltar Vanilla» (etapa 8) y comprueba que
el menú pasa a ofrecer «Saltar Terrallende». El salto sólo abre contenido:
comprueba que no modifica nivel, dinero, equipo visible ni objetos de la
mochila (M38). Después `.ip set` avanza sin el Cronista.

Logros, reputación y attunements (declarados también en M38) se quedan fuera:
el cliente sintético no interpreta `SMSG_INITIALIZE_FACTIONS` ni
`SMSG_ALL_ACHIEVEMENT_DATA` (no hay ningún otro caso que los use, y no existe
un comando GM de sólo lectura para consultarlos), y no hay un attunement de
referencia conocido con el que comparar antes/después sin inventar uno frágil.
Escribir esos analizadores de protocolo sólo para esta comprobación sería
desproporcionado frente a lo que hace el propio módulo: el Cronista no toca
ninguna de las tres tablas (no hay SQL ni C++ que las mencione en
mod-progression-skip), así que la ausencia de cambio es la que cabría esperar
por diseño, aunque esta prueba no la mida directamente.
""",
      etiquetas=("progresion", "ip", "npc", "personaje"),
      acciones=("conectar", "leer", "personaje", "gm"), duracion_max=240,
      protege=("mod-individual-progression", "mod-progression-skip", "PLAN M38"), control="directo",
      observa=("`.ip get`", "SMSG_GOSSIP_MESSAGE del Cronista",
               "nivel, dinero, equipo visible y objetos de la mochila (PLAYER_FIELD_PACK_SLOT_1)"),
      no_cubre=("logros, reputación y attunements: sin analizador de protocolo para "
                "SMSG_INITIALIZE_FACTIONS/SMSG_ALL_ACHIEVEMENT_DATA ni comando GM de sólo lectura "
                "(ver descripción); el módulo no toca esas tablas por diseño, pero no está medido aquí",
                "contenidos de bolsas EQUIPADAS aparte de la mochila", "texto del menú en Wow.exe"))
def ejecutar(ctx):
    inf, sec = ctx.inf, "progresión"
    m = ctx.nueva_sesion()
    with PersonajeTemporal(ctx, m, RAZA, CLASE, sec) as pj:
        ctx.anotar_servidor(m)
        etapa = etapa_ip(m, pj.nombre)
        inf.comprobar(sec, etapa == 0, "personaje nuevo en la etapa 0 de IP", "etapa %d" % etapa,
                      esperado=0, observado=etapa)

        m.comando("go creature %d" % CRONISTA_GADGETZAN, 6)
        cronista = next((c for c in m.criaturas() if c["entrada"] == CRONISTA), None)
        if not inf.comprobar(sec, cronista is not None, "el Cronista de las Eras está colocado y a la vista",
                             "mapa %s" % m.mapa):
            return
        m.ir_a(cronista["pos"])
        menu = m.conversar(cronista["guid"])
        if not inf.comprobar(sec, menu is not None, "el Cronista abre su menú"):
            return
        menu_id, opciones = menu
        inf.datos["cronista_menu"] = [o["texto"] for o in opciones]
        saltar = _opcion(opciones, "Saltar Vanilla")
        if not inf.comprobar(sec, saltar is not None, "ofrece «Saltar Vanilla» en la etapa 0",
                             "%d opciones" % len(opciones)):
            return
        confirmacion = m.elegir(cronista["guid"], menu_id, saltar["id"])
        si = _opcion(confirmacion[1], "Si: saltar Vanilla") if confirmacion else None
        if not inf.comprobar(sec, si is not None, "pide confirmación antes de un salto permanente"):
            return
        antes = {"nivel": m.valor_propio(upd.UNIT_FIELD_LEVEL),
                 "dinero": m.valor_propio(upd.PLAYER_FIELD_COINAGE),
                 "equipo": m.equipo_visible(),
                 "bolsas": m.objetos_bolsas()}
        m.elegir(cronista["guid"], confirmacion[0], si["id"], timeout=3)
        m.bombear(2)
        etapa = etapa_ip(m, pj.nombre)
        inf.comprobar(sec, etapa == SKIP_VANILLA, "tras confirmar, la etapa pasa a %d" % SKIP_VANILLA,
                      "etapa %d" % etapa, esperado=SKIP_VANILLA, observado=etapa)
        despues = {"nivel": m.valor_propio(upd.UNIT_FIELD_LEVEL),
                   "dinero": m.valor_propio(upd.PLAYER_FIELD_COINAGE),
                   "equipo": m.equipo_visible(),
                   "bolsas": m.objetos_bolsas()}
        inf.comprobar(sec, despues == antes,
                      "el salto no entrega nivel, dinero, equipo visible ni objetos en la mochila",
                      "antes: %s; después: %s" % (antes, despues),
                      esperado=antes, observado=despues)
        menu = m.conversar(cronista["guid"])
        siguiente = _opcion(menu[1], "Saltar Terrallende") if menu else None
        inf.comprobar(sec, siguiente is not None and _opcion(menu[1], "Saltar Vanilla") is None,
                      "el menú ya ofrece «Saltar Terrallende» y no «Saltar Vanilla»")

        m.comando("ip set %s %d" % (pj.nombre, SKIP_VANILLA + 1), 2)
        etapa = etapa_ip(m, pj.nombre)
        inf.comprobar(sec, etapa == SKIP_VANILLA + 1, "IP avanza también sin el Cronista (`.ip set`)",
                      "etapa %d" % etapa, esperado=SKIP_VANILLA + 1, observado=etapa)
        inf.comprobar(sec, not m.errores_lectura, "paquetes del mundo leídos sin error",
                      "; ".join(m.errores_lectura[:3]), origen="cliente")
