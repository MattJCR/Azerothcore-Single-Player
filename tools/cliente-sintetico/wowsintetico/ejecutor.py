# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Ejecuta casos del catálogo con precondiciones, plazos y aislamiento.

Cada caso recibe un `Contexto`: sus parámetros, su `ResultadoCaso` (`ctx.inf`),
los DBC del cliente si los pidió y `ctx.nueva_sesion()`, que abre una sesión
nueva ya comprobada (reino esperado, plazo del caso). Un caso roto no arrastra
a los siguientes: cada uno abre y cierra sus propias sesiones.
"""
import os
import re
import socket
import time
import traceback

from . import registro
from .auth import ErrorAuth, iniciar_sesion
from .informe import BLOQUEADO, ERROR, FALLO, OMITIDO, ResultadoCaso
from .mundo import ErrorMundo, Mundo, PlazoAgotado

ERRORES_ENTORNO = (ErrorAuth, ConnectionError, socket.timeout, socket.gaierror, OSError)


class Bloqueo(Exception):
    """Una precondición que sólo se descubre al ejecutar (p. ej. el servidor no es el esperado)."""


class Contexto:
    def __init__(self, entorno, caso, inf: ResultadoCaso, parametros: dict, dbc, carpeta_artefactos, informe):
        self.entorno, self.caso, self.inf, self.p = entorno, caso, inf, parametros
        self.dbc = dbc
        self.carpeta_artefactos = carpeta_artefactos
        self.informe = informe
        self.plazo = time.time() + caso.duracion_max
        self.sesiones = []

    # ── sesiones ────────────────────────────────────────────────────────────
    def reinos(self, cuenta="principal"):
        usuario, clave = self.entorno.credenciales(cuenta)
        return iniciar_sesion(self.entorno.host, usuario, clave)

    def nueva_sesion(self, cuenta="principal") -> Mundo:
        usuario, _ = self.entorno.credenciales(cuenta)
        K, reinos = self.reinos(cuenta)
        if not reinos:
            raise Bloqueo("el authserver no anuncia ningún reino")
        reino = reinos[0]
        if self.entorno.reino_esperado and reino["nombre"] != self.entorno.reino_esperado:
            raise Bloqueo("el reino es %r y el perfil espera %r: no se actúa" % (
                reino["nombre"], self.entorno.reino_esperado))
        host, _, puerto = reino["direccion"].rpartition(":")
        m = Mundo(self.entorno.host if host in ("127.0.0.1", "localhost") else host, int(puerto),
                  usuario, K, reino["id"], timeout_conexion=self.entorno.timeout_conexion)
        m.plazo = self.plazo
        self.sesiones.append(m)
        return m

    def cerrar(self):
        for m in self.sesiones:
            m.cerrar()

    def artefacto(self, nombre: str) -> str:
        os.makedirs(self.carpeta_artefactos, exist_ok=True)
        ruta = os.path.join(self.carpeta_artefactos, "%s-%s" % (self.caso.id, nombre))
        self.inf.artefactos.append(ruta)
        return ruta

    def anotar_servidor(self, m: Mundo):
        """`.server info` una vez por ejecución: revisión del core y población."""
        if self.informe.meta["servidor"].get("revision"):
            return
        texto = " ".join(m.comando("server info", 2))
        rev = re.search(r"AzerothCore rev\. ([0-9a-f]{7,40})", texto)
        self.informe.meta["servidor"] = {"texto": texto[:400], "revision": rev.group(1) if rev else None}


def precondiciones(caso, entorno, dbc) -> str:
    """Motivo de bloqueo o cadena vacía."""
    faltan = entorno.admite(caso.acciones)
    if faltan:
        if not entorno.permitir_escrituras and faltan - {"conectar", "leer"}:
            return "el perfil %s no permite escrituras (necesita: %s)" % (entorno.nombre, ", ".join(sorted(faltan)))
        return "el perfil %s no admite: %s" % (entorno.nombre, ", ".join(sorted(faltan)))
    if "dbc" in caso.requiere and dbc is None:
        return "necesita los DBC del cliente (mpyq y la carpeta cliente_wow del perfil)"
    return ""


def ejecutar_caso(caso, entorno, parametros, dbc, carpeta_artefactos, informe, dry_run=False, eco=print):
    inf = ResultadoCaso(caso, parametros, eco=eco)
    informe.agregar(inf)
    eco("\n== %s — %s" % (caso.id, caso.titulo))
    motivo = precondiciones(caso, entorno, dbc)
    if motivo:
        inf.forzar(BLOQUEADO, motivo)
        return inf
    if dry_run:
        inf.forzar(OMITIDO, "--dry-run: se ejecutaría con %s (acciones: %s, máx. %d s)" % (
            parametros, ", ".join(sorted(caso.acciones)), caso.duracion_max))
        return inf
    ctx = Contexto(entorno, caso, inf, parametros, dbc, carpeta_artefactos, informe)
    inf.empezar()
    try:
        caso.funcion(ctx)
    except Bloqueo as e:
        inf.forzar(BLOQUEADO, str(e))
    except PlazoAgotado as e:
        inf.anotar(caso.id, FALLO, "tiempo máximo superado (%d s)" % caso.duracion_max, str(e), origen="desarrollo")
    except ERRORES_ENTORNO as e:
        inf.error(repr(e), "entorno")
        inf.forzar(ERROR, "infraestructura: %r" % e)
    except ErrorMundo as e:
        inf.error(repr(e), "entorno")
        inf.forzar(ERROR, "el worldserver cortó la sesión: %s" % e)
    except Exception as e:                                   # noqa: BLE001 — un caso no para a los demás
        inf.error(traceback.format_exc(limit=6), "cliente")
        inf.anotar(caso.id, FALLO, "excepción no prevista", repr(e), origen="cliente")
    finally:
        ctx.cerrar()
        inf.terminar()
    eco("  → " + inf.resumen())
    return inf


def limpiar_huerfanos(entorno, eco=print, forzar_nombres=(), cuenta="principal") -> dict:
    """Borra los personajes temporales huérfanos que se pueden probar como nuestros."""
    usuario, clave = entorno.credenciales(cuenta)
    K, reinos = iniciar_sesion(entorno.host, usuario, clave)
    reino = reinos[0]
    if entorno.reino_esperado and reino["nombre"] != entorno.reino_esperado:
        raise Bloqueo("reino inesperado %r" % reino["nombre"])
    host, _, puerto = reino["direccion"].rpartition(":")
    m = Mundo(entorno.host if host in ("127.0.0.1", "localhost") else host, int(puerto), usuario, K,
              reino["id"], timeout_conexion=entorno.timeout_conexion)
    try:
        lista = m.personajes()
        borrables, sospechosos, ausentes = registro.clasificar(lista, entorno.host, usuario, entorno.prefijo)
        borrados, en_mundo = [], []
        for p in borrables + [s for s in sospechosos if s["nombre"] in forzar_nombres]:
            try:
                r = m.borrar_personaje(p["guid"])
            except TimeoutError:
                # El core no contesta al borrado de un personaje que sigue en el mundo
                # (sesión anterior aún viva tras un corte): se deja para otro intento.
                en_mundo.append(p["nombre"])
                eco("  %s sigue en el mundo: reintenta `limpiar` en un par de minutos" % p["nombre"])
                continue
            eco("  borrado %s (guid %d): código 0x%02X" % (p["nombre"], p["guid"], r))
            if r == 0x47:
                registro.quitar(entorno.host, p["guid"])
                borrados.append(p["nombre"])
        for e in ausentes:
            registro.quitar(entorno.host, e["guid"])
        return {"borrados": borrados, "siguen_en_el_mundo": en_mundo,
                "sin_registro": [s["nombre"] for s in sospechosos if s["nombre"] not in forzar_nombres],
                "registro_sin_personaje": [e["nombre"] for e in ausentes],
                "personajes_en_cuenta": len(lista)}
    finally:
        m.cerrar()
