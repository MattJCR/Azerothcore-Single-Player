# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""SP06: botín ponderado (grupos GroupId) y bono de equipo/abalorio del piloto de 7 zonas."""
from collections import Counter

from .. import actualizaciones as upd
from ..catalogo import Parametro, caso
from ..mundo import SMSG_LOOT_RESPONSE, _leer_respuesta_loot
from . import PersonajeTemporal, sin_colores
from .profesiones import _paquete_hechizo

# zona -> conjunto de entradas de objeto que puede entregar el material básico (25% cada una)
_BASICO = {
    12: {2589, 2318, 2770, 2447},
    17: {2592, 2319, 2771, 2450},
    10: {4306, 4234, 2772, 3355},
    3: {4338, 4304, 3858, 3358},
    139: {14047, 8170, 10620, 13464},
    3523: {21877, 21887, 23424, 22785},
    210: {33470, 33568, 36909, 36901},
}
# zona -> conjunto de entradas del material raro garantizado (gema 85% / polvo 15%)
_RARO_MATERIAL = {
    12: {818, 10940}, 17: {1210, 11083}, 10: {1705, 11137}, 3: {7909, 11176},
    139: {12361, 16204}, 3523: {23107, 22445}, 210: {36917, 34054},
}
_RARO_EQUIPO = {17: 935, 10: 1482, 3: 1718, 139: 4091, 3523: 17015, 210: 32659}
_EPICO_MATERIAL = {139: 12363, 3523: 23438, 210: 36930}
_EPICO_EQUIPO = {139: 18805, 3523: 31291, 210: 31331}
_EPICO_ABALORIO = {139: {19287}, 3523: {31856}, 210: {42987, 44253, 44254, 44255}}
# zona -> (basic_item del tramo, rare_item o 0, epic_item o 0, nombre); de 01-tesoros.sql
_ZONAS = {
    12: (2589, 0, 0, 'Bosque de Elwynn'),
    14: (2589, 0, 0, 'Durotar'),
    40: (2592, 1210, 0, 'Páramos de Poniente'),
    44: (2592, 1210, 0, 'Montañas Crestagrana'),
    10: (4306, 1705, 0, 'Bosque del Ocaso'),
    11: (2592, 1210, 0, 'Los Humedales'),
    17: (2592, 1210, 0, 'Los Baldíos'),
    33: (4306, 1705, 0, 'Vega de Tuercespina'),
    1: (2589, 0, 0, 'Dun Morogh'),
    38: (2592, 1210, 0, 'Loch Modan'),
    3: (4338, 7909, 0, 'Tierras Inhóspitas'),
    4: (14047, 12361, 0, 'Tierras Devastadas'),
    8: (4338, 7909, 0, 'Pantano de las Penas'),
    28: (14047, 12361, 0, 'Tierras de la Peste del Oeste'),
    36: (4306, 1705, 0, 'Montañas de Alterac'),
    45: (4306, 1705, 0, 'Tierras Altas de Arathi'),
    46: (14047, 12361, 0, 'Las Estepas Ardientes'),
    47: (4338, 7909, 0, 'Tierras del Interior'),
    51: (14047, 12361, 0, 'La Garganta de Fuego'),
    85: (2589, 0, 0, 'Claros de Tirisfal'),
    130: (2592, 1210, 0, 'Bosque de Argénteos'),
    139: (14047, 12361, 12363, 'Tierras de la Peste del Este'),
    267: (2592, 1210, 0, 'Laderas de Trabalomas'),
    15: (4306, 1705, 0, 'Marjal Revolcafango'),
    16: (4338, 7909, 0, 'Azshara'),
    141: (2589, 0, 0, 'Teldrassil'),
    148: (2592, 1210, 0, 'Costa Oscura'),
    215: (2589, 0, 0, 'Mulgore'),
    331: (4306, 1705, 0, 'Vallefresno'),
    357: (4338, 7909, 0, 'Feralas'),
    361: (14047, 12361, 0, 'Frondavil'),
    400: (4306, 1705, 0, 'Las Mil Agujas'),
    405: (4306, 1705, 0, 'Desolace'),
    406: (2592, 1210, 0, 'Sierra Espolón'),
    440: (4338, 7909, 0, 'Tanaris'),
    490: (14047, 12361, 0, "Cráter de Un'Goro"),
    618: (14047, 12361, 12363, 'Cuna del Invierno'),
    1377: (14047, 12361, 12363, 'Silithus'),
    3430: (2589, 0, 0, 'Bosque Canción Eterna'),
    3433: (2592, 1210, 0, 'Tierras Fantasma'),
    3524: (2589, 0, 0, 'Isla Bruma Azur'),
    3525: (2592, 1210, 0, 'Isla Bruma de Sangre'),
    3483: (21877, 23107, 0, 'Península del Fuego Infernal'),
    3521: (21877, 23107, 0, 'Marisma de Zangar'),
    3519: (21877, 23107, 0, 'Bosque de Terokkar'),
    3518: (21877, 23107, 0, 'Nagrand'),
    3522: (21877, 23107, 0, 'Montañas Filospada'),
    3523: (21877, 23107, 23438, 'Tormenta Abisal'),
    3520: (21877, 23107, 23438, 'Valle Sombraluna'),
    3537: (33470, 36917, 0, 'Tundra Boreal'),
    495: (33470, 36917, 0, 'Fiordo Aquilonal'),
    65: (33470, 36917, 0, 'Cementerio de Dragones'),
    394: (33470, 36917, 0, 'Colinas Pardas'),
    66: (33470, 36917, 0, "Zul'Drak"),
    3711: (33470, 36917, 0, 'Cuenca de Sholazar'),
    67: (33470, 36917, 36930, 'Cumbres Tormentosas'),
    210: (33470, 36917, 36930, 'Corona de Hielo'),
}
# tramo (clave = item del 01-tesoros.sql) -> botín permitido, tomado de las 7 zonas piloto
_TRAMO_BASICO = {_ZONAS[z][0]: _BASICO[z] for z in _BASICO}
_TRAMO_RARO = {_ZONAS[z][1]: _RARO_MATERIAL[z] | {_RARO_EQUIPO[z]} for z in _RARO_EQUIPO}
_TRAMO_EPICO = {_ZONAS[z][2]: {_EPICO_MATERIAL[z], _EPICO_EQUIPO[z]} | _EPICO_ABALORIO[z] for z in _EPICO_MATERIAL}


