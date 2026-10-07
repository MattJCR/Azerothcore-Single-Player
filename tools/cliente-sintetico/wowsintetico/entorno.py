# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Perfil del entorno de pruebas: a qué servidor se conecta y qué se permite hacer.

Nada operativo va en el código ni en Git. El perfil sale, por este orden, de:

1. `--entorno <fichero>` o la variable `VERIFICADOR_ENTORNO`;
2. `tools/cliente-sintetico/entorno.local.json` (excluido de Git).

Las variables `VERIFICADOR_HOST`, `VERIFICADOR_CUENTA` y `VERIFICADOR_CLAVE`
pisan los valores del fichero. Ver `entorno.ejemplo.json`.

La clave nunca se imprime ni llega al informe: `publico()` es lo único que se
serializa.
"""
import json
import os

CARPETA_HERRAMIENTA = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FICHERO_LOCAL = os.path.join(CARPETA_HERRAMIENTA, "entorno.local.json")

# Acciones que un caso puede declarar. Un perfil sin `permitir_escrituras` sólo
# admite "conectar" y "leer" (diagnóstico: login, lista de personajes, reinos).
ACCIONES = {
    "conectar": "login en authserver y worldserver, lista de reinos y personajes",
    "leer": "comandos GM de sólo lectura (.server info, .gps, .ip get, estados de módulos)",
    "personaje": "crear, usar y borrar personajes temporales con el prefijo del perfil",
    "gm": "comandos GM que cambian el personaje temporal (nivel, dinero, teletransporte, equipo)",
    "comprar": "comprar a instructores y vendedores con el personaje temporal",
    "selfbot": "activar `.playerbots bot self` sobre el personaje temporal",
    "grupo": "pedir bots (.grupo, addclass), invitar y echar del grupo",
    "cola": "apuntarse al buscador de mazmorras",
    "mazmorra": "entrar en una instancia y activar dungeon-clear",
}
LECTURA = {"conectar", "leer"}


class ErrorEntorno(Exception):
    pass


class Entorno:
    def __init__(self, datos: dict, origen: str):
        self.origen = origen
        self.nombre = datos.get("nombre") or "sin-nombre"
        self.host = datos.get("host")
        self.cuenta = (datos.get("cuenta") or "").upper()
        self._clave = datos.get("clave")
        adicionales = datos.get("cuentas_adicionales", {})
        if not isinstance(adicionales, dict):
            raise ErrorEntorno("cuentas_adicionales debe ser un objeto")
        self._cuentas_adicionales = {}
        for alias, credenciales in adicionales.items():
            if not isinstance(alias, str) or not alias or alias == "principal" or not isinstance(credenciales, dict):
                raise ErrorEntorno("alias o credenciales inválidos en cuentas_adicionales")
            usuario = (credenciales.get("cuenta") or "").upper()
            clave = credenciales.get("clave")
            if not usuario or not clave or usuario == self.cuenta:
                raise ErrorEntorno("cuenta o clave inválida para %s" % alias)
            self._cuentas_adicionales[alias] = (usuario, clave)
        self.reino_esperado = datos.get("reino_esperado")
        self.cliente_wow = datos.get("cliente_wow", "")
        self.idioma = datos.get("idioma", "esES")
        self.prefijo = datos.get("prefijo", "Vs")
        self.permitir_escrituras = bool(datos.get("permitir_escrituras", False))
        admitidas = datos.get("acciones_admitidas")
        if admitidas is None:
            admitidas = sorted(ACCIONES) if self.permitir_escrituras else sorted(LECTURA)
        desconocidas = set(admitidas) - set(ACCIONES)
        if desconocidas:
            raise ErrorEntorno("acciones desconocidas en el perfil: %s" % ", ".join(sorted(desconocidas)))
        self.acciones = set(admitidas) if self.permitir_escrituras else set(admitidas) & LECTURA
        self.timeout_conexion = float(datos.get("timeout_conexion", 60))
        faltan = [k for k, v in (("host", self.host), ("cuenta", self.cuenta), ("clave", self._clave)) if not v]
        if faltan:
            raise ErrorEntorno("el perfil %s no define %s" % (origen, ", ".join(faltan)))
        if not self.prefijo.isalpha() or not (2 <= len(self.prefijo) <= 4):
            raise ErrorEntorno("prefijo inválido %r: 2-4 letras" % self.prefijo)

    @property
    def clave(self) -> str:
        return self._clave

    def credenciales(self, alias="principal") -> tuple:
        if alias == "principal":
            return self.cuenta, self._clave
        try:
            return self._cuentas_adicionales[alias]
        except KeyError:
            raise ErrorEntorno("el perfil no define la cuenta %r" % alias) from None

    def tiene_cuenta(self, alias) -> bool:
        return alias == "principal" or alias in self._cuentas_adicionales

    def admite(self, acciones) -> set:
        """Devuelve las acciones que el perfil NO admite (vacío = todo admitido)."""
        return set(acciones) - self.acciones

    def publico(self) -> dict:
        return {"nombre": self.nombre, "origen": os.path.basename(self.origen), "host": self.host,
                "cuenta": self.cuenta, "reino_esperado": self.reino_esperado, "idioma": self.idioma,
                "cuentas_adicionales": sorted(self._cuentas_adicionales),
                "prefijo": self.prefijo, "permitir_escrituras": self.permitir_escrituras,
                "acciones_admitidas": sorted(self.acciones)}

    def __repr__(self):
        return "Entorno(%s, %s@%s)" % (self.nombre, self.cuenta, self.host)


def cargar(ruta: str = None) -> Entorno:
    ruta = ruta or os.environ.get("VERIFICADOR_ENTORNO") or FICHERO_LOCAL
    datos = {}
    if os.path.exists(ruta):
        try:
            with open(ruta, encoding="utf-8") as f:
                datos = json.load(f)
        except ValueError as e:
            raise ErrorEntorno("el perfil %s no es JSON válido: %s" % (ruta, e))
    elif ruta != FICHERO_LOCAL or not any(os.environ.get(v) for v in ("VERIFICADOR_HOST", "VERIFICADOR_CUENTA")):
        raise ErrorEntorno("no existe el perfil %s: copia entorno.ejemplo.json a entorno.local.json" % ruta)
    for clave, var in (("host", "VERIFICADOR_HOST"), ("cuenta", "VERIFICADOR_CUENTA"), ("clave", "VERIFICADOR_CLAVE")):
        if os.environ.get(var):
            datos[clave] = os.environ[var]
    return Entorno(datos, ruta)
