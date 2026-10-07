# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""SP01: XP profesional mediante hechizos reales y estado recibido del servidor."""
import struct
import time
import re
from functools import wraps
import json
from pathlib import Path

from .. import actualizaciones as upd
from ..catalogo import Parametro, caso
from ..binario import Escritor
from ..mundo import SMSG_GOSSIP_MESSAGE, SMSG_LOOT_RESPONSE
from . import PersonajeTemporal, sin_colores
from .progresion import etapa_ip

XP = upd.UNIT_END + 0x01E6
NEXT_XP = upd.UNIT_END + 0x01E7
NO_XP = 0x02000000


class _FalloDePrueba(Exception):
    """Aserción ya registrada con su origen; cortar y limpiar sin otro fallo ficticio."""


def _cortar_al_fallar(funcion):
    @wraps(funcion)
    def ejecutar(ctx):
        try:
            return funcion(ctx)
        except _FalloDePrueba:
            return None
    return ejecutar


def _paquete_hechizo(hechizo, objeto=None):
    """SpellHandler: contador u8, spell u32, flags u8, máscara u32 y GUID opcional."""
    w = Escritor().u8(1).u32(hechizo).u8(0).u32(0x800 if objeto is not None else 0)
    if objeto is not None:
        w.guid_empaquetado(objeto)
    return w.valor()


def _exigir(ctx, condicion, texto, esperado=None, observado=None, origen="desarrollo"):
    if not ctx.inf.comprobar("profesiones", condicion, texto, esperado=esperado,
                             observado=observado, origen=origen):
        raise _FalloDePrueba(texto)


def _habilidad(ctx, m, ident, valor, maximo):
    m.comando("setskill %d %d %d" % (ident, valor, maximo), 0.7)
    real = m.habilidades().get(ident)
    _exigir(ctx, real == (valor, maximo), "habilidad %d preparada" % ident,
            (valor, maximo), real, "entorno")


def _nivel(ctx, m, nivel):
    actual = m.valor_propio(upd.UNIT_FIELD_LEVEL)
    if actual != nivel:
        m.comando("levelup %d" % (nivel - actual), 1)
    _exigir(ctx, m.valor_propio(upd.UNIT_FIELD_LEVEL) == nivel,
            "nivel preparado", nivel, m.valor_propio(upd.UNIT_FIELD_LEVEL), "entorno")


def _xp(ctx, m, valor):
    m.comando("debug setvalue %d %d" % (XP, valor), 0.7)
    _exigir(ctx, m.valor_propio(XP) == valor, "XP inicial preparada", valor,
            m.valor_propio(XP), "entorno")


def _fabricar(ctx, m, hechizo, factor, etiqueta, nivel_final=None):
    antes = m.valor_propio(XP)
    requerido = m.valor_propio(NEXT_XP)
    nivel = m.valor_propio(upd.UNIT_FIELD_LEVEL)
    habilidad = m.habilidades().get(129)
    inicio = time.monotonic()
    m.ultimo_hechizo = (0, 0)
    # Cast normal: usa tiempo de lanzamiento, reactivos y creación del core.
    respuesta = m.comando("cast %d" % hechizo, 4)
    _exigir(ctx, m.ultimo_hechizo[1] == hechizo, "fabricación real: " + etiqueta,
            hechizo, {"hechizo": m.ultimo_hechizo[1], "respuesta": respuesta}, "entorno")
    premio = int(int(requerido * 0.01) * factor)
    esperado = antes + premio
    if nivel_final is not None and nivel_final > nivel:
        esperado -= requerido
    observado = m.valor_propio(XP)
    _exigir(ctx, observado == esperado, "XP una sola vez: " + etiqueta, esperado, observado)
    _exigir(ctx, m.valor_propio(upd.UNIT_FIELD_LEVEL) == (nivel_final or nivel),
            "nivel tras fabricar: " + etiqueta, nivel_final or nivel,
            m.valor_propio(upd.UNIT_FIELD_LEVEL))
    ctx.inf.datos.setdefault("muestras", []).append({
        "caso": etiqueta, "hechizo": hechizo, "nivel": nivel, "xp_nivel": requerido,
        "xp_antes": antes, "xp_despues": observado, "premio": premio,
        "habilidad_antes": habilidad, "habilidad_despues": m.habilidades().get(129),
        "segundos": round(time.monotonic() - inicio, 3)})


