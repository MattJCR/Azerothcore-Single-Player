# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""SP02: compra real, piedra, portal y retirada de una sede temporal."""

import math
import struct
import time

from .. import actualizaciones as upd
from .. import registro
from ..catalogo import Parametro, caso
from ..ejecutor import Bloqueo
from ..mundo import CHAR_CREATE_SUCCESS, CHAR_DELETE_SUCCESS
from . import PersonajeTemporal, nombre_aleatorio
from .objetos import _nueva_ranura


VENDEDOR = 500030
MAYORDOMO = 500031
POSADERA = 500032
PIEDRA = 600001
PORTAL_VENTORMENTA = 500000
MOCHILA_BASE = 23


def _objeto(m, entry):
    return next((guid for guid, obj in m.objetos.items()
                 if obj.get("valores", {}).get(upd.OBJECT_FIELD_ENTRY) == entry), None)


def _opcion(opciones, *palabras):
    return next((o for o in opciones if any(p in o["texto"].lower() for p in palabras)), None)


def _ranura_item(m, entry):
    for indice, (lo, hi) in enumerate(m.objetos_bolsas()):
        guid = lo | (hi << 32)
        if guid and m.objetos.get(guid, {}).get("valores", {}).get(upd.OBJECT_FIELD_ENTRY) == entry:
            return MOCHILA_BASE + indice, guid
    return None, None


@caso(id="guildhouse-acceso", titulo="SP02: compra, posadera, piedra, portal y venta",
      descripcion="Comprueba el rechazo sin hermandad; una hermandad temporal compra su "
                  "sede y posadera, recibe la piedra, entra con ella, sale por el portal, "
                  "comprueba el enfriamiento compartido del comando y vende la sede. "
                  "Disuelve la hermandad y borra el personaje.",
      etiquetas=("sp02", "guildhouse", "hermandad"),
      acciones=("conectar", "leer", "personaje", "gm", "grupo"),
      parametros={
          "x": Parametro(float, -8884.7, "X del vendedor en Ventormenta (junto al Maestro de hermandad)"),
          "y": Parametro(float, 616.7, "Y del vendedor en Ventormenta"),
          "z": Parametro(float, 95.6, "Z del vendedor en Ventormenta"),
      },
      duracion_max=240, protege=("SP02", "mod-guildhouse", "piedra de la sede"),
      control="directo", en_todo=False,
      observa=("rechazo sin hermandad", "oro real", "posadera visible",
               "objeto 600001 en bolsa", "posición en la isla",
               "portal de Shattrath bloqueado en etapa 0 y abierto en etapa 8",
               "portal de Ventormenta recibido", "viaje de regreso", "denegación por cooldown",
               "vendedor comparte el enfriamiento de la piedra",
               "piedra de hogar normal usable", "reembolso de la mitad al vender"),
      no_cubre=("dos hermandades simultáneas", "otras mejoras y permisos de rango",
                "bots del grupo", "otros portales y etapas", "reinicio del servidor"))