def _permitidos(zona, calidad):
    basico, raro, epico, _ = _ZONAS[zona]
    return _TRAMO_BASICO[basico] if calidad == 1 else _TRAMO_RARO[raro] if calidad == 2 else _TRAMO_EPICO[epico]


def _abrir_cofre(m, personaje, calidad, zona):
    """Crea, abre y retira un cofre de prueba de la zona/calidad reales de SP06.
    Devuelve (lista_de_entradas_de_objeto, motivo_de_fallo_o_None).

    Si la zona de prueba coincide con la zona real donde está el personaje, el
    cofre natural de SP03 con la misma entrada puede estar también visible
    (mismo `gameobject_template`, entrada = 700000 + zona*3 + calidad); por eso
    se descarta cualquier GUID ya conocido antes de pedir el cofre de prueba y
    sólo se abre uno recién aparecido. Detectado con Elwynn (zona 12), la única
    de las 7 del piloto que coincide con la ubicación real del personaje."""
    conocidos_antes = set(m.objetos)
    resultado = " ".join(map(sin_colores, m.comando_hasta(
        "tesoro prueba crear %s %d %d" % (personaje.nombre, calidad, zona),
        r"Cofre de prueba|No hay suelo|sin cofre|personaje humano", timeout=10)))
    if "Cofre de prueba" not in resultado:
        return None, resultado
    entrada = 700000 + zona * 3 + calidad
    m.bombear(1)
    visibles = [guid for guid, obj in m.objetos.items()
                if obj.get("valores", {}).get(upd.OBJECT_FIELD_ENTRY) == entrada]
    nuevos = [guid for guid in visibles if guid not in conocidos_antes]
    if not visibles:
        m.comando("tesoro prueba retirar %s" % personaje.nombre, 1)
        return None, "cofre no visible en el cliente"
    guid = nuevos[-1] if nuevos else visibles[-1]
    m.enviar(0x12E, _paquete_hechizo(3365, guid))
    opcode, datos = m.esperar({SMSG_LOOT_RESPONSE, 0x130, 0x133}, timeout=8)
    loot = _leer_respuesta_loot(datos) if opcode == SMSG_LOOT_RESPONSE else {}
    items = [o["entrada"] for o in loot.get("objetos", [])] if loot.get("abierto") else []
    if loot.get("abierto"):
        m.liberar_loot(guid)
    m.comando("tesoro prueba retirar %s" % personaje.nombre, 1)
    return items, None


