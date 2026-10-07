# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""mod-bot-operations: `.botops estado` lee `bot_operations_action` sin SQL directo (PLAN M41).

`bot_operations_action` es el buzón entre el panel web y mod-bot-operations
(ver la cabecera de `mod_bot_operations.cpp`): el panel deja filas `pending`,
el módulo las traduce a peticiones internas para el módulo destino y vuelca el
resultado (`done`/`failed`/`expired`) a la misma fila. Hasta este comando,
comprobar su estado desde el cliente sintético exigía un SELECT directo contra
`acore_world` -- justo lo que M13 (CHANGELOG.md, 25/09/2026) tuvo que sortear
insertando filas a mano en vez de dejar un caso repetible en el catálogo.

`.botops estado` (GM, sólo lectura) expone lo mismo en texto: si el puente
está activo, cuántas filas pendientes/resueltas hay (con el desglose
done/failed/expired), y hasta 10 pendientes / 5 resueltas más recientes con su
tipo, parámetro/resultado y antigüedad. Este caso sólo comprueba que el propio
comando es internamente coherente (los recuentos casan con las filas
listadas, sin duplicados, sin edades negativas): no dispara ninguna acción
real ni depende de SQL directo -- ese es justo el punto de la tarea. El
parseo se validó a mano una vez, comparando la salida de este comando contra
un SELECT directo sobre una fila de prueba (ver CHANGELOG.md).
"""
import re

from ..catalogo import caso
from . import PersonajeTemporal, sin_colores

LINEA_PUENTE = re.compile(r"bot-operations:\s*puente\s*(activo|inactivo)")
LINEA_RESUMEN = re.compile(
    r"colas:\s*(\d+)\s*pendientes,\s*(\d+)\s*resueltas\s*\((\d+)\s*done,\s*(\d+)\s*failed,\s*(\d+)\s*expired\)")
LINEA_PENDIENTE = re.compile(r"pendiente #(\d+)\s+(\S+)\s+param=(\S+)\s+edad=(\d+)s")
LINEA_RESUELTA = re.compile(r"resuelta #(\d+)\s+(\S+)\s+\[(\w+)\]\s+edad=(\d+)s:\s*(.*)")


def leer_estado_botops(m):
    """`.botops estado` -> dict con puente, resumen (tupla) y filas pendientes/resueltas."""
    textos = [sin_colores(t) for t in m.comando("botops estado", 4)]
    estado = {"puente": None, "resumen": None, "pendientes": [], "resueltas": [], "bruto": textos}
    for t in textos:
        mobj = LINEA_PUENTE.search(t)
        if mobj:
            estado["puente"] = mobj.group(1)
            continue
        mobj = LINEA_RESUMEN.search(t)
        if mobj:
            estado["resumen"] = tuple(int(x) for x in mobj.groups())
            continue
        mobj = LINEA_PENDIENTE.search(t)
        if mobj:
            estado["pendientes"].append({"id": int(mobj.group(1)), "accion": mobj.group(2),
                                          "param": mobj.group(3), "edad": int(mobj.group(4))})
            continue
        mobj = LINEA_RESUELTA.search(t)
        if mobj:
            estado["resueltas"].append({"id": int(mobj.group(1)), "accion": mobj.group(2),
                                         "estado_fila": mobj.group(3), "edad": int(mobj.group(4)),
                                         "resultado": mobj.group(5)})
    return estado


@caso(id="bot-operations-estado", titulo="mod-bot-operations: `.botops estado` sin SQL directo",
      descripcion=__doc__,
      etiquetas=("bot-operations", "lectura", "rapido", "personaje"),
      acciones=("conectar", "leer", "personaje", "gm"),
      duracion_max=120, protege=("mod-bot-operations", "PLAN M41"), control="directo",
      observa=("`.botops estado`",),
      no_cubre=("que el panel encole/resuelva de verdad una accion (mira `.wpvp estado` u otros comandos de "
                "cada modulo destino para eso)", "el contenido exacto de una fila concreta (se valido a mano una "
                "vez contra un SELECT directo, no en cada ejecucion de este caso)"))
def ejecutar(ctx):
    inf, sec = ctx.inf, "bot-operations-estado"
    m = ctx.nueva_sesion()
    with PersonajeTemporal(ctx, m, 1, 1, sec):
        ctx.anotar_servidor(m)

        estado = leer_estado_botops(m)
        inf.datos["botops_estado"] = estado

        if not inf.comprobar(sec, estado["puente"] is not None,
                              "`.botops estado` indica si el puente esta activo",
                              " | ".join(estado["bruto"][:2]) or "sin respuesta", origen="desarrollo"):
            return
        inf.comprobar(sec, estado["puente"] == "activo",
                      "el puente de mod-bot-operations esta activo en la VM (BotOperations.Enable)",
                      estado["puente"], estado_si_no="AVISO")

        if not inf.comprobar(sec, estado["resumen"] is not None,
                              "la linea de resumen (pendientes/resueltas) tiene el formato esperado",
                              " | ".join(estado["bruto"][:3]), origen="desarrollo"):
            return
        pendientes, resueltas, done, failed, expired = estado["resumen"]
        inf.comprobar(sec, resueltas == done + failed + expired,
                      "el total de resueltas es la suma de done+failed+expired",
                      "resumen=%s" % (estado["resumen"],), esperado=done + failed + expired, observado=resueltas)

        # El comando lista como mucho 10 pendientes y 5 resueltas (limite fijo
        # del propio comando, mod_bot_operations.cpp): con menos filas que el
        # limite, el recuento de la linea de resumen y las filas listadas
        # tienen que casar exacto.
        inf.comprobar(sec, len(estado["pendientes"]) == min(pendientes, 10),
                      "pendientes listadas == min(recuento, 10)",
                      "listadas=%d, recuento=%d" % (len(estado["pendientes"]), pendientes),
                      esperado=min(pendientes, 10), observado=len(estado["pendientes"]))
        inf.comprobar(sec, len(estado["resueltas"]) == min(resueltas, 5),
                      "resueltas listadas == min(recuento, 5)",
                      "listadas=%d, recuento=%d" % (len(estado["resueltas"]), resueltas),
                      esperado=min(resueltas, 5), observado=len(estado["resueltas"]))

        ids = [p["id"] for p in estado["pendientes"]] + [r["id"] for r in estado["resueltas"]]
        inf.comprobar(sec, len(ids) == len(set(ids)),
                      "cada fila listada (pendiente o resuelta) tiene un id de bot_operations_action distinto",
                      "ids=%s" % ids, estado_si_no="AVISO")
        edades = [p["edad"] for p in estado["pendientes"]] + [r["edad"] for r in estado["resueltas"]]
        inf.comprobar(sec, all(e >= 0 for e in edades),
                      "la antiguedad de cada fila listada es un entero no negativo",
                      "edades=%s" % edades)

        inf.comprobar(sec, not m.errores_lectura, "paquetes leidos sin error", "; ".join(m.errores_lectura[:3]),
                      origen="cliente")