def ejecutar(ctx):
    sec, inf = "guildhouse-acceso", ctx.inf
    m = ctx.nueva_sesion()
    estado = {"guild": None}

    def limpiar():
        if estado["guild"]:
            m.comando("hermandad desactivar", 0.5)
            m.comando('guild delete "%s"' % estado["guild"], 2)
            inf.comprobar(sec, m.valor_propio(upd.PLAYER_GUILDID) == 0,
                          "hermandad de prueba disuelta", origen="entorno")

    def comprobar(condicion, titulo, observado=None, esperado=None, origen="desarrollo"):
        return inf.comprobar(sec, condicion, titulo, observado=observado,
                            esperado=esperado, origen=origen)

    with PersonajeTemporal(ctx, m, 1, 1, sec, antes_de_salir=limpiar) as pj:
        ctx.anotar_servidor(m)
        m.comando("gm on", 0.5)
        m.seleccionar(m.guid)
        m.comando("go xyz %.3f %.3f %.3f 0" % (ctx.p["x"], ctx.p["y"], ctx.p["z"]), 2)
        m.comando("gm off", 0.5)
        gps = m.gps()
        if not comprobar(gps and gps.get("mapa") == 0, "llegada al vendedor", gps, origen="entorno"):
            return
        m.bombear(1)
        vendedor = _objeto(m, VENDEDOR)
        if not comprobar(bool(vendedor), "vendedor 500030 visible", vendedor, origen="entorno"):
            return

        sin_hermandad = m.conversar(vendedor, timeout=2)
        comprobar(sin_hermandad is None or not sin_hermandad[1],
                 "el vendedor rechaza a quien no tiene hermandad",
                 sin_hermandad)
        m.comando('guild create %s "%s"' % (pj.nombre, pj.nombre), 1)
        guild_id = m.valor_propio(upd.PLAYER_GUILDID)
        if not comprobar(guild_id != 0, "hermandad temporal creada", guild_id, origen="entorno"):
            return
        estado["guild"] = pj.nombre
        m.comando("hermandad desactivar", 0.5)
        m.comando("modify money 12000000", 1)
        oro_antes = m.valor_propio(upd.PLAYER_FIELD_COINAGE)
        if not comprobar(oro_antes >= 10000000, "oro de compra preparado", oro_antes,
                         origen="entorno"):
            return

        bolsas_antes = m.objetos_bolsas()
        menu = m.conversar(vendedor)
        if not comprobar(menu is not None, "menú del vendedor abierto", menu):
            return
        comprar = _opcion(menu[1], "comprar", "buy")
        if not comprobar(comprar is not None, "opción de compra disponible", menu[1]):
            return
        submenu = m.elegir(vendedor, menu[0], comprar["id"])
        if not comprobar(submenu is not None, "menú de la Isla de los MJ", submenu):
            return
        isla = _opcion(submenu[1], "isla", "island")
        if not comprobar(isla is not None, "sede de la isla ofertada", submenu[1]):
            return
        m.elegir(vendedor, submenu[0], isla["id"], timeout=2)
        m.bombear(1)
        oro_despues = m.valor_propio(upd.PLAYER_FIELD_COINAGE)
        if not comprobar(oro_antes - oro_despues == 10000000,
                         "precio original de 1.000 oro cobrado una sola vez",
                         oro_antes - oro_despues, 10000000):
            return
        idx, guid_piedra = _nueva_ranura(bolsas_antes, m.objetos_bolsas())
        if not comprobar(idx is not None and guid_piedra,
                         "piedra propia entregada al comprador", (idx, guid_piedra)):
            return

        fallo = m.usar_objeto(MOCHILA_BASE + idx, PIEDRA, guid_piedra, espera=1)
        if not comprobar(fallo is None, "uso de la piedra aceptado", fallo):
            return
        m.bombear(11)
        isla_gps = m.gps()
        if not comprobar(isla_gps and isla_gps.get("mapa") == 1 and
                         math.hypot(isla_gps["x"] - 16223, isla_gps["y"] - 16268) < 80,
                         "piedra lleva a la sede de la isla", isla_gps):
            return

        m.comando("go xyz 16202.185 16255.917 21.160 1", 2)
        m.bombear(1)
        mayordomo = _objeto(m, MAYORDOMO)
        if not comprobar(bool(mayordomo), "mayordomo inicial visible", mayordomo):
            return
        menu = m.conversar(mayordomo)
        posadera = _opcion(menu[1], "taberner", "posader", "innkeeper") if menu else None
        if not comprobar(posadera is not None, "mejora de posadera disponible", menu):
            return
        oro_mejora = m.valor_propio(upd.PLAYER_FIELD_COINAGE)
        mensajes_antes = len(m.mensajes)
        m.elegir(mayordomo, menu[0], posadera["id"], timeout=2)
        m.bombear(2)
        oro_mejorado = m.valor_propio(upd.PLAYER_FIELD_COINAGE)
        if not comprobar(oro_mejora - oro_mejorado == 1000000,
                         "precio original de posadera cobrado",
                         {"cobrado": oro_mejora - oro_mejorado,
                          "mensajes": m.mensajes[mensajes_antes:]},
                         1000000):
            return
        comprobar(bool(_objeto(m, POSADERA)), "posadera aparece en la fase de la sede")

        menu = m.conversar(mayordomo)
        portales = _opcion(menu[1], "portal") if menu else None
        if not comprobar(portales is not None, "mejoras de portales disponibles", menu):
            return
        submenu = m.elegir(mayordomo, menu[0], portales["id"])
        shattrath = _opcion(submenu[1], "shattrath") if submenu else None
        if not comprobar(shattrath is not None, "portal de Shattrath ofertado", submenu):
            return
        oro_portal = m.valor_propio(upd.PLAYER_FIELD_COINAGE)
        m.elegir(mayordomo, submenu[0], shattrath["id"], timeout=2)
        m.bombear(2)
        if not comprobar(oro_portal - m.valor_propio(upd.PLAYER_FIELD_COINAGE) == 1000000,
                         "precio original del portal cobrado",
                         oro_portal - m.valor_propio(upd.PLAYER_FIELD_COINAGE), 1000000):
            return
        m.comando("go xyz 16211.1 16266.9 13.746 1", 2)
        m.bombear(1)
        portal_shattrath = _objeto(m, 500008)
        if not comprobar(bool(portal_shattrath), "portal de Shattrath visible",
                         portal_shattrath):
            return
        m.enviar(0x0B1, struct.pack("<Q", portal_shattrath))
        m.bombear(2)
        if not comprobar(m.gps().get("mapa") == 1,
                         "Shattrath denegada antes de su etapa"):
            return
        m.comando("ip set %s 8" % pj.nombre, 2)
        m.enviar(0x0B1, struct.pack("<Q", portal_shattrath))
        m.bombear(3)
        if not comprobar(m.gps().get("mapa") == 530,
                         "Shattrath permitida en etapa 8"):
            return
        m.comando("go xyz 16223.0 16267.8 13.137 1", 2)
        if not comprobar(m.gps().get("mapa") == 1,
                         "vuelta a la sede tras probar Shattrath", origen="entorno"):
            return

        # El punto de llegada está a unos 10 m del portal; el uso exige menos de 5 m.
        m.comando("go xyz 16232.9 16264.1 13.556 1", 2)
        cerca = m.gps()
        if not comprobar(cerca and cerca.get("mapa") == 1 and
                         math.hypot(cerca["x"] - 16232.9, cerca["y"] - 16264.1) < 3,
                         "acercamiento al portal original", cerca, origen="entorno"):
            return
        m.bombear(1)
        portal = _objeto(m, PORTAL_VENTORMENTA)
        if not comprobar(bool(portal), "portal inicial de Ventormenta visible", portal):
            return
        m.enviar(0x0B1, struct.pack("<Q", portal))  # CMSG_GAMEOBJ_USE
        m.bombear(3)
        ciudad = m.gps()
        if not comprobar(ciudad and ciudad.get("mapa") == 0,
                         "portal devuelve a Ventormenta", ciudad):
            return
        m.comando("gh teleport", 2)
        m.bombear(1)
        despues_cd = m.gps()
        if not comprobar(despues_cd and despues_cd.get("mapa") == 0,
                         "comando respeta el enfriamiento de la piedra", despues_cd):
            return

        m.comando("go xyz %.3f %.3f %.3f 0" % (ctx.p["x"], ctx.p["y"], ctx.p["z"]), 2)
        menu = m.conversar(vendedor)
        teleportar = _opcion(menu[1], "teletransport", "teleport") if menu else None
        if not comprobar(teleportar is not None,
                         "teletransporte del vendedor disponible", menu):
            return
        m.elegir(vendedor, menu[0], teleportar["id"], timeout=2)
        m.bombear(1)
        if not comprobar(m.gps().get("mapa") == 0,
                         "vendedor respeta el enfriamiento de la piedra"):
            return

        ranura_hogar, guid_hogar = _ranura_item(m, 6948)
        if not comprobar(ranura_hogar is not None and guid_hogar,
                         "piedra de hogar común sigue en la mochila", guid_hogar):
            return
        fallo_hogar = m.usar_objeto(ranura_hogar, 8690, guid_hogar, espera=1)
        if not comprobar(fallo_hogar is None,
                         "piedra de hogar no comparte enfriamiento", fallo_hogar):
            return
        m.bombear(11)
        destino_hogar = m.gps()
        if not comprobar(destino_hogar and destino_hogar.get("mapa") == 0 and
                         math.hypot(destino_hogar["x"] - ctx.p["x"],
                                    destino_hogar["y"] - ctx.p["y"]) > 100,
                         "piedra común lleva al hogar y no a la sede", destino_hogar):
            return

        m.comando("go xyz %.3f %.3f %.3f 0" % (ctx.p["x"], ctx.p["y"], ctx.p["z"]), 2)
        menu = m.conversar(vendedor)
        vender = _opcion(menu[1], "vender", "sell") if menu else None
        if not comprobar(vender is not None, "opción de venta disponible", menu):
            return
        oro_antes_venta = m.valor_propio(upd.PLAYER_FIELD_COINAGE)
        m.elegir(vendedor, menu[0], vender["id"], timeout=2)
        m.bombear(1)
        oro_final = m.valor_propio(upd.PLAYER_FIELD_COINAGE)
        comprobar(oro_final - oro_antes_venta == 5000000,
                 "venta devuelve la mitad del precio original", oro_final - oro_antes_venta,
                 5000000)
        menu = m.conversar(vendedor)
        comprobar(menu is not None and _opcion(menu[1], "comprar", "buy") is not None,
                 "sede retirada y compra disponible de nuevo", menu)
        inf.datos["sede"] = {"guild_id": guild_id, "oro_compra": oro_antes - oro_despues,
                             "oro_reembolso": oro_final - oro_antes_venta,
                             "piedra_guid": guid_piedra, "portal_guid": portal}


