# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Comprueba la distancia mínima entre las plazas naturales de mod-treasure."""
import math
import re

from ..catalogo import caso
from . import PersonajeTemporal, sin_colores

PUNTO = re.compile(r"calidad \d+ plaza \d+: punto ([1-9]\d*) .*? pos ([-\d.]+) ([-\d.]+) ([-\d.]+)")


@caso(id="tesoros-distancia", titulo="SP03: distancia entre cofres naturales",
      descripcion="Comprueba en vivo que dos plazas asignadas de Elwynn están a 100 m o más.",
      etiquetas=("tesoros", "sp03", "distancia"),
      acciones=("conectar", "leer", "personaje", "gm"),
      duracion_max=90, protege=("SP03", "mod-treasure"), control="directo",
      en_todo=False,
      observa=("dos o más plazas con punto", "separación mínima de 100 m"),
      no_cubre=("zonas vecinas; se comprueban con la consulta global de plazas",))
def ejecutar(ctx):
    inf, sec = ctx.inf, "tesoros-distancia"
    m = ctx.nueva_sesion()
    with PersonajeTemporal(ctx, m, 1, 1, sec):
        ctx.anotar_servidor(m)
        m.comando("gm on", 1)
        m.comando("go xyz -9751.66 184.723 55.732 0", 3)
        gps = m.gps()
        if not inf.comprobar(sec, gps is not None and gps.get("mapa") == 0,
                             "personaje en Elwynn", str(gps), origen="entorno"):
            return
        m.bombear(12)
        estado = " ".join(map(sin_colores, m.comando("tesoro estado", 2)))
        puntos = [(int(g), float(x), float(y)) for g, x, y, _ in PUNTO.findall(estado)]
        if not inf.comprobar(sec, len(puntos) >= 2,
                             "dos o más cofres naturales con destino", str(puntos)):
            return
        menor = min(math.hypot(a[1] - b[1], a[2] - b[2])
                    for i, a in enumerate(puntos) for b in puntos[i + 1:])
        inf.comprobar(sec, menor >= 100.0, "ningún cofre natural a menos de 100 m",
                      "distancia mínima %.1f m entre %d cofres" % (menor, len(puntos)),
                      esperado=">= 100 m", observado=menor)