def _desafio(ctx, m, texto):
    m.comando("go xyz -8920.64 -178.191 80.891 0", 2)
    objetos = [(g, o) for g, o in m.objetos.items()
               if o.get("valores", {}).get(upd.OBJECT_FIELD_ENTRY) == 254605]
    _exigir(ctx, bool(objetos), "santuario existente disponible", origen="entorno")
    guid = objetos[0][0]
    m.enviar(0x0B1, struct.pack("<Q", guid))  # CMSG_GAMEOBJ_USE, sólo GUID u64
    _, datos = m.esperar({SMSG_GOSSIP_MESSAGE}, timeout=5)
    menu, opciones = m._leer_menu(datos)
    elegida = next((o for o in opciones if texto in o["texto"]), None)
    _exigir(ctx, elegida is not None, "opción de desafío: " + texto, observado=opciones)
    m.elegir(guid, menu, elegida["id"], timeout=1)
    # Reabrir verifica que el servidor guardó la selección (la opción desaparece).
    m.enviar(0x0B1, struct.pack("<Q", guid))
    _, datos = m.esperar({SMSG_GOSSIP_MESSAGE}, timeout=5)
    _, opciones = m._leer_menu(datos)
    _exigir(ctx, not any(texto in o["texto"] for o in opciones), "desafío activado: " + texto)


@caso(id="profesiones-fabricacion", titulo="XP por profesiones: colores, tope y límites",
      descripcion="Fabrica vendas con reactivos en un personaje temporal; compara XP y nivel reales. "
                  "Prueba colores, tope aprendido y entrenamiento, bloqueo de XP, cruces 59/60 y 69/70, "
                  "nivel 80, XP lenta y sólo misiones mediante el santuario existente.",
      etiquetas=("profesiones", "sp01", "progresion"),
      acciones=("conectar", "leer", "personaje", "gm"), duracion_max=480,
      protege=("SP01", "mod-profession-experience", "mod-challenge-modes", "mod-individual-progression"),
      control="directo", en_todo=False,
      observa=("PLAYER_XP y PLAYER_NEXT_LEVEL_XP", "SMSG_SPELL_GO", "habilidades y nivel",
               "menú real del Santuario del Desafío"),
      no_cubre=("recolección y pesca: caso profesiones-recoleccion", "cada receta individual",
                "bots y hermandad", "apagado y reaplicación: comprobaciones del instalador"))