@caso(id="guildhouse-aislamiento", titulo="SP02: dos hermandades en fases aisladas",
      descripcion="Dos cuentas fundan y compran sendas sedes. La primera instala una "
                  "posadera; la segunda no la ve ni puede usar el portal ajeno. Ambas "
                  "salen por su portal y disuelven sus hermandades temporales.",
      etiquetas=("sp02", "guildhouse", "dos-cuentas"),
      acciones=("conectar", "leer", "personaje", "gm", "grupo"),
      duracion_max=300, protege=("SP02", "mod-guildhouse"), control="directo",
      en_todo=False,
      observa=("dos sedes compradas", "portales con GUID distintos",
               "posadera visible solo en su fase", "portal ajeno denegado",
               "ambos portales propios funcionan", "disolución de ambas hermandades"),
      no_cubre=("más de dos hermandades", "bots", "reinicio del servidor"))
def ejecutar_aislamiento(ctx):
    if not ctx.entorno.tiene_cuenta("secundaria"):
        raise Bloqueo("falta cuentas_adicionales.secundaria en el perfil")
    sec, inf = "guildhouse-aislamiento", ctx.inf
    a, b = ctx.nueva_sesion(), ctx.nueva_sesion("secundaria")
    estado = {"a": None, "b": None}

    def comprobar(condicion, titulo, observado=None, esperado=None, origen="desarrollo"):
        return inf.comprobar(sec, condicion, titulo, observado=observado,
                            esperado=esperado, origen=origen)

    def limpiar(m, clave):
        if estado[clave]:
            m.comando("hermandad desactivar", 0.5)
            m.comando('guild delete "%s"' % estado[clave], 2)
            comprobar(m.valor_propio(upd.PLAYER_GUILDID) == 0,
                     "hermandad %s disuelta" % clave, origen="entorno")

    def comprar(m, pj, clave):
        m.comando("gm on", 0.5)
        m.comando("go xyz -8884.700 616.700 95.600 0", 2)
        m.comando("gm off", 0.5)
        vendedor = _objeto(m, VENDEDOR)
        if not comprobar(bool(vendedor), "vendedor visible para %s" % clave,
                         vendedor, origen="entorno"):
            return None
        m.comando('guild create %s "%s"' % (pj.nombre, pj.nombre), 1)
        guild_id = m.valor_propio(upd.PLAYER_GUILDID)
        if not comprobar(guild_id != 0, "hermandad %s creada" % clave,
                         guild_id, origen="entorno"):
            return None
        estado[clave] = pj.nombre
        m.comando("hermandad desactivar", 0.5)
        m.comando("modify money 12000000", 1)
        if not comprobar(m.valor_propio(upd.PLAYER_FIELD_COINAGE) >= 12000000,
                         "fondos de %s preparados" % clave, origen="entorno"):
            return None
        menu = m.conversar(vendedor)
        comprar_op = _opcion(menu[1], "comprar", "buy") if menu else None
        if not comprobar(comprar_op is not None, "compra disponible para %s" % clave,
                         menu):
            return None
        submenu = m.elegir(vendedor, menu[0], comprar_op["id"])
        isla = _opcion(submenu[1], "isla", "island") if submenu else None
        if not comprobar(isla is not None, "isla ofertada a %s" % clave, submenu):
            return None
        oro_antes = m.valor_propio(upd.PLAYER_FIELD_COINAGE)
        bolsas_antes = m.objetos_bolsas()
        m.elegir(vendedor, submenu[0], isla["id"], timeout=2)
        m.bombear(1)
        if not comprobar(oro_antes - m.valor_propio(upd.PLAYER_FIELD_COINAGE) == 10000000,
                         "sede %s comprada por 1.000 oro" % clave):
            return None
        idx, piedra = _nueva_ranura(bolsas_antes, m.objetos_bolsas())
        if not comprobar(idx is not None and piedra,
                         "piedra entregada a %s" % clave, piedra):
            return None
        if not comprobar(m.usar_objeto(MOCHILA_BASE + idx, PIEDRA, piedra, espera=1) is None,
                         "piedra de %s aceptada" % clave):
            return None
        m.bombear(11)
        gps = m.gps()
        if not comprobar(gps and gps.get("mapa") == 1,
                         "%s entra en su sede" % clave, gps):
            return None
        m.comando("go xyz 16232.9 16264.1 13.556 1", 2)
        m.bombear(1)
        portal = _objeto(m, PORTAL_VENTORMENTA)
        if not comprobar(bool(portal), "portal propio visible para %s" % clave,
                         portal):
            return None
        return guild_id, portal

    with PersonajeTemporal(ctx, a, 1, 1, sec,
                           antes_de_salir=lambda: limpiar(a, "a")) as pa:
        with PersonajeTemporal(ctx, b, 1, 1, sec,
                               antes_de_salir=lambda: limpiar(b, "b")) as pb:
            ctx.anotar_servidor(a)
            sede_a = comprar(a, pa, "a")
            if not sede_a:
                return
            a.comando("go xyz 16202.185 16255.917 21.160 1", 2)
            a.bombear(1)
            mayordomo = _objeto(a, MAYORDOMO)
            if not comprobar(bool(mayordomo), "mayordomo de A visible", mayordomo):
                return
            menu = a.conversar(mayordomo)
            posadera = _opcion(menu[1], "taberner", "posader", "innkeeper") if menu else None
            if not comprobar(posadera is not None, "posadera ofertada a A", menu):
                return
            a.elegir(mayordomo, menu[0], posadera["id"], timeout=2)
            a.bombear(2)
            if not comprobar(bool(_objeto(a, POSADERA)), "A ve su posadera"):
                return
            sede_b = comprar(b, pb, "b")
            if not sede_b:
                return
            comprobar(sede_a[0] != sede_b[0] and sede_a[1] != sede_b[1],
                     "hermandades y portales distintos", (sede_a, sede_b))
            comprobar(not _objeto(b, POSADERA), "B no ve la posadera de A")
            comprobar(_objeto(b, PORTAL_VENTORMENTA) == sede_b[1],
                     "B solo ve su portal", _objeto(b, PORTAL_VENTORMENTA))
            b.enviar(0x0B1, struct.pack("<Q", sede_a[1]))
            b.bombear(2)
            comprobar(b.gps().get("mapa") == 1, "portal de A no transporta a B")
            b.enviar(0x0B1, struct.pack("<Q", sede_b[1]))
            b.bombear(2)
            comprobar(b.gps().get("mapa") == 0, "B sale por su portal")
            a.comando("go xyz 16232.9 16264.1 13.556 1", 2)
            a.enviar(0x0B1, struct.pack("<Q", sede_a[1]))
            a.bombear(2)
            comprobar(a.gps().get("mapa") == 0, "A sale por su portal")
            b.comando('guild delete "%s"' % estado["b"], 2)
            if not comprobar(b.valor_propio(upd.PLAYER_GUILDID) == 0,
                             "hermandad B disuelta antes que A"):
                return
            estado["b"] = None
            a.comando("go xyz 16223.0 16267.8 13.137 1", 2)
            a.bombear(1)
            comprobar(_objeto(a, PORTAL_VENTORMENTA) == sede_a[1] and
                     bool(_objeto(a, POSADERA)),
                     "portal y posadera de A sobreviven a la disolución de B")
            a.comando("go xyz 16232.9 16264.1 13.556 1", 2)
            a.enviar(0x0B1, struct.pack("<Q", sede_a[1]))
            a.bombear(2)
            comprobar(a.gps().get("mapa") == 0,
                     "A sigue pudiendo salir tras disolver B")
            inf.datos["sedes"] = {"guild_a": sede_a[0], "guild_b": sede_b[0],
                                  "portal_a": sede_a[1], "portal_b": sede_b[1]}