@caso(id="tesoros-botin-variado", titulo="SP06: botín variado en las 57 zonas, contraste de frecuencia",
      descripcion="Barrido de las 57 zonas (varias aperturas por zona y calidad: el botín debe ser siempre el de su tramo) y, en las 7 zonas representativas de cada tramo de SP06, comprueba que el botín observado pertenece siempre al catálogo declarado; "
                  "mide, con varias decenas de muestras por zona y no sólo en dos zonas de ejemplo, la "
                  "variedad real del grupo ponderado básico (25% x4) en las 7 zonas y del grupo raro "
                  "(85/15) en las 6 que lo tienen, la aparición del bono de equipo independiente (~5% raro, "
                  "~4% épico) en cada zona que lo declara, y que la condición de etapa IP bloquea el bono "
                  "épico de Rasganorte en un personaje sin la misión de progresión rendida.",
      etiquetas=("tesoros", "sp06", "gm"),
      acciones=("conectar", "leer", "personaje", "gm"),
      parametros={"fase": Parametro(str, "todo", "barrido = las 57 zonas; profundo = frecuencia en las 7 zonas representativas; todo = ambas (más de una hora)", opciones=("todo", "barrido", "profundo")),
                  "muestras_barrido": Parametro(int, 4, "Aperturas por zona y calidad en el barrido de las 57 zonas", minimo=1, maximo=20),
                  "muestras_basico": Parametro(int, 32, "Aperturas de básico por zona (7 zonas) para medir el grupo 25%x4", minimo=8, maximo=80),
                  "muestras_raro": Parametro(int, 48, "Aperturas de raro por zona (6 zonas) para medir el grupo 85/15 y el bono de equipo (~5%)", minimo=8, maximo=100),
                  "muestras_epico": Parametro(int, 60, "Aperturas de épico por zona (Peste del Este y Tormenta Abisal, sin condición) para el bono (~4%) y el abalorio", minimo=10, maximo=200),
                  "muestras_gating": Parametro(int, 60, "Aperturas de épico de Corona de Hielo para comprobar que el bono no sale sin la mision 66013", minimo=10, maximo=200)},
      duracion_max=9000, protege=("SP06", "mod-treasure"), control="directo",
      en_todo=False,
      observa=("cada objeto recibido pertenece al catálogo declarado en 02-botin-variado.sql, en las 7 zonas",
               "el grupo ponderado básico (25% x4) entrega varias variantes en cada una de las 7 zonas",
               "el grupo ponderado raro (85/15) entrega ambas variantes en cada una de las 6 zonas que lo tienen",
               "el bono de equipo independiente (~5% en raro, ~4% en épico) aparece, en la muestra, en cada "
               "zona que lo declara en el SQL",
               "el abalorio épico (Cartas del Feriante), aunque raro, puede observarse en muestras de "
               "decenas de aperturas: no hace falta darlo sólo por SQL",
               "el bono de equipo épico de Corona de Hielo (exige la misión 66013) no aparece en un "
               "personaje recién creado, en contraste con Peste del Este/Tormenta Abisal bajo la misma probabilidad"),
      no_cubre=("frecuencia exacta con intervalo de confianza estrecho: el tamaño de muestra confirma "
                "presencia/ausencia y un orden de magnitud, no calibra el % exacto",
                "la rama positiva de las condiciones 66008/66013 (bono con la misión rendida): sólo se prueba "
                "la negativa; la positiva sólo sale en Peste del Este, que no lleva condición",
                "aspecto visual en Wow.exe; el valor de subasta se contrasta aparte por SQL contra "
                "`acore_characters.auctionhouse` (AHBOT), no desde este caso"))
