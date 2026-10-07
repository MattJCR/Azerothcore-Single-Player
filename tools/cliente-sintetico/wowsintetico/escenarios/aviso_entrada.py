# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Comprueba que mod-update-notice anuncia las novedades al entrar en el mundo."""

from ..catalogo import Parametro, caso
from . import PersonajeTemporal, sin_colores


@caso(id="aviso-entrada", titulo="Aviso automático de actualizaciones al entrar",
      descripcion="""
Entra con un GM temporal y escucha el aviso espontáneo de mod-update-notice.
Después consulta `.actualizaciones` para saber si el informe actual contiene
novedades. Sólo exige el aviso de login cuando hay novedades que mostrar; un
informe «Todo al día» deja la vía positiva sin ejercitar.
""",
      etiquetas=("modulos", "update-notice", "personaje"),
      acciones=("conectar", "leer", "personaje"),
      parametros={"espera": Parametro(int, 15, "segundos para el aviso de login", minimo=8, maximo=60)},
      duracion_max=100, protege=("mod-update-notice", "M37"), control="directo",
      observa=("mensaje espontáneo [Actualizaciones]", "`.actualizaciones`"),
      no_cubre=("el aviso con novedades si el informe de la VM está al día en el momento de la pasada "
                "(la vía positiva se verificó a mano el 25/09/2026 escribiendo una línea de prueba en "
                "updates-pending.txt antes del login y restaurándolo después; no se automatiza aquí "
                "porque tocar ese fichero no es de solo lectura)",), en_todo=False)
def ejecutar(ctx):
    inf, sec = ctx.inf, "update-notice"
    m = ctx.nueva_sesion()
    with PersonajeTemporal(ctx, m, 1, 1, sec):
        # No se manda ningún comando hasta acabar el plazo: el mensaje debe
        # venir de OnPlayerLogin/OnUpdate y no de `.actualizaciones`.
        m.bombear(ctx.p["espera"])
        automaticos = [sin_colores(t) for _, t in m.mensajes if "[Actualizaciones]" in sin_colores(t)]
        ctx.anotar_servidor(m)
        consulta = sin_colores(" ".join(m.comando("actualizaciones", 2)))
        novedades = "Hay versiones nuevas rio arriba" in consulta
        inf.datos["aviso_automatico"] = automaticos[:3]
        inf.datos["consulta"] = consulta[:500]
        inf.comprobar(sec, bool(consulta) and "[Actualizaciones]" in consulta,
                      "`.actualizaciones` muestra el estado del informe", consulta[:180])
        if novedades:
            inf.comprobar(sec, bool(automaticos), "las novedades se anuncian al entrar sin comando",
                          " | ".join(automaticos[:2]) or "sin aviso en %d s" % ctx.p["espera"])
        else:
            inf.comprobar(sec, not automaticos, "sin novedades no aparece un aviso automático",
                          " | ".join(automaticos[:2]) or "ninguno")
            inf.anotar(sec, "AVISO", "aviso de login con novedades",
                       "el informe actual no contiene novedades; la vía positiva queda pendiente")
        inf.comprobar(sec, not m.errores_lectura, "paquetes leídos sin error",
                      "; ".join(m.errores_lectura[:3]), origen="cliente")