@caso(id="guildhouse-invitado", titulo="SP02: compañero bot invitado a la sede",
      descripcion="Un jugador con sede forma grupo con un bot ajeno a su hermandad, "
                  "usa la piedra y comprueba que el bot le sigue dentro de su fase y "
                  "regresa con él. Despide el grupo, vende la sede y borra la hermandad.",
      etiquetas=("sp02", "guildhouse", "bots", "grupo"),
      acciones=("conectar", "leer", "personaje", "gm", "grupo"),
      duracion_max=360, protege=("SP02", "mod-guildhouse", "mod-party-here"),
      control="directo", en_todo=False,
      observa=("bot en grupo", "bot visible en fase de sede",
               "bot visible de vuelta en Ventormenta", "grupo despedido"),
      no_cubre=("bots no agrupados", "colas", "bots de otras hermandades propietarias"))
def ejecutar_invitado(ctx):
    sec, inf = "guildhouse-invitado", ctx.inf
    m = ctx.nueva_sesion()
    estado = {"guild": None, "grupo": False}

    def comprobar(condicion, titulo, observado=None, origen="desarrollo"):
        return inf.comprobar(sec, condicion, titulo, observado=observado, origen=origen)

    def limpiar():
        if estado["grupo"]:
            m.comando("grupo fuera", 3)
        if estado["guild"]:
            m.comando("hermandad desactivar", 0.5)
            m.comando('guild delete "%s"' % estado["guild"], 2)
            comprobar(m.valor_propio(upd.PLAYER_GUILDID) == 0,
                     "hermandad temporal disuelta", origen="entorno")

    with PersonajeTemporal(ctx, m, 1, 8, sec, antes_de_salir=limpiar) as pj:
        ctx.anotar_servidor(m)
        m.comando("levelup 24", 2)
        if not comprobar(m.valor_propio(upd.UNIT_FIELD_LEVEL) == 25,
                         "nivel 25 preparado", m.valor_propio(upd.UNIT_FIELD_LEVEL),
                         origen="entorno"):
            return
        m.comando("gm on", 0.5)
        m.comando("go xyz -8884.700 616.700 95.600 0", 2)
        m.comando("gm off", 0.5)
        vendedor = _objeto(m, VENDEDOR)
        if not comprobar(bool(vendedor), "vendedor visible", vendedor, origen="entorno"):
            return
        m.comando('guild create %s "%s"' % (pj.nombre, pj.nombre), 2)
        guild_id = m.valor_propio(upd.PLAYER_GUILDID)
        if not comprobar(guild_id != 0, "hermandad creada", guild_id, origen="entorno"):
            return
        estado["guild"] = pj.nombre
        m.comando("hermandad desactivar", 0.5)
        m.comando("modify money 10000000", 1)
        if not comprobar(m.valor_propio(upd.PLAYER_FIELD_COINAGE) >= 10000000,
                         "fondos preparados", origen="entorno"):
            return
        menu = m.conversar(vendedor)
        comprar = _opcion(menu[1], "comprar", "buy") if menu else None
        submenu = m.elegir(vendedor, menu[0], comprar["id"]) if comprar else None
        isla = _opcion(submenu[1], "isla", "island") if submenu else None
        if not comprobar(isla is not None, "isla disponible", submenu):
            return
        bolsas_antes = m.objetos_bolsas()
        m.elegir(vendedor, submenu[0], isla["id"], timeout=2)
        m.bombear(1)
        idx, piedra = _nueva_ranura(bolsas_antes, m.objetos_bolsas())
        if not comprobar(idx is not None and piedra, "piedra entregada", piedra):
            return
        m.comando("grupo 1", 2)
        estado["grupo"] = True
        limite = time.time() + 60
        companeros = []
        while time.time() < limite:
            m.bombear(2)
            companeros = [x for x in (m.grupo or {}).get("miembros", [])
                          if x["guid"] != m.guid]
            if companeros:
                break
        if not comprobar(bool(companeros), "bot invitado al grupo", companeros):
            return
        bot_guid = companeros[0]["guid"]
        inf.datos["invitado"] = {"guild": guild_id, "bot_guid": bot_guid,
                                 "bot_nombre": companeros[0]["nombre"]}
        if not comprobar(m.usar_objeto(MOCHILA_BASE + idx, PIEDRA, piedra, espera=1) is None,
                         "piedra acepta el viaje con grupo"):
            return
        m.bombear(11)
        if not comprobar(m.gps().get("mapa") == 1, "jugador entra en la sede"):
            return
        m.comando("grupo sigue", 2)
        limite = time.time() + 120
        while time.time() < limite and bot_guid not in m.objetos:
            m.bombear(3)
        if not comprobar(bot_guid in m.objetos,
                         "bot invitado aparece en la fase de la sede", bot_guid):
            return
        m.comando("go xyz 16232.9 16264.1 13.556 1", 2)
        portal = _objeto(m, PORTAL_VENTORMENTA)
        if not comprobar(bool(portal), "portal propio visible", portal):
            return
        m.enviar(0x0B1, struct.pack("<Q", portal))
        m.bombear(3)
        if not comprobar(m.gps().get("mapa") == 0, "jugador sale por el portal"):
            return
        m.comando("grupo sigue", 2)
        limite = time.time() + 90
        while time.time() < limite and bot_guid not in m.objetos:
            m.bombear(3)
        comprobar(bot_guid in m.objetos, "bot invitado regresa con el jugador", bot_guid)
        m.comando("grupo fuera", 3)
        estado["grupo"] = False
        m.bombear(3)
        comprobar(not [x for x in (m.grupo or {}).get("miembros", [])
                      if x["guid"] != m.guid], "grupo despedido")
        m.comando("go xyz -8884.700 616.700 95.600 0", 2)
        menu = m.conversar(vendedor)
        vender = _opcion(menu[1], "vender", "sell") if menu else None
        if comprobar(vender is not None, "venta disponible", menu):
            m.elegir(vendedor, menu[0], vender["id"], timeout=2)