@_cortar_al_fallar
def fabricacion(ctx):
    m = ctx.nueva_sesion()
    with PersonajeTemporal(ctx, m, 1, 8, "profesiones") as pj:
        ctx.anotar_servidor(m)
        m.comando("gm on", 0.5)
        m.seleccionar(m.guid)
        _nivel(ctx, m, 20)
        m.comando("learn 3273", 0.7)  # Aprendiz de primeros auxilios.
        m.comando("learn 3276", 0.7)  # Venda de lino gruesa, prueba del tope 75.
        m.comando("additem 2589 80", 0.7)
        for valor, factor, color in ((1, 1, "naranja"), (30, .5, "amarillo"),
                                      (45, .25, "verde"), (60, 0, "gris")):
            _habilidad(ctx, m, 129, valor, 75)
            _fabricar(ctx, m, 3275, factor, color)
        _habilidad(ctx, m, 129, 75, 75)
        _fabricar(ctx, m, 3276, 0, "tope 75/75 aunque la receta sea verde")
        _habilidad(ctx, m, 129, 75, 150)
        _fabricar(ctx, m, 3276, .25, "siguiente rango permite XP")
        # Buscar una tirada sin subida entre actividades verdes. No se altera
        # el azar del servidor; si todas suben se informa del límite de la muestra.
        sin_subida = False
        for _ in range(8):
            habilidad = m.habilidades()[129][0]
            _fabricar(ctx, m, 3276, .25, "verde con tirada de habilidad")
            if m.habilidades()[129][0] == habilidad:
                sin_subida = True
                break
        ctx.inf.comprobar("profesiones", sin_subida, "XP sin subida de habilidad observada",
                          estado_si_no="AVISO")
        _habilidad(ctx, m, 129, 1, 75)
        flags = m.valor_propio(upd.PLAYER_FLAGS)
        m.comando("debug setvalue %d %d" % (upd.PLAYER_FLAGS, flags | NO_XP), .7)
        _exigir(ctx, bool(m.valor_propio(upd.PLAYER_FLAGS) & NO_XP), "XP bloqueada aplicada")
        _fabricar(ctx, m, 3275, 0, "XP bloqueada")
        m.comando("debug setvalue %d %d" % (upd.PLAYER_FLAGS, flags), .7)
        _exigir(ctx, not m.valor_propio(upd.PLAYER_FLAGS) & NO_XP, "XP desbloqueada")
        for nivel, etapa in ((59, 0), (69, 8)):
            m.comando("ip set %s %d" % (pj.nombre, etapa), .7)
            _exigir(ctx, etapa_ip(m, pj.nombre) == etapa, "etapa IP preparada", etapa,
                    etapa_ip(m, pj.nombre), "entorno")
            _nivel(ctx, m, nivel)
            _habilidad(ctx, m, 129, 1, 75)
            _xp(ctx, m, m.valor_propio(NEXT_XP) - 1)
            _fabricar(ctx, m, 3275, 1, "cruce hacia %d" % (nivel+1), nivel+1)
            _fabricar(ctx, m, 3275, 0, "tope IP %d" % (nivel+1))
        m.comando("ip set %s 18" % pj.nombre, .7)
        _nivel(ctx, m, 80)
        _fabricar(ctx, m, 3275, 0, "nivel máximo")
        _nivel(ctx, m, 20)
        _xp(ctx, m, 0)
        _habilidad(ctx, m, 129, 1, 75)
        _desafio(ctx, m, "Experiencia lenta")
        _fabricar(ctx, m, 3275, .5, "desafío XP lenta")
        _desafio(ctx, m, "Solo experiencia por misiones")
        _fabricar(ctx, m, 3275, 0, "desafío sólo misiones")
        _exigir(ctx, not m.errores_lectura, "paquetes sin errores", observado=m.errores_lectura,
                origen="cliente")


@caso(id="profesiones-recoleccion", titulo="XP real al recolectar y pescar",
      descripcion="Crea plantas normales, registra sus GUID y las borra al terminar; verifica colores, "
                  "tope y segunda apertura sin duplicar XP. Pesca en la costa de Páramos de Poniente "
                  "y comprueba la XP al picar y el tope de habilidad.",
      etiquetas=("profesiones", "sp01"), acciones=("conectar", "leer", "personaje", "gm"),
      parametros={"familia": Parametro(str, "ambas", "familia que se comprueba",
                                        opciones=("ambas", "recoleccion", "pesca"))},
      duracion_max=480, protege=("SP01", "mod-profession-experience"), control="directo", en_todo=False,
      observa=("XP real y habilidad", "hechizos de recolección", "estado del flotador y botín"),
      no_cubre=("cada mena/piel/cerradura individual", "separar pescado y basura por el tipo del "
                "paquete: el core envía LOOT_FISHING para ambos; la igualdad se prueba también en C++",
                "bots y hermandad"))
