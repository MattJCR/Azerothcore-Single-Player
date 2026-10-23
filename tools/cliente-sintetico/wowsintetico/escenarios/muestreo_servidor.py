# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Muestreo del coste del servidor con un jugador de prueba presente (UPD-O y similares).

Sin jugador conectado el servidor no tiene bots activos, así que cualquier medida del coste de
una conducta de bots necesita a alguien dentro. Este caso deja un personaje temporal parado en
un sitio y, cada `intervalo` segundos durante `minutos`, anota lo que dice `.server info`: bots
en el mundo y el resumen de los últimos 500 ciclos (media, mediana, percentiles 95 y 99, máximo).
No juzga nada: devuelve OK y deja los datos en el informe (`datos.muestras`, `datos.resumen`)
para comparar dos configuraciones con el mismo método. Quien lo lanza anota la configuración.
"""
import re
import statistics
import time

from ..catalogo import Parametro, caso
from . import PersonajeTemporal, asegurar_vivo

# Goldshire: zona con hierbas y filones, junto a la que se pobló el servidor en las pruebas.
SITIOS = {"goldshire": (0, -9464.0, 64.0, 56.0), "crucepolvo": (1, -844.0, -3700.0, 12.0)}


def _leer_info(texto):
    """Extrae del texto de `.server info` (en inglés o en español) los bots en el mundo y el resumen de ciclos (ms)."""
    d = {}
    m = re.search(r"(?:Characters in world|Personajes en el mundo): (\d+)", texto)
    if m:
        d["en_mundo"] = int(m.group(1))
    m = re.search(r"(?:Mean|Media): (\d+) ?ms.*?(?:Median|Mediana): (\d+) ?ms.*?"
                  r"Percentiles \(95, 99, (?:max|máx\.)\): (\d+) ?ms, (\d+) ?ms, (\d+) ?ms", texto, re.S)
    if m:
        d.update(media=int(m.group(1)), mediana=int(m.group(2)), p95=int(m.group(3)), p99=int(m.group(4)),
                 maximo=int(m.group(5)))
    return d


@caso(id="muestreo-servidor", titulo="Muestreo del coste del servidor con un jugador presente",
      descripcion=__doc__, etiquetas=("rendimiento", "bots", "personaje"),
      acciones=("conectar", "leer", "personaje", "gm"),
      parametros={"minutos": Parametro(int, 10, "duración de la medida", minimo=1, maximo=60),
                  "intervalo": Parametro(int, 30, "segundos entre muestras", minimo=10, maximo=300),
                  "nivel": Parametro(int, 20, "nivel del personaje (los bots se reparten según él)", minimo=1, maximo=79),
                  "sitio": Parametro(str, "goldshire", "dónde se queda parado", opciones=tuple(SITIOS))},
      duracion_max=4200, en_todo=False, protege=("PLAN UPD-O (recolección de bots)",), control="directo",
      observa=("`.server info`: bots en el mundo, media, mediana, p95, p99 y máximo de los últimos 500 ciclos",),
      no_cubre=("el coste por bot de una conducta concreta: sólo el conjunto del servidor",
                "variación entre ejecuciones: los bots se reparten al azar, dos ventanas nunca son idénticas"))
def ejecutar(ctx):
    inf, sec, p = ctx.inf, "muestreo-servidor", ctx.p
    m = ctx.nueva_sesion()
    with PersonajeTemporal(ctx, m, 1, 1, sec):
        ctx.anotar_servidor(m)
        asegurar_vivo(m)
        if p["nivel"] > 1:
            m.comando("levelup %d" % (p["nivel"] - 1), 3)
        mapa, x, y, z = SITIOS[p["sitio"]]
        m.comando("go xyz %f %f %f %d" % (x, y, z, mapa), 4)
        m.bombear(3)
        muestras = []
        fin = time.time() + p["minutos"] * 60
        while time.time() < fin:
            t = time.time()
            lineas = m.comando("server info", 3)
            if not muestras and not _leer_info(" ".join(lineas)):
                inf.anotar(sec, "AVISO", "texto de `.server info` sin reconocer", repr(lineas)[:600])
            info = _leer_info(" ".join(lineas))
            if info:
                info["t"] = round(t - (fin - p["minutos"] * 60))
                muestras.append(info)
            m.bombear(max(1, p["intervalo"] - (time.time() - t)))
        inf.datos["muestras"] = muestras
        if not inf.comprobar(sec, len(muestras) >= 2, "`.server info` se leyó varias veces",
                             "%d muestra(s) en %d min" % (len(muestras), p["minutos"]), origen="cliente"):
            return
        resumen = {}
        for clave in ("en_mundo", "media", "mediana", "p95", "p99", "maximo"):
            v = [s[clave] for s in muestras if clave in s]
            if v:
                resumen[clave] = {"media": round(statistics.mean(v), 1), "mediana": statistics.median(v),
                                  "min": min(v), "max": max(v)}
        inf.datos["resumen"] = resumen
        inf.anotar(sec, "OK", "resumen de %d muestras en %d min" % (len(muestras), p["minutos"]),
                   "; ".join("%s: media %s, mediana %s, rango %s-%s" % (k, v["media"], v["mediana"], v["min"], v["max"])
                             for k, v in resumen.items()))