@caso(id="guildhouse-etapas", titulo="SP02: portal según la etapa del viajero",
      descripcion="El dueño en etapa 8 compra el portal de Shattrath. Invita a un "
                  "segundo jugador en etapa 0: este ve la sede y el portal, pero no "
                  "puede cruzarlo; el dueño sí puede. Después lo admite en la hermandad, "
                  "obtiene gratis su piedra y entra con ella. Ambos personajes se borran.",
      etiquetas=("sp02", "guildhouse", "dos-cuentas", "progresion"),
      acciones=("conectar", "leer", "personaje", "gm", "grupo"),
      duracion_max=240, protege=("SP02", "mod-guildhouse", "mod-individual-progression"),
      control="directo", en_todo=False,
      observa=("invitado ve el portal", "invitado etapa 0 queda en la isla",
               "dueño etapa 8 llega a Shattrath", "invitado sale por portal original",
               "miembro nuevo recibe piedra gratis y entra con ella"),
      no_cubre=("portal de Dalaran", "cambio de etapa durante el viaje"))
def ejecutar_etapas(ctx):
    if not ctx.entorno.tiene_cuenta("secundaria"):
        raise Bloqueo("falta cuentas_adicionales.secundaria en el perfil")
    sec, inf = "guildhouse-etapas", ctx.inf
    a, b = ctx.nueva_sesion(), ctx.nueva_sesion("secundaria")
    estado = {"guild": None, "grupo": False}

    def comprobar(condicion, titulo, observado=None, origen="desarrollo"):
        return inf.comprobar(sec, condicion, titulo, observado=observado, origen=origen)

    def limpiar():
        if estado["grupo"]:
            a.dejar_grupo()
        if estado["guild"]:
            a.comando("hermandad desactivar", 0.5)
            a.comando('guild delete "%s"' % estado["guild"], 2)
            comprobar(a.valor_propio(upd.PLAYER_GUILDID) == 0,
                     "hermandad temporal disuelta", origen="entorno")

    with PersonajeTemporal(ctx, a, 1, 1, sec, antes_de_salir=limpiar) as pa:
        with PersonajeTemporal(ctx, b, 1, 1, sec) as pb:
            ctx.anotar_servidor(a)
            a.comando("gm on", 0.5)
            a.comando("go xyz -8884.700 616.700 95.600 0", 2)
            a.comando("gm off", 0.5)
            vendedor = _objeto(a, VENDEDOR)
            if not comprobar(bool(vendedor), "vendedor visible", origen="entorno"):
                return
            a.comando('guild create %s "%s"' % (pa.nombre, pa.nombre), 1)
            if not comprobar(a.valor_propio(upd.PLAYER_GUILDID) != 0,
                             "hermandad creada", origen="entorno"):
                return
            estado["guild"] = pa.nombre
            a.comando("hermandad desactivar", 0.5)
            a.comando("modify money 11000000", 1)
            menu = a.conversar(vendedor)
            comprar = _opcion(menu[1], "comprar", "buy") if menu else None
            submenu = a.elegir(vendedor, menu[0], comprar["id"]) if comprar else None
            isla = _opcion(submenu[1], "isla", "island") if submenu else None
            if not comprobar(isla is not None, "isla ofertada", submenu):
                return
            a.elegir(vendedor, submenu[0], isla["id"], timeout=2)
            a.bombear(1)
            a.comando("ip set %s 8" % pa.nombre, 2)
            a.comando("go xyz 16202.185 16255.917 21.160 1", 2)
            mayordomo = _objeto(a, MAYORDOMO)
            if not comprobar(bool(mayordomo), "mayordomo visible", mayordomo):
                return
            menu = a.conversar(mayordomo)
            portales = _opcion(menu[1], "portal") if menu else None
            submenu = a.elegir(mayordomo, menu[0], portales["id"]) if portales else None
            shattrath = _opcion(submenu[1], "shattrath") if submenu else None
            if not comprobar(shattrath is not None, "Shattrath ofertada", submenu):
                return
            a.elegir(mayordomo, submenu[0], shattrath["id"], timeout=2)
            a.bombear(1)
            a.comando("go xyz 16211.1 16266.9 13.746 1", 2)
            if not comprobar(bool(_objeto(a, 500008)),
                             "dueño ve Shattrath antes de invitar", _objeto(a, 500008)):
                return
            a.invitar_grupo(pb.nombre)
            b.bombear(1)
            b.aceptar_grupo()
            a.bombear(2)
            b.bombear(2)
            if not comprobar(any(x["guid"] == pa.guid for x in
                             (b.grupo or {}).get("miembros", [])),
                             "invitado en el grupo del dueño", b.grupo):
                return
            estado["grupo"] = True
            b.comando("gm on", 0.5)
            b.comando("go xyz 16211.1 16266.9 13.746 1", 2)
            b.comando("gm off", 0.5)
            b.bombear(6)
            if not comprobar(b.gps().get("mapa") == 1,
                             "invitado permanece en la isla al llegar", b.gps()):
                return
            portal_b = _objeto(b, 500008)
            if not comprobar(bool(portal_b), "invitado ve Shattrath en la fase",
                             portal_b):
                return
            b.enviar(0x0B1, struct.pack("<Q", portal_b))
            b.bombear(2)
            if not comprobar(b.gps().get("mapa") == 1,
                             "invitado en etapa 0 no cruza a Shattrath"):
                return
            a.comando("go xyz 16211.1 16266.9 13.746 1", 2)
            portal_a = _objeto(a, 500008)
            if not comprobar(portal_a == portal_b,
                             "ambos ven el mismo portal", (portal_a, portal_b)):
                return
            a.enviar(0x0B1, struct.pack("<Q", portal_a))
            a.bombear(3)
            comprobar(a.gps().get("mapa") == 530,
                     "dueño en etapa 8 llega a Shattrath")
            b.comando("go xyz 16232.9 16264.1 13.556 1", 2)
            salida = _objeto(b, PORTAL_VENTORMENTA)
            if comprobar(bool(salida), "invitado ve el portal original", salida):
                b.enviar(0x0B1, struct.pack("<Q", salida))
                b.bombear(3)
                if not comprobar(b.gps().get("mapa") == 0,
                                 "invitado sale por el portal original"):
                    return
            a.comando('guild invite %s "%s"' % (pb.nombre, pa.nombre), 2)
            b.bombear(1)
            if not comprobar(b.valor_propio(upd.PLAYER_GUILDID) ==
                             a.valor_propio(upd.PLAYER_GUILDID),
                             "invitado incorporado a la hermandad"):
                return
            b.comando("gm on", 0.5)
            b.comando("go xyz -8884.700 616.700 95.600 0", 2)
            b.comando("gm off", 0.5)
            vendedor_b = _objeto(b, VENDEDOR)
            menu = b.conversar(vendedor_b) if vendedor_b else None
            if not comprobar(menu is not None and
                             _opcion(menu[1], "vender", "sell") is None,
                             "miembro sin rango de líder no puede vender", menu):
                return
            pedir = _opcion(menu[1], "piedra", "stone") if menu else None
            if not comprobar(pedir is not None, "miembro nuevo puede pedir piedra gratis",
                             menu):
                return
            bolsas_antes = b.objetos_bolsas()
            oro_antes = b.valor_propio(upd.PLAYER_FIELD_COINAGE)
            b.elegir(vendedor_b, menu[0], pedir["id"], timeout=2)
            b.bombear(1)
            idx, guid_piedra = _nueva_ranura(bolsas_antes, b.objetos_bolsas())
            if not comprobar(idx is not None and guid_piedra and
                             b.valor_propio(upd.PLAYER_FIELD_COINAGE) == oro_antes,
                             "piedra de miembro entregada sin coste", guid_piedra):
                return
            if not comprobar(b.usar_objeto(MOCHILA_BASE + idx, PIEDRA,
                                           guid_piedra, espera=1) is None,
                             "miembro usa su propia piedra"):
                return
            b.bombear(11)
            if not comprobar(b.gps().get("mapa") == 1,
                             "miembro entra en la sede con su piedra"):
                return
            b.comando("go xyz 16202.185 16255.917 21.160 1", 2)
            mayordomo_b = _objeto(b, MAYORDOMO)
            menu_b = b.conversar(mayordomo_b) if mayordomo_b else None
            comprobar(menu_b is not None and bool(menu_b[1]),
                     "rango de miembro puede consultar mejoras por defecto", menu_b)
            b.comando("go xyz 16232.9 16264.1 13.556 1", 2)
            salida_b = _objeto(b, PORTAL_VENTORMENTA)
            if comprobar(bool(salida_b), "miembro ve el portal original", salida_b):
                b.enviar(0x0B1, struct.pack("<Q", salida_b))
                b.bombear(3)
                comprobar(b.gps().get("mapa") == 0,
                         "miembro regresa por el portal original")


