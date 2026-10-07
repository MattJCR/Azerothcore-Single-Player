# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Escenarios de verificación. Cada módulo registra sus casos con `@caso(...)`
(ver catalogo.py) y cada caso es `f(ctx)` (ver ejecutor.Contexto)."""
import random
import re

from .. import actualizaciones as upd
from .. import registro
from ..mundo import CHAR_CREATE_SUCCESS, CHAR_DELETE_SUCCESS

RAZAS = {1: "humano", 2: "orco", 3: "enano", 4: "elfo de la noche", 5: "no-muerto", 6: "tauren",
         7: "gnomo", 8: "trol", 10: "elfo de sangre", 11: "draenei"}
CLASES = {1: "guerrero", 2: "paladín", 3: "cazador", 4: "pícaro", 5: "sacerdote",
          6: "caballero de la Muerte", 7: "chamán", 8: "mago", 9: "brujo", 11: "druida"}


def sin_colores(texto: str) -> str:
    """Quita |cAARRGGBB y |r: sus cifras hexadecimales engañan a cualquier regex de números."""
    return re.sub(r"\|c[0-9a-fA-F]{8}|\|r", "", texto)


def nombre_aleatorio(prefijo: str = "Vs") -> str:
    """Nombre válido y casi seguro libre: prefijo + letras alternando consonante/vocal (12 como mucho)."""
    n = max(4, 8 - len(prefijo))
    letras = [random.choice("aeiou" if i % 2 else "bcdfglmnprstvz") for i in range(n)]
    return prefijo + "".join(letras)


def asegurar_vivo(m) -> bool:
    """Un personaje recién creado puede llegar con PLAYER_FIELD_HEALTH=0 (visto en vivo, M39:
    Northshire con los bots activos del perfil — algo lo mata en los primeros segundos). Varios
    comandos GM (`Player::CanUseItem`, `HandleOpenItemOpcode`...) rechazan entonces con
    EQUIP_ERR_YOU_ARE_DEAD sin relación con lo que se esté probando; revive con `.revive` si
    hace falta (mismo patrón que ya usaba `companeros_transiciones.py` para M29)."""
    if m.valor_propio(upd.UNIT_FIELD_HEALTH) <= 0:
        m.comando("revive", 3)
        m.bombear(1)
    return m.valor_propio(upd.UNIT_FIELD_HEALTH) > 0


class PersonajeTemporal:
    """Crea un personaje al entrar en el bloque `with` y lo borra al salir, pase lo que pase.

    Se apunta en el registro local antes de entrar en el mundo, así que un corte del
    proceso deja un huérfano que `verificar.py limpiar` puede probar como propio."""

    def __init__(self, ctx, mundo, raza, clase, seccion, genero=0, antes_de_salir=None):
        self.ctx, self.m, self.raza, self.clase, self.sec, self.genero = ctx, mundo, raza, clase, seccion, genero
        self.inf = ctx.inf
        self.guid, self.nombre, self.dentro = None, None, False
        self.antes_de_salir = antes_de_salir        # f(): deshacer selfbot, grupo... antes del logout

    def __enter__(self):
        r = None
        for _ in range(3):
            self.nombre = nombre_aleatorio(self.ctx.entorno.prefijo)
            r = self.m.crear_personaje(self.nombre, self.raza, self.clase, self.genero)
            if r == CHAR_CREATE_SUCCESS:
                break
        self.inf.comprobar(self.sec, r == CHAR_CREATE_SUCCESS, "creación del personaje",
                           "%s (código 0x%02X)" % (self.nombre, r), origen="entorno")
        if r != CHAR_CREATE_SUCCESS:
            raise RuntimeError("no se pudo crear el personaje")
        try:
            self.guid = next(p["guid"] for p in self.m.personajes() if p["nombre"] == self.nombre)
            registro.apuntar(self.ctx.entorno.host, self.m.usuario, self.nombre, self.guid, self.ctx.caso.id)
            self.inf.datos.setdefault("personajes", []).append({"nombre": self.nombre, "guid": self.guid,
                                                                  "raza": self.raza, "clase": self.clase})
            self.m.entrar(self.guid, self.raza, nombre=self.nombre)
            self.dentro = True
        except BaseException:
            self.__exit__(None, None, None)
            raise
        return self

    def __exit__(self, *exc):
        plazo, self.m.plazo = self.m.plazo, None       # la limpieza no se corta por el plazo del caso
        try:
            if self.dentro and self.antes_de_salir:
                try:
                    self.antes_de_salir()
                except Exception as e:                  # noqa: BLE001 — se anota y se sigue limpiando
                    self.inf.anotar(self.sec, "AVISO", "limpieza previa al logout", repr(e))
            if self.dentro:
                self.m.salir()
        finally:
            if self.guid:
                try:
                    r = self.m.borrar_personaje(self.guid)
                except Exception as e:                  # noqa: BLE001 — sesión rota: queda para `limpiar`
                    self.inf.anotar(self.sec, "AVISO", "borrado del personaje temporal",
                                    "no se pudo (%r); sigue en el registro para `verificar.py limpiar`" % e)
                else:
                    ok = r == CHAR_DELETE_SUCCESS
                    if ok:
                        registro.quitar(self.ctx.entorno.host, self.guid)
                    self.inf.comprobar(self.sec, ok, "borrado del personaje temporal",
                                       "código 0x%02X" % r, estado_si_no="AVISO")
            self.m.plazo = plazo
        return False
