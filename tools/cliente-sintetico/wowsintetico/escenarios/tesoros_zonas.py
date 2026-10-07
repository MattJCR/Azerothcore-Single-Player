# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""SP03: recorre las 57 zonas del catálogo de mod-treasure para que el validador
automático apruebe al menos un punto y se materialice un cofre en cada una.

No es una prueba de regresión al uso: no borra nada al terminar. Los puntos
aprobados y las plazas asignadas son el estado normal y permanente de SP03
(zonas activas de fábrica, per README de mod-treasure), no datos de prueba;
sólo hacía falta que alguien las visitara una vez para que el validador
tuviera ocasión de aprobar candidatos cerca. El personaje temporal usado para
recorrerlas se borra igual que en cualquier otro caso.
"""
import re

from ..catalogo import caso
from . import PersonajeTemporal, sin_colores

ESTADO_RE = re.compile(r"Puntos aprobados (\d+)/")
PUNTO_RE = re.compile(r"calidad 1 plaza \d+: punto ([1-9]\d*) .*? pos ([-\d.]+) ([-\d.]+) ([-\d.]+)")


def _estado(m):
    return " ".join(sin_colores(linea) for linea in m.comando("tesoro estado", 2))


# 3 candidatos por zona, tomados de sp_treasure_point (nodos de recolección ya
# desplazados por el propio módulo); no todos tienen por qué validar, de ahí
# los tres intentos por zona antes de darla por fallida.
TODAS_LAS_ZONAS = (
    (1, 0, ((-5070.87, -52.70, 394.439), (-5402.62, 516.12, 386.359), (-5635.47, -666.35, 403.311))),  # Dun Morogh
    (3, 0, ((-6484.90, -3379.76, 261.872), (-6787.10, -2961.94, 249.699), (-7196.89, -3129.44, 288.870))),  # Tierras Inhóspitas
    (4, 0, ((-11253.70, -2701.60, 15.438), (-11513.30, -2961.58, 35.674), (-11283.20, -3423.87, 10.032))),  # Tierras Devastadas
    (8, 0, ((-9771.24, -3738.16, 28.737), (-9777.10, -4428.63, -1.540), (-10051.70, -3113.76, 32.044))),  # Pantano de las Penas
    (10, 0, ((-10415.60, -1257.29, 52.580), (-11191.60, -874.03, 80.832), (-10600.20, -1483.00, 94.350))),  # Bosque del Ocaso
    (11, 0, ((-2579.45, -1723.57, 10.207), (-3423.72, -2121.09, 94.661), (-3458.28, -1839.76, 18.645))),  # Los Humedales
    (12, 0, ((-9893.42, -226.00, 41.339), (-8996.22, -1378.25, 125.361), (-9790.68, 123.07, 25.896))),  # Bosque de Elwynn
    (14, 1, ((-237.70, -4737.49, 30.961), (2.28, -4578.69, 53.998), (-808.89, -5358.79, 2.165))),  # Durotar
    (15, 1, ((-4297.98, -3023.16, 34.995), (-4046.31, -3541.31, 30.453), (-3259.52, -2844.30, 30.870))),  # Marjal Revolcafango
    (16, 1, ((2250.08, -7126.77, -14.941), (3801.09, -4846.40, 158.188), (3249.98, -4371.95, 126.518))),  # Azshara
    (17, 1, ((-724.33, -2347.20, 137.162), (384.74, -2245.49, 196.681), (-1711.27, -3156.70, 93.510))),  # Los Baldíos
    (28, 0, ((1402.24, -1106.51, 71.519), (2330.20, -1935.16, 115.164), (1085.51, -1647.78, 67.952))),  # Tierras de la Peste del Oeste
    (33, 0, ((-11502.80, -658.91, 32.110), (-11680.80, 226.79, 39.692), (-12557.90, -180.46, 13.932))),  # Vega de Tuercespina
    (36, 0, ((-27.67, -221.65, 138.074), (1017.78, -354.29, 61.560), (776.23, -346.30, 151.973))),  # Montañas de Alterac
    (38, 0, ((-4775.68, -4055.91, 311.679), (-5796.06, -2963.43, 374.455), (-4749.33, -3073.86, 313.461))),  # Loch Modan
    (40, 0, ((-9914.44, 1728.46, 30.770), (-11408.40, 1935.58, 10.229), (-11612.10, 2182.64, -45.597))),  # Páramos de Poniente
    (44, 0, ((-8940.11, -2183.28, 140.537), (-9741.10, -2925.08, 67.047), (-8769.31, -2456.81, 154.145))),  # Montañas Crestagrana
    (45, 0, ((-1709.25, -3222.51, 37.577), (-1056.01, -3763.98, 107.311), (-1934.63, -2390.06, 77.025))),  # Tierras Altas de Arathi
    (46, 0, ((-8043.05, -2989.31, 143.277), (-7737.77, -2309.65, 141.825), (-8399.14, -1012.59, 190.268))),  # Las Estepas Ardientes
    (47, 0, ((-489.37, -4719.62, -29.846), (195.37, -3814.73, 133.341), (396.03, -4750.78, -9.891))),  # Tierras del Interior
    (51, 0, ((-6666.01, -887.52, 254.610), (-6741.21, -610.85, 241.217), (-7229.66, -1430.91, 266.900))),  # La Garganta de Fuego
    (65, 571, ((4914.59, -1235.26, 174.732), (3980.66, -501.29, 234.313), (2873.91, -757.43, 35.489))),  # Cementerio de Dragones
    (66, 571, ((5536.81, -1707.75, 243.412), (6816.88, -4162.86, 463.530), (5407.07, -2534.09, 292.406))),  # Zul'Drak
    (67, 571, ((6888.50, 37.61, 792.318), (7849.87, -2721.05, 1135.850), (6752.77, -732.89, 747.132))),  # Cumbres Tormentosas
    (85, 0, ((1702.92, 771.66, 69.063), (2676.63, 1071.51, 116.882), (1748.07, 1164.62, 75.095))),  # Claros de Tirisfal
    (130, 0, ((-324.50, 930.97, 131.106), (796.86, 1688.58, 27.334), (1265.82, 1024.38, 43.539))),  # Bosque de Argénteos
    (139, 0, ((2347.30, -3592.84, 180.113), (1848.97, -3357.77, 121.265), (2216.45, -5167.62, 55.044))),  # Tierras de la Peste del Este
    (141, 1, ((10617.30, 2050.48, 1337.850), (10202.10, 1769.65, 1338.030), (9588.16, 1087.26, 1267.090))),  # Teldrassil
    (148, 1, ((6252.61, 786.11, -11.711), (6521.72, 711.02, -36.638), (5802.97, 111.22, 31.723))),  # Costa Oscura
    (210, 571, ((8027.82, 1738.44, 393.779), (7028.88, 2512.16, 409.558), (6580.08, 1057.04, 283.587))),  # Corona de Hielo
    (215, 1, ((-1502.99, 375.20, 67.053), (-554.20, -469.59, 26.319), (-1165.96, -985.11, 2.301))),  # Mulgore
    (267, 0, ((-753.12, -980.95, 54.229), (-172.26, 21.16, 82.234), (-256.99, -1491.46, 100.147))),  # Laderas de Trabalomas
    (331, 1, ((1736.19, -2038.20, 108.563), (3140.42, -1479.39, 203.235), (3528.58, 495.38, 9.419))),  # Vallefresno
    (357, 1, ((-3123.68, 1822.67, 46.930), (-4011.25, 1942.18, 104.728), (-5218.19, 1818.32, 117.495))),  # Feralas
    (361, 1, ((5927.70, -1237.80, 388.772), (6306.93, -852.93, 416.503), (5139.88, -459.37, 301.225))),  # Frondavil
    (394, 571, ((4151.42, -4611.01, 144.271), (3486.29, -3444.50, 267.081), (4759.86, -4708.72, 54.177))),  # Colinas Pardas
    (400, 1, ((-5492.38, -3420.00, -37.019), (-6252.66, -3551.20, -58.750), (-5454.16, -2200.77, -57.579))),  # Las Mil Agujas
    (405, 1, ((-196.44, 1113.09, 87.224), (-1517.24, 2329.11, 97.411), (-1847.22, 1459.77, 66.059))),  # Desolace
    (406, 1, ((2445.04, 1100.72, 338.232), (1265.68, -11.38, -5.890), (1760.82, 846.00, 148.717))),  # Sierra Espolón
    (440, 1, ((-8730.15, -2166.74, 19.214), (-8792.83, -2347.61, 12.770), (-7965.87, -5083.84, 19.403))),  # Tanaris
    (490, 1, ((-6832.09, -1685.69, -264.789), (-7075.35, -1194.03, -248.725), (-6406.83, -1914.53, -262.490))),  # Cráter de Un'Goro
    (495, 571, ((1660.28, -6278.71, -0.203), (2112.95, -5703.52, 220.806), (898.97, -4540.75, 159.154))),  # Fiordo Aquilonal
    (618, 1, ((6466.51, -3881.10, 663.217), (6861.47, -2973.88, 605.116), (6837.70, -3706.99, 735.866))),  # Cuna del Invierno
    (1377, 1, ((-6556.39, 1672.14, 34.708), (-6328.24, 1631.31, 25.683), (-6402.55, 585.31, 2.785))),  # Silithus
    (3430, 530, ((9030.70, -6325.94, 15.221), (8241.14, -6561.06, 82.928), (9337.20, -6758.92, 23.027))),  # Bosque Canción Eterna
    (3433, 530, ((7483.24, -7637.14, 138.604), (7104.62, -7563.57, 48.569), (7416.77, -6039.29, 11.616))),  # Tierras Fantasma
    (3483, 530, ((74.13, 3043.45, -0.683), (-451.80, 4582.69, 45.165), (-725.69, 2254.43, 13.296))),  # Península del Fuego Infernal
    (3518, 530, ((-801.55, 7539.71, 66.295), (-1116.07, 8678.14, 54.286), (-1581.00, 8118.52, -98.640))),  # Nagrand
    (3519, 530, ((-3141.46, 4454.74, -22.821), (-2350.25, 3070.07, 21.978), (-2067.14, 3349.48, -60.380))),  # Bosque de Terokkar
    (3520, 530, ((-3189.22, 1822.89, 128.106), (-3358.78, 2397.34, 61.484), (-3356.44, 2756.03, 129.291))),  # Valle Sombraluna
    (3521, 530, ((-1068.17, 5750.82, 54.158), (-1314.40, 5746.08, 33.784), (-499.44, 5781.34, -21.383))),  # Marisma de Zangar
    (3522, 530, ((2126.73, 6457.26, 4.116), (2104.02, 6338.76, 3.005), (2902.06, 6836.39, 364.487))),  # Montañas Filospada
    (3523, 530, ((4773.76, 2390.56, 126.019), (3356.12, 3469.75, 139.691), (2596.92, 4118.21, 149.764))),  # Tormenta Abisal
    (3524, 530, ((-4688.53, -11543.50, 27.088), (-4532.73, -12662.90, 16.832), (-4738.24, -12183.40, 18.621))),  # Isla Bruma Azur
    (3525, 530, ((-1623.93, -12286.00, -16.118), (-1987.83, -11510.90, 59.790), (-1348.95, -12588.80, 10.494))),  # Isla Bruma de Sangre
    (3537, 571, ((3030.06, 5503.01, 51.805), (3806.03, 4620.65, -4.256), (2897.29, 5242.04, 62.275))),  # Tundra Boreal
    (3711, 571, ((5933.65, 5467.16, -91.341), (4815.72, 5388.67, -76.099), (6170.94, 5256.57, -127.884))),  # Cuenca de Sholazar
)


@caso(id="tesoros-zonas-todas", titulo="SP03: validar automáticamente las 57 zonas del catálogo",
      descripcion="Recorre las 57 zonas de mod-treasure sin usar .tesoro validar, dando tiempo al "
                  "validador automático en cada una. No aborta al primer fallo: sigue con el resto y "
                  "reporta al final qué zonas quedaron sin punto aprobado.",
      etiquetas=("tesoros", "sp03", "gm", "despliegue"),
      acciones=("conectar", "leer", "personaje", "gm"),
      duracion_max=3600, protege=("SP03", "mod-treasure"), control="directo",
      en_todo=False,
      observa=("cada zona activa con al menos un punto aprobado y un cofre materializado",),
      no_cubre=("aspecto visual en Wow.exe", "el resto de candidatos de cada zona más allá del primero"))
def ejecutar_todas(ctx):
    inf, sec = ctx.inf, "tesoros-zonas-todas"
    m = ctx.nueva_sesion()
    ok, fallidas = [], []
    with PersonajeTemporal(ctx, m, 1, 1, sec):
        ctx.anotar_servidor(m)
        m.comando("gm on", 1)
        for zona, mapa, anclas in TODAS_LAS_ZONAS:
            resultado = "sin visitar"
            aprobado = False
            for x, y, z in anclas:
                m.comando("go xyz %.3f %.3f %.3f %d" % (x, y, z, mapa), 4)
                gps = m.gps()
                if not (gps is not None and gps.get("mapa") == mapa):
                    resultado = "no llegó al mapa %s: %s" % (mapa, gps)
                    continue
                m.bombear(15)
                resultado = _estado(m)
                aprobados = ESTADO_RE.search(resultado)
                if ("Tesoros zona %d: activos" % zona in resultado and aprobados
                        and int(aprobados.group(1)) > 0 and PUNTO_RE.search(resultado)):
                    aprobado = True
                    break
            (ok if aprobado else fallidas).append(zona)
            inf.comprobar(sec, aprobado,
                          "zona %d valida y asigna cofre automáticamente" % zona,
                          resultado, esperado=True, observado=aprobado)
        inf.anotar(sec, "INFO", "resumen del recorrido de las 57 zonas",
                   "%d con punto aprobado, %d sin aprobar tras 3 candidatos: %s" %
                   (len(ok), len(fallidas), fallidas))
