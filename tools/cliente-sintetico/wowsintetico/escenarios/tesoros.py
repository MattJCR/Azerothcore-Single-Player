# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""SP03: valida puntos, crea un cofre y lo abre por el protocolo 3.3.5a."""
import re
import math
import time

from .. import actualizaciones as upd
from ..catalogo import Parametro, caso
from ..mundo import SMSG_LOOT_RESPONSE, _leer_respuesta_loot
from . import PersonajeTemporal, sin_colores
from .profesiones import _paquete_hechizo

COFRE_ELWYNN = 700000 + 12 * 3 + 1
PUNTO = re.compile(r"calidad 1 plaza \d+: punto (\d+) .*? pos ([-\d.]+) ([-\d.]+) ([-\d.]+)")


@caso(id="cofres-nativos-equipo", titulo="Cofres nativos de Vanilla con equipo aleatorio",
      descripcion="Crea temporalmente tres cofres originales delante de un jugador para "
                  "que pruebe su botín de equipo sin cambiar plantillas ni tablas.",
      etiquetas=("tesoros", "cofres", "manual"),
      acciones=("conectar", "leer", "personaje", "gm"),
      parametros={"x": Parametro(float, 0.0, "X del jugador"),
                  "y": Parametro(float, 0.0, "Y del jugador"),
                  "z": Parametro(float, 0.0, "Z del jugador"),
                  "orientacion": Parametro(float, 0.0, "Orientación del jugador"),
                  "entrada": Parametro(int, 2850, "Entrada de cofre nativo", opciones=(106318,2850,2852)),
                  "abrir": Parametro(bool, False, "Abrir el primer cofre con el cliente sintético"),
                  "cantidad": Parametro(int, 3, "Cofres de esta tanda", minimo=1, maximo=3),
                  "segundos": Parametro(int, 120, "Vida temporal de los cofres", minimo=30, maximo=900)},
      duracion_max=970, protege=("cofres nativos",), control="directo",
      en_todo=False,
      observa=("tres cofres nativos recibidos cerca del jugador", "desaparición por plazo"),
      no_cubre=("botín concreto, que se elige aleatoriamente al abrir",))
def ejecutar_nativos_equipo(ctx):
    inf, sec = ctx.inf, "cofres-nativos-equipo"
    m = ctx.nueva_sesion()
    x, y, z, o = (ctx.p[k] for k in ("x", "y", "z", "orientacion"))
    entrada = ctx.p["entrada"]
    segundos = ctx.p["segundos"]
    with PersonajeTemporal(ctx, m, 1, 1, sec):
        ctx.anotar_servidor(m)
        m.comando("gm on", 1)
        creados = []
        deltas = (0.0,) if ctx.p["cantidad"] == 1 else (-0.5, 0.0, 0.5)[:ctx.p["cantidad"]]
        for indice, delta in enumerate(deltas):
            px = x + math.cos(o + delta) * 4.5
            py = y + math.sin(o + delta) * 4.5
            m.comando("go xyz %.3f %.3f %.3f 0" % (px, py, z), 2)
            gps = m.gps()
            if not inf.comprobar(sec, gps is not None and gps.get("mapa") == 0,
                                 "llegada al punto del cofre", str(gps), origen="entorno"):
                return
            antes = set(m.objetos)
            m.comando("gobject add temp %d %d" % (entrada, segundos), 1)
            m.bombear(1)
            nuevos = [guid for guid, obj in m.objetos.items()
                      if guid not in antes and obj.get("valores", {}).get(upd.OBJECT_FIELD_ENTRY) == entrada]
            if not inf.comprobar(sec, bool(nuevos), "cofre original aparece",
                                 "posición %.2f %.2f; GUIDs %s" % (px, py, nuevos)):
                return
            creados.extend(nuevos)
            if indice == 0 and ctx.p["abrir"]:
                m.enviar(0x12E, _paquete_hechizo(3365, nuevos[0]))
                opcode, datos = m.esperar({SMSG_LOOT_RESPONSE, 0x130, 0x133}, timeout=8)
                loot = _leer_respuesta_loot(datos) if opcode == SMSG_LOOT_RESPONSE else {}
                inf.comprobar(sec, loot.get("abierto") and len(datos) >= 14 and datos[13] > 0,
                              "el cofre original se abre con botín",
                              "opcode 0x%03X; %s; items %s" % (opcode, loot,
                                                              datos[13] if len(datos) >= 14 else None))
                if loot.get("abierto"):
                    m.liberar_loot(nuevos[0])
        inf.anotar(sec, "INFO", "cofres para Darkcore",
                   "entrada %d; GUIDs %s; duración %d s" % (entrada, creados, segundos))
        m.bombear(segundos + 5)
        # Un cofre abierto con botín pendiente puede seguir visible para el saqueador
        # hasta el logout; los dos que no se abrieron deben vencer por plazo.
        esperados = creados[1:] if ctx.p["abrir"] else creados
        inf.comprobar(sec, all(guid not in m.objetos for guid in esperados),
                      "los cofres temporales cerrados desaparecen",
                      "GUIDs restantes %s" % [guid for guid in esperados if guid in m.objetos])


