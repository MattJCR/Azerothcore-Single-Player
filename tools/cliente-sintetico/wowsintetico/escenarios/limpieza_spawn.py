# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Verifica el spawn tras la limpieza auditada del 26/09/2026; no borra NPC."""
import re

from ..catalogo import caso
from .. import actualizaciones as upd
from . import PersonajeTemporal, sin_colores

GUIDS = (8010604, 8011920, 8011921, 8011922, 8011923, 8011924,
         8011926, 8011928, 8011929, 8011931, 8011941)


def presentes(texto):
    return {g for g in GUIDS if re.search(r"(?<!\d)%d(?!\d)" % g, texto)}


@caso(id="limpieza-spawn-auditado", titulo="Restos de pruebas auditados en Northshire",
      descripcion="Comprueba ausencia de once GUID auditados y supervivencia de un humano recién creado. Nunca borra NPC.",
      etiquetas=("mantenimiento",), acciones=("conectar", "leer", "personaje", "gm"),
      duracion_max=240, en_todo=False, control="directo",
      observa=("npc near por GUID de base de datos", "vida del humano tras 15 segundos sin protección GM"),
      no_cubre=("otros NPC; registros persistentes o criaturas fuera de 100 yardas se verifican por SQL",))
def ejecutar(ctx):
    m = ctx.nueva_sesion()
    sec = "spawn-auditado"
    with PersonajeTemporal(ctx, m, 1, 1, sec):
        m.comando("gm off", 1)
        m.bombear(15)
        salud = m.valor_propio(upd.UNIT_FIELD_HEALTH)
        ctx.inf.comprobar(sec, salud > 0, "humano vivo tras 15 segundos en el spawn", str(salud), origen="entorno")
        m.comando("gm on", 1)
        m.comando("go xyz -8949.95 -132.493 83.5312 0", 4)
        antes = sin_colores(" ".join(m.comando("npc near 100", 2)))
        encontrados = presentes(antes)
        ctx.inf.datos["consulta_antes"] = antes
        ctx.inf.datos["guid_presentes"] = sorted(encontrados)
        ctx.inf.comprobar(sec, bool(antes) and not encontrados,
                          "ningún GUID del manifiesto visible en 100 yardas", str(sorted(encontrados)), origen="entorno")
