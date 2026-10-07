# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Dos personajes de cuentas distintas conectados al mismo tiempo (M42)."""

from ..catalogo import caso
from ..ejecutor import Bloqueo
from . import PersonajeTemporal, sin_colores


@caso(id="dos-cuentas", titulo="Dos jugadores reales simultáneos",
      descripcion="""
Abre dos sesiones de cuentas de prueba distintas y mantiene ambos personajes
en el mundo. Cada uno ejecuta `.server info` mientras el otro sigue conectado;
la segunda conexión no debe expulsar a la primera.
""",
      etiquetas=("infraestructura", "dos-cuentas", "personaje"),
      acciones=("conectar", "leer", "personaje"), duracion_max=150,
      protege=("M42",), control="directo",
      observa=("ambos personajes conectados", "respuesta de cada sesión tras el segundo login"),
      no_cubre=("interacciones entre los dos jugadores: M43-M45",), en_todo=False)
def ejecutar(ctx):
    if not ctx.entorno.tiene_cuenta("secundaria"):
        raise Bloqueo("falta cuentas_adicionales.secundaria en el perfil")
    inf, sec = ctx.inf, "dos-cuentas"
    primero = ctx.nueva_sesion()
    segundo = ctx.nueva_sesion("secundaria")
    with PersonajeTemporal(ctx, primero, 1, 1, sec) as a:
        with PersonajeTemporal(ctx, segundo, 1, 1, sec) as b:
            ctx.anotar_servidor(primero)
            inf.comprobar(sec, primero.usuario != segundo.usuario and a.guid != b.guid,
                          "las sesiones pertenecen a cuentas y personajes distintos")
            texto_a = sin_colores(" ".join(primero.comando("server info", 2)))
            texto_b = sin_colores(" ".join(segundo.comando("server info", 2)))
            inf.comprobar(sec, "AzerothCore" in texto_a and primero.guid == a.guid,
                          "la primera sesión responde tras entrar la segunda", texto_a[:150])
            inf.comprobar(sec, "AzerothCore" in texto_b and segundo.guid == b.guid,
                          "la segunda sesión responde mientras la primera sigue activa", texto_b[:150])
            inf.comprobar(sec, not primero.errores_lectura and not segundo.errores_lectura,
                          "ambas sesiones leen paquetes sin errores",
                          "; ".join((primero.errores_lectura + segundo.errores_lectura)[:3]), origen="cliente")