@caso(id="tesoros-prueba", titulo="SP03: aparición y retirada de cofres de cualquier zona",
      descripcion="Crea delante de un personaje temporal cofres básico, raro y épico de tres zonas "
                  "sin moverlo; comprueba paquete de aparición, flare y retirada visible.",
      etiquetas=("tesoros", "sp03", "gm"),
      acciones=("conectar", "leer", "personaje", "gm"),
      duracion_max=120, protege=("SP03", "mod-treasure"), control="directo",
      en_todo=False,
      observa=("cofres de tres zonas y calidades visibles delante del jugador",
               "SMSG_SPELL_GO de la bengala y desaparición al retirar cada cofre"),
      no_cubre=("aspecto visual en Wow.exe ni la caducidad automática de 90 segundos",))
def ejecutar_prueba(ctx):
    inf, sec = ctx.inf, "tesoros-prueba"
    m = ctx.nueva_sesion()
    with PersonajeTemporal(ctx, m, 1, 1, sec) as personaje:
        ctx.anotar_servidor(m)
        m.comando("gm on", 1)
        m.comando("go xyz -9751.66 184.723 55.732 0", 3)
        gps = m.gps()
        if not inf.comprobar(sec, gps is not None and gps.get("mapa") == 0,
                             "personaje en Elwynn", str(gps), origen="entorno"):
            return
        efectos = []
        m.oyentes.append(lambda _t, evento, datos:
                          efectos.append(datos) if evento == "hechizo_mundo" and
                          datos.get("hechizo") == 30262 else None)
        try:
            for zona, calidad in ((12, 1), (44, 2), (139, 3)):
                entrada = 700000 + zona * 3 + calidad
                resultado = " ".join(map(sin_colores, m.comando_hasta(
                    "tesoro prueba crear %s %d %d" % (personaje.nombre, calidad, zona),
                    r"Cofre de prueba|No hay suelo|sin cofre|personaje humano", timeout=10)))
                if not inf.comprobar(sec, "Cofre de prueba" in resultado,
                                     "cofre %d de zona %d creado" % (calidad, zona), resultado):
                    return
                m.bombear(2)
                visibles = [guid for guid, obj in m.objetos.items()
                            if obj.get("valores", {}).get(upd.OBJECT_FIELD_ENTRY) == entrada]
                if not inf.comprobar(sec, bool(visibles),
                                     "cofre %d visible en cliente" % entrada,
                                     "GUIDs %s" % visibles):
                    return
                if not inf.comprobar(sec, bool(efectos),
                                     "flare emitida al crear cofre %d" % entrada,
                                     "efectos recibidos %d" % len(efectos)):
                    return
                efectos.clear()
                retirada = " ".join(map(sin_colores, m.comando_hasta(
                    "tesoro prueba retirar %s" % personaje.nombre,
                    r"Retirados", timeout=8)))
                m.bombear(2)
                inf.comprobar(sec, "Retirados 1" in retirada and
                              all(guid not in m.objetos for guid in visibles),
                              "cofre %d desaparece ante el cliente" % entrada,
                              retirada)
        finally:
            m.comando("tesoro prueba retirar %s" % personaje.nombre, 1)