@_cortar_al_fallar
def recoleccion(ctx):
    m = ctx.nueva_sesion()
    plantas = {}
    registro = Path(ctx.artefacto("plantas.json"))

    def guardar():
        registro.write_text(json.dumps({"host": ctx.entorno.host, "pendientes": plantas}), encoding="utf8")

    def limpiar():
        for dbguid, guid in list(plantas.items()):
            respuesta = m.comando("gobject delete %d" % dbguid, 1)
            ok = guid not in m.objetos
            ctx.inf.comprobar("profesiones", ok, "planta de prueba retirada: %d" % dbguid,
                              observado=respuesta)
            if ok:
                del plantas[dbguid]
                guardar()

    with PersonajeTemporal(ctx, m, 1, 8, "profesiones", antes_de_salir=limpiar):
        m.comando("gm on", .5)
        m.seleccionar(m.guid)
        _nivel(ctx, m, 20)
        m.comando("learn 2366", .7)
        muestras = ((1, 150, 1), (26, 150, .5), (51, 150, .25), (101, 150, 0), (75, 75, 0))
        for valor, maximo, factor in (() if ctx.p["familia"] == "pesca" else muestras):
            _habilidad(ctx, m, 182, valor, maximo)
            anteriores = set(m.objetos)
            respuesta = sin_colores(" ".join(m.comando("gobject add 1617", 1)))
            registro_guid = re.search(r"GUID:\s*(\d+)", respuesta)
            _exigir(ctx, registro_guid is not None, "GUID persistente registrado", observado=respuesta,
                    origen="entorno")
            dbguid = int(registro_guid.group(1))
            plantas[dbguid] = 0
            guardar()
            ctx.inf.datos.setdefault("guid_plantas", []).append(dbguid)
            nuevos = [g for g, o in m.objetos.items() if g not in anteriores
                      and o.get("valores", {}).get(upd.OBJECT_FIELD_ENTRY) == 1617]
            _exigir(ctx, len(nuevos) == 1, "planta temporal registrada", 1, len(nuevos), "entorno")
            guid = nuevos[0]
            plantas[dbguid] = guid
            guardar()
            antes, requerido = m.valor_propio(XP), m.valor_propio(NEXT_XP)
            m.ultimo_hechizo = (0, 0)
            m.enviar(0x12E, _paquete_hechizo(2366, guid))
            _, datos = m.esperar({SMSG_LOOT_RESPONSE}, timeout=8)
            m.bombear(.5)
            _exigir(ctx, m.ultimo_hechizo[1] == 2366 and datos[8] != 0,
                    "recolección real con botín", observado=m.ultimo_hechizo)
            premio = int(int(requerido * .01) * factor)
            _exigir(ctx, m.valor_propio(XP) - antes == premio,
                    "XP de herboristería %d/%d" % (valor, maximo), premio, m.valor_propio(XP)-antes)
            m.liberar_loot(guid)
            m.ultimo_hechizo = (0, 0)
            m.enviar(0x12E, _paquete_hechizo(2366, guid))
            _, datos = m.esperar({SMSG_LOOT_RESPONSE}, timeout=8)
            m.bombear(.5)
            _exigir(ctx, m.ultimo_hechizo[1] == 2366 and datos[8] != 0,
                    "segunda apertura real de la misma planta")
            _exigir(ctx, m.valor_propio(XP) - antes == premio, "reabrir no duplica XP",
                    premio, m.valor_propio(XP)-antes)
            m.liberar_loot(guid)
            m.bombear(1)
            limpiar()
        limpiar()
        if ctx.p["familia"] == "recoleccion":
            return
        m.comando("learn 7620", .7)
        m.comando("additem 6256 1", .7)
        ranura = next((23+i for i, (lo, hi) in enumerate(m.objetos_bolsas())
                       if m.objetos.get(lo | hi << 32, {}).get("valores", {}).get(upd.OBJECT_FIELD_ENTRY) == 6256), None)
        _exigir(ctx, ranura is not None, "caña en mochila", origen="entorno")
        m.enviar(0x10A, struct.pack("<BB", 255, ranura))  # CMSG_AUTOEQUIP_ITEM
        m.bombear(1)
        _exigir(ctx, m.equipo_visible()[15] == 6256, "caña equipada", observado=m.equipo_visible(), origen="entorno")
        # Costa junto a los bancos registrados en -9801.39,1766.6 (mapa 0).
        # Mirar hacia +Y (mar): hacia -X el flotador cae sobre tierra y el
        # servidor devuelve SPELL_FAILED_NOT_HERE después de iniciar el casteo.
        # Caña equipada y lanzamiento normal, con flotador y recogida del core.
        m.comando("waterwalk on", .5)
        m.comando("go xyz -9801.39 1766.6 0.1 0 1.57", 3)
        paquetes = []
        procesar = m._procesar
        def capturar(op, datos):
            if op in (0x130, 0x131, 0x132, 0x139, 0x13A):
                paquetes.append({"opcode": hex(op), "datos": datos.hex()})
            procesar(op, datos)
        m._procesar = capturar
        for valor, maximo, factor in ((1, 75, 1), (75, 75, 0), (75, 150, .25)):
            _habilidad(ctx, m, 356, valor, maximo)
            antes, requerido = m.valor_propio(XP), m.valor_propio(NEXT_XP)
            anteriores = set(m.objetos)
            m.ultimo_hechizo = (0, 0)
            paquetes.clear()
            respuesta = m.comando("cast 7620", 2)
            ctx.inf.datos.setdefault("lanzamientos_pesca", []).append({
                "respuesta": respuesta, "hechizo": m.ultimo_hechizo[1], "posicion": m.posicion_actual(),
                "paquetes": list(paquetes), "objetos_nuevos": [o for g,o in m.objetos.items()
                    if g not in anteriores and o.get("typeid") == 5]})
            fallos = [p for p in paquetes if p["opcode"] == "0x130"]
            _exigir(ctx, not fallos, "pesca aceptada por el servidor", observado=fallos, origen="entorno")
            # Margen para el canal y su actualización; el flotador, no el
            # último SPELL_GO (que otros hechizos pueden reemplazar), decide.
            limite = time.monotonic() + 35
            flotador = None
            while time.monotonic() < limite:
                m.bombear(.2)
                for g, o in m.objetos.items():
                    if g in anteriores or o.get("typeid") != 5:
                        continue
                    campos = o.get("valores", {})
                    tipo_estado = campos.get(upd.OBJECT_END + 0xB, 0)
                    if (tipo_estado >> 8) & 0xFF == 17 and tipo_estado & 0xFF == 0:
                        flotador = g
                        break
                if flotador:
                    break
            _exigir(ctx, flotador is not None, "flotador listo para recoger", origen="entorno")
            m.enviar(0x0B1, struct.pack("<Q", flotador))
            _, datos = m.esperar({SMSG_LOOT_RESPONSE}, timeout=5)
            m.bombear(.5)
            _exigir(ctx, datos[8] != 0, "pesca con botín")
            premio = int(int(requerido * .01) * factor)
            ctx.inf.datos.setdefault("capturas", []).append({
                "habilidad_antes": [valor, maximo], "habilidad_despues": m.habilidades().get(356),
                "xp_antes": antes, "xp_despues": m.valor_propio(XP), "esperado": premio,
                "botin": datos.hex()})
            _exigir(ctx, m.valor_propio(XP)-antes == premio,
                    "XP de pesca %d/%d" % (valor, maximo), premio, m.valor_propio(XP)-antes)
            m.liberar_loot(flotador)
            m.bombear(1)
        _exigir(ctx, not m.errores_lectura, "paquetes sin errores", observado=m.errores_lectura,
                origen="cliente")


