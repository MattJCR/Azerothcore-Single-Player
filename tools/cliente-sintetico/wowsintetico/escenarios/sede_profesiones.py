# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""SP07: la sede de hermandad como centro de profesiones.

Ling (mod-reagent-bank, 290011) se compra al mayordomo de la sede como una mejora
más. Su banco es de cada personaje —no de la hermandad—, así que es el mismo que el
de las capitales y sobrevive a vender la sede. Estos casos lo comprueban contra el
servidor real: precio y texto del cuadro de confirmación, NPC en la fase de la sede,
menú en español, depósito y retirada con los objetos reales de la mochila, el mismo
stock desde la Ling de la capital, y el recorrido cofre de SP06 → banco → fragua.
"""
import math
import re
import struct
import time

from .. import actualizaciones as upd
from .. import registro
from ..binario import Escritor
from ..catalogo import Parametro, caso
from ..ejecutor import Bloqueo
from ..mundo import (CHAR_CREATE_SUCCESS, CHAR_DELETE_SUCCESS, CMSG_GOSSIP_SELECT_OPTION,
                     SMSG_LOOT_RESPONSE, _leer_respuesta_loot)
from . import PersonajeTemporal, nombre_aleatorio, sin_colores
from .guildhouse import (MAYORDOMO, MOCHILA_BASE, PIEDRA, VENDEDOR, _objeto, _opcion,
                         _ranura_item)
from .objetos import _nueva_ranura
from .profesiones import _paquete_hechizo
from .tesoros_botin import _BASICO

LING = 290011
FORJA = 1685
POSADERA = 500032
ITEM_FIELD_STACK_COUNT = 0x000E
CMSG_AUTOSTORE_LOOT_ITEM = 0x108
PRECIO_LING = 1000000            # GuildHouseReagentBank (100 oro, como el banco)
PRECIO_FORJA = 500000            # GuildHouseObject (50 oro)
PRECIO_SEDE = 10000000

# (mapa, x, y, z) del vendedor de sedes y de la Ling de cada capital (creature en acore_world)
VENDEDORES = {"alianza": (0, -8884.700, 616.700, 95.600), "horda": (1, 1575.000, -4291.900, 26.500)}
LING_CAPITAL = {"alianza": (0, -8844.13, 630.524, 95.309), "horda": (1, 1678.75, -4311.83, 62.13)}
RAZA_CLASE = {"alianza": (1, 8), "horda": (2, 5)}
MAYORDOMO_POS = (16202.185, 16255.917, 21.160)
LING_POS = (16227.0, 16283.0, 13.176)
FORJA_POS = (16252.0, 16294.3, 13.176)
PORTAL_INICIAL = {"alianza": (500000, (16232.9, 16264.1, 13.556)),
                  "horda": (500004, (16232.9, 16264.1, 13.556))}

# etiquetas que deben estar traducidas (sin el icono) y sus equivalentes inglesas del módulo original
CATEGORIAS_ES = ("Piezas", "Explosivos", "Dispositivos", "Joyería", "Tela", "Cuero", "Metal y piedra",
                 "Carne", "Hierbas", "Elementales", "Encantamiento", "Material abisal",
                 "Otras mercancías", "Vitela de armadura", "Vitela de arma")
CATEGORIAS_EN = {"Parts", "Explosives", "Devices", "Jewelcrafting", "Cloth", "Leather", "Metal & Stone",
                 "Meat", "Herb", "Elemental", "Enchanting", "Nether Material", "Other Trade Goods",
                 "Armor Vellum", "Weapon Vellum", "Deposit All Reagents", "Previous Page", "Next Page",
                 "Back..."}
DEPOSITAR = "Depositar todos los materiales"
CATEGORIA_DE = {2589: "tela", 2318: "cuero", 2770: "metal y piedra", 2447: "hierbas", 1206: "joyer",
                2840: "metal y piedra"}
# 24 entradas distintas de "Metal y piedra" (clase 7, subclase 7, apilables, sin ligadura)
METAL_PIEDRA = (2770, 2771, 2772, 2775, 2776, 2835, 2836, 2838, 2840, 2841, 2842, 3470,
                3478, 3486, 3575, 3576, 3577, 3857, 3858, 3859, 3860, 3861, 7911, 7912)


# ── lectura del estado real ────────────────────────────────────────────────

def _etiqueta(texto):
    """Texto de una opción sin colores ni el icono |TInterface...|t."""
    return re.sub(r"\|T[^|]*\|t", "", sin_colores(texto)).strip()


def _cantidad(m, entry):
    """Unidades de `entry` en las 16 ranuras de la mochila, según los campos de actualización."""
    total = 0
    for bajo, alto in m.objetos_bolsas():
        guid = bajo | (alto << 32)
        valores = m.objetos.get(guid, {}).get("valores", {}) if guid else {}
        if valores.get(upd.OBJECT_FIELD_ENTRY) == entry:
            total += valores.get(ITEM_FIELD_STACK_COUNT, 1)
    return total


def _filas(opciones):
    """entrada -> (cantidad, opción) de un menú de categoría del banco ("[Tela de lino] (45)")."""
    filas = {}
    for o in opciones:
        hallado = re.search(r"\|Hitem:(\d+):[^|]*\|h\[[^\]]*\]\|h\s*\((\d+)\)\s*$", sin_colores(o["texto"]))
        if hallado:
            filas[int(hallado.group(1))] = (int(hallado.group(2)), o)
    return filas


def _ver_categoria(m, ling, palabra, pagina=0):
    """Abre el menú de Ling, entra en la categoría y avanza `pagina` páginas.
    Devuelve (menu, opciones) o None."""
    menu = m.conversar(ling)
    if not menu:
        return None
    elegida = (next((o for o in menu[1] if _etiqueta(o["texto"]).lower() == palabra), None) or
               _opcion(menu[1], palabra))             # etiqueta exacta antes que por contenido
    if not elegida:
        return None
    sub = m.elegir(ling, menu[0], elegida["id"])
    for _ in range(pagina):
        siguiente = _opcion(sub[1], "página siguiente") if sub else None
        if not siguiente:
            return None
        sub = m.elegir(ling, sub[0], siguiente["id"])
    return sub


CATEGORIAS_STOCK = ("tela", "cuero", "metal y piedra", "hierbas", "joyer")
CATEGORIAS_TODAS = tuple(c.lower().rstrip("í") if c == "Joyería" else c.lower() for c in CATEGORIAS_ES)


def _stock_detallado(m, ling, categorias=CATEGORIAS_STOCK):
    """entrada -> (cantidad, palabra de su categoría) en la primera página de esas categorías."""
    total = {}
    for palabra in categorias:
        sub = _ver_categoria(m, ling, palabra)
        for entrada, (cantidad, _) in _filas(sub[1] if sub else []).items():
            total[entrada] = (cantidad, palabra)
    return total


def _stock(m, ling, categorias=CATEGORIAS_STOCK):
    """entrada -> cantidad guardada en esas categorías del banco."""
    return {e: c for e, (c, _) in _stock_detallado(m, ling, categorias).items()}


def _depositar(m, ling):
    menu = m.conversar(ling)
    elegida = _opcion(menu[1], "depositar todos") if menu else None
    if not elegida:
        return False
    m.elegir(ling, menu[0], elegida["id"], timeout=2)      # cierra el menú: no hay respuesta
    m.bombear(3)                                            # el depósito va por consulta asíncrona
    return True


def _retirar(m, ling, entrada, palabra):
    """Una retirada (una pila) de `entrada`; devuelve las unidades que llegaron a la mochila."""
    sub = _ver_categoria(m, ling, palabra)
    fila = _filas(sub[1] if sub else []).get(entrada)
    if not fila:
        return None
    antes = _cantidad(m, entrada)
    m.elegir(ling, sub[0], fila[1]["id"])
    m.bombear(1)
    return _cantidad(m, entrada) - antes


def _vaciar_banco(m, ling, categorias=CATEGORIAS_TODAS):
    """Retira todo lo guardado (libera la mochila con `.additem -n` entre retiradas) para que el
    banco del personaje de prueba quede sin filas en `custom_reagent_bank`."""
    retirado = {}
    for _ in range(60):
        pendiente = _stock_detallado(m, ling, categorias)
        if not pendiente:
            return retirado
        for entrada, (_, palabra) in pendiente.items():
            llegadas = _retirar(m, ling, entrada, palabra)
            if llegadas:
                retirado[entrada] = retirado.get(entrada, 0) + llegadas
                m.comando("additem %d -%d" % (entrada, _cantidad(m, entrada)), 0.7)
    return retirado


def _fuera_de_la_isla(gps):
    """La isla de los MJ está en Kalimdor (mapa 1) hacia x=16200; Orgrimmar es también el mapa 1."""
    return bool(gps) and (gps.get("mapa") != 1 or gps["x"] < 15000)


def _mensajes_desde(m, indice):
    return [t for _, t in m.mensajes[indice:]]


# ── la sede ────────────────────────────────────────────────────────────────

class Sede:
    """Compra, entrada, mejoras y venta de una sede temporal, con la comprobación de cada paso."""

    def __init__(self, ctx, m, pj, sec, bando="alianza", home_guild=False):
        self.ctx, self.m, self.pj, self.sec, self.bando = ctx, m, pj, sec, bando
        self.home_guild = home_guild
        self.inf = ctx.inf
        self.guild = None            # nombre de la hermandad creada
        self.guild_id = 0
        self.comprada = False
        self.piedra = None           # (ranura, guid)
        self.depositos = False       # ¿queda algo en el banco por limpiar?

    def comprobar(self, condicion, titulo, observado=None, esperado=None, origen="desarrollo"):
        return self.inf.comprobar(self.sec, condicion, titulo, observado=observado,
                                  esperado=esperado, origen=origen)

    def oro(self):
        return self.m.valor_propio(upd.PLAYER_FIELD_COINAGE)

    def ir(self, mapa, x, y, z, espera=2):
        self.m.comando("go xyz %.3f %.3f %.3f %d" % (x, y, z, mapa), espera)
        self.m.bombear(1)

    def ir_vendedor(self):
        mapa, x, y, z = VENDEDORES[self.bando]
        self.m.comando("gm on", 0.5)
        self.ir(mapa, x, y, z)
        self.m.comando("gm off", 0.5)

    def comprar(self, fondos=12000000, entrar=True):
        """Funda una hermandad temporal, compra la sede (precio original) y, salvo entrar=False,
        entra con la piedra."""
        m = self.m
        self.ir_vendedor()
        vendedor = _objeto(m, VENDEDOR)
        if not self.comprobar(bool(vendedor), "vendedor de sedes visible", vendedor, origen="entorno"):
            return False
        m.comando('guild create %s "%s"' % (self.pj.nombre, self.pj.nombre), 1)
        self.guild_id = m.valor_propio(upd.PLAYER_GUILDID)
        if not self.comprobar(self.guild_id != 0, "hermandad temporal creada", self.guild_id,
                              origen="entorno"):
            return False
        self.guild = self.pj.nombre
        if not self.home_guild:
            m.comando("hermandad desactivar", 0.5)
        m.comando("modify money %d" % fondos, 1)
        if not self.comprobar(self.oro() >= fondos, "fondos preparados", self.oro(), origen="entorno"):
            return False
        menu = m.conversar(vendedor)
        comprar = _opcion(menu[1], "comprar", "buy") if menu else None
        sub = m.elegir(vendedor, menu[0], comprar["id"]) if comprar else None
        isla = _opcion(sub[1], "isla", "island") if sub else None
        if not self.comprobar(isla is not None, "sede ofertada", sub):
            return False
        antes, bolsas = self.oro(), m.objetos_bolsas()
        m.elegir(vendedor, sub[0], isla["id"], timeout=2)
        m.bombear(1)
        self.comprada = True
        if not self.comprobar(antes - self.oro() == PRECIO_SEDE, "sede comprada por 1.000 oro",
                              antes - self.oro(), PRECIO_SEDE):
            return False
        idx, guid = _nueva_ranura(bolsas, m.objetos_bolsas())
        if not self.comprobar(idx is not None and guid, "piedra de la sede entregada", guid):
            return False
        self.piedra = (MOCHILA_BASE + idx, guid)
        return self.entrar() if entrar else True

    def entrar(self, reiniciar_enfriamiento=False):
        m = self.m
        ranura, guid = self.piedra
        if reiniciar_enfriamiento:           # la piedra tiene 30 minutos de enfriamiento propio
            m.seleccionar(m.guid)
            m.comando("cooldown", 1)
        if not self.comprobar(m.usar_objeto(ranura, PIEDRA, guid, espera=1) is None,
                              "piedra de la sede aceptada"):
            return False
        m.bombear(11)
        gps = m.gps()
        return self.comprobar(bool(gps) and gps.get("mapa") == 1 and
                              math.hypot(gps["x"] - 16223, gps["y"] - 16268) < 80,
                              "la piedra lleva a la sede", gps)

    def mayordomo(self):
        self.ir(1, *MAYORDOMO_POS)
        guid = _objeto(self.m, MAYORDOMO)
        self.comprobar(bool(guid), "mayordomo visible en la fase de la sede", guid)
        return guid

    def ofertas(self, mayordomo):
        """Opciones del menú principal del mayordomo (o None)."""
        menu = self.m.conversar(mayordomo)
        return menu

    def comprar_mejora(self, mayordomo, palabras, precio, titulo, ruta=None):
        """Elige una mejora (opcionalmente dentro de un submenú) y comprueba el cobro."""
        m = self.m
        menu = m.conversar(mayordomo)
        if ruta and menu:
            entrada = _opcion(menu[1], ruta)
            menu = m.elegir(mayordomo, menu[0], entrada["id"]) if entrada else None
        opcion = _opcion(menu[1], *palabras) if menu else None
        if not self.comprobar(opcion is not None, "%s ofertada" % titulo,
                              [o["texto"] for o in menu[1]] if menu else None):
            return False
        antes, mensajes = self.oro(), len(m.mensajes)
        m.elegir(mayordomo, menu[0], opcion["id"], timeout=2)
        m.bombear(2)
        return self.comprobar(antes - self.oro() == precio, "%s: precio cobrado" % titulo,
                              {"cobrado": antes - self.oro(), "mensajes": _mensajes_desde(m, mensajes)},
                              precio)

    def ling(self):
        """Va a Ling dentro de la sede y devuelve su GUID (None si no está)."""
        self.ir(1, *LING_POS)
        return _objeto(self.m, LING)

    def vender(self):
        m = self.m
        mapa, x, y, z = VENDEDORES[self.bando]
        m.comando("gm on", 0.5)
        self.ir(mapa, x, y, z)
        m.comando("gm off", 0.5)
        vendedor = _objeto(m, VENDEDOR)
        menu = m.conversar(vendedor) if vendedor else None
        vender = _opcion(menu[1], "vender", "sell") if menu else None
        if not self.comprobar(vender is not None, "venta de la sede disponible", menu):
            return False
        antes = self.oro()
        m.elegir(vendedor, menu[0], vender["id"], timeout=2)
        m.bombear(1)
        self.comprada = False
        return self.comprobar(self.oro() - antes == PRECIO_SEDE // 2,
                              "venta devuelve la mitad del precio de la sede", self.oro() - antes,
                              PRECIO_SEDE // 2)

    def ling_capital(self):
        """Va a la Ling de la capital del bando y devuelve su GUID."""
        mapa, x, y, z = LING_CAPITAL[self.bando]
        self.m.comando("gm on", 0.5)
        self.ir(mapa, x, y, z)
        self.m.comando("gm off", 0.5)
        return _objeto(self.m, LING)

    def limpiar(self):
        """Antes del logout: vacía el banco de prueba (las 15 categorías), vende la sede y disuelve la
        hermandad. Cada paso se protege del anterior: un fallo no deja la sede ni la hermandad vivas."""
        m = self.m
        if self.depositos:
            try:
                ling = self.ling_capital()
                if ling:
                    _vaciar_banco(m, ling)
                    resto = _stock_detallado(m, ling, CATEGORIAS_TODAS)
                    if resto:
                        self.inf.anotar(self.sec, "AVISO", "banco sin vaciar del todo",
                                        "quedan %s: borrar custom_reagent_bank de %s" % (resto, self.pj.guid))
                else:
                    self.inf.anotar(self.sec, "AVISO", "banco sin vaciar",
                                    "no se vio a Ling en la capital: borrar custom_reagent_bank de %s" % self.pj.guid)
            except Exception as e:                  # noqa: BLE001 — se anota y se sigue limpiando
                self.inf.anotar(self.sec, "AVISO", "banco sin vaciar",
                                "%r: borrar custom_reagent_bank de %s" % (e, self.pj.guid))
        try:
            if self.comprada:
                self.vender()
        finally:
            if self.guild:
                m.comando("hermandad desactivar", 0.5)
                m.comando('guild delete "%s"' % self.guild, 2)
                self.comprobar(m.valor_propio(upd.PLAYER_GUILDID) == 0, "hermandad temporal disuelta",
                               origen="entorno")


def _preparar_materiales(m, lote):
    """`.additem` de cada (entrada, cantidad) y comprobación contra la mochila real."""
    for entrada, cantidad in lote:
        m.comando("additem %d %d" % (entrada, cantidad), 0.7)
    m.bombear(1)
    return {entrada: _cantidad(m, entrada) for entrada, _ in lote}


def _comprobar_menu_principal(sede, ling):
    """Menú de Ling en español, sin etiquetas del módulo original en inglés."""
    menu = sede.m.conversar(ling)
    if not sede.comprobar(menu is not None and bool(menu[1]), "Ling atiende en la sede", menu):
        return False
    etiquetas = [_etiqueta(o["texto"]) for o in menu[1]]
    en_ingles = [e for e in etiquetas if e in CATEGORIAS_EN]
    ok = sede.comprobar(not en_ingles, "menú de Ling sin etiquetas en inglés", en_ingles, [])
    faltan = [c for c in CATEGORIAS_ES + (DEPOSITAR,) if c not in etiquetas]
    return sede.comprobar(not faltan, "menú de Ling con las 15 categorías y el depósito en español",
                          etiquetas, None) and ok


# ── caso principal ─────────────────────────────────────────────────────────

@caso(id="sede-profesiones", titulo="SP07: Ling, el banquero de materiales, en la sede",
      descripcion="Un personaje funda una hermandad temporal, compra la sede y le pide a Ling al "
                  "mayordomo (precio y confirmación en español, rechazo del segundo pedido). Deposita "
                  "materiales reales y comprueba la mochila, el menú en español y la retirada por "
                  "pilas; el mismo stock aparece en la Ling de la capital y sobrevive a vender la "
                  "sede. Con oferta=no comprueba que el mayordomo no la ofrece (apagado por "
                  "GuildHouseReagentBank = -1).",
      etiquetas=("sp07", "guildhouse", "profesiones"),
      acciones=("conectar", "leer", "personaje", "gm", "grupo"),
      parametros={
          "bando": Parametro(str, "alianza", "facción del personaje", opciones=("alianza", "horda")),
          "oferta": Parametro(bool, True, "no = la mejora está apagada y no debe ofrecerse"),
          "precio": Parametro(int, PRECIO_LING, "precio de Ling en cobre (GuildHouseReagentBank)", minimo=0),
          "home_guild": Parametro(bool, False, "no desactivar la hermandad de casa: sus bots entran "
                                               "en la hermandad temporal"),
      },
      duracion_max=420, protege=("SP07", "mod-guildhouse", "mod-reagent-bank"), control="directo",
      en_todo=False,
      observa=("opción de Ling en el mayordomo con su precio y texto de confirmación",
               "oro cobrado y segundo pedido rechazado sin cobro",
               "Ling visible en la fase de la sede", "menú de Ling íntegramente en español",
               "materiales retirados de la mochila al depositar y no depositados los demás objetos",
               "retirada por pilas hasta devolver todo", "mercancía genérica (subclase 0) recuperable", "mismo stock en la Ling de la capital",
               "stock conservado tras vender la sede"),
      no_cubre=("la retirada de Ling al vender la sede: sin sede, entrar en la isla devuelve al jugador a la "
                "capital antes de poder mirar; se comprueba por SQL (creature y mod_guildhouse_owned_spawn)",
                "invitados y bots en el grupo: casos sede-profesiones-grupo e -invitado",
                "reinicio del servidor: caso sede-profesiones-persistencia",
                "oro de las capitales: Ling no cuesta nada allí"))
def ejecutar(ctx):
    sec, inf = "sede-profesiones", ctx.inf
    bando, oferta, precio = ctx.p["bando"], ctx.p["oferta"], ctx.p["precio"]
    raza, clase = RAZA_CLASE[bando]
    m = ctx.nueva_sesion()
    holder = {}

    with PersonajeTemporal(ctx, m, raza, clase, sec,
                           antes_de_salir=lambda: holder["sede"].limpiar()) as pj:
        sede = holder["sede"] = Sede(ctx, m, pj, sec, bando, ctx.p["home_guild"])
        ctx.anotar_servidor(m)
        comprobar = sede.comprobar
        if not sede.comprar():
            return
        mayordomo = sede.mayordomo()
        if not mayordomo:
            return

        # 1) oferta del mayordomo
        menu = m.conversar(mayordomo)
        if not comprobar(menu is not None and bool(menu[1]), "menú del mayordomo abierto", menu):
            return
        opcion = _opcion(menu[1], "banquero de materiales")
        if not oferta:
            comprobar(opcion is None and not any("reagent" in o["texto"].lower() or "materiales" in o["texto"].lower()
                                                 for o in menu[1]),
                      "el mayordomo no ofrece a Ling con GuildHouseReagentBank = -1 (ni en inglés)",
                      [o["texto"] for o in menu[1]])
            comprobar(_opcion(menu[1], "banquero") is not None,
                      "el resto de mejoras (banquero) sigue ofertado", [o["texto"] for o in menu[1]])
            return
        if not comprobar(opcion is not None, "Ling ofertada por el mayordomo", [o["texto"] for o in menu[1]]):
            return
        comprobar(opcion["texto"] == "Invocar banquero de materiales",
                  "texto de la oferta en español", opcion["texto"], "Invocar banquero de materiales")
        comprobar(opcion["confirmacion"] == "¿Invocar banquero de materiales?",
                  "confirmación en español", opcion["confirmacion"], "¿Invocar banquero de materiales?")
        comprobar(opcion["dinero"] == precio, "cuadro de confirmación con el precio configurado",
                  opcion["dinero"], precio)
        comprobar(len(menu[1]) <= 32, "el menú cabe en las 32 opciones de un gossip", len(menu[1]))
        etiquetas = [_etiqueta(o["texto"]) for o in menu[1]]
        comprobar(len(set(etiquetas)) == len(etiquetas), "sin opciones repetidas en el menú", etiquetas)

        # 2) compra, cobro y segundo pedido
        antes, mensajes = sede.oro(), len(m.mensajes)
        m.elegir(mayordomo, menu[0], opcion["id"], timeout=2)
        m.bombear(2)
        if not comprobar(antes - sede.oro() == precio, "precio de Ling cobrado una vez",
                         {"cobrado": antes - sede.oro(), "mensajes": _mensajes_desde(m, mensajes)}, precio):
            return
        menu = m.conversar(mayordomo)
        otra = _opcion(menu[1], "banquero de materiales") if menu else None
        antes, mensajes = sede.oro(), len(m.mensajes)
        if otra:
            m.elegir(mayordomo, menu[0], otra["id"], timeout=2)
            m.bombear(2)
        comprobar(sede.oro() == antes, "un segundo pedido no cobra", antes - sede.oro(), 0)
        comprobar(any("Ya tienes este servicio" in t for t in _mensajes_desde(m, mensajes)),
                  "aviso de servicio repetido en español", _mensajes_desde(m, mensajes))

        # 3) Ling en la fase de la sede
        ling = sede.ling()
        if not comprobar(bool(ling), "Ling visible en la sede", ling):
            return
        if not _comprobar_menu_principal(sede, ling):
            return

        # 4) depósito, mochila y menú
        lote = ((2589, 45), (2770, 30), (2447, 25), (1206, 3), (25, 1))
        previos = _preparar_materiales(m, lote)
        if not comprobar(previos == {2589: 45, 2770: 30, 2447: 25, 1206: 3, 25: 1},
                         "materiales de prueba en la mochila", previos, origen="entorno"):
            return
        sede.depositos = True
        comprobar(_depositar(m, ling), "opción de depósito disponible")
        despues = {e: _cantidad(m, e) for e, _ in lote}
        comprobar(despues == {2589: 0, 2770: 0, 2447: 0, 1206: 0, 25: 1},
                  "los materiales salen de la mochila y la espada se queda", despues,
                  {2589: 0, 2770: 0, 2447: 0, 1206: 0, 25: 1})
        stock = _stock(m, ling)
        esperado = {2589: 45, 2770: 30, 2447: 25, 1206: 3}
        if not comprobar(stock == esperado, "el menú de Ling lista lo depositado, por categorías",
                         stock, esperado):
            return

        # 5) retirada por pilas
        llegadas = [_retirar(m, ling, 2589, "tela") for _ in range(3)]
        comprobar(llegadas == [20, 20, 5], "tres retiradas devuelven pilas de 20, 20 y 5", llegadas,
                  [20, 20, 5])
        comprobar(_cantidad(m, 2589) == 45, "las 45 unidades vuelven a la mochila", _cantidad(m, 2589), 45)
        stock = _stock(m, ling)
        comprobar(2589 not in stock and stock.get(2770) == 30,
                  "el banco ya no lista la tela y conserva el resto", stock)

        # 5b) mercancía genérica (subclase 0: Hierba mortal, reactivos de venenos) también se recupera
        m.comando("additem 5173 4", 0.7)
        m.bombear(1)
        comprobar(_cantidad(m, 5173) == 4, "hierba mortal (mercancía genérica) en la mochila",
                  _cantidad(m, 5173), 4, origen="entorno")
        _depositar(m, ling)
        otras = _stock(m, ling, ("otras mercanc",))
        comprobar(otras == {5173: 4}, "la mercancía genérica se lista en «Otras mercancías»", otras, {5173: 4})
        llegadas = _retirar(m, ling, 5173, "otras mercanc")
        comprobar(llegadas == 4, "la mercancía genérica se puede retirar", llegadas, 4)

        # 6) el banco es del personaje: lo mismo desde la capital y tras vender la sede
        if not sede.vender():
            return
        ling_capital = sede.ling_capital()
        if not comprobar(bool(ling_capital), "Ling de la capital visible", ling_capital, origen="entorno"):
            return
        stock = _stock(m, ling_capital)
        comprobar(stock.get(2770) == 30 and stock.get(2447) == 25 and stock.get(1206) == 3,
                  "el stock depositado en la sede sigue en la Ling de la capital", stock)
        retirado = _vaciar_banco(m, ling_capital)
        comprobar(retirado.get(2770) == 30 and retirado.get(2447) == 25 and retirado.get(1206) == 3,
                  "todo se retira en la capital", retirado)
        sede.depositos = False
        comprobar(not _stock(m, ling_capital), "banco del personaje vacío al terminar")
        inf.datos["sede"] = {"guild_id": sede.guild_id, "bando": bando, "retirado": retirado,
                             "precio": precio}


# ── Party Here y Home Guild ────────────────────────────────────────────────

@caso(id="sede-profesiones-grupo", titulo="SP07: Ling con un bot de Party Here en la sede",
      descripcion="Con la hermandad de casa activa (sus bots entran en la hermandad de la sede), el "
                  "personaje compra la sede y a Ling, forma grupo con un bot de party-here, entra con "
                  "la piedra y comprueba que el bot le sigue y que Ling atiende con el grupo dentro; "
                  "sale por el portal y el bot regresa con él. El stock depositado se ve en la capital.",
      etiquetas=("sp07", "guildhouse", "profesiones", "bots", "grupo"),
      acciones=("conectar", "leer", "personaje", "gm", "grupo"),
      parametros={
          "bando": Parametro(str, "alianza", "facción del personaje", opciones=("alianza", "horda")),
          "home_guild": Parametro(bool, True, "dejar activa la hermandad de casa (sus bots se unen)"),
      },
      duracion_max=600, protege=("SP07", "mod-guildhouse", "mod-reagent-bank", "mod-party-here",
                                 "mod-home-guild"),
      control="directo", en_todo=False,
      observa=("bot visible en la fase de la sede junto al jugador", "Ling atiende con el grupo dentro",
               "el depósito del jugador no depende de los bots", "bot de vuelta tras el portal",
               "hermandad del bot cuando la hermandad de casa está activa"),
      no_cubre=("bots que la hermandad de casa mantiene conectados lejos del jugador",
                "uso del banco por los bots: no tienen interfaz, no lo usan"))
def ejecutar_grupo(ctx):
    sec, inf = "sede-profesiones-grupo", ctx.inf
    bando = ctx.p["bando"]
    raza, clase = RAZA_CLASE[bando]
    m = ctx.nueva_sesion()
    holder = {"grupo": False}

    def limpiar():
        if holder["grupo"]:
            m.comando("grupo fuera", 3)
        holder["sede"].limpiar()

    with PersonajeTemporal(ctx, m, raza, clase, sec, antes_de_salir=limpiar) as pj:
        sede = holder["sede"] = Sede(ctx, m, pj, sec, bando, ctx.p["home_guild"])
        comprobar = sede.comprobar
        ctx.anotar_servidor(m)
        m.comando("levelup 24", 2)
        if not comprobar(m.valor_propio(upd.UNIT_FIELD_LEVEL) == 25, "nivel 25 preparado",
                         m.valor_propio(upd.UNIT_FIELD_LEVEL), origen="entorno"):
            return
        if not sede.comprar():
            return
        mayordomo = sede.mayordomo()
        if not mayordomo or not sede.comprar_mejora(mayordomo, ("banquero de materiales",),
                                                    PRECIO_LING, "Ling"):
            return
        # el grupo se forma fuera de la sede y la piedra se usa ya con el bot dentro del grupo
        sede.ir(1, *PORTAL_INICIAL[bando][1])
        portal = _objeto(m, PORTAL_INICIAL[bando][0])
        if not comprobar(bool(portal), "portal inicial visible", portal):
            return
        m.enviar(0x0B1, struct.pack("<Q", portal))
        m.bombear(3)
        if not comprobar(_fuera_de_la_isla(m.gps()), "el jugador sale de la sede por su portal", m.gps()):
            return
        m.comando("grupo 1", 2)
        holder["grupo"] = True
        companeros, limite = [], time.time() + 60
        while time.time() < limite and not companeros:
            m.bombear(2)
            companeros = [x for x in (m.grupo or {}).get("miembros", []) if x["guid"] != m.guid]
        if not comprobar(bool(companeros), "bot de party-here en el grupo", companeros):
            return
        bot = companeros[0]["guid"]
        if not sede.entrar(reiniciar_enfriamiento=True):
            return
        m.comando("grupo sigue", 2)
        limite = time.time() + 120
        while time.time() < limite and bot not in m.objetos:
            m.bombear(3)
        if not comprobar(bot in m.objetos, "el bot aparece en la fase de la sede",
                         {"bot": bot, "estadisticas": m.miembros.get(bot), "yo": m.gps(),
                          "grupo": (m.grupo or {}).get("miembros")}):
            return
        guild_bot = m.objetos[bot]["valores"].get(upd.PLAYER_GUILDID)
        inf.datos["bot"] = {"guid": bot, "guild": guild_bot, "guild_propia": sede.guild_id}
        if ctx.p["home_guild"]:       # party-here prefiere a los bots de la hermandad de casa, sin garantía
            inf.comprobar(sec, guild_bot == sede.guild_id, "el bot invitado es de la hermandad de casa",
                          observado=guild_bot, esperado=sede.guild_id, estado_si_no="AVISO")
        ling = sede.ling()
        if not comprobar(bool(ling), "Ling visible con el grupo dentro", ling):
            return
        if not _comprobar_menu_principal(sede, ling):
            return
        previos = _preparar_materiales(m, ((2589, 45), (2770, 30)))
        if not comprobar(previos == {2589: 45, 2770: 30}, "materiales de prueba en la mochila", previos,
                         origen="entorno"):
            return
        sede.depositos = True
        _depositar(m, ling)
        stock = _stock(m, ling)
        comprobar(stock == {2589: 45, 2770: 30} and _cantidad(m, 2589) == 0,
                  "Ling deposita con el bot en el grupo", stock, {2589: 45, 2770: 30})
        comprobar(bot in m.objetos, "el bot sigue junto al jugador tras usar el banco", bot)
        # salida con el bot
        sede.ir(1, *PORTAL_INICIAL[bando][1])
        portal = _objeto(m, PORTAL_INICIAL[bando][0])
        if not comprobar(bool(portal), "portal visible para la salida", portal):
            return
        m.enviar(0x0B1, struct.pack("<Q", portal))
        m.bombear(3)
        comprobar(_fuera_de_la_isla(m.gps()), "el jugador sale con el grupo", m.gps())
        m.comando("grupo sigue", 2)
        limite = time.time() + 90
        while time.time() < limite and bot not in m.objetos:
            m.bombear(3)
        comprobar(bot in m.objetos, "el bot regresa con el jugador", bot)
        m.comando("grupo fuera", 3)
        holder["grupo"] = False
        ling_capital = sede.ling_capital()
        if not comprobar(bool(ling_capital), "Ling de la capital visible", ling_capital, origen="entorno"):
            return
        stock = _stock(m, ling_capital)
        comprobar(stock == {2589: 45, 2770: 30}, "el mismo stock en la Ling de la capital", stock,
                  {2589: 45, 2770: 30})
        retirado = _vaciar_banco(m, ling_capital)
        comprobar(retirado == {2589: 45, 2770: 30}, "stock retirado entero", retirado)
        sede.depositos = False


# ── jugador invitado: su banco es suyo ─────────────────────────────────────

@caso(id="sede-profesiones-invitado", titulo="SP07: invitado en la sede, banco propio e independiente",
      descripcion="El dueño compra la sede y a Ling y deposita tela. Un segundo jugador sin hermandad "
                  "entra por el grupo del dueño: no puede comprar mejoras, ve a Ling con su banco vacío, "
                  "deposita mineral y ninguno de los dos ve el stock del otro. Ambos lo retiran.",
      etiquetas=("sp07", "guildhouse", "profesiones", "dos-cuentas"),
      acciones=("conectar", "leer", "personaje", "gm", "grupo"),
      duracion_max=480, protege=("SP07", "mod-guildhouse", "mod-reagent-bank"), control="directo",
      en_todo=False,
      observa=("invitado sin hermandad no recibe el menú de mejoras", "Ling visible para el invitado",
               "banco del invitado vacío al principio", "stocks independientes", "retirada de ambos"),
      no_cubre=("invitado que se une después a la hermandad", "varios invitados a la vez"))
def ejecutar_invitado(ctx):
    if not ctx.entorno.tiene_cuenta("secundaria"):
        raise Bloqueo("falta cuentas_adicionales.secundaria en el perfil")
    sec, inf = "sede-profesiones-invitado", ctx.inf
    a, b = ctx.nueva_sesion(), ctx.nueva_sesion("secundaria")
    holder = {"grupo": False}

    def limpiar_a():
        if holder["grupo"]:
            a.dejar_grupo()
        holder["a"].limpiar()

    with PersonajeTemporal(ctx, a, 1, 8, sec, antes_de_salir=limpiar_a) as pa:
        with PersonajeTemporal(ctx, b, 1, 1, sec,
                               antes_de_salir=lambda: holder["b"].limpiar()) as pb:
            sa = holder["a"] = Sede(ctx, a, pa, sec, "alianza")
            sb = holder["b"] = Sede(ctx, b, pb, sec, "alianza")
            ctx.anotar_servidor(a)
            comprobar = sa.comprobar
            if not sa.comprar():
                return
            mayordomo = sa.mayordomo()
            if not mayordomo or not sa.comprar_mejora(mayordomo, ("banquero de materiales",),
                                                      PRECIO_LING, "Ling"):
                return
            ling_a = sa.ling()
            if not comprobar(bool(ling_a), "Ling visible para el dueño", ling_a):
                return
            previos = _preparar_materiales(a, ((2589, 45),))
            comprobar(previos == {2589: 45}, "tela del dueño en la mochila", previos, origen="entorno")
            sa.depositos = True
            _depositar(a, ling_a)
            comprobar(_stock(a, ling_a) == {2589: 45}, "stock del dueño depositado", _stock(a, ling_a))

            a.invitar_grupo(pb.nombre)
            b.bombear(1)
            b.aceptar_grupo()
            a.bombear(2)
            b.bombear(2)
            if not comprobar(any(x["guid"] == pa.guid for x in (b.grupo or {}).get("miembros", [])),
                             "invitado en el grupo del dueño", b.grupo):
                return
            holder["grupo"] = True
            b.comando("gm on", 0.5)
            b.comando("go xyz %.3f %.3f %.3f 1" % MAYORDOMO_POS, 2)
            b.comando("gm off", 0.5)
            b.bombear(6)
            if not comprobar(b.gps().get("mapa") == 1, "el invitado permanece en la isla", b.gps()):
                return
            mayordomo_b = _objeto(b, MAYORDOMO)
            menu_b = b.conversar(mayordomo_b, timeout=3) if mayordomo_b else None
            comprobar(mayordomo_b is not None and not (menu_b and menu_b[1]),
                      "el invitado sin hermandad no recibe el menú de mejoras", menu_b)
            sb.ir(1, *LING_POS)
            ling_b = _objeto(b, LING)
            if not comprobar(bool(ling_b), "Ling visible para el invitado", ling_b):
                return
            comprobar(ling_b == ling_a, "dueño e invitado ven a la misma Ling", (ling_a, ling_b))
            if not _comprobar_menu_principal(sb, ling_b):
                return
            comprobar(_stock(b, ling_b) == {}, "el banco del invitado empieza vacío", _stock(b, ling_b), {})
            previos = _preparar_materiales(b, ((2770, 30),))
            comprobar(previos == {2770: 30}, "mineral del invitado en la mochila", previos, origen="entorno")
            sb.depositos = True
            _depositar(b, ling_b)
            comprobar(_stock(b, ling_b) == {2770: 30}, "stock del invitado depositado", _stock(b, ling_b))
            comprobar(_stock(a, ling_a) == {2589: 45},
                      "el dueño no ve el mineral del invitado", _stock(a, ling_a), {2589: 45})
            comprobar(_stock(b, ling_b) == {2770: 30}, "el invitado no ve la tela del dueño",
                      _stock(b, ling_b), {2770: 30})
            retirado_b = _vaciar_banco(b, ling_b)
            sb.depositos = False
            comprobar(retirado_b == {2770: 30}, "el invitado retira lo suyo", retirado_b)
            retirado_a = _vaciar_banco(a, ling_a)
            sa.depositos = False
            comprobar(retirado_a == {2589: 45}, "el dueño retira lo suyo", retirado_a)
            inf.datos["stocks"] = {"dueno": retirado_a, "invitado": retirado_b}


# ── paginación del banco (independiente de la sede) ────────────────────────

def _retirar_de_pagina(m, ling, palabra, entrada, pagina):
    sub = _ver_categoria(m, ling, palabra, pagina)
    fila = _filas(sub[1] if sub else []).get(entrada)
    if not fila:
        return None
    antes = _cantidad(m, entrada)
    m.elegir(ling, sub[0], fila[1]["id"])
    m.bombear(1)
    return _cantidad(m, entrada) - antes


def _pie(pagina):
    """Últimas etiquetas de un menú, para el detalle de un fallo."""
    return [_etiqueta(o["texto"]) for o in pagina[1]][-3:] if pagina else None


@caso(id="sede-profesiones-paginas", titulo="SP07: paginación del banco de materiales",
      descripcion="En la Ling de la capital, 24 entradas distintas de 'Metal y piedra' ocupan dos "
                  "páginas (23 + 1); con exactamente 23 ya no aparece 'Página siguiente' (corrección "
                  "del parche de mod-reagent-bank). Se retira todo al terminar.",
      etiquetas=("sp07", "profesiones"),
      acciones=("conectar", "leer", "personaje", "gm"),
      duracion_max=420, protege=("SP07", "mod-reagent-bank"), control="directo", en_todo=False,
      observa=("23 filas y 'Página siguiente' con 24 entradas", "una fila y 'Página anterior' en la segunda",
               "sin 'Página siguiente' con exactamente 23 entradas"),
      no_cubre=("más de dos páginas", "otras categorías"))
def ejecutar_paginas(ctx):
    sec = "sede-profesiones-paginas"
    m = ctx.nueva_sesion()
    holder = {}

    with PersonajeTemporal(ctx, m, 1, 8, sec, antes_de_salir=lambda: holder["s"].limpiar()) as pj:
        s = holder["s"] = Sede(ctx, m, pj, sec, "alianza")
        comprobar = s.comprobar
        ctx.anotar_servidor(m)
        ling = s.ling_capital()
        if not comprobar(bool(ling), "Ling de la capital visible", ling, origen="entorno"):
            return
        s.depositos = True
        for lote in (METAL_PIEDRA[:12], METAL_PIEDRA[12:]):    # el personaje ya lleva objetos iniciales
            for entrada in lote:
                m.comando("additem %d 2" % entrada, 0.4)
            m.bombear(1)
            if not comprobar(all(_cantidad(m, e) == 2 for e in lote), "lote de metal y piedra en la mochila",
                             {e: _cantidad(m, e) for e in lote}, origen="entorno"):
                return
            _depositar(m, ling)
            comprobar(all(_cantidad(m, e) == 0 for e in lote), "el lote sale de la mochila")
        pagina0 = _ver_categoria(m, ling, "metal y piedra", 0)
        pagina1 = _ver_categoria(m, ling, "metal y piedra", 1)
        filas0, filas1 = _filas(pagina0[1] if pagina0 else []), _filas(pagina1[1] if pagina1 else [])
        comprobar(len(filas0) == 23 and pagina0 and _opcion(pagina0[1], "página siguiente") is not None,
                  "primera página: 23 filas y 'Página siguiente'", (len(filas0), _pie(pagina0)))
        comprobar(len(filas1) == 1 and pagina1 and _opcion(pagina1[1], "página anterior") is not None and
                  _opcion(pagina1[1], "página siguiente") is None,
                  "segunda página: 1 fila y 'Página anterior'", (len(filas1), _pie(pagina1)))
        sobrante = next(iter(filas1), None)
        if sobrante is None:
            return
        llegadas = _retirar_de_pagina(m, ling, "metal y piedra", sobrante, 1)
        comprobar(llegadas == 2, "la fila de la segunda página se retira", llegadas, 2)
        m.comando("additem %d -%d" % (sobrante, _cantidad(m, sobrante)), 0.7)
        pagina0 = _ver_categoria(m, ling, "metal y piedra", 0)
        comprobar(pagina0 is not None and len(_filas(pagina0[1])) == 23 and
                  _opcion(pagina0[1], "página siguiente") is None,
                  "con exactamente 23 entradas no hay 'Página siguiente'",
                  (len(_filas(pagina0[1])) if pagina0 else None, _pie(pagina0)))
        retirado = _vaciar_banco(m, ling, ("metal y piedra",))
        s.depositos = False
        comprobar(len(retirado) == 23, "las 23 entradas restantes se retiran", len(retirado), 23)
        comprobar(not _stock(m, ling), "banco vacío al terminar")


# ── instructores de profesión de la sede ───────────────────────────────────

# entrada del instructor -> (profesión, habilidad, x, y, z); posiciones de guild_house_spawns
INSTRUCTORES = {
    "primarios": {
        19052: ("Alquimia", 171, 16218.1, 16281.8, 13.1756),
        2836: ("Herrería", 164, 16220.5, 16302.3, 13.176),
        8736: ("Ingeniería", 202, 16219.8, 16296.9, 13.1746),
        2627: ("Sastrería", 197, 16220.4, 16278.7, 13.1756),
        19187: ("Peletería", 165, 16231.2, 16295.0, 13.1761),
        19180: ("Desuello", 393, 16228.9, 16304.7, 13.1819),
        8128: ("Minería", 186, 16220.2, 16299.6, 13.178),
        908: ("Herboristería", 182, 16218.3, 16284.3, 13.1756),
        18773: ("Encantamiento", 333, 16227.5, 16292.3, 13.1839),     # Horda: 18753
        18774: ("Joyería", 755, 16222.4, 16293.0, 13.1813),            # Horda: 18751
        30721: ("Inscripción", 773, 16231.6, 16301.0, 13.1757),        # Horda: 30722
    },
    "secundarios": {
        19184: ("Primeros auxilios", 129, 16225.0, 16310.9, 29.262),
        2834: ("Pesca", 356, 16225.3, 16313.9, 29.262),
        19185: ("Cocina", 185, 16227.0, 16278.0, 13.1762),
    },
}
HORDA_POR_ALIANZA = {18773: 18753, 18774: 18751, 30721: 30722}


@caso(id="sede-profesiones-instructores", titulo="SP07: instructores de profesión de la sede",
      descripcion="El dueño compra los 14 instructores de profesión del mayordomo (11 primarias y 3 "
                  "secundarias, 50 oro cada uno), comprueba que cada uno aparece en la fase de la sede "
                  "y abre una lista de recetas, registra el rango máximo que enseña, y aprende Minería "
                  "de verdad con el instructor de la sede. Vende la sede y borra todo al terminar.",
      etiquetas=("sp07", "guildhouse", "profesiones"),
      acciones=("conectar", "leer", "personaje", "gm", "grupo"),
      parametros={"bando": Parametro(str, "alianza", "facción del personaje",
                                     opciones=("alianza", "horda"))},
      duracion_max=600, protege=("SP07", "mod-guildhouse", "mod-individual-progression"),
      control="directo", en_todo=False,
      observa=("precio de cada instructor", "instructor visible en la fase de la sede",
               "SMSG_TRAINER_LIST con recetas", "rango máximo de habilidad que enseña cada uno",
               "aprendizaje real de Minería (hechizo 2581) y habilidad 186 en el personaje"),
      no_cubre=("entrenar todos los rangos: cada instructor enseña hasta su rango (README de la sede)",
                "recetas que dependen de la etapa del viajero (sólo se prueba que Peletería se oculta hasta la etapa 12)"))
def ejecutar_instructores(ctx):
    sec, inf = "sede-profesiones-instructores", ctx.inf
    bando = ctx.p["bando"]
    raza, clase = RAZA_CLASE[bando]
    m = ctx.nueva_sesion()
    holder = {}

    with PersonajeTemporal(ctx, m, raza, clase, sec, antes_de_salir=lambda: holder["sede"].limpiar()) as pj:
        sede = holder["sede"] = Sede(ctx, m, pj, sec, bando)
        comprobar = sede.comprobar
        ctx.anotar_servidor(m)
        m.comando("levelup 9", 2)
        if not comprobar(m.valor_propio(upd.UNIT_FIELD_LEVEL) == 10, "nivel 10 preparado",
                         m.valor_propio(upd.UNIT_FIELD_LEVEL), origen="entorno"):
            return
        if not sede.comprar(fondos=30000000):
            return
        mayordomo = sede.mayordomo()
        if not mayordomo:
            return
        comprados = {}
        for grupo, palabra in (("primarios", "primarias"), ("secundarios", "secundarias")):
            menu = m.conversar(mayordomo)
            entrada = _opcion(menu[1], palabra) if menu else None
            sub = m.elegir(mayordomo, menu[0], entrada["id"]) if entrada else None
            if not comprobar(sub is not None and bool(sub[1]), "submenú de instructores %s" % grupo,
                             [o["texto"] for o in sub[1]] if sub else None):
                return
            n_ofertados = len([o for o in sub[1] if o["dinero"]])            # las de compra llevan precio
            comprobar(n_ofertados == len(INSTRUCTORES[grupo]),
                      "instructores %s ofertados" % grupo, n_ofertados, len(INSTRUCTORES[grupo]))
            precios = {o["dinero"] for o in sub[1] if o["dinero"]}
            comprobar(precios == {PRECIO_FORJA}, "cada instructor cuesta 50 oro en el cuadro de confirmación",
                      precios, {PRECIO_FORJA})
            for i in range(n_ofertados):
                menu = m.conversar(mayordomo)
                entrada = _opcion(menu[1], palabra) if menu else None
                sub = m.elegir(mayordomo, menu[0], entrada["id"]) if entrada else None
                opciones = [o for o in sub[1] if o["dinero"]] if sub else []
                if i >= len(opciones):
                    break
                antes = sede.oro()
                m.elegir(mayordomo, sub[0], opciones[i]["id"], timeout=2)
                m.bombear(1)
                comprados[opciones[i]["texto"]] = antes - sede.oro()
        comprobar(len(comprados) == 14 and set(comprados.values()) == {PRECIO_FORJA},
                  "los 14 instructores cobrados a 50 oro", comprados)

        resumen = {}
        aprendiz = None
        for grupo in ("primarios", "secundarios"):
            for entry, (nombre, habilidad, x, y, z) in INSTRUCTORES[grupo].items():
                if bando == "horda" and entry in HORDA_POR_ALIANZA:
                    entry = HORDA_POR_ALIANZA[entry]
                sede.ir(1, x + 1.5, y, z)
                guid = _objeto(m, entry)
                if entry == 19187:
                    # Darmari lo oculta Individual Progression (npc_ipp_tbc_t3) hasta pasar TBC tier 2
                    comprobar(not guid, "instructor de Peletería oculto por la etapa del viajero (etapa 0)", guid)
                    m.comando("ip set %s 12" % pj.nombre, 2)
                    etapa = sin_colores(" ".join(m.comando_hasta("ip get %s" % pj.nombre, r"Progression Level", 4)))
                    inf.datos["etapa_tras_ip_set"] = etapa
                    m.comando("gm on", 0.5)               # CanBeSeen se reevalúa al volver a entrar en el mapa
                    sede.ir(*VENDEDORES[bando])
                    sede.ir(1, x + 1.5, y, z)
                    m.comando("gm off", 0.5)
                    m.bombear(3)
                    guid = _objeto(m, entry)
                if not comprobar(bool(guid), "instructor de %s (%d) visible en la sede" % (nombre, entry), guid):
                    continue
                lista = m.lista_instructor(guid)
                if not comprobar(lista is not None and bool(lista["hechizos"]),
                                 "instructor de %s abre su lista de recetas" % nombre,
                                 None if lista is None else len(lista["hechizos"])):
                    continue
                rangos = [h["rango"] for h in lista["hechizos"] if h["habilidad"] == habilidad]
                resumen[nombre] = {"entrada": entry, "recetas": len(lista["hechizos"]),
                                   "rango_max": max(rangos) if rangos else None,
                                   "primer_rango": min(rangos) if rangos else None}
                if habilidad == 186:
                    aprendiz = (guid, lista)
        inf.datos["instructores"] = resumen
        if not aprendiz:
            return
        guid, lista = aprendiz
        inicial = next((h for h in lista["hechizos"] if h["hechizo"] == 2581), None)
        if not comprobar(inicial is not None and inicial["estado"] == 0,
                         "Minería de aprendiz ofrecida y aprendible", inicial):
            return
        sede.ir(1, 16221.7, 16299.6, 13.178)
        oro = sede.oro()
        ok, motivo = m.comprar(guid, 2581)
        comprobar(ok, "el instructor de Minería de la sede enseña", motivo)
        m.bombear(1)
        comprobar(186 in m.habilidades(), "la habilidad Minería aparece en el personaje",
                  m.habilidades().get(186))
        comprobar(oro - sede.oro() == inicial["coste"], "coste de entrenamiento cobrado",
                  oro - sede.oro(), inicial["coste"])


# ── recorrido completo: cofre de SP06 → banco de la sede → forja ───────────

def _abrir_cofre_y_recoger(m, nombre, calidad, zona):
    """Crea un cofre de prueba de SP06, lo abre con el hechizo real y recoge todo el botín a la mochila.
    Devuelve (lista de (entrada, cantidad), motivo_de_fallo)."""
    conocidos = set(m.objetos)
    texto = " ".join(map(sin_colores, m.comando_hasta(
        "tesoro prueba crear %s %d %d" % (nombre, calidad, zona),
        r"Cofre de prueba|No hay suelo|sin cofre|personaje humano", timeout=10)))
    if "Cofre de prueba" not in texto:
        return None, texto
    entrada = 700000 + zona * 3 + calidad
    m.bombear(1)
    visibles = [g for g, o in m.objetos.items()
                if o.get("valores", {}).get(upd.OBJECT_FIELD_ENTRY) == entrada]
    if not visibles:
        m.comando("tesoro prueba retirar %s" % nombre, 1)
        return None, "cofre no visible en el cliente"
    nuevos = [g for g in visibles if g not in conocidos]
    guid = nuevos[-1] if nuevos else visibles[-1]
    m.enviar(0x12E, _paquete_hechizo(3365, guid))
    opcode, datos = m.esperar({SMSG_LOOT_RESPONSE, 0x130, 0x133}, timeout=8)
    loot = _leer_respuesta_loot(datos) if opcode == SMSG_LOOT_RESPONSE else {}
    recogido = []
    if loot.get("abierto"):
        for fila in loot["objetos"]:
            m.enviar(CMSG_AUTOSTORE_LOOT_ITEM, Escritor().u8(fila["ranura"]).valor())
            m.bombear(0.7)
            recogido.append((fila["entrada"], fila["cantidad"]))
        m.liberar_loot(guid)
    m.comando("tesoro prueba retirar %s" % nombre, 1)
    return recogido, (None if loot.get("abierto") else "el cofre no se abrió")


@caso(id="sede-profesiones-recorrido", titulo="SP07: cofre de SP06 → banco de la sede → fundición en la forja",
      descripcion="Recorrido completo de profesión: abre cofres de prueba básicos de Elwynn (SP06) "
                  "hasta reunir mineral de cobre y recoge el botín; compra la sede, la forja y a Ling; "
                  "deposita todo, retira el mineral y lo funde con el hechizo real 'Fundir cobre' "
                  "—sin la forja a menos de 10 yardas no funde, junto a ella sí— y comprueba los "
                  "lingotes. Retira lo sobrante, vende la sede y borra el personaje.",
      etiquetas=("sp07", "sp06", "guildhouse", "profesiones"),
      acciones=("conectar", "leer", "personaje", "gm", "grupo"),
      parametros={"minerales": Parametro(int, 2, "mineral de cobre a reunir antes de seguir", minimo=1,
                                         maximo=6)},
      duracion_max=900, protege=("SP07", "SP06", "mod-treasure", "mod-guildhouse", "mod-reagent-bank"),
      control="directo", en_todo=False,
      observa=("botín de los cofres dentro del catálogo de Elwynn y presente en la mochila",
               "forja y Ling compradas al mayordomo con su precio", "depósito del botín real",
               "fundición negada lejos de la forja y aceptada a su lado", "lingotes de cobre creados",
               "banco del personaje vacío al terminar"),
      no_cubre=("el resto de recetas y de profesiones", "XP de profesión (SP01) y su tope",
                "cofres de otras zonas: el barrido de las 57 zonas es tesoros-botin-variado"))
def ejecutar_recorrido(ctx):
    sec, inf = "sede-profesiones-recorrido", ctx.inf
    objetivo = ctx.p["minerales"]
    m = ctx.nueva_sesion()
    holder = {}

    with PersonajeTemporal(ctx, m, 1, 8, sec, antes_de_salir=lambda: holder["sede"].limpiar()) as pj:
        sede = holder["sede"] = Sede(ctx, m, pj, sec, "alianza")
        comprobar = sede.comprobar
        ctx.anotar_servidor(m)
        m.comando("gm on", 1)
        m.comando("go xyz -9751.66 184.723 55.732 0", 3)
        gps = m.gps()
        if not comprobar(gps is not None and gps.get("mapa") == 0, "personaje en Elwynn", gps, origen="entorno"):
            return

        # 1) exploración: cofres básicos de SP06 hasta tener mineral de cobre
        recogido, fallos = {}, []
        intentos = 0
        while recogido.get(2770, 0) < objetivo and intentos < 30:
            intentos += 1
            botin, error = _abrir_cofre_y_recoger(m, pj.nombre, 1, 12)
            if error or not botin:
                fallos.append(error or "sin botín")
                continue
            for entrada, cantidad in botin:
                recogido[entrada] = recogido.get(entrada, 0) + cantidad
        m.comando("gm off", 0.5)
        inf.datos["cofres"] = {"abiertos": intentos, "botin": recogido, "fallos": fallos}
        inf.comprobar(sec, not fallos, "los cofres de prueba se abren y entregan botín", observado=fallos[:3],
                      estado_si_no="AVISO")       # un cofre que no abre se reintenta; se anota su causa
        if not comprobar(recogido.get(2770, 0) >= objetivo, "mineral de cobre reunido en %d cofres" % intentos,
                         recogido, ">= %d" % objetivo):
            return
        comprobar(set(recogido) <= _BASICO[12], "todo el botín pertenece al catálogo básico de Elwynn",
                  sorted(set(recogido) - _BASICO[12]), [])
        mochila = {e: _cantidad(m, e) for e in recogido}
        comprobar(mochila == recogido, "el botín está en la mochila", mochila, recogido)
        minerales = recogido[2770]

        # 2) sede, Ling y forja
        if not sede.comprar(fondos=20000000):
            return
        mayordomo = sede.mayordomo()
        if not mayordomo:
            return
        if not sede.comprar_mejora(mayordomo, ("banquero de materiales",), PRECIO_LING, "Ling"):
            return
        if not sede.comprar_mejora(mayordomo, ("forja", "forge"), PRECIO_FORJA, "forja", ruta="portal"):
            return
        ling = sede.ling()
        if not comprobar(bool(ling), "Ling visible", ling):
            return

        # 3) depósito y retirada del mineral
        sede.depositos = True
        _depositar(m, ling)
        comprobar(all(_cantidad(m, e) == 0 for e in recogido), "el botín entero se deposita",
                  {e: _cantidad(m, e) for e in recogido})
        stock = _stock(m, ling)
        comprobar(stock == recogido, "el banco guarda exactamente lo recogido", stock, recogido)
        while _cantidad(m, 2770) < minerales:
            llegadas = _retirar(m, ling, 2770, "metal y piedra")
            if not llegadas:
                break
        comprobar(_cantidad(m, 2770) == minerales, "el mineral vuelve a la mochila", _cantidad(m, 2770),
                  minerales)

        # 4) fundición: lejos de la forja no, a su lado sí
        m.seleccionar(m.guid)
        m.comando("learn 2575", 0.7)       # Minería
        m.comando("learn 2657", 0.7)       # Fundir cobre
        m.comando("setskill 186 1 75", 0.7)
        if not comprobar(2657 in m.hechizos and 186 in m.habilidades(), "Minería y Fundir cobre aprendidos",
                         (2657 in m.hechizos, m.habilidades().get(186)), origen="entorno"):
            return
        m.ultimo_hechizo = (0, 0)
        m.comando("cast 2657", 4)
        comprobar(m.ultimo_hechizo[1] != 2657 and _cantidad(m, 2770) == minerales and _cantidad(m, 2840) == 0,
                  "sin forja cerca la fundición se niega y no gasta mineral",
                  {"hechizo": m.ultimo_hechizo[1], "mineral": _cantidad(m, 2770), "lingotes": _cantidad(m, 2840)})
        sede.ir(1, *FORJA_POS)
        forja = _objeto(m, FORJA)
        if not comprobar(bool(forja), "forja comprada visible en la fase de la sede", forja):
            return
        for _ in range(minerales):
            m.ultimo_hechizo = (0, 0)
            m.comando("cast 2657", 4)
        comprobar(_cantidad(m, 2840) == minerales and _cantidad(m, 2770) == 0,
                  "junto a la forja el mineral se funde en lingotes",
                  {"lingotes": _cantidad(m, 2840), "mineral": _cantidad(m, 2770)},
                  {"lingotes": minerales, "mineral": 0})
        # lo que no se usó (tela, cuero, hierbas) sigue en el banco: lo vacía limpiar()
        inf.datos["recorrido"] = {"minerales": minerales, "lingotes": _cantidad(m, 2840)}


# ── persistencia tras reinicio ─────────────────────────────────────────────

@caso(id="sede-profesiones-persistencia", titulo="SP07: Ling y su stock tras reiniciar el worldserver",
      descripcion="fase=crear compra la sede y a Ling, deposita materiales y deja al personaje "
                  "desconectado (sin gastar la piedra); tras reiniciar el worldserver, fase=verificar "
                  "reconecta, entra con la piedra, comprueba que Ling sigue en la sede con su stock, "
                  "lo retira, vende la sede y borra todo.",
      etiquetas=("sp07", "guildhouse", "profesiones", "persistencia"),
      acciones=("conectar", "leer", "personaje", "gm", "grupo"),
      parametros={"fase": Parametro(str, "crear", "fase del caso", opciones=("crear", "verificar")),
                  "personaje": Parametro(str, "", "nombre devuelto por fase=crear"),
                  "guid": Parametro(int, 0, "GUID devuelto por fase=crear", minimo=0),
                  "guild_name": Parametro(str, "", "hermandad devuelta por fase=crear")},
      duracion_max=240, protege=("SP07", "mod-guildhouse", "mod-reagent-bank"), control="directo",
      en_todo=False,
      observa=("Ling persistida en la fase de la sede", "stock del banco persistido",
               "retirada y venta tras el reinicio"),
      no_cubre=("corte de luz durante un depósito", "reinicio con el personaje dentro de la isla"))
def ejecutar_persistencia(ctx):
    sec, inf = "sede-profesiones-persistencia", ctx.inf
    m = ctx.nueva_sesion()

    def comprobar(condicion, titulo, observado=None, esperado=None, origen="desarrollo"):
        return inf.comprobar(sec, condicion, titulo, observado=observado, esperado=esperado, origen=origen)

    if ctx.p["fase"] == "crear":
        nombre = nombre_aleatorio(ctx.entorno.prefijo)
        resultado = m.crear_personaje(nombre, 1, 8)
        if not comprobar(resultado == CHAR_CREATE_SUCCESS, "personaje temporal creado", resultado,
                         origen="entorno"):
            return
        guid = next(p["guid"] for p in m.personajes() if p["nombre"] == nombre)
        registro.apuntar(ctx.entorno.host, m.usuario, nombre, guid, ctx.caso.id)
        inf.datos["personaje"] = {"nombre": nombre, "guid": guid, "guild_name": nombre}
        preservar, dentro, sede = False, False, None
        try:
            m.entrar(guid, 1, nombre=nombre)
            dentro = True
            ctx.anotar_servidor(m)
            pj = type("Pj", (), {"nombre": nombre, "guid": guid})()
            sede = Sede(ctx, m, pj, sec, "alianza")
            if not sede.comprar(entrar=False):
                return
            m.comando("gm on", 0.5)
            sede.ir(1, *MAYORDOMO_POS)       # la piedra queda sin usar para la fase 2
            m.comando("gm off", 0.5)
            m.bombear(6)                     # la fase de la sede se aplica en la siguiente comprobación
            mayordomo = _objeto(m, MAYORDOMO)
            if not comprobar(bool(mayordomo), "mayordomo visible", mayordomo):
                return
            if not sede.comprar_mejora(mayordomo, ("banquero de materiales",), PRECIO_LING, "Ling"):
                return
            ling = sede.ling()
            if not comprobar(bool(ling), "Ling visible", ling):
                return
            previos = _preparar_materiales(m, ((2589, 45), (2770, 30)))
            comprobar(previos == {2589: 45, 2770: 30}, "materiales en la mochila", previos, origen="entorno")
            sede.depositos = True
            _depositar(m, ling)
            stock = _stock(m, ling)
            if not comprobar(stock == {2589: 45, 2770: 30}, "stock depositado antes del reinicio", stock):
                return
            preservar = True
            inf.datos["personaje"]["guild_id"] = sede.guild_id
        finally:
            if dentro:
                if not preservar and sede:
                    sede.limpiar()
                m.salir()
            if not preservar:
                borrado = m.borrar_personaje(guid)
                if borrado == CHAR_DELETE_SUCCESS:
                    registro.quitar(ctx.entorno.host, guid)
                comprobar(borrado == CHAR_DELETE_SUCCESS, "personaje retirado tras preparación incompleta",
                          borrado, origen="entorno")
        return

    nombre, guid, guild_name = (ctx.p[k] for k in ("personaje", "guid", "guild_name"))
    if not nombre or not guid or not guild_name:
        raise Bloqueo("fase=verificar requiere personaje, guid y guild_name de fase=crear")
    dentro, sede = False, None
    try:
        m.personajes()
        m.entrar(guid, 1, nombre=nombre)
        dentro = True
        ctx.anotar_servidor(m)
        pj = type("Pj", (), {"nombre": nombre, "guid": guid})()
        sede = Sede(ctx, m, pj, sec, "alianza")
        sede.guild, sede.comprada, sede.depositos = guild_name, True, True
        sede.guild_id = m.valor_propio(upd.PLAYER_GUILDID)
        if not comprobar(sede.guild_id != 0, "hermandad persiste tras el reinicio"):
            return
        ranura, piedra = _ranura_item(m, PIEDRA)
        if not comprobar(ranura is not None, "piedra de la sede persiste", piedra):
            return
        sede.piedra = (ranura, piedra)
        if not sede.entrar():
            return
        ling = sede.ling()
        if not comprobar(bool(ling), "Ling reaparece en la sede tras el reinicio", ling):
            return
        stock = _stock(m, ling)
        comprobar(stock == {2589: 45, 2770: 30}, "el stock persiste tras el reinicio", stock,
                  {2589: 45, 2770: 30})
        retirado = _vaciar_banco(m, ling)
        comprobar(retirado == {2589: 45, 2770: 30}, "todo se retira tras el reinicio", retirado)
        sede.depositos = False
        comprobar(not _stock(m, ling), "banco vacío")
    finally:
        if dentro and sede:
            sede.limpiar()
            m.salir()
        borrado = m.borrar_personaje(guid)
        if borrado == CHAR_DELETE_SUCCESS:
            registro.quitar(ctx.entorno.host, guid)
        comprobar(borrado == CHAR_DELETE_SUCCESS, "personaje temporal borrado", borrado, origen="entorno")


# ── vendedor de sedes junto al Maestro de hermandad ────────────────────────

# (capital, mapa, entrada del Maestro de hermandad, x, y, z) de acore_world.creature
MAESTROS_HERMANDAD = (
    ("Ventormenta", 0, 4974, -8885.2, 614.4, 95.4),
    ("Forjaz", 0, 5130, -5016.2, -997.4, 504.0),
    ("Entrañas", 0, 4613, 1591.2, 204.5, -55.3),
    ("Darnassus", 1, 4161, 10075.9, 2199.7, 1346.7),
    ("Orgrimmar", 1, 3370, 1575.8, -4292.7, 26.3),
    ("Cima del Trueno", 1, 5054, -1291.8, 127.2, 131.7),
    ("Exodar", 530, 16734, -4092.4, -11626.6, -138.7),
    ("Lunargenta", 530, 16568, 9474.5, -7345.4, 16.2),
    ("Dalaran", 571, 28774, 5768.0, 627.2, 650.2),
)


@caso(id="sede-vendedor-hermandad", titulo="SP07: vendedor de sedes junto al Maestro de hermandad",
      descripcion="En las 9 capitales con Maestro de hermandad, comprueba que el vendedor de sedes (500030) "
                  "está a unas 2,5 yardas del Maestro y a su altura. Su menú de compra ya lo cubren los casos guildhouse-*.",
      etiquetas=("sp07", "guildhouse"),
      acciones=("conectar", "leer", "personaje", "gm"),
      duracion_max=300, protege=("SP07", "mod-guildhouse", "spawn_service_npcs"), control="directo",
      en_todo=False,
      observa=("distancia entre el vendedor y el Maestro de hermandad", "ambos visibles", "misma altura"),
      no_cubre=("Bahía del Botín y Shattrath: no tienen Maestro de hermandad y el vendedor sigue junto "
                "al Dungeon Master", "aspecto visual en Wow.exe: que no quede dentro de una pared"))
def ejecutar_vendedor_hermandad(ctx):
    sec = "sede-vendedor-hermandad"
    m = ctx.nueva_sesion()
    with PersonajeTemporal(ctx, m, 1, 8, sec) as pj:
        ctx.anotar_servidor(m)
        ctx.inf.datos["distancias"] = {}
        for ciudad, mapa, maestro, x, y, z in MAESTROS_HERMANDAD:
            m.comando("gm on", 0.5)
            m.comando("go xyz %.3f %.3f %.3f %d" % (x + 1.5, y, z + 1, mapa), 3)
            m.comando("gm off", 0.5)
            m.bombear(2)
            guid_maestro, guid_vendedor = _objeto(m, maestro), _objeto(m, VENDEDOR)
            if not ctx.inf.comprobar(sec, bool(guid_maestro) and bool(guid_vendedor),
                                     "%s: Maestro de hermandad y vendedor visibles" % ciudad,
                                     observado=(guid_maestro, guid_vendedor), origen="desarrollo"):
                continue
            a, b = m.objetos[guid_maestro]["pos"], m.objetos[guid_vendedor]["pos"]
            dist = math.hypot(a["x"] - b["x"], a["y"] - b["y"])
            ctx.inf.datos["distancias"][ciudad] = round(dist, 2)
            ctx.inf.comprobar(sec, 1.5 <= dist <= 3.5 and abs(a["z"] - b["z"]) < 2,
                              "%s: vendedor a unas 2,5 yardas del Maestro y a su altura" % ciudad,
                              observado={"distancia": round(dist, 2), "dz": round(b["z"] - a["z"], 2)},
                              esperado="2,5")


# ── robustez del banco: depósito atómico y cierre de sesión en el mismo tick ──

@caso(id="sede-profesiones-robustez", titulo="SP07: depósito atómico y /logout en el mismo tick del clic",
      descripcion="En la Ling de la capital: depósitos sucesivos que suman (sin pisarse), aviso cuando no hay "
                  "nada que depositar y confirmación sólo tras guardar. Después envía un clic de Ling (categoría "
                  "y depósito) y el cierre de sesión uno tras otro, varias veces: el servidor sigue vivo, el "
                  "banco no pierde ni duplica nada al volver a entrar.",
      etiquetas=("sp07", "profesiones"),
      acciones=("conectar", "leer", "personaje", "gm"),
      parametros={"repeticiones": Parametro(int, 4, "veces que se repite cada carrera clic + logout",
                                            minimo=1, maximo=12)},
      duracion_max=600, protege=("SP07", "SP07-R", "mod-reagent-bank"), control="directo", en_todo=False,
      observa=("depósitos sucesivos suman", "mensaje sin materiales", "mensaje de éxito",
               "consulta asíncrona de categoría + logout sin caída del servidor",
               "depósito + logout sin pérdida ni duplicado de materiales"),
      no_cubre=("fallo real de escritura en la BD (no se puede provocar desde el cliente): se prueba "
                "que la verificación posterior no da falsos negativos, no el camino de error",
                "caída del proceso a mitad de un depósito"))
def ejecutar_robustez(ctx):
    sec, inf = "sede-profesiones-robustez", ctx.inf
    m = ctx.nueva_sesion()
    holder = {}

    with PersonajeTemporal(ctx, m, 1, 8, sec, antes_de_salir=lambda: holder["s"].limpiar()) as pj:
        s = holder["s"] = Sede(ctx, m, pj, sec, "alianza")
        comprobar = s.comprobar
        ctx.anotar_servidor(m)
        ling = s.ling_capital()
        if not comprobar(bool(ling), "Ling de la capital visible", ling, origen="entorno"):
            return
        s.depositos = True

        # 1) nada que depositar
        mensajes = len(m.mensajes)
        _depositar(m, ling)
        comprobar(any("No tienes materiales que depositar" in t for t in _mensajes_desde(m, mensajes)),
                  "sin materiales: aviso en español y nada se guarda", _mensajes_desde(m, mensajes))
        comprobar(_stock(m, ling) == {}, "banco vacío tras el depósito vacío", _stock(m, ling), {})

        # 2) depósitos sucesivos suman
        for cantidad, total in ((20, 20), (25, 45)):
            m.comando("additem 2589 %d" % cantidad, 0.7)
            m.bombear(1)
            mensajes = len(m.mensajes)
            _depositar(m, ling)
            comprobar(any("depositado correctamente" in t for t in _mensajes_desde(m, mensajes)),
                      "aviso de éxito tras guardar (+%d)" % cantidad, _mensajes_desde(m, mensajes))
            comprobar(_stock(m, ling).get(2589) == total and _cantidad(m, 2589) == 0,
                      "el segundo depósito suma al primero (%d)" % total,
                      {"banco": _stock(m, ling).get(2589), "mochila": _cantidad(m, 2589)}, total)
        _vaciar_banco(m, ling)
        comprobar(_stock(m, ling) == {}, "banco vacío antes de las carreras")

        # 3) clic + cierre de sesión en el mismo instante, varias veces
        carreras = []
        for i in range(ctx.p["repeticiones"]):
            ling = s.ling_capital()
            if not ling:
                comprobar(False, "Ling visible en la carrera %d" % (i + 1), ling)
                return
            # a) consulta asíncrona de una categoría
            menu = m.conversar(ling)
            categoria = _opcion(menu[1], "tela") if menu else None
            if not categoria:
                comprobar(False, "menú de Ling en la carrera %d" % (i + 1), menu)
                return
            m.enviar(CMSG_GOSSIP_SELECT_OPTION, struct.pack("<QII", ling, menu[0], categoria["id"]))
            m.salir()
            vivo = _reentrar(ctx, m, pj)
            carreras.append(("categoría", "servidor vivo" if vivo else "SIN RESPUESTA"))
            if not vivo:
                return
            # b) depósito + logout: nada se pierde ni se duplica
            ling = s.ling_capital()
            banco0 = _stock(m, ling).get(2770, 0)
            m.comando("additem 2770 10", 0.7)
            m.bombear(1)
            menu = m.conversar(ling)
            depositar = _opcion(menu[1], "depositar todos") if menu else None
            if not depositar:
                comprobar(False, "opción de depósito en la carrera %d" % (i + 1), menu)
                return
            m.enviar(CMSG_GOSSIP_SELECT_OPTION, struct.pack("<QII", ling, menu[0], depositar["id"]))
            m.salir()
            vivo = _reentrar(ctx, m, pj)
            if not vivo:
                return
            ling = s.ling_capital()
            banco, mochila = _stock(m, ling).get(2770, 0), _cantidad(m, 2770)
            # o el depósito se procesó antes del logout (10 más en el banco) o no llegó a procesarse (10 siguen
            # en la mochila); nunca menos de 10 en total (pérdida) ni más (duplicado)
            tramitado = (banco, mochila) == (banco0 + 10, 0)
            descartado = (banco, mochila) == (banco0, 10)
            comprobar(tramitado or descartado,
                      "carrera %d: el mineral no se pierde ni se duplica" % (i + 1),
                      {"banco": banco, "mochila": mochila, "banco_antes": banco0},
                      "banco %d y mochila 0, o banco %d y mochila 10" % (banco0 + 10, banco0))
            carreras.append(("depósito", "tramitado" if tramitado else "descartado" if descartado else "ANOMALÍA"))
            if mochila:
                m.comando("additem 2770 -%d" % mochila, 0.7)
        inf.datos["carreras"] = carreras
        comprobar(len(carreras) == 2 * ctx.p["repeticiones"], "todas las carreras terminaron con el servidor vivo",
                  carreras)
        _vaciar_banco(m, ling)
        s.depositos = False
        comprobar(_stock(m, ling) == {}, "banco vacío al terminar")


def _reentrar(ctx, m, pj):
    """Vuelve a entrar con el personaje tras un logout provocado; False si el servidor no responde."""
    try:
        m.entrar(pj.guid, 1, nombre=pj.nombre)
        return True
    except Exception as e:                      # noqa: BLE001 — se anota como fallo del servidor
        ctx.inf.comprobar("sede-profesiones-robustez", False, "reentrada tras clic + logout", observado=repr(e))
        return False