def _estado(m):
    return " ".join(sin_colores(linea) for linea in
                    m.comando("tesoro estado", 2))


def _puntos(estado):
    return [(int(n), float(x), float(y), float(z))
            for n, x, y, z in PUNTO.findall(estado) if int(n)]


@caso(id="tesoros-elwynn", titulo="SP03: posición accesible, aparición y apertura",
      descripcion="Valida candidatos desde un punto accesible de Elwynn, activa temporalmente la zona, "
                  "localiza un cofre propio y lo abre. Con espera_rotacion=75 comprueba el cambio de "
                  "punto sin recogida; con espera_apertura=75 comprueba la reposición de un cofre abierto. "
                  "Ambas esperas requieren LocationSeconds=60.",
      etiquetas=("tesoros", "sp03", "gm"),
      acciones=("conectar", "leer", "personaje", "gm"),
      parametros={"espera_rotacion": Parametro(int, 0, "Segundos hasta comprobar la rotación sin apertura", minimo=0, maximo=120),
                  "espera_apertura": Parametro(int, 0, "Segundos hasta comprobar que un cofre abierto entra en reposición", minimo=0, maximo=120)},
      duracion_max=300, protege=("SP03", "mod-treasure"), control="directo",
      en_todo=False,
      observa=("comandos .tesoro validar/estado y estado persistido de la plaza",
               "SMSG_UPDATE_OBJECT del cofre y SMSG_LOOT_RESPONSE al usarlo",
               "punto distinto sin recogida o reposición tras apertura, si se solicita"),
      no_cubre=("representación visual de la bengala y del modelo en Wow.exe",
                "botín completo, resto de zonas y apagado/reinicio del servidor"))