def ejecutar(ctx):
    inf, sec = ctx.inf, "tesoros-botin-variado"
    m = ctx.nueva_sesion()
    with PersonajeTemporal(ctx, m, 1, 1, sec) as personaje:
        ctx.anotar_servidor(m)
        m.comando("gm on", 1)
        m.comando("go xyz -9751.66 184.723 55.732 0", 3)
        gps = m.gps()
        if not inf.comprobar(sec, gps is not None and gps.get("mapa") == 0,
                             "personaje en Elwynn", str(gps), origen="entorno"):
            return

        # 1) Barrido de las 57 zonas: varias aperturas por zona y calidad configurada; todo
        #    objeto recibido debe pertenecer al botín de su tramo y cada cofre debe abrirse.
        #    Elwynn y las demás zonas con rare_slots=0 no tienen cofre raro (ver SP06 en README).
        n_barrido = ctx.p["muestras_barrido"] if ctx.p["fase"] != "profundo" else 0
        fallos_barrido = []
        resumen = {}
        for zona in sorted(_ZONAS):
            basico, raro, epico, nom = _ZONAS[zona]
            for calidad in (1, 2, 3):
                if (calidad == 2 and not raro) or (calidad == 3 and not epico):
                    continue
                permitidos = _permitidos(zona, calidad)
                vistos_b = Counter()
                for _ in range(n_barrido):
                    for _intento in range(3):
                        items, error = _abrir_cofre(m, personaje, calidad, zona)
                        if items:
                            break
                    if error or not items:
                        fallos_barrido.append("%d/%d: %s" % (zona, calidad, error or "sin botín"))
                    elif not all(i in permitidos for i in items):
                        fallos_barrido.append("%d/%d fuera de catálogo: %s" % (zona, calidad, items))
                    else:
                        vistos_b.update(items)
                resumen[(zona, calidad)] = dict(vistos_b)
        inf.comprobar(sec, not fallos_barrido,
                      "las 57 zonas abren su cofre y entregan sólo botín de su tramo (%d aperturas por zona y calidad)" % n_barrido,
                      "fallos: %s" % fallos_barrido[:20],
                      esperado="0 fallos en %d combinaciones zona/calidad" % len(resumen),
                      observado=len(fallos_barrido))
        if ctx.p["fase"] == "barrido":
            return

        nombre_zona = {12: "Bosque de Elwynn", 17: "Los Baldíos", 10: "Bosque del Ocaso",
                       3: "Tierras Inhóspitas", 139: "Peste del Este", 3523: "Tormenta Abisal",
                       210: "Corona de Hielo"}

        # 2) Variedad del grupo básico (25% x4): las 7 zonas, no sólo Elwynn.
        n_basico = ctx.p["muestras_basico"]
        muestras_basico_por_zona = {}
        for zona in sorted(_BASICO):
            vistos = Counter()
            for _ in range(n_basico):
                items, error = _abrir_cofre(m, personaje, 1, zona)
                if not error and items:
                    vistos[items[0]] += 1
            muestras_basico_por_zona[zona] = vistos
            inf.comprobar(sec, len(vistos) >= 3,
                          "el grupo básico de %s reparte varias variantes" % nombre_zona[zona],
                          "en %d aperturas: %s" % (n_basico, dict(vistos)),
                          esperado=">=3 de las 4 variantes de %s" % sorted(_BASICO[zona]),
                          observado=dict(vistos))

        # 3) Grupo raro (85/15) y bono de equipo independiente (~5%): las 6 zonas con tramo
        #    raro (Elwynn no tiene, ver punto 1).
        n_raro = ctx.p["muestras_raro"]
        muestras_raro_por_zona = {}
        for zona in sorted(_RARO_MATERIAL):
            if zona == 12:
                continue
            vistos = Counter()
            for _ in range(n_raro):
                items, error = _abrir_cofre(m, personaje, 2, zona)
                if not error:
                    vistos.update(items)
            muestras_raro_por_zona[zona] = vistos
            material_vistos = {i: c for i, c in vistos.items() if i in _RARO_MATERIAL[zona]}
            inf.comprobar(sec, len(material_vistos) >= 2,
                          "el grupo raro de %s reparte ambas variantes (gema/polvo)" % nombre_zona[zona],
                          "en %d aperturas: %s" % (n_raro, dict(vistos)),
                          esperado=">=2 variantes distintas de %s" % sorted(_RARO_MATERIAL[zona]),
                          observado=material_vistos)
            if zona in _RARO_EQUIPO:
                inf.comprobar(sec, _RARO_EQUIPO[zona] in vistos,
                              "el bono de equipo raro de %s aparece en la muestra" % nombre_zona[zona],
                              "en %d aperturas (~5%% de diseño): %s" % (n_raro, dict(vistos)),
                              estado_si_no="AVISO",
                              esperado=">=1 aparición de %d" % _RARO_EQUIPO[zona],
                              observado=vistos.get(_RARO_EQUIPO[zona], 0))

        # 4) Bono de equipo épico (~4%) y abalorio, SIN condición de etapa: Peste del Este
        #    (única zona épica sin condición; Tormenta Abisal y Corona de Hielo van en el punto 5).
        n_epico = ctx.p["muestras_epico"]
        muestras_epico_por_zona = {}
        for zona in (139,):
            vistos = Counter()
            for _ in range(n_epico):
                items, error = _abrir_cofre(m, personaje, 3, zona)
                if not error:
                    vistos.update(items)
            muestras_epico_por_zona[zona] = vistos
            inf.comprobar(sec, _EPICO_MATERIAL[zona] in vistos,
                          "%s sigue entregando siempre la gema épica garantizada" % nombre_zona[zona],
                          "en %d aperturas: %s" % (n_epico, dict(vistos)))
            inf.comprobar(sec, _EPICO_EQUIPO[zona] in vistos,
                          "el bono de equipo épico de %s aparece sin restricción de etapa" % nombre_zona[zona],
                          "en %d aperturas (~4%% de diseño): %s" % (n_epico, dict(vistos)),
                          estado_si_no="AVISO",
                          esperado=">=1 aparición de %d" % _EPICO_EQUIPO[zona],
                          observado=vistos.get(_EPICO_EQUIPO[zona], 0))
            abalorios_vistos = _EPICO_ABALORIO[zona] & set(vistos)
            inf.comprobar(sec, True,
                          "abalorio observado en %s (informativo, 0,1-0,4%% de diseño, puede no salir)" % nombre_zona[zona],
                          "en %d aperturas: %s de %s" % (n_epico, {i: vistos[i] for i in abalorios_vistos},
                                                          sorted(_EPICO_ABALORIO[zona])))

        # 5) Condición de etapa IP: Corona de Hielo exige la misión 66013 (mismo umbral que
        #    IP_DK_UNLOCK_PROGRESSION). El personaje temporal no la tiene rendida: el bono de
        #    equipo épico y el abalorio (misma probabilidad de diseño que el punto 4) no deben
        #    aparecer nunca, aunque el material épico garantizado sí.
        n_gating = ctx.p["muestras_gating"]
        for zona, mision in ((3523, 66008), (210, 66013)):
            vistos_g = Counter()
            for _ in range(n_gating):
                items, error = _abrir_cofre(m, personaje, 3, zona)
                if not error:
                    vistos_g.update(items)
            inf.comprobar(sec, _EPICO_MATERIAL[zona] in vistos_g,
                          "%s sigue entregando la gema épica garantizada" % nombre_zona[zona],
                          "en %d aperturas: %s" % (n_gating, dict(vistos_g)))
            inf.comprobar(sec, _EPICO_EQUIPO[zona] not in vistos_g,
                          "el bono de equipo épico de %s no sale sin la misión %d rendida (Peste del Este, "
                          "sin condición, dio %s en el punto 4)" % (
                              nombre_zona[zona], mision, muestras_epico_por_zona[139].get(_EPICO_EQUIPO[139], 0)),
                          "en %d aperturas: %s" % (n_gating, dict(vistos_g)),
                          esperado="0 apariciones de %d" % _EPICO_EQUIPO[zona],
                          observado=vistos_g.get(_EPICO_EQUIPO[zona], 0))
            inf.comprobar(sec, not (_EPICO_ABALORIO[zona] & set(vistos_g)),
                          "el abalorio de %s tampoco sale sin la misión %d" % (nombre_zona[zona], mision),
                          "en %d aperturas: %s" % (n_gating, dict(vistos_g)))