@caso(id="guildhouse-persistencia", titulo="SP02: sede y portal tras reinicio",
      descripcion="fase=crear compra una sede y deja al personaje desconectado; "
                  "tras reiniciar el worldserver, fase=verificar reconecta al "
                  "mismo personaje, usa su piedra, sale por el portal y retira "
                  "todos los datos temporales.",
      etiquetas=("sp02", "guildhouse", "persistencia"),
      acciones=("conectar", "leer", "personaje", "gm", "grupo"),
      parametros={"fase": Parametro(str, "crear", "fase del caso",
                                     opciones=("crear", "verificar")),
                  "personaje": Parametro(str, "", "nombre devuelto por fase=crear"),
                  "guid": Parametro(int, 0, "GUID devuelto por fase=crear", minimo=0),
                  "guild_name": Parametro(str, "", "hermandad devuelta por fase=crear")},
      duracion_max=150, protege=("SP02", "mod-guildhouse"), control="directo",
      en_todo=False,
      observa=("piedra persistente", "portal cargado tras reinicio",
               "venta, disolución y borrado del personaje"),
      no_cubre=("apagón durante una compra", "reconexión con el personaje dentro de la isla"))
def ejecutar_persistencia(ctx):
    sec, inf = "guildhouse-persistencia", ctx.inf
    m = ctx.nueva_sesion()

    def comprobar(condicion, titulo, observado=None, origen="desarrollo"):
        return inf.comprobar(sec, condicion, titulo, observado=observado, origen=origen)

    if ctx.p["fase"] == "crear":
        nombre = nombre_aleatorio(ctx.entorno.prefijo)
        resultado = m.crear_personaje(nombre, 1, 1)
        if not comprobar(resultado == CHAR_CREATE_SUCCESS, "personaje temporal creado",
                         resultado, origen="entorno"):
            return
        guid = next(p["guid"] for p in m.personajes() if p["nombre"] == nombre)
        registro.apuntar(ctx.entorno.host, m.usuario, nombre, guid, ctx.caso.id)
        inf.datos["personaje"] = {"nombre": nombre, "guid": guid,
                                   "guild_name": nombre}
        preservar = False
        guild_creada = False
        dentro = False
        try:
            m.entrar(guid, 1, nombre=nombre)
            dentro = True
            ctx.anotar_servidor(m)
            m.comando("gm on", 0.5)
            m.comando("go xyz -8884.700 616.700 95.600 0", 2)
            m.comando("gm off", 0.5)
            vendedor = _objeto(m, VENDEDOR)
            if not comprobar(bool(vendedor), "vendedor visible", vendedor,
                             origen="entorno"):
                return
            m.comando('guild create %s "%s"' % (nombre, nombre), 1)
            guild_creada = m.valor_propio(upd.PLAYER_GUILDID) != 0
            if not comprobar(guild_creada, "hermandad temporal creada",
                             origen="entorno"):
                return
            m.comando("hermandad desactivar", 0.5)
            m.comando("modify money 10000000", 1)
            menu = m.conversar(vendedor)
            comprar = _opcion(menu[1], "comprar", "buy") if menu else None
            submenu = m.elegir(vendedor, menu[0], comprar["id"]) if comprar else None
            isla = _opcion(submenu[1], "isla", "island") if submenu else None
            if not comprobar(isla is not None, "isla ofertada", submenu):
                return
            oro_antes = m.valor_propio(upd.PLAYER_FIELD_COINAGE)
            m.elegir(vendedor, submenu[0], isla["id"], timeout=2)
            m.bombear(1)
            piedra = _ranura_item(m, PIEDRA)
            if not comprobar(oro_antes - m.valor_propio(upd.PLAYER_FIELD_COINAGE) ==
                             10000000 and piedra[0] is not None,
                             "sede comprada y piedra en mochila", piedra):
                return
            preservar = True
            inf.datos["personaje"]["guild_id"] = m.valor_propio(upd.PLAYER_GUILDID)
        finally:
            if dentro:
                if guild_creada and not preservar:
                    m.comando('guild delete "%s"' % nombre, 2)
                m.salir()
            if not preservar:
                borrado = m.borrar_personaje(guid)
                if borrado == CHAR_DELETE_SUCCESS:
                    registro.quitar(ctx.entorno.host, guid)
                comprobar(borrado == CHAR_DELETE_SUCCESS,
                         "personaje retirado tras preparación incompleta",
                         borrado, origen="entorno")
        return

    nombre, guid, guild_name = (ctx.p[k] for k in
                                ("personaje", "guid", "guild_name"))
    if not nombre or not guid or not guild_name:
        raise Bloqueo("fase=verificar requiere personaje, guid y guild_name de fase=crear")
    dentro = False
    try:
        m.personajes()
        m.entrar(guid, 1, nombre=nombre)
        dentro = True
        ctx.anotar_servidor(m)
        if not comprobar(m.valor_propio(upd.PLAYER_GUILDID) != 0,
                         "hermandad persiste tras reinicio"):
            return
        ranura, piedra = _ranura_item(m, PIEDRA)
        if not comprobar(ranura is not None, "piedra persiste tras reinicio",
                         piedra):
            return
        if not comprobar(m.usar_objeto(ranura, PIEDRA, piedra, espera=1) is None,
                         "piedra funciona tras reinicio"):
            return
        m.bombear(11)
        if not comprobar(m.gps().get("mapa") == 1,
                         "piedra lleva a la sede persistida"):
            return
        m.comando("go xyz 16232.9 16264.1 13.556 1", 2)
        portal = _objeto(m, PORTAL_VENTORMENTA)
        if not comprobar(bool(portal), "portal reaparece tras reinicio", portal):
            return
        m.enviar(0x0B1, struct.pack("<Q", portal))
        m.bombear(3)
        comprobar(m.gps().get("mapa") == 0,
                 "portal de salida funciona tras reinicio")
    finally:
        if dentro:
            m.comando("gm on", 0.5)
            m.comando("go xyz -8884.700 616.700 95.600 0", 2)
            m.comando("gm off", 0.5)
            vendedor = _objeto(m, VENDEDOR)
            menu = m.conversar(vendedor) if vendedor else None
            vender = _opcion(menu[1], "vender", "sell") if menu else None
            if vender:
                m.elegir(vendedor, menu[0], vender["id"], timeout=2)
                m.bombear(1)
            comprobar(vender is not None, "sede vendida al terminar", menu)
            m.comando("hermandad desactivar", 0.5)
            m.comando('guild delete "%s"' % guild_name, 2)
            comprobar(m.valor_propio(upd.PLAYER_GUILDID) == 0,
                     "hermandad temporal disuelta", origen="entorno")
            m.salir()
        borrado = m.borrar_personaje(guid)
        if borrado == CHAR_DELETE_SUCCESS:
            registro.quitar(ctx.entorno.host, guid)
        comprobar(borrado == CHAR_DELETE_SUCCESS,
                 "personaje temporal borrado", borrado, origen="entorno")