def ejecutar(ctx):
    inf, sec = ctx.inf, "tesoros"
    m = ctx.nueva_sesion()
    activo_previo = False

    def limpiar_zona():
        if not activo_previo:
            m.comando("tesoro desactivar", 2)

    with PersonajeTemporal(ctx, m, 1, 1, sec, antes_de_salir=limpiar_zona) as personaje:
        ctx.anotar_servidor(m)
        m.comando("gm on", 1)
        m.comando("go xyz -9751.66 184.723 55.732 0", 3)
        gps = m.gps()
        if not inf.comprobar(sec, gps is not None and gps.get("mapa") == 0,
                             "el personaje llega a Elwynn", str(gps), origen="entorno"):
            return

        antes = _estado(m)
        activo_previo = "Tesoros zona 12: activos" in antes
        inf.comprobar(sec, "Tesoros zona 12:" in antes,
                      "Elwynn está configurado en SP03", antes)
        aprobados = re.search(r"Puntos aprobados (\d+)/", antes)
        if not aprobados or int(aprobados.group(1)) < 6:
            lineas = m.comando_hasta("tesoro validar", r"puntos examinados", timeout=30)
            validacion = " ".join(map(sin_colores, lineas))
            match = re.search(r"(\d+) puntos examinados; (\d+) aprobados", validacion)
            inf.comprobar(sec, match is not None,
                          "el validador devuelve un resultado verificable", validacion)
        else:
            inf.comprobar(sec, True, "hay puntos registrados por el validador",
                          "%s aprobados" % aprobados.group(1))
        activacion = " ".join(map(sin_colores,
                                  m.comando_hasta("tesoro activar", r"activados|Faltan puntos", timeout=8)))
        if not inf.comprobar(sec, "activados" in activacion,
                             "la zona supera el umbral de puntos aprobados", activacion):
            return

        efectos = []
        m.oyentes.append(lambda _t, evento, datos:
                          efectos.append(datos) if evento == "hechizo_mundo" and
                          datos.get("hechizo") == 30262 else None)
        m.bombear(8)
        estado = _estado(m)
        puntos = _puntos(estado)
        if not inf.comprobar(sec, bool(puntos),
                             "hay al menos una plaza con destino asignado", estado):
            return
        if not efectos:
            # Una plaza ya materializada antes del login no vuelve a lanzar la bengala.
            # Provocamos una aparición de prueba ante este cliente y la retiramos.
            try:
                m.comando_hasta("tesoro prueba crear %s 1 12" % personaje.nombre,
                                r"Cofre de prueba|No hay suelo", timeout=10)
                m.bombear(2)
            finally:
                m.comando("tesoro prueba retirar %s" % personaje.nombre, 1)
        inf.comprobar(sec, bool(efectos),
                      "el servidor emite el efecto de bengala al aparecer",
                      "hechizos 30262 recibidos: %s" % len(efectos),
                      esperado=True, observado=bool(efectos))
        punto, x, y, z = puntos[0]
        espera = ctx.p["espera_rotacion"]
        if espera:
            limite = time.monotonic() + espera
            while time.monotonic() < limite:
                m.bombear(max(0.1, min(5, limite - time.monotonic())))
            estado_final = _estado(m)
            nuevos = _puntos(estado_final)
            if not inf.comprobar(sec, any(n != punto for n, *_ in nuevos),
                                 "el cofre cambia de punto sin recogerse",
                                 estado_final, esperado="otro punto", observado=nuevos):
                return
            punto, x, y, z = nuevos[0]
        m.comando("go xyz %.3f %.3f %.3f 0" % (x, y, z), 3)
        m.bombear(3)
        if not inf.comprobar(sec, m.valor_propio(upd.UNIT_FIELD_HEALTH, 0) > 0,
                             "el personaje llega vivo al punto del cofre",
                             str(m.pos), origen="entorno"):
            return
        visibles = [(guid, obj) for guid, obj in m.objetos.items()
                    if obj.get("valores", {}).get(upd.OBJECT_FIELD_ENTRY) == COFRE_ELWYNN
                    and abs(obj.get("pos", {}).get("x", 1e9) - x) < 2
                    and abs(obj.get("pos", {}).get("y", 1e9) - y) < 2]
        if not inf.comprobar(sec, bool(visibles),
                             "el cliente recibe un cofre SP03 en el punto elegido",
                             "punto %s; visibles %s" % (punto, len(visibles))):
            return
        guid = visibles[0][0]
        m.enviar(0x12E, _paquete_hechizo(3365, guid))  # OPEN_LOCK contra el cofre
        opcode, datos = m.esperar({SMSG_LOOT_RESPONSE, 0x130, 0x133}, timeout=8)
        if not inf.comprobar(sec, opcode == SMSG_LOOT_RESPONSE,
                             "el hechizo de apertura entrega respuesta de botín",
                             "opcode 0x%03X, datos %s" % (opcode, datos.hex())):
            return
        loot = _leer_respuesta_loot(datos)
        inf.comprobar(sec, loot["abierto"], "el cofre se puede abrir de verdad",
                      str(loot), esperado=True, observado=loot["abierto"])
        if loot["abierto"]:
            despues = _estado(m)
            inf.comprobar(sec,
                          bool(re.search(r"calidad 1 plaza 0:.*?abierto true", despues)),
                          "la apertura se guarda en el estado persistente",
                          despues)
            m.liberar_loot(guid)
            espera_abierto = ctx.p["espera_apertura"]
            if espera_abierto:
                limite = time.monotonic() + espera_abierto
                while time.monotonic() < limite:
                    m.bombear(max(0.1, min(5, limite - time.monotonic())))
                despues_plazo = _estado(m)
                linea = re.search(r"calidad 1 plaza 0: punto (\d+) .*? repone (\d+)",
                                  despues_plazo)
                inf.comprobar(sec, bool(linea and int(linea.group(1)) == 0
                                       and int(linea.group(2)) > 0),
                              "un cofre abierto no duplica botín al vencer su ubicación",
                              despues_plazo)


