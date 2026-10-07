# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""SP04: copias de botín blanco de misión para dos humanos con AoE Loot.

Usa la misión 33 (8 carnes de lobo resistentes, objeto 750) y lobos 704 con
90 % de caída de ese objeto en la BD fijada. Crea y borra cada criatura.
"""
import time

from .. import actualizaciones as upd
from ..binario import Escritor
from ..catalogo import caso
from ..mundo import LOOT_METHOD_GROUP_LOOT
from . import PersonajeTemporal, asegurar_vivo
from .bots import poblacion
from .mazmorra import selfbot

MISION, LOBO, OBJETO = 33, 704, 750
ITEM_FIELD_STACK_COUNT = 0x000E
CMSG_AUTOSTORE_LOOT_ITEM = 0x108


def _cantidad(m):
    total = 0
    for bajo, alto in m.objetos_bolsas():
        guid = bajo | (alto << 32)
        valores = m.objetos.get(guid, {}).get("valores", {})
        if valores.get(upd.OBJECT_FIELD_ENTRY) == OBJETO:
            total += valores.get(ITEM_FIELD_STACK_COUNT, 1)
    return total


def _matar_lobo(m, entry=LOBO):
    anteriores = {guid for guid, obj in m.objetos.items()
                  if obj.get("valores", {}).get(upd.OBJECT_FIELD_ENTRY) == entry}
    m.comando("npc add %d" % entry, 2)
    m.bombear(1)
    nuevos = [guid for guid, obj in m.objetos.items()
              if guid not in anteriores and obj.get("valores", {}).get(upd.OBJECT_FIELD_ENTRY) == entry]
    if not nuevos:
        return None
    guid = nuevos[0]
    m.seleccionar(guid)
    m.atacar(guid)
    limite = time.monotonic() + 35
    while time.monotonic() < limite:
        m.bombear(1)
        if m.objetos.get(guid, {}).get("valores", {}).get(upd.UNIT_FIELD_HEALTH) == 0:
            break
    m.dejar_de_atacar()
    m.bombear(1)
    return guid


def _recoger_ventana(m, respuesta):
    filas = [obj for obj in respuesta.get("objetos", []) if obj["entrada"] == OBJETO]
    for fila in filas:
        m.enviar(CMSG_AUTOSTORE_LOOT_ITEM, Escritor().u8(fila["ranura"]).valor())
        m.bombear(1)
    return filas


def _limpiar_criaturas(m, guids, inf, sec):
    for guid in guids:
        m.seleccionar(guid)
        resultado = " ".join(m.comando("npc delete", 2))
        inf.comprobar(sec, "removida" in resultado.lower(),
                      "criatura temporal %d retirada" % guid, resultado[:180],
                      origen="entorno")


@caso(id="botin-mision-grupo", titulo="SP04: dos humanos conservan su copia con AoE Loot",
      descripcion=__doc__, etiquetas=("sp04", "misiones", "loot", "grupo", "dos-cuentas"),
      acciones=("conectar", "leer", "personaje", "gm", "grupo"), duracion_max=360,
      protege=("mod-quest-loot-party", "patches/mod-aoe-loot/02-quest-copies.patch"),
      control="directo", en_todo=False,
      observa=("SMSG_LOOT_RESPONSE con objeto 750 y su ranura",
               "inventario de ambos jugadores después de abrir cadáveres distintos"),
      no_cubre=("bot en el grupo", "bolsa llena", "llegada tardía",
                "otra calidad e instancia"))
def ejecutar(ctx):
    if not ctx.entorno.tiene_cuenta("secundaria"):
        raise RuntimeError("falta la cuenta secundaria VERIFICADOR2")
    inf, sec = ctx.inf, "botin-mision-grupo"
    a, b = ctx.nueva_sesion(), ctx.nueva_sesion("secundaria")
    creados = []

    def salir(m):
        if (m.grupo or {}).get("miembros"):
            m.dejar_grupo()
            m.bombear(1)

    with PersonajeTemporal(ctx, a, 1, 1, sec, antes_de_salir=lambda: salir(a)) as pa:
        with PersonajeTemporal(ctx, b, 1, 1, sec, antes_de_salir=lambda: salir(b)) as pb:
            try:
                ctx.anotar_servidor(a)
                asegurar_vivo(a)
                asegurar_vivo(b)
                for m in (a, b):
                    m.comando("levelup 5", 2)
                    m.comando("quest add %d" % MISION, 2)
                    m.bombear(1)
                if not inf.comprobar(sec, MISION in a.misiones() and MISION in b.misiones(),
                                     "ambos tienen la misión que pide objeto 750",
                                     "diario A: %s; B: %s" % (a.misiones(), b.misiones()),
                                     origen="entorno"):
                    return

                a.invitar_grupo(pb.nombre)
                b.bombear(1)
                b.aceptar_grupo()
                a.bombear(2)
                b.bombear(2)
                a.establecer_metodo_loot(LOOT_METHOD_GROUP_LOOT, umbral=2)
                if not inf.comprobar(sec, any(x["guid"] == pb.guid
                                               for x in (a.grupo or {}).get("miembros", [])),
                                     "ambos están en el mismo grupo", origen="entorno"):
                    return

                for _ in range(3):
                    guid = _matar_lobo(a)
                    if guid:
                        creados.append(guid)
                    a.bombear(1)
                    b.bombear(1)
                if not inf.comprobar(sec, len(creados) == 3,
                                     "tres cadáveres de prueba creados y abatidos",
                                     "GUIDs %s" % creados, origen="entorno"):
                    return

                inicial_a, inicial_b = _cantidad(a), _cantidad(b)
                poblacion_prueba = poblacion(a)
                inicio_apertura = time.monotonic()
                respuesta_a = a.abrir_criatura(creados[0], espera=15)
                segundos_a = time.monotonic() - inicio_apertura
                a.bombear(2)
                b.bombear(1)
                auto_a = _cantidad(a) - inicial_a
                inf.comprobar(sec, auto_a >= 1,
                              "A recoge por AoE al menos una copia de cadáver vecino",
                              "inventario +%d; respuesta %s" % (auto_a, respuesta_a))
                filas_a = _recoger_ventana(a, respuesta_a)
                a.bombear(1)
                a.liberar_loot(creados[0])

                inicio_apertura = time.monotonic()
                respuesta_b = b.abrir_criatura(creados[1], espera=15)
                segundos_b = time.monotonic() - inicio_apertura
                b.bombear(2)
                a.bombear(1)
                auto_b = _cantidad(b) - inicial_b
                inf.comprobar(sec, auto_b >= 1,
                              "B conserva y recoge su copia tras saquear A",
                              "inventario +%d; respuesta %s" % (auto_b, respuesta_b))
                filas_b = _recoger_ventana(b, respuesta_b)
                b.bombear(1)
                b.liberar_loot(creados[1])

                inf.comprobar(sec, _cantidad(a) - inicial_a >= auto_a and
                              _cantidad(b) - inicial_b >= auto_b,
                              "recogida manual mantiene ambas copias",
                              "ventana A %s; ventana B %s; inventarios %d/%d"
                              % (filas_a, filas_b, _cantidad(a), _cantidad(b)))
                inf.comprobar(sec, not a.errores_lectura and not b.errores_lectura,
                              "paquetes leídos sin error",
                              "%s / %s" % (a.errores_lectura[:2], b.errores_lectura[:2]),
                              origen="cliente")
                inf.datos["sp04_carga"] = {"poblacion": poblacion_prueba,
                                          "apertura_a_s": round(segundos_a, 3),
                                          "apertura_b_s": round(segundos_b, 3)}
                inf.comprobar(sec, poblacion_prueba["en_mundo"] > poblacion_prueba["conectados"],
                              "la prueba transcurre con bots del mundo conectados",
                              "sesiones %d; personajes %d; aperturas %.2f/%.2f s"
                              % (poblacion_prueba["conectados"], poblacion_prueba["en_mundo"],
                                 segundos_a, segundos_b), origen="entorno")
            finally:
                _limpiar_criaturas(a, creados, inf, sec)


@caso(id="botin-mision-bolsas", titulo="SP04: bolsa llena conserva la copia del grupo",
      descripcion="A llena sus 16 huecos con espadas temporales; B conserva y recoge su propia copia.",
      etiquetas=("sp04", "misiones", "loot", "grupo", "dos-cuentas"),
      acciones=("conectar", "leer", "personaje", "gm", "grupo"), duracion_max=360,
      protege=("patches/mod-aoe-loot/02-quest-copies.patch",), control="directo",
      en_todo=False,
      observa=("inventario lleno de A y vacío de objeto 750", "inventario de B con su copia",
               "SMSG_LOOT_RESPONSE de A aún muestra su objeto 750"),
      no_cubre=("correo AOELoot.MailEnable=1", "bolsas adicionales equipadas"))
def ejecutar_bolsas(ctx):
    if not ctx.entorno.tiene_cuenta("secundaria"):
        raise RuntimeError("falta la cuenta secundaria VERIFICADOR2")
    inf, sec = ctx.inf, "botin-mision-bolsas"
    a, b = ctx.nueva_sesion(), ctx.nueva_sesion("secundaria")
    creados = []

    def salir(m):
        if (m.grupo or {}).get("miembros"):
            m.dejar_grupo()
            m.bombear(1)

    with PersonajeTemporal(ctx, a, 1, 1, sec, antes_de_salir=lambda: salir(a)) as pa:
        with PersonajeTemporal(ctx, b, 1, 1, sec, antes_de_salir=lambda: salir(b)) as pb:
            try:
                ctx.anotar_servidor(a)
                for m in (a, b):
                    asegurar_vivo(m)
                    m.comando("levelup 5", 2)
                    m.comando("quest add %d" % MISION, 2)
                a.invitar_grupo(pb.nombre)
                b.bombear(1)
                b.aceptar_grupo()
                a.bombear(2)
                b.bombear(2)
                if not inf.comprobar(sec, MISION in a.misiones() and MISION in b.misiones() and
                                     any(x["guid"] == pb.guid for x in (a.grupo or {}).get("miembros", [])),
                                     "dos jugadores con la misión comparten grupo", origen="entorno"):
                    return

                for _ in range(2):
                    a.comando("additem 25 16", 2)
                    a.bombear(1)
                    if all(guid != (0, 0) for guid in a.objetos_bolsas()):
                        break
                if not inf.comprobar(sec, all(guid != (0, 0) for guid in a.objetos_bolsas()),
                                     "los 16 huecos de la mochila de A están llenos",
                                     str(a.objetos_bolsas()), origen="entorno"):
                    return

                for _ in range(3):
                    guid = _matar_lobo(a)
                    if guid:
                        creados.append(guid)
                    a.bombear(1)
                    b.bombear(1)
                if not inf.comprobar(sec, len(creados) == 3,
                                     "tres lobos de prueba abatidos", str(creados), origen="entorno"):
                    return

                antes_b = _cantidad(b)
                respuesta_a = {}
                fuente_a = None
                for guid in creados:
                    respuesta_a = a.abrir_criatura(guid, espera=15)
                    a.bombear(1)
                    a.liberar_loot(guid)
                    if any(o["entrada"] == OBJETO for o in respuesta_a.get("objetos", [])):
                        fuente_a = guid
                        break
                if not inf.comprobar(sec, fuente_a is not None,
                                     "cayó el objeto 750 en al menos un lobo",
                                     str(respuesta_a), origen="entorno"):
                    return
                a.bombear(2)
                b.bombear(1)
                inf.comprobar(sec, _cantidad(a) == 0,
                              "A no recibe copia parcial con la mochila llena",
                              "objetos 750: %d; ventana: %s" % (_cantidad(a), respuesta_a))

                # Abrir el cadáver cuya caída quedó demostrada; otro lobo
                # puede no tener botín por el 10 % de fallo de su tabla.
                respuesta_b = b.abrir_criatura(fuente_a, espera=15)
                b.bombear(2)
                a.bombear(1)
                filas_b = _recoger_ventana(b, respuesta_b)
                b.bombear(1)
                inf.comprobar(sec, _cantidad(b) > antes_b,
                              "B recibe su copia aunque A no tenga espacio",
                              "inventario B +%d; filas B %s; ventana B %s"
                              % (_cantidad(b) - antes_b, filas_b, respuesta_b))
                b.liberar_loot(fuente_a)

                respuesta_pendiente = a.abrir_criatura(fuente_a, espera=15)
                inf.comprobar(sec, any(o["entrada"] == OBJETO for o in respuesta_pendiente.get("objetos", [])),
                              "la copia de A sigue disponible en su cadáver",
                              str(respuesta_pendiente))
                a.liberar_loot(fuente_a)
            finally:
                _limpiar_criaturas(a, creados, inf, sec)


@caso(id="botin-mision-elegibilidad",
      titulo="SP04: misión y pertenencia al grupo al morir",
      descripcion="Comprueba ausencia de misión, aceptación tardía, abandono y botín en solitario.",
      etiquetas=("sp04", "misiones", "loot", "grupo", "dos-cuentas"),
      acciones=("conectar", "leer", "personaje", "gm", "grupo"), duracion_max=540,
      protege=("mod-quest-loot-party", "patches/mod-aoe-loot/02-quest-copies.patch"),
      control="directo", en_todo=False,
      observa=("SMSG_LOOT_RESPONSE de ambos miembros antes y después de cambiar la misión",
               "ausencia de copias para quien no era elegible al morir"),
      no_cubre=("misión completada", "entrada en mazmorra", "bot en el grupo"))
def ejecutar_elegibilidad(ctx):
    if not ctx.entorno.tiene_cuenta("secundaria"):
        raise RuntimeError("falta la cuenta secundaria VERIFICADOR2")
    inf, sec = ctx.inf, "botin-mision-elegibilidad"
    a, b = ctx.nueva_sesion(), ctx.nueva_sesion("secundaria")
    creados = []

    def salir(m):
        if (m.grupo or {}).get("miembros"):
            m.dejar_grupo()
            m.bombear(1)

    with PersonajeTemporal(ctx, a, 1, 1, sec, antes_de_salir=lambda: salir(a)) as pa:
        with PersonajeTemporal(ctx, b, 1, 1, sec, antes_de_salir=lambda: salir(b)) as pb:
            try:
                ctx.anotar_servidor(a)
                for m in (a, b):
                    asegurar_vivo(m)
                    m.comando("levelup 5", 2)
                a.comando("quest add %d" % MISION, 2)
                a.bombear(1)
                b.bombear(1)
                if not inf.comprobar(sec, MISION in a.misiones() and MISION not in b.misiones(),
                                     "sólo A tiene la misión antes de formar grupo", origen="entorno"):
                    return
                a.invitar_grupo(pb.nombre)
                b.bombear(1)
                b.aceptar_grupo()
                a.bombear(2)
                b.bombear(2)
                if not inf.comprobar(sec, any(x["guid"] == pb.guid
                                               for x in (a.grupo or {}).get("miembros", [])),
                                     "A y B comparten grupo", origen="entorno"):
                    return

                def con_objeto(m):
                    for _ in range(4):
                        guid = _matar_lobo(m)
                        if guid is None:
                            continue
                        creados.append(guid)
                        m.bombear(1)
                        otro = b if m is a else a
                        otro.bombear(1)
                        respuesta = m.abrir_criatura(guid, espera=15)
                        m.liberar_loot(guid)
                        if any(o["entrada"] == OBJETO for o in respuesta.get("objetos", [])):
                            return guid, respuesta
                        _limpiar_criaturas(m, [guid], inf, sec)
                        creados.remove(guid)
                    return None, {}

                primero, respuesta_a = con_objeto(a)
                if not inf.comprobar(sec, primero is not None,
                                     "cae objeto 750 para A mientras B no tiene misión",
                                     str(respuesta_a), origen="entorno"):
                    return
                respuesta_b = b.abrir_criatura(primero, espera=15)
                inf.comprobar(sec, not any(o["entrada"] == OBJETO for o in respuesta_b.get("objetos", [])),
                              "B sin misión no ve una copia", str(respuesta_b))
                b.liberar_loot(primero)

                b.comando("quest add %d" % MISION, 2)
                b.bombear(1)
                if not inf.comprobar(sec, MISION in b.misiones(),
                                     "B acepta la misión después de la muerte", origen="entorno"):
                    return
                respuesta_tardia = b.abrir_criatura(primero, espera=15)
                inf.comprobar(sec, not any(o["entrada"] == OBJETO for o in respuesta_tardia.get("objetos", [])),
                              "aceptar tarde no crea una copia retroactiva", str(respuesta_tardia))
                b.liberar_loot(primero)
                _limpiar_criaturas(a, [primero], inf, sec)
                creados.remove(primero)

                if not inf.comprobar(sec, a.abandonar_mision(MISION),
                                     "A abandona la misión antes de otra muerte", origen="entorno"):
                    return
                a.bombear(1)
                b.bombear(1)
                segundo, respuesta_b = con_objeto(b)
                if not inf.comprobar(sec, segundo is not None,
                                     "cae objeto 750 para B después del abandono de A",
                                     str(respuesta_b), origen="entorno"):
                    return
                respuesta_a = a.abrir_criatura(segundo, espera=15)
                inf.comprobar(sec, not any(o["entrada"] == OBJETO for o in respuesta_a.get("objetos", [])),
                              "A tras abandonar no ve una copia", str(respuesta_a))
                a.liberar_loot(segundo)
                _limpiar_criaturas(a, [segundo], inf, sec)
                creados.remove(segundo)

                # `.grupo fuera` gestiona los compañeros de Party Here; para
                # abandonar un grupo de dos humanos se envía el opcode real.
                b.dejar_grupo()
                a.bombear(2)
                b.bombear(2)
                if not inf.comprobar(sec, not (b.grupo or {}).get("miembros"),
                                     "B queda en solitario", str(b.grupo), origen="entorno"):
                    return
                tercero, respuesta_solo = con_objeto(b)
                inf.comprobar(sec, tercero is not None,
                              "B conserva el botín normal de su misión en solitario",
                              str(respuesta_solo))
                inf.comprobar(sec, not a.errores_lectura and not b.errores_lectura,
                              "paquetes leídos sin error", origen="cliente")
            finally:
                _limpiar_criaturas(a, creados, inf, sec)


@caso(id="botin-mision-completa", titulo="SP04: la misión completa no genera más copias",
      descripcion="A completa la misión 33 mientras B la mantiene activa; se comprueba un cadáver nuevo.",
      etiquetas=("sp04", "misiones", "loot", "grupo", "dos-cuentas"),
      acciones=("conectar", "leer", "personaje", "gm", "grupo"), duracion_max=360,
      protege=("mod-quest-loot-party",), control="directo", en_todo=False,
      observa=("misión completa de A y activa de B", "objeto 750 sólo en ventana de B"),
      no_cubre=("entrega de la misión", "recompensas de misión"))
def ejecutar_completa(ctx):
    if not ctx.entorno.tiene_cuenta("secundaria"):
        raise RuntimeError("falta la cuenta secundaria VERIFICADOR2")
    inf, sec = ctx.inf, "botin-mision-completa"
    a, b = ctx.nueva_sesion(), ctx.nueva_sesion("secundaria")
    creados = []

    def salir(m):
        if (m.grupo or {}).get("miembros"):
            m.dejar_grupo()
            m.bombear(1)

    with PersonajeTemporal(ctx, a, 1, 1, sec, antes_de_salir=lambda: salir(a)) as pa:
        with PersonajeTemporal(ctx, b, 1, 1, sec, antes_de_salir=lambda: salir(b)) as pb:
            try:
                ctx.anotar_servidor(a)
                for m in (a, b):
                    asegurar_vivo(m)
                    m.comando("levelup 5", 2)
                    m.comando("quest add %d" % MISION, 2)
                    m.bombear(1)
                if not inf.comprobar(sec, MISION in a.misiones() and MISION in b.misiones(),
                                     "ambos aceptaron la misión", origen="entorno"):
                    return
                a.comando("quest complete %d" % MISION, 2)
                a.bombear(1)
                estado = " ".join(a.comando("quest status %d" % MISION, 2))
                if not inf.comprobar(sec, "Complete" in estado and _cantidad(a) >= 8,
                                     "A tiene la misión completa y ocho objetos 750",
                                     "estado %s, objetos %d" % (estado, _cantidad(a)), origen="entorno"):
                    return
                a.invitar_grupo(pb.nombre)
                b.bombear(1)
                b.aceptar_grupo()
                a.bombear(2)
                b.bombear(2)
                if not inf.comprobar(sec, any(x["guid"] == pb.guid
                                               for x in (a.grupo or {}).get("miembros", [])),
                                     "ambos están en el grupo", origen="entorno"):
                    return
                fuente = None
                respuesta_b = {}
                for _ in range(4):
                    guid = _matar_lobo(b)
                    if guid is None:
                        continue
                    creados.append(guid)
                    a.bombear(1)
                    respuesta_b = b.abrir_criatura(guid, espera=15)
                    b.liberar_loot(guid)
                    if any(o["entrada"] == OBJETO for o in respuesta_b.get("objetos", [])):
                        fuente = guid
                        break
                    _limpiar_criaturas(a, [guid], inf, sec)
                    creados.remove(guid)
                if not inf.comprobar(sec, fuente is not None,
                                     "el objeto cae para B con la misión activa",
                                     str(respuesta_b), origen="entorno"):
                    return
                respuesta_a = a.abrir_criatura(fuente, espera=15)
                inf.comprobar(sec, not any(o["entrada"] == OBJETO for o in respuesta_a.get("objetos", [])),
                              "A con la misión completa no ve otra copia", str(respuesta_a))
                a.liberar_loot(fuente)
                inf.comprobar(sec, not a.errores_lectura and not b.errores_lectura,
                              "paquetes leídos sin error", origen="cliente")
            finally:
                _limpiar_criaturas(a, creados, inf, sec)


@caso(id="botin-mision-bot", titulo="SP04: humano con compañero bot",
      descripcion="Party Here añade un bot real; el humano con misión conserva su copia.",
      etiquetas=("sp04", "misiones", "loot", "grupo", "bots"),
      acciones=("conectar", "leer", "personaje", "gm", "grupo"), duracion_max=360,
      protege=("mod-quest-loot-party", "mod-party-here", "mod-playerbots"),
      control="directo", en_todo=False,
      observa=("grupo con un bot conectado", "objeto 750 en la ventana del humano"),
      no_cubre=("bot con la misión 33 activa", "inventario y saqueo autónomo del bot"))
def ejecutar_bot(ctx):
    inf, sec = ctx.inf, "botin-mision-bot"
    a = ctx.nueva_sesion()
    creados = []

    def salir():
        if (a.grupo or {}).get("miembros"):
            a.comando("grupo fuera", 2)
            a.bombear(1)

    with PersonajeTemporal(ctx, a, 1, 1, sec, antes_de_salir=salir):
        try:
            ctx.anotar_servidor(a)
            asegurar_vivo(a)
            a.comando("levelup 5", 2)
            a.comando("quest add %d" % MISION, 2)
            a.bombear(1)
            if not inf.comprobar(sec, MISION in a.misiones(),
                                 "el humano tiene la misión 33", origen="entorno"):
                return
            a.comando("grupo 1", 2)
            limite = time.monotonic() + 90
            miembros = []
            while time.monotonic() < limite:
                a.bombear(2)
                miembros = [x for x in (a.grupo or {}).get("miembros", [])
                            if x["guid"] != a.guid and x["conectado"]]
                if miembros:
                    break
            if not inf.comprobar(sec, len(miembros) == 1,
                                 "Party Here trae un compañero bot conectado",
                                 str(miembros), origen="entorno"):
                return
            respuesta = {}
            fuente = None
            for _ in range(4):
                guid = _matar_lobo(a)
                if guid is None:
                    continue
                creados.append(guid)
                respuesta = a.abrir_criatura(guid, espera=15)
                a.liberar_loot(guid)
                if any(o["entrada"] == OBJETO for o in respuesta.get("objetos", [])):
                    fuente = guid
                    break
                _limpiar_criaturas(a, [guid], inf, sec)
                creados.remove(guid)
            inf.comprobar(sec, fuente is not None,
                          "el humano conserva su copia blanca de misión junto al bot",
                          str(respuesta))
            inf.comprobar(sec, not a.errores_lectura,
                          "paquetes leídos sin error", origen="cliente")
        finally:
            _limpiar_criaturas(a, creados, inf, sec)


@caso(id="botin-mision-mazmorra", titulo="SP04: copias en una instancia de mazmorra",
      descripcion="Dos humanos agrupados entran en Sima Ígnea y saquean un lobo de prueba.",
      etiquetas=("sp04", "misiones", "loot", "grupo", "mazmorra", "dos-cuentas"),
      acciones=("conectar", "leer", "personaje", "gm", "grupo", "mazmorra"),
      duracion_max=480, protege=("mod-quest-loot-party", "patches/mod-aoe-loot/02-quest-copies.patch"),
      control="directo", en_todo=False,
      observa=("mapa e instancia idénticos para ambos", "objeto 750 visible para ambos"),
      no_cubre=("recorrido de la mazmorra", "jefes y botín propio de la mazmorra"))
def ejecutar_mazmorra(ctx):
    if not ctx.entorno.tiene_cuenta("secundaria"):
        raise RuntimeError("falta la cuenta secundaria VERIFICADOR2")
    inf, sec = ctx.inf, "botin-mision-mazmorra"
    a, b = ctx.nueva_sesion(), ctx.nueva_sesion("secundaria")
    creados = []

    def salir(m):
        if (m.grupo or {}).get("miembros"):
            m.dejar_grupo()
            m.bombear(1)

    with PersonajeTemporal(ctx, a, 1, 1, sec, antes_de_salir=lambda: salir(a)) as pa:
        with PersonajeTemporal(ctx, b, 1, 1, sec, antes_de_salir=lambda: salir(b)) as pb:
            try:
                ctx.anotar_servidor(a)
                for m in (a, b):
                    asegurar_vivo(m)
                    m.comando("levelup 15", 2)
                    m.comando("quest add %d" % MISION, 2)
                    m.bombear(1)
                if not inf.comprobar(sec, MISION in a.misiones() and MISION in b.misiones(),
                                     "ambos tienen la misión", origen="entorno"):
                    return
                a.invitar_grupo(pb.nombre)
                b.bombear(1)
                b.aceptar_grupo()
                a.bombear(2)
                b.bombear(2)
                if not inf.comprobar(sec, any(x["guid"] == pb.guid
                                               for x in (a.grupo or {}).get("miembros", [])),
                                     "ambos comparten grupo", origen="entorno"):
                    return
                dentro = {"x": 3.81, "y": -14.82, "z": -17.84}
                a.ir_a(dentro, 4, mapa=389)
                b.ir_a(dentro, 4, mapa=389)
                a.bombear(2)
                b.bombear(2)
                pos_a, pos_b = a.gps(), b.gps()
                if not inf.comprobar(sec, pos_a and pos_b and pos_a["mapa"] == 389 and
                                     pos_b["mapa"] == 389 and pos_a["instancia"] and
                                     pos_a["instancia"] == pos_b["instancia"],
                                     "ambos entraron en la misma Sima Ígnea",
                                     "A %s, B %s" % (pos_a, pos_b), origen="entorno"):
                    return
                fuente = None
                respuesta_a = {}
                for _ in range(4):
                    guid = _matar_lobo(a)
                    if guid is None:
                        continue
                    creados.append(guid)
                    b.bombear(1)
                    respuesta_a = a.abrir_criatura(guid, espera=15)
                    a.liberar_loot(guid)
                    if any(o["entrada"] == OBJETO for o in respuesta_a.get("objetos", [])):
                        fuente = guid
                        break
                    _limpiar_criaturas(a, [guid], inf, sec)
                    creados.remove(guid)
                if not inf.comprobar(sec, fuente is not None,
                                     "A ve objeto 750 en la mazmorra",
                                     str(respuesta_a), origen="entorno"):
                    return
                respuesta_b = b.abrir_criatura(fuente, espera=15)
                inf.comprobar(sec, any(o["entrada"] == OBJETO for o in respuesta_b.get("objetos", [])),
                              "B también conserva su copia en la misma instancia",
                              str(respuesta_b))
                b.liberar_loot(fuente)
                inf.comprobar(sec, not a.errores_lectura and not b.errores_lectura,
                              "paquetes leídos sin error", origen="cliente")
            finally:
                _limpiar_criaturas(a, creados, inf, sec)


@caso(id="botin-mision-equipo", titulo="SP04: el equipo verde conserva sus tiradas",
      descripcion="Dos humanos con misión 33 abren un cadáver con armadura verde garantizada.",
      etiquetas=("sp04", "misiones", "loot", "grupo", "dos-cuentas"),
      acciones=("conectar", "leer", "personaje", "gm", "grupo"), duracion_max=360,
      protege=("mod-quest-loot-party", "mod-aoe-loot", "mod-playerbots"),
      control="directo", en_todo=False,
      observa=("SMSG_LOOT_START_ROLL para armadura verde 17922 en ambos jugadores",
               "votos de pase y ninguna copia personal del equipo"),
      no_cubre=("objetos de misión épicos/legendarios", "desencantar y maestro de botín"))
def ejecutar_equipo(ctx):
    if not ctx.entorno.tiene_cuenta("secundaria"):
        raise RuntimeError("falta la cuenta secundaria VERIFICADOR2")
    inf, sec = ctx.inf, "botin-mision-equipo"
    a, b = ctx.nueva_sesion(), ctx.nueva_sesion("secundaria")
    creados = []

    def salir(m):
        if (m.grupo or {}).get("miembros"):
            m.dejar_grupo()
            m.bombear(1)

    with PersonajeTemporal(ctx, a, 1, 1, sec, antes_de_salir=lambda: salir(a)) as pa:
        with PersonajeTemporal(ctx, b, 1, 1, sec, antes_de_salir=lambda: salir(b)) as pb:
            try:
                ctx.anotar_servidor(a)
                for m in (a, b):
                    asegurar_vivo(m)
                    # El NPC 5807 conserva su requisito de daño original.
                    # Un jugador 80 lo supera con daño real antes de abrir.
                    m.comando("levelup 79", 2)
                    m.comando("quest add %d" % MISION, 2)
                    m.bombear(1)
                a.invitar_grupo(pb.nombre)
                b.bombear(1)
                b.aceptar_grupo()
                a.bombear(2)
                b.bombear(2)
                a.establecer_metodo_loot(LOOT_METHOD_GROUP_LOOT, umbral=2)
                if not inf.comprobar(sec, MISION in a.misiones() and MISION in b.misiones() and
                                     any(x["guid"] == pb.guid for x in (a.grupo or {}).get("miembros", [])),
                                     "ambos con misión y reparto de grupo", origen="entorno"):
                    return
                criatura = 5807
                anteriores = {g for g, o in a.objetos.items()
                              if o.get("valores", {}).get(upd.OBJECT_FIELD_ENTRY) == criatura}
                a.comando("npc add %d" % criatura, 2)
                a.bombear(1)
                nuevos = [g for g, o in a.objetos.items()
                          if g not in anteriores and o.get("valores", {}).get(upd.OBJECT_FIELD_ENTRY) == criatura]
                if not inf.comprobar(sec, bool(nuevos),
                                     "se crea la criatura con armadura verde garantizada",
                                     str(nuevos), origen="entorno"):
                    return
                guid = nuevos[0]
                creados.append(guid)
                a.seleccionar(guid)
                a.atacar(guid)
                limite = time.monotonic() + 35
                while time.monotonic() < limite:
                    a.bombear(1)
                    if a.objetos.get(guid, {}).get("valores", {}).get(upd.UNIT_FIELD_HEALTH) == 0:
                        break
                a.dejar_de_atacar()
                b.bombear(1)
                if not inf.comprobar(sec, a.objetos.get(guid, {}).get("valores", {}).get(upd.UNIT_FIELD_HEALTH) == 0,
                                     "criatura abatida por daño real", origen="entorno"):
                    return
                respuesta = a.abrir_criatura(guid, espera=25)
                a.bombear(1)
                b.bombear(2)
                tirada_a = next(((g, t) for g, t in a.tiradas.items() if t["itemid"] == 17922), None)
                tirada_b = next(((g, t) for g, t in b.tiradas.items() if t["itemid"] == 17922), None)
                if not inf.comprobar(sec, tirada_a is not None and tirada_b is not None,
                                     "ambos reciben la tirada de la armadura verde 17922",
                                     "A %s, B %s, ventana %s" % (tirada_a, tirada_b, respuesta)):
                    return
                inf.comprobar(sec, tirada_a[0] == tirada_b[0],
                              "los dos ven la misma tirada compartida, sin copias separadas",
                              "GUID %s / %s" % (tirada_a[0], tirada_b[0]))
                a.votar_loot(tirada_a[0], tirada_a[1]["ranura"], 0)
                b.votar_loot(tirada_b[0], tirada_b[1]["ranura"], 0)
                a.bombear(1)
                b.bombear(1)
                inf.comprobar(sec, not a.errores_lectura and not b.errores_lectura,
                              "paquetes leídos sin error", origen="cliente")
                a.liberar_loot(guid)
            finally:
                _limpiar_criaturas(a, creados, inf, sec)


@caso(id="botin-mision-selfbot", titulo="SP04: copia del miembro llevado por playerbots",
      descripcion="B activa su IA selfbot al morir el lobo; tras desactivarla recoge su copia.",
      etiquetas=("sp04", "misiones", "loot", "grupo", "dos-cuentas", "selfbot"),
      acciones=("conectar", "leer", "personaje", "gm", "grupo", "selfbot"),
      duracion_max=420, protege=("mod-quest-loot-party", "mod-playerbots"),
      control="directo", en_todo=False,
      observa=("selfbot activo al morir", "objeto 750 disponible para ambos miembros"),
      no_cubre=("bot desconectado controlado por AI sin sesión de prueba",))
def ejecutar_selfbot(ctx):
    if not ctx.entorno.tiene_cuenta("secundaria"):
        raise RuntimeError("falta la cuenta secundaria VERIFICADOR2")
    inf, sec = ctx.inf, "botin-mision-selfbot"
    a, b = ctx.nueva_sesion(), ctx.nueva_sesion("secundaria")
    creados = []
    ia_activa = False

    def salir(m):
        if (m.grupo or {}).get("miembros"):
            m.dejar_grupo()
            m.bombear(1)

    with PersonajeTemporal(ctx, a, 1, 1, sec, antes_de_salir=lambda: salir(a)) as pa:
        with PersonajeTemporal(ctx, b, 1, 1, sec, antes_de_salir=lambda: salir(b)) as pb:
            try:
                ctx.anotar_servidor(a)
                for m in (a, b):
                    asegurar_vivo(m)
                    m.comando("levelup 5", 2)
                    m.comando("quest add %d" % MISION, 2)
                    m.bombear(1)
                a.invitar_grupo(pb.nombre)
                b.bombear(1)
                b.aceptar_grupo()
                a.bombear(2)
                b.bombear(2)
                if not inf.comprobar(sec, MISION in a.misiones() and MISION in b.misiones() and
                                     any(x["guid"] == pb.guid for x in (a.grupo or {}).get("miembros", [])),
                                     "ambos con misión y en el mismo grupo", origen="entorno"):
                    return
                activo, textos = selfbot(b, True)
                ia_activa = bool(activo)
                if not inf.comprobar(sec, ia_activa,
                                     "la IA de playerbots lleva a B al morir el lobo",
                                     " / ".join(textos), origen="entorno"):
                    return
                fuente = None
                respuesta_a = {}
                for _ in range(4):
                    guid = _matar_lobo(a)
                    if guid is None:
                        continue
                    creados.append(guid)
                    b.bombear(1)
                    respuesta_a = a.abrir_criatura(guid, espera=15)
                    a.liberar_loot(guid)
                    if any(o["entrada"] == OBJETO for o in respuesta_a.get("objetos", [])):
                        fuente = guid
                        break
                    _limpiar_criaturas(a, [guid], inf, sec)
                    creados.remove(guid)
                if not inf.comprobar(sec, fuente is not None,
                                     "A ve su copia mientras B era selfbot",
                                     str(respuesta_a), origen="entorno"):
                    return
                activo, textos = selfbot(b, False)
                ia_activa = bool(activo)
                if not inf.comprobar(sec, not ia_activa,
                                     "B recupera control directo antes de abrir botín",
                                     " / ".join(textos), origen="entorno"):
                    return
                respuesta_b = b.abrir_criatura(fuente, espera=15)
                inf.comprobar(sec, _cantidad(b) > 0 or
                              any(o["entrada"] == OBJETO for o in respuesta_b.get("objetos", [])),
                              "la copia de B sigue en su inventario o cadáver",
                              "inventario %d, ventana %s" % (_cantidad(b), respuesta_b))
                b.liberar_loot(fuente)
                inf.comprobar(sec, not a.errores_lectura and not b.errores_lectura,
                              "paquetes leídos sin error", origen="cliente")
            finally:
                if ia_activa:
                    selfbot(b, False)
                _limpiar_criaturas(a, creados, inf, sec)


@caso(id="botin-mision-ordinario", titulo="SP04: botín ordinario sigue compartido",
      descripcion="En un mismo cadáver, el objeto de misión tiene dos copias y los restos grises no.",
      etiquetas=("sp04", "misiones", "loot", "grupo", "dos-cuentas"),
      acciones=("conectar", "leer", "personaje", "gm", "grupo"), duracion_max=420,
      protege=("mod-quest-loot-party", "patches/mod-aoe-loot/02-quest-copies.patch"),
      control="directo", en_todo=False,
      observa=("SMSG_LOOT_RESPONSE de ambos sobre el mismo cadáver",
               "750 para ambos y filas grises sin duplicar"),
      no_cubre=("material de misión almacenado en Loot::items", "despellejar"))
def ejecutar_ordinario(ctx):
    if not ctx.entorno.tiene_cuenta("secundaria"):
        raise RuntimeError("falta la cuenta secundaria VERIFICADOR2")
    inf, sec = ctx.inf, "botin-mision-ordinario"
    a, b = ctx.nueva_sesion(), ctx.nueva_sesion("secundaria")
    creados = []

    def salir(m):
        if (m.grupo or {}).get("miembros"):
            m.dejar_grupo()
            m.bombear(1)

    with PersonajeTemporal(ctx, a, 1, 1, sec, antes_de_salir=lambda: salir(a)) as pa:
        with PersonajeTemporal(ctx, b, 1, 1, sec, antes_de_salir=lambda: salir(b)) as pb:
            try:
                ctx.anotar_servidor(a)
                for m in (a, b):
                    asegurar_vivo(m)
                    m.comando("levelup 5", 2)
                    m.comando("quest add %d" % MISION, 2)
                    m.bombear(1)
                a.invitar_grupo(pb.nombre)
                b.bombear(1)
                b.aceptar_grupo()
                a.bombear(2)
                b.bombear(2)
                if not inf.comprobar(sec, MISION in a.misiones() and MISION in b.misiones() and
                                     any(x["guid"] == pb.guid for x in (a.grupo or {}).get("miembros", [])),
                                     "ambos con misión y en grupo", origen="entorno"):
                    return
                respuesta_a = {}
                fuente = None
                for _ in range(5):
                    guid = _matar_lobo(a)
                    if guid is None:
                        continue
                    creados.append(guid)
                    b.bombear(1)
                    respuesta_a = a.abrir_criatura(guid, espera=15)
                    a.liberar_loot(guid)
                    filas_a = respuesta_a.get("objetos", [])
                    if (any(o["entrada"] == OBJETO for o in filas_a) and
                            any(o["entrada"] != OBJETO for o in filas_a)):
                        fuente = guid
                        break
                    _limpiar_criaturas(a, [guid], inf, sec)
                    creados.remove(guid)
                if not inf.comprobar(sec, fuente is not None,
                                     "cadáver con objeto 750 y restos ordinarios",
                                     str(respuesta_a), origen="entorno"):
                    return
                respuesta_b = b.abrir_criatura(fuente, espera=15)
                filas_b = respuesta_b.get("objetos", [])
                inf.comprobar(sec, any(o["entrada"] == OBJETO for o in filas_b),
                              "B conserva su copia de misión", str(respuesta_b))
                grises_a = {(o["ranura"], o["entrada"]) for o in respuesta_a["objetos"]
                            if o["entrada"] != OBJETO}
                grises_b = {(o["ranura"], o["entrada"]) for o in filas_b
                            if o["entrada"] != OBJETO}
                inf.comprobar(sec, not grises_a.intersection(grises_b),
                              "ninguna fila ordinaria se duplica entre las dos ventanas",
                              "A %s, B %s" % (grises_a, grises_b))
                b.liberar_loot(fuente)
                inf.comprobar(sec, not a.errores_lectura and not b.errores_lectura,
                              "paquetes leídos sin error", origen="cliente")
            finally:
                _limpiar_criaturas(a, creados, inf, sec)


@caso(id="botin-mision-material", titulo="SP04: material de misión del botín ordinario",
      descripcion="La carne 769 pedida por la misión 317 cae de Loot::items, sin copias personales.",
      etiquetas=("sp04", "misiones", "loot", "grupo", "dos-cuentas"),
      acciones=("conectar", "leer", "personaje", "gm", "grupo"), duracion_max=420,
      protege=("mod-quest-loot-party", "patches/mod-aoe-loot/02-quest-copies.patch"),
      control="directo", en_todo=False,
      observa=("misión 317 activa para ambos", "objeto 769 visible para un solo miembro"),
      no_cubre=("cocinar o entregar la misión",))
def ejecutar_material(ctx):
    if not ctx.entorno.tiene_cuenta("secundaria"):
        raise RuntimeError("falta la cuenta secundaria VERIFICADOR2")
    inf, sec = ctx.inf, "botin-mision-material"
    a, b = ctx.nueva_sesion(), ctx.nueva_sesion("secundaria")
    creados = []
    mision_material, jabali, objeto_material = 317, 113, 769

    def salir(m):
        if (m.grupo or {}).get("miembros"):
            m.dejar_grupo()
            m.bombear(1)

    with PersonajeTemporal(ctx, a, 1, 1, sec, antes_de_salir=lambda: salir(a)) as pa:
        with PersonajeTemporal(ctx, b, 1, 1, sec, antes_de_salir=lambda: salir(b)) as pb:
            try:
                ctx.anotar_servidor(a)
                for m in (a, b):
                    asegurar_vivo(m)
                    m.comando("levelup 5", 2)
                    m.comando("quest add %d" % mision_material, 2)
                    m.bombear(1)
                a.invitar_grupo(pb.nombre)
                b.bombear(1)
                b.aceptar_grupo()
                a.bombear(2)
                b.bombear(2)
                if not inf.comprobar(sec, mision_material in a.misiones() and
                                     mision_material in b.misiones() and
                                     any(x["guid"] == pb.guid for x in (a.grupo or {}).get("miembros", [])),
                                     "ambos tienen misión 317 y comparten grupo", origen="entorno"):
                    return
                fuente, respuesta_a, respuesta_b = None, {}, {}
                for _ in range(8):
                    guid = _matar_lobo(a, jabali)
                    if guid is None:
                        continue
                    creados.append(guid)
                    b.bombear(1)
                    respuesta_a = a.abrir_criatura(guid, espera=15)
                    a.liberar_loot(guid)
                    en_a = any(o["entrada"] == objeto_material for o in respuesta_a.get("objetos", []))
                    if en_a:
                        fuente = guid
                        break
                    _limpiar_criaturas(a, [guid], inf, sec)
                    creados.remove(guid)
                if not inf.comprobar(sec, fuente is not None,
                                     "cayó carne 769 para A de un jabalí 113",
                                     str(respuesta_a), origen="entorno"):
                    return
                try:
                    respuesta_b = b.abrir_criatura(fuente, espera=15)
                    b.liberar_loot(fuente)
                except TimeoutError:
                    # Sin derecho a esta fila, el core puede ignorar CMSG_LOOT
                    # si el cadáver no contiene ninguna otra fila para B.
                    respuesta_b = {"objetos": [], "sin_respuesta": True}
                    if not inf.comprobar(sec, bool(b.comando("server info", 3)),
                                         "B sigue conectado tras el rechazo silencioso",
                                         origen="entorno"):
                        return
                en_b = any(o["entrada"] == objeto_material for o in respuesta_b.get("objetos", []))
                inf.comprobar(sec, not en_b,
                              "la carne de Loot::items no se duplicó entre ambos",
                              "A %s, B %s" % (respuesta_a, respuesta_b))
                inf.comprobar(sec, not a.errores_lectura and not b.errores_lectura,
                              "paquetes leídos sin error", origen="cliente")
            finally:
                _limpiar_criaturas(a, creados, inf, sec)


@caso(id="botin-mision-llegada", titulo="SP04: un miembro tardío no recibe copia",
      descripcion="A mata agrupado con un bot; B con misión entra al grupo después de la muerte.",
      etiquetas=("sp04", "misiones", "loot", "grupo", "bots", "dos-cuentas"),
      acciones=("conectar", "leer", "personaje", "gm", "grupo"), duracion_max=420,
      protege=("mod-quest-loot-party", "mod-party-here"), control="directo",
      en_todo=False,
      observa=("grupo y cadáver anteriores a la invitación de B",
               "copia 750 de A sin copia retroactiva de B"),
      no_cubre=("miembro tardío que no tenía misión",))
def ejecutar_llegada(ctx):
    if not ctx.entorno.tiene_cuenta("secundaria"):
        raise RuntimeError("falta la cuenta secundaria VERIFICADOR2")
    inf, sec = ctx.inf, "botin-mision-llegada"
    a, b = ctx.nueva_sesion(), ctx.nueva_sesion("secundaria")
    creados = []

    def salir(m):
        if m is a:
            m.comando("grupo fuera", 2)
        if (m.grupo or {}).get("miembros"):
            m.dejar_grupo()
            m.bombear(1)

    with PersonajeTemporal(ctx, a, 1, 1, sec, antes_de_salir=lambda: salir(a)) as pa:
        with PersonajeTemporal(ctx, b, 1, 1, sec, antes_de_salir=lambda: salir(b)) as pb:
            try:
                ctx.anotar_servidor(a)
                for m in (a, b):
                    asegurar_vivo(m)
                    m.comando("levelup 5", 2)
                    m.comando("quest add %d" % MISION, 2)
                    m.bombear(1)
                a.comando("grupo 1", 2)
                limite = time.monotonic() + 90
                miembros = []
                while time.monotonic() < limite:
                    a.bombear(2)
                    miembros = [x for x in (a.grupo or {}).get("miembros", [])
                                if x["guid"] != a.guid and x["conectado"]]
                    if miembros:
                        break
                if not inf.comprobar(sec, len(miembros) == 1 and
                                     MISION in a.misiones() and MISION in b.misiones(),
                                     "A con bot, B fuera, ambos con misión",
                                     "grupo A %s; grupo B %s" % (miembros, b.grupo), origen="entorno"):
                    return
                fuente, respuesta_a = None, {}
                for _ in range(4):
                    guid = _matar_lobo(a)
                    if guid is None:
                        continue
                    creados.append(guid)
                    a.bombear(1)
                    b.bombear(1)
                    respuesta_a = a.abrir_criatura(guid, espera=15)
                    a.liberar_loot(guid)
                    if any(o["entrada"] == OBJETO for o in respuesta_a.get("objetos", [])):
                        fuente = guid
                        break
                    _limpiar_criaturas(a, [guid], inf, sec)
                    creados.remove(guid)
                if not inf.comprobar(sec, fuente is not None,
                                     "A tenía copia 750 antes de invitar a B",
                                     str(respuesta_a), origen="entorno"):
                    return
                a.invitar_grupo(pb.nombre)
                b.bombear(1)
                b.aceptar_grupo()
                a.bombear(2)
                b.bombear(2)
                if not inf.comprobar(sec, any(x["guid"] == pb.guid
                                               for x in (a.grupo or {}).get("miembros", [])),
                                     "B entra en el grupo después de la muerte", origen="entorno"):
                    return
                try:
                    respuesta_b = b.abrir_criatura(fuente, espera=15)
                    b.liberar_loot(fuente)
                except TimeoutError:
                    respuesta_b = {"objetos": [], "sin_respuesta": True}
                    if not inf.comprobar(sec, bool(b.comando("server info", 3)),
                                         "B sigue conectado tras rechazo silencioso",
                                         origen="entorno"):
                        return
                inf.comprobar(sec, not any(o["entrada"] == OBJETO
                                          for o in respuesta_b.get("objetos", [])) and _cantidad(b) == 0,
                              "B no obtiene copia retroactiva",
                              "inventario %d; ventana %s" % (_cantidad(b), respuesta_b))
                respuesta_final = a.abrir_criatura(fuente, espera=15)
                inf.comprobar(sec, any(o["entrada"] == OBJETO
                                          for o in respuesta_final.get("objetos", [])),
                              "la copia original de A sigue disponible",
                              str(respuesta_final))
                a.liberar_loot(fuente)
            finally:
                _limpiar_criaturas(a, creados, inf, sec)