@caso(id="profesiones-hermandad", titulo="XP profesional independiente de Guild Levels",
      descripcion="Crea una hermandad temporal, desactiva su reclutamiento Home Guild y la lleva "
                  "a nivel 6. Fabrica y comprueba XP del jugador sin Fast Track y XP de hermandad intacta. "
                  "Disuelve la hermandad y borra el personaje con sus objetos al terminar.",
      etiquetas=("profesiones", "sp01", "hermandad"),
      acciones=("conectar", "leer", "personaje", "gm", "grupo"), duracion_max=180,
      protege=("SP01", "mod-profession-experience", "mod-guild-levels"), control="directo", en_todo=False,
      observa=("XP del jugador", "guildlevels info antes/después"),
      no_cubre=("XP de bots aleatorios", "otros perks de hermandad"))
@_cortar_al_fallar
def hermandad(ctx):
    m = ctx.nueva_sesion()
    estado = {"nombre": None}

    def deshacer():
        if estado["nombre"]:
            m.comando("hermandad desactivar", 1)
            m.comando('guild delete "%s"' % estado["nombre"], 2)
            _exigir(ctx, m.valor_propio(upd.PLAYER_GUILDID) == 0, "hermandad temporal disuelta")

    def datos():
        texto = sin_colores(" ".join(m.comando("guildlevels info", 1)))
        r = re.search(r"guild (\d+).*level (\d+).*xp (\d+)", texto)
        _exigir(ctx, r is not None, "estado real de Guild Levels", observado=texto)
        return tuple(map(int, r.groups()))

    with PersonajeTemporal(ctx, m, 1, 8, "profesiones", antes_de_salir=deshacer) as pj:
        m.comando("gm on", .5)
        m.seleccionar(m.guid)
        _nivel(ctx, m, 20)
        m.comando("learn 3273", .7)
        m.comando("additem 2589 10", .7)
        _habilidad(ctx, m, 129, 1, 75)
        estado["nombre"] = pj.nombre
        m.comando('guild create %s "%s"' % (pj.nombre, pj.nombre), 1)
        _exigir(ctx, m.valor_propio(upd.PLAYER_GUILDID) != 0, "hermandad temporal creada")
        m.comando("hermandad desactivar", 1)
        m.comando("guildlevels setlevel 6", 1)
        antes = datos()
        _exigir(ctx, antes[1] == 6, "hermandad con Fast Track disponible", 6, antes[1])
        _fabricar(ctx, m, 3275, 1, "sin bonificación de hermandad")
        despues = datos()
        _exigir(ctx, despues == antes, "XP de hermandad sin contribución", antes, despues)
        ctx.inf.datos["hermandad"] = {"antes": antes, "despues": despues}


@caso(id="profesiones-apagado", titulo="Sin XP con las actividades desactivadas",
      descripcion="Requiere todas las opciones *.Experience a cero y configuración recargada. "
                  "Fabrica una venda naranja y verifica XP cero; el operador restaura la configuración.",
      etiquetas=("sp01-apagado",), acciones=("conectar", "leer", "personaje", "gm"),
      duracion_max=120, protege=("SP01", "mod-profession-experience"), control="directo", en_todo=False,
      observa=("hechizo de fabricación completado", "XP intacta"),
      no_cubre=("retirada del módulo del binario", "configura/restaura las opciones: tarea del operador"))
@_cortar_al_fallar
def apagado(ctx):
    m = ctx.nueva_sesion()
    with PersonajeTemporal(ctx, m, 1, 8, "profesiones"):
        m.comando("gm on", .5)
        m.seleccionar(m.guid)
        _nivel(ctx, m, 20)
        m.comando("learn 3273", .7)
        m.comando("additem 2589 5", .7)
        _habilidad(ctx, m, 129, 1, 75)
        _fabricar(ctx, m, 3275, 0, "actividades apagadas por configuración")
