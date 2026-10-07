# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Comandos GM de mantenimiento que no encajan en `modulos` (sólo lectura):
`.wpvp parar` (PLAN M13) y `.ayuda export` (PLAN M14) escriben, así que
necesitan su propio caso en vez de la consulta genérica.

`.wpvp parar` — antes de M13, un identificador de evento vacío ("todos") y un
0 explícito compartían el mismo `uint32 id = 0` de la petición interna: no
había forma de distinguir "para todos" de "para el evento 0" (que nunca
existe: los id empiezan en 1). Con `Optional<uint32>` ahora son dos ramas
distintas del mismo `if` (`all = !r.id.has_value()`), así que un id concreto
-exista o no, incluido 0 o el máximo de 32 bits- nunca puede arrastrar al
resto de la guerra de mundo. Sin una guerra activa (no se fuerza una para no
interferir con quien esté jugando) los tres casos informan "0 eventos
terminando": la prueba es que ninguno falla, no la cuenta.

`.ayuda export` — vuelca a `server_help_command` los comandos sin ficha
curada como filas `auto=1`, dentro de una única transacción, y confirma con
una relectura real (`SELECT COUNT(*) ... WHERE auto=1`) en vez de dar el
número de filas por escrito por supuesto (M14). Se ejecuta dos veces
seguidas -el escenario que perdía las fichas curadas deshabilitadas- y se
comprueba que ninguna de las dos deja el aviso de "quedaron distintas
fichas de las intentadas".
"""
import re

from ..catalogo import caso
from . import PersonajeTemporal, sin_colores

RAZA, CLASE = 1, 1

CMDS_WPVP = ("wpvp parar", "wpvp parar 0", "wpvp parar 4294967295")


@caso(id="mantenimiento", titulo="Comandos GM de mantenimiento (`.wpvp parar`, `.ayuda export`)",
      descripcion=__doc__,
      etiquetas=("mantenimiento", "world-bots", "server-help", "personaje"),
      acciones=("conectar", "leer", "personaje", "gm"),
      duracion_max=180,
      protege=("mod-world-bots (pvp)", "mod-server-help", "PLAN M13", "PLAN M14"),
      control="directo",
      observa=("`.wpvp parar` [vacío|0|4294967295]", "`.ayuda export` x2"),
      no_cubre=("una guerra de mundo activa parada por id (no se fuerza un evento real)",
                 "la ficha curada deshabilitada protegida en la exportación (fixture SQL fuera del catálogo)"))
def ejecutar(ctx):
    inf, sec = ctx.inf, "mantenimiento"
    m = ctx.nueva_sesion()
    with PersonajeTemporal(ctx, m, RAZA, CLASE, sec):
        ctx.anotar_servidor(m)

        # M13: ninguna variante de `.wpvp parar` debe fallar ni colgarse,
        # exista o no un evento con ese id.
        for orden in CMDS_WPVP:
            texto = sin_colores(" ".join(m.comando(orden, 3)))
            inf.datos.setdefault("wpvp", {})[orden] = texto[:200]
            ok = bool(re.search(r"evento", texto, re.IGNORECASE))
            inf.comprobar(sec, ok, "`.%s` responde sin error" % orden, texto[:160] or "sin respuesta",
                          estado_si_no="FALLO" if not texto else "AVISO")

        # M14: recarga antes de exportar (si alguien tocó server_help_command
        # por SQL, como el fixture de una ficha curada deshabilitada, que se
        # vea con los datos frescos) y dos exportaciones seguidas, ninguna
        # con el aviso de recuento distinto (transacción a medias / fichas
        # perdidas).
        texto_recargar = sin_colores(" ".join(m.comando("ayuda recargar", 3)))
        inf.datos["ayuda_recargar"] = texto_recargar[:250]
        inf.comprobar(sec, "recargad" in texto_recargar.lower(), "`.ayuda recargar` responde antes de exportar",
                      texto_recargar[:200] or "sin respuesta", estado_si_no="AVISO")
        resultados = []
        for i in range(2):
            texto = sin_colores(" ".join(m.comando_hasta("ayuda export", r"fichas autom|desactiv", 8)))
            resultados.append(texto)
            inf.datos.setdefault("ayuda_export", []).append(texto[:250])
            hay_aviso = "aviso" in texto.lower() and "quedaron" in texto.lower()
            m_n = re.search(r"(\d+)\s+fichas automaticas escritas", texto)
            inf.comprobar(sec, bool(m_n) and not hay_aviso,
                          "`.ayuda export` (pasada %d) confirma el recuento tras la transacción" % (i + 1),
                          texto[:200] or "sin respuesta", esperado="N fichas escritas, sin aviso de discrepancia",
                          observado=texto[:200] or "sin respuesta",
                          estado_si_no="FALLO" if not texto else "AVISO")

        inf.comprobar(sec, not m.errores_lectura, "paquetes del mundo leídos sin error",
                      "; ".join(m.errores_lectura[:3]), origen="cliente")
