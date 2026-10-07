# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Estado de los módulos propios por sus comandos de consulta (sólo lectura en el servidor).

Entra con un personaje temporal (hace falta para escribir comandos) y pregunta
a cada módulo propio activo por su estado. Un comando que no responde, que
responde «desactivado» o «Unknown command» indica que el módulo no está cargado
o no está activo en la VM. No cambia nada del servidor.
"""
from ..catalogo import caso
from . import PersonajeTemporal, sin_colores

# (módulo, comando, texto que debe aparecer, qué demuestra)
CONSULTAS = [
    ("mod-standby", "standby estado", r"standby|espera|inactiv", "el apagado por inactividad está cargado"),
    ("mod-queue-bots", "queuebots estado", r"queue|cola|bots", "queue-bots responde"),
    ("mod-world-bots", "wbots estado", r"world-bots|etapa|bots", "world-bots responde"),
    ("mod-world-bots", "bots estado", r"\d", "población por tramos"),
    ("mod-world-bots (pvp)", "wpvp estado", r"pvp|PvP|world|Guerra|puntos", "PvP del mundo responde"),
    ("mod-party-here", "grupo estado", r"compa|grupo|Sin", "party-here responde"),
    ("mod-home-guild", "hermandad estado", r"hermandad|guild|Hermandad", "home-guild responde"),
    ("mod-server-help", "ayuda version", r"\d", "la base de ayuda está cargada"),
    ("mod-server-help", "ayuda buscar grupo", r"grupo|\.grupo", "la búsqueda de ayuda encuentra artículos"),
    ("mod-update-notice", "actualizaciones", r"actualiz|al día|pendiente|Actualiz", "aviso de actualizaciones"),
    ("mod-dungeon-clear", "dc config", r"DungeonClear|dc|Dungeon", "dungeon-clear responde fuera de mazmorra"),
    ("mod-playerbots", "playerbots bot list", r"bot|Bot|\w", "playerbots responde"),
]
NO_EXISTE = ("Unknown command", "Comando desconocido", "There is no such command", "No existe")


@caso(id="modulos", titulo="Los módulos propios responden a sus consultas",
      descripcion=__doc__,
      etiquetas=("modulos", "lectura", "rapido", "personaje"), acciones=("conectar", "leer", "personaje"),
      duracion_max=240,
      protege=tuple(sorted({c[0] for c in CONSULTAS})), control="directo",
      observa=("respuesta de sistema de cada comando",),
      no_cubre=("el comportamiento de cada módulo (tiene su caso o su prueba manual en COBERTURA.md)",))
def ejecutar(ctx):
    inf = ctx.inf
    import re
    m = ctx.nueva_sesion()
    with PersonajeTemporal(ctx, m, 1, 1, "módulos"):
        ctx.anotar_servidor(m)
        respuestas = {}
        for modulo, orden, patron, que in CONSULTAS:
            texto = sin_colores(" ".join(m.comando(orden, 2)))
            respuestas[orden] = texto[:600]
            desconocido = any(n in texto for n in NO_EXISTE)
            desactivado = "desactivado" in texto.lower() and modulo != "mod-update-notice"
            ok = bool(texto) and not desconocido and re.search(patron, texto) is not None
            inf.comprobar(modulo, ok and not desactivado, "`.%s`: %s" % (orden, que),
                          texto[:160] or "sin respuesta", estado_si_no="FALLO" if (desconocido or not texto) else "AVISO")
        inf.datos["respuestas"] = respuestas