@caso(id="guildhouse-horda", titulo="SP02: servicios ARAC y portal de la Horda",
      descripcion="Un sacerdote orco compra sede desde Orgrimmar, invoca su instructor y compra el portal de Dalaran, "
                  "comprueba su bloqueo en etapa 0 y su apertura en etapa 13; "
                  "sale por el portal inicial a Orgrimmar y vende la sede.",
      etiquetas=("sp02", "guildhouse", "horda", "progresion"),
      acciones=("conectar", "leer", "personaje", "gm", "grupo"),
      duracion_max=180, protege=("SP02", "mod-guildhouse"), control="directo",
      en_todo=False,
      observa=("instructor de sacerdote orco funcional",
               "portal de Dalaran bloqueado en etapa 0",
               "Dalaran accesible en etapa 13", "portal inicial a Orgrimmar"),
      no_cubre=("otros portales de la Horda", "viaje en grupo"))
def ejecutar_horda(ctx):
    sec, inf = "guildhouse-horda", ctx.inf
    m = ctx.nueva_sesion()
    estado = {"guild": None}

    def comprobar(condicion, titulo, observado=None, origen="desarrollo"):
        return inf.comprobar(sec, condicion, titulo, observado=observado, origen=origen)

    def limpiar():
        if estado["guild"]:
            m.comando("hermandad desactivar", 0.5)
            m.comando('guild delete "%s"' % estado["guild"], 2)
            comprobar(m.valor_propio(upd.PLAYER_GUILDID) == 0,
                     "hermandad de la Horda disuelta", origen="entorno")

    with PersonajeTemporal(ctx, m, 2, 5, sec, antes_de_salir=limpiar) as pj:
        ctx.anotar_servidor(m)
        m.comando("gm on", 0.5)
        m.comando("go xyz 1575.000 -4291.900 26.500 1", 2)
        m.comando("gm off", 0.5)
        vendedor = _objeto(m, VENDEDOR)
        if not comprobar(bool(vendedor), "vendedor en Orgrimmar", vendedor,
                         origen="entorno"):
            return
        m.comando('guild create %s "%s"' % (pj.nombre, pj.nombre), 1)
        if not comprobar(m.valor_propio(upd.PLAYER_GUILDID) != 0,
                         "hermandad orca creada", origen="entorno"):
            return
        estado["guild"] = pj.nombre
        m.comando("hermandad desactivar", 0.5)
        m.comando("modify money 12000000", 1)
        menu = m.conversar(vendedor)
        comprar = _opcion(menu[1], "comprar", "buy") if menu else None
        submenu = m.elegir(vendedor, menu[0], comprar["id"]) if comprar else None
        isla = _opcion(submenu[1], "isla", "island") if submenu else None
        if not comprobar(isla is not None, "sede ofertada a la Horda", submenu):
            return
        m.elegir(vendedor, submenu[0], isla["id"], timeout=2)
        m.bombear(1)
        ranura, piedra = _ranura_item(m, PIEDRA)
        if not comprobar(ranura is not None, "piedra entregada al orco", piedra):
            return
        if not comprobar(m.usar_objeto(ranura, PIEDRA, piedra, espera=1) is None,
                         "piedra de la Horda aceptada"):
            return
        m.bombear(11)
        if not comprobar(m.gps().get("mapa") == 1,
                         "orco entra en su sede"):
            return
        m.comando("go xyz 16202.185 16255.917 21.160 1", 2)
        mayordomo = _objeto(m, MAYORDOMO)
        menu = m.conversar(mayordomo) if mayordomo else None
        instructores = _opcion(menu[1], "instructor de clase", "class trainer") if menu else None
        submenu = m.elegir(mayordomo, menu[0], instructores["id"]) if instructores else None
        sacerdote = _opcion(submenu[1], "sacerdote", "priest") if submenu else None
        if not comprobar(sacerdote is not None, "instructor de sacerdote ofertado", submenu):
            return
        m.elegir(mayordomo, submenu[0], sacerdote["id"], timeout=2)
        m.bombear(1)
        m.comando("go xyz 16227.9 16275.9 20.926 1", 2)
        instructor = _objeto(m, 26328)
        if not comprobar(bool(instructor), "instructor de sacerdote invocado", instructor):
            return
        lista = m.lista_instructor(instructor)
        if not comprobar(lista is not None and bool(lista["hechizos"]),
                         "instructor atiende al sacerdote orco", lista):
            return
        m.comando("go xyz 16202.185 16255.917 21.160 1", 2)
        menu = m.conversar(mayordomo) if mayordomo else None
        portales = _opcion(menu[1], "portal") if menu else None
        submenu = m.elegir(mayordomo, menu[0], portales["id"]) if portales else None
        dalaran = _opcion(submenu[1], "dalaran") if submenu else None
        if not comprobar(dalaran is not None, "Dalaran ofertada a la Horda", submenu):
            return
        m.elegir(mayordomo, submenu[0], dalaran["id"], timeout=2)
        m.bombear(1)
        m.comando("go xyz 16213.9 16270.5 13.138 1", 2)
        portal_dalaran = _objeto(m, 500009)
        if not comprobar(bool(portal_dalaran), "portal de Dalaran visible",
                         portal_dalaran):
            return
        m.enviar(0x0B1, struct.pack("<Q", portal_dalaran))
        m.bombear(2)
        if not comprobar(m.gps().get("mapa") == 1,
                         "Dalaran denegada en etapa 0"):
            return
        m.comando("ip set %s 13" % pj.nombre, 2)
        m.enviar(0x0B1, struct.pack("<Q", portal_dalaran))
        m.bombear(3)
        if not comprobar(m.gps().get("mapa") == 571,
                         "Dalaran permitida en etapa 13"):
            return
        m.comando("go xyz 16232.9 16264.1 13.556 1", 2)
        portal_org = _objeto(m, 500004)
        if not comprobar(bool(portal_org) and not _objeto(m, PORTAL_VENTORMENTA),
                         "solo portal inicial de Orgrimmar visible", portal_org):
            return
        m.enviar(0x0B1, struct.pack("<Q", portal_org))
        m.bombear(3)
        gps = m.gps()
        if not comprobar(gps and gps.get("mapa") == 1 and
                         math.hypot(gps["x"] - 1470, gps["y"] + 4222) < 100,
                         "portal original devuelve a Orgrimmar", gps):
            return
        m.comando("go xyz 1575.000 -4291.900 26.500 1", 2)
        menu = m.conversar(vendedor)
        vender = _opcion(menu[1], "vender", "sell") if menu else None
        if comprobar(vender is not None, "venta disponible en Orgrimmar", menu):
            m.elegir(vendedor, menu[0], vender["id"], timeout=2)
