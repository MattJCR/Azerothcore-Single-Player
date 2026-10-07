// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// metricsCache.js — caché compartida de un único vuelo para "Estado y
// rendimiento".
//
// EL PROBLEMA
// Varias personas pueden tener la vista abierta a la vez, y cada una podría
// pedir una muestra al entrar y luego cada cinco minutos. Sin nada en medio,
// eso multiplica las consultas a MySQL/SOAP/systemctl por cada usuario
// conectado en vez de una sola por intervalo — justo lo que hay que
// evitar ("varias aperturas simultáneas no deben multiplicar consultas").
//
// LA SOLUCIÓN
// Una caché de proceso (un solo Node, sin necesidad de coordinarse entre
// varios): si la última muestra tiene menos de `ttlMs`, se devuelve tal
// cual; si ya toca una nueva pero otra petición ya la está pidiendo, todas
// esperan la MISMA promesa en vez de lanzar una cada una (patrón
// "single-flight"). `now`/`fetcher`/`persist` son inyectables para poder
// probar con relojes falsos sin esperar cinco minutos de verdad.
export function createMetricsCache({ ttlMs = 5 * 60_000, now = () => Date.now(), fetcher, persist } = {}) {
  if (typeof fetcher !== 'function') throw new Error('createMetricsCache necesita un fetcher');

  let cached = null; // { at, data }
  let inFlight = null;

  async function get() {
    const nowMs = now();
    if (cached && nowMs - cached.at < ttlMs) {
      return { data: cached.data, fromCache: true, ageMs: nowMs - cached.at };
    }
    if (inFlight) {
      const data = await inFlight;
      return { data, fromCache: true, ageMs: now() - cached.at };
    }

    // el muestreo y su persistencia son dos promesas separadas.
    // Antes, `await persist(...)` corría DENTRO de la promesa que resuelve
    // "inFlight", así que la petición que disparó el muestreo (y cualquier
    // otra que llegara a esperar el mismo vuelo) se quedaba bloqueada hasta
    // que la escritura del historial terminara, pese al comentario de que
    // "no bloquea" — reproducido con persistencia retenida: la primera
    // petición seguía pendiente mientras una segunda, tras esa misma
    // ventana, ya recibía la muestra desde caché.
    inFlight = (async () => {
      const data = await fetcher();
      cached = { at: now(), data };
      return data;
    })();

    let data;
    try {
      data = await inFlight;
    } finally {
      inFlight = null;
    }

    if (persist) {
      // Disparada aparte, después de resolver esta respuesta: un fallo o una
      // escritura lenta no retrasa a nadie ni invalida la muestra ya
      // publicada en `cached`. Se registra el error en vez de tragárselo en
      // silencio, para poder diagnosticar un historial con huecos.
      Promise.resolve()
        .then(() => persist(data, cached.at))
        .catch((error) => {
          console.error('[metricsCache] no se pudo persistir la muestra:', error);
        });
    }

    return { data, fromCache: false, ageMs: 0 };
  }

  // Para pruebas e introspección: no forma parte del contrato normal.
  function peek() {
    return cached;
  }

  return { get, peek };
}
