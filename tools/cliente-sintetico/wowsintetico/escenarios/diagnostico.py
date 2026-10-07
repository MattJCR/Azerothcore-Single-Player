# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Diagnóstico de conexión: sólo lectura, sin personajes ni comandos.

Es el primer caso que debe correr cualquiera: comprueba que el perfil apunta al
servidor esperado antes de dejar que otro caso escriba.
"""
from .. import registro
from ..catalogo import caso


@caso(id="diagnostico", titulo="Conexión, reino y cuenta de pruebas (sólo lectura)",
      descripcion="""
Login SRP6 en el authserver, lista de reinos (nombre esperado del perfil),
autenticación en el worldserver (lo despierta si está en espera) y lista de
personajes de la cuenta. Informa de personajes con el prefijo de pruebas que no
estén en el registro local (huérfanos de otra máquina o de un corte).
""",
      etiquetas=("diagnostico", "lectura", "rapido"), acciones=("conectar",), duracion_max=120,
      protege=("authserver", "worldserver", "mod-standby (despertar por conexión)"), control="lectura",
      observa=("AUTH_LOGON_CHALLENGE/PROOF", "REALM_LIST", "SMSG_AUTH_RESPONSE", "SMSG_CHAR_ENUM"),
      no_cubre=("nada del mundo: no entra con ningún personaje",))
def ejecutar(ctx):
    inf, sec = ctx.inf, "diagnóstico"
    K, reinos = ctx.reinos()
    inf.comprobar(sec, bool(reinos), "el authserver acepta la cuenta y anuncia reinos",
                  ", ".join("%s (%s)" % (r["nombre"], r["direccion"]) for r in reinos))
    inf.datos["reinos"] = reinos
    esperado = ctx.entorno.reino_esperado
    if esperado:
        inf.comprobar(sec, reinos and reinos[0]["nombre"] == esperado, "el reino es el del perfil",
                      esperado=esperado, observado=reinos[0]["nombre"] if reinos else None, origen="entorno")
    m = ctx.nueva_sesion()
    inf.comprobar(sec, True, "el worldserver acepta la sesión (SMSG_AUTH_RESPONSE OK)")
    lista = m.personajes()
    borrables, sospechosos, ausentes = registro.clasificar(lista, ctx.entorno.host, ctx.entorno.cuenta,
                                                           ctx.entorno.prefijo)
    inf.datos["personajes"] = [{"nombre": p["nombre"], "nivel": p["nivel"], "mapa": p["mapa"]} for p in lista]
    inf.comprobar(sec, not borrables, "sin personajes temporales huérfanos registrados",
                  "huérfanos: %s (verificar.py limpiar)" % ", ".join(p["nombre"] for p in borrables),
                  estado_si_no="AVISO")
    inf.comprobar(sec, not sospechosos, "sin personajes con el prefijo %s fuera del registro" % ctx.entorno.prefijo,
                  ", ".join(p["nombre"] for p in sospechosos), estado_si_no="AVISO")
    inf.comprobar(sec, not m.errores_lectura, "paquetes leídos sin error", "; ".join(m.errores_lectura[:3]),
                  origen="cliente")
