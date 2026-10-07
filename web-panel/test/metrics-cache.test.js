// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import test from 'node:test';
import assert from 'node:assert/strict';
import { createMetricsCache } from '../src/metricsCache.js';

function fakeClock(start = 1_700_000_000_000) {
  let value = start;
  return { now: () => value, advance: (ms) => { value += ms; } };
}

function countingFetcher(result = 'valor') {
  let calls = 0;
  return { fn: async () => { calls += 1; return result; }, calls: () => calls };
}

test('la primera llamada siempre pide una muestra nueva', async () => {
  const clock = fakeClock();
  const fetcher = countingFetcher('a');
  const cache = createMetricsCache({ now: clock.now, fetcher: fetcher.fn });

  const result = await cache.get();
  assert.equal(result.data, 'a');
  assert.equal(result.fromCache, false);
  assert.equal(fetcher.calls(), 1);
});

test('antes de que pasen 5 minutos, no se consulta otra vez', async () => {
  const clock = fakeClock();
  const fetcher = countingFetcher('a');
  const cache = createMetricsCache({ now: clock.now, fetcher: fetcher.fn });

  await cache.get();
  clock.advance(4 * 60_000 + 59_000); // 4m59s
  const second = await cache.get();

  assert.equal(fetcher.calls(), 1);
  assert.equal(second.fromCache, true);
  assert.equal(second.data, 'a');
});

test('a los 5 minutos exactos ya toca refrescar ("cinco minutos o más")', async () => {
  const clock = fakeClock();
  const fetcher = countingFetcher('a');
  const cache = createMetricsCache({ now: clock.now, fetcher: fetcher.fn, ttlMs: 5 * 60_000 });

  await cache.get();
  clock.advance(5 * 60_000);
  await cache.get();

  assert.equal(fetcher.calls(), 2); // a los 5:00 exactos ya cumple "5 minutos o más"
});

test('pasados los 5 minutos, la siguiente lectura pide una muestra nueva', async () => {
  const clock = fakeClock();
  let call = 0;
  const cache = createMetricsCache({
    now: clock.now,
    fetcher: async () => { call += 1; return `muestra-${call}`; },
  });

  await cache.get();
  clock.advance(5 * 60_000 + 1);
  const second = await cache.get();

  assert.equal(call, 2);
  assert.equal(second.data, 'muestra-2');
  assert.equal(second.fromCache, false);
});

test('varias peticiones a la vez comparten UNA sola muestra en curso', async () => {
  const clock = fakeClock();
  let calls = 0;
  let resolveFetch;
  const cache = createMetricsCache({
    now: clock.now,
    fetcher: () => {
      calls += 1;
      return new Promise((resolve) => { resolveFetch = resolve; });
    },
  });

  const p1 = cache.get();
  const p2 = cache.get();
  const p3 = cache.get();
  assert.equal(calls, 1); // las tres llegaron antes de que la primera terminase

  resolveFetch('compartida');
  const [r1, r2, r3] = await Promise.all([p1, p2, p3]);
  assert.equal(r1.data, 'compartida');
  assert.equal(r2.data, 'compartida');
  assert.equal(r3.data, 'compartida');
  assert.equal(calls, 1);
});

test('una actualización manual también respeta el intervalo mínimo', async () => {
  // No hay un "modo forzado" en la caché a propósito: get() es la única
  // entrada, tanto para la carga automática como para un botón "actualizar"
  // del frontend, así que ambos caminos comparten el mismo límite sin tener
  // que duplicar la comprobación en dos sitios.
  const clock = fakeClock();
  const fetcher = countingFetcher('a');
  const cache = createMetricsCache({ now: clock.now, fetcher: fetcher.fn });

  await cache.get();
  await cache.get();
  await cache.get();

  assert.equal(fetcher.calls(), 1);
});

test('persist() se llama con la muestra y el instante, pero un fallo no rompe get()', async () => {
  const clock = fakeClock();
  const persisted = [];
  let resolvePersistCalled;
  const persistCalled = new Promise((resolve) => { resolvePersistCalled = resolve; });
  const cache = createMetricsCache({
    now: clock.now,
    fetcher: async () => 'valor',
    persist: async (data, at) => { persisted.push({ data, at }); resolvePersistCalled(); throw new Error('MySQL caído'); },
  });

  const result = await cache.get();
  assert.equal(result.data, 'valor');
  // persist() ahora corre aparte de get(): esperar de forma
  // explícita a que se haya llamado, en vez de asumir un orden de microtasks
  // concreto entre su .then() suelto y la resolución de get().
  await persistCalled;
  assert.deepEqual(persisted, [{ data: 'valor', at: clock.now() }]);
});

test('una persistencia lenta no retrasa la respuesta de get()', async () => {
  const clock = fakeClock();
  let resolvePersist;
  const persistStarted = [];
  const cache = createMetricsCache({
    now: clock.now,
    fetcher: async () => 'valor',
    persist: (data) => {
      persistStarted.push(data);
      return new Promise((resolve) => { resolvePersist = resolve; }); // nunca se resuelve durante el test
    },
  });

  const result = await cache.get(); // antes del arreglo, esto se habría quedado esperando a persist()
  assert.equal(result.data, 'valor');
  assert.equal(persistStarted.length, 1); // ya se disparó...
  assert.equal(typeof resolvePersist, 'function'); // ...pero get() no esperó a que terminase
});

test('persist() NO se llama en una lectura servida desde caché', async () => {
  const clock = fakeClock();
  let persistCalls = 0;
  const cache = createMetricsCache({
    now: clock.now,
    fetcher: async () => 'valor',
    persist: async () => { persistCalls += 1; },
  });

  await cache.get();
  await cache.get();
  clock.advance(60_000);
  await cache.get();

  assert.equal(persistCalls, 1);
});