@caso(id="tesoros-mapas", titulo="SP03: validación automática en siete zonas de los cuatro continentes",
      descripcion="Visita Durotar, Frondavil, Cementerio de Dragones, Cuna del Invierno, Isla Bruma "
                  "Azur, Península del Fuego Infernal y Tundra Boreal sin usar .tesoro validar; "
                  "comprueba que aparecen destinos aprobados en cada una.",
      etiquetas=("tesoros", "sp03", "gm"),
      acciones=("conectar", "leer", "personaje", "gm"),
      duracion_max=480, protege=("SP03", "mod-treasure"), control="directo",
      en_todo=False,
      observa=("las zonas están activas de fábrica",
               "puntos aprobados automáticamente y plazas con destino en mapas 1, 530 y 571"),
      no_cubre=("los demás destinos de cada zona, apariencia visual y accesibilidad de todos los candidatos",))
def ejecutar_mapas(ctx):
    inf, sec = ctx.inf, "tesoros-mapas"
    m = ctx.nueva_sesion()
    zonas = (
        (14, 1, ((1450.59, -4902.48, 9.71), (1131.9, -4685.81, 20.09),
                 (962.67, -4245.07, -8.29))),
        (361, 1, ((3883.91, -1448.03, 216.9), (4705.27, -843.21, 318.18),
                  (5851.77, -1181.76, 403.35))),
        (618, 1, ((6841.2, -3703.49, 735.866), (6809.59, -3181.32, 598.276),
                  (6864.97, -2977.38, 605.116))),
        (3483, 530, ((-690.3, 4832.09, 48.87), (176.2, 2280.04, 44.6),
                     (-222.51, 2917.19, -56.07))),
        (3524, 530, ((-3475.28, -12242.6, 8.72732), (-3591.24, -12391.0, 1.58807),
                     (-4423.26, -11646.8, 7.29847))),
        (3537, 571, ((3221.63, 4160.54, 27.41), (4232.36, 4538.01, 31.79),
                     (3722.63, 5176.64, 23.98))),
        (65, 571, ((2645.53, -524.177, 7.65113), (3508.85, -148.534, 62.5883),
                   (2875.42, -861.108, 7.08071))),
    )
    with PersonajeTemporal(ctx, m, 1, 1, sec):
        ctx.anotar_servidor(m)
        m.comando("gm on", 1)
        for zona, mapa, anchors in zonas:
            resultado = "sin visitar"
            aprobado = False
            for x, y, z in anchors:
                m.comando("go xyz %.3f %.3f %.3f %d" % (x, y, z, mapa), 4)
                gps = m.gps()
                if not inf.comprobar(sec, gps is not None and gps.get("mapa") == mapa,
                                     "entrada al mapa %s" % mapa, str(gps), origen="entorno"):
                    return
                m.bombear(15)
                resultado = _estado(m)
                aprobados = re.search(r"Puntos aprobados (\d+)/", resultado)
                if ("Tesoros zona %d: activos" % zona in resultado and aprobados
                        and int(aprobados.group(1)) > 0 and _puntos(resultado)):
                    aprobado = True
                    break
            inf.comprobar(sec, aprobado,
                          "zona %d valida y asigna cofres automáticamente" % zona,
                          resultado, esperado=True, observado=aprobado)
            if not aprobado:
                continue
            _, px, py, pz = _puntos(resultado)[0]
            m.comando("go xyz %.3f %.3f %.3f %d" % (px, py, pz, mapa), 3)
            m.bombear(3)
            entrada = 700000 + zona * 3 + 1
            visible = any(o.get("valores", {}).get(upd.OBJECT_FIELD_ENTRY) == entrada
                          and abs(o.get("pos", {}).get("x", 1e9) - px) < 2
                          and abs(o.get("pos", {}).get("y", 1e9) - py) < 2
                          for o in m.objetos.values())
            inf.comprobar(sec, visible,
                          "el cofre de la zona %d se materializa en su destino" % zona,
                          "entrada %d, punto %.2f %.2f %.2f" % (entrada, px, py, pz))
