// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// SERVER_STATUS_LABELS.unknown compartía texto con "online"
// ("En vivo"), así que el HTML inicial y un sondeo fallido se veían igual
// que un servidor realmente en línea; y un sondeo fallido dejaba el último
// estado sin marcar como desactualizado. Ver test/dom-helpers.js para por
// qué todo este fichero comparte un único mountApp().
import test, { after } from 'node:test';
import assert from 'node:assert/strict';
import { mountApp, flush } from './dom-helpers.js';

let serverStateResponse = null; // null = la petición falla (simula un sondeo caído)

const { shared, document } = await mountApp({
  fetchHandler: (url) => {
    if (url.endsWith('/api/me')) return { status: 401, body: {} };
    if (url.endsWith('/api/players')) return { status: 200, body: { players: [], updatedAt: new Date().toISOString() } };
    if (url.endsWith('/api/server/status')) {
      if (serverStateResponse === null) return { status: 500, body: { error: 'caído' } };
      return { status: 200, body: { state: serverStateResponse } };
    }
    return { status: 404, body: {} };
  },
});

after(() => { shared.stopRefreshPolling(); shared.stopServerStatusPolling(); });

function statusEl() { return document.querySelector('#server-status'); }

test('M54: antes de la primera respuesta, el HTML inicial ya dice "Comprobando", no "En vivo"', () => {
  // Sin llamar todavía a refreshServerStatus(): el estado inicial servido
  // por index.html es el que ve el usuario en el primer instante.
  assert.equal(statusEl().dataset.state, 'checking');
  assert.equal(statusEl().querySelector('span').textContent, 'Comprobando');
});

test('M54: un sondeo correcto pasa a "En vivo" y distingue standby/offline', async () => {
  serverStateResponse = 'online';
  await shared.refreshServerStatus();
  assert.equal(statusEl().dataset.state, 'online');
  assert.equal(statusEl().querySelector('span').textContent, 'En vivo');

  serverStateResponse = 'standby';
  await shared.refreshServerStatus();
  assert.equal(statusEl().dataset.state, 'standby');
  assert.equal(statusEl().querySelector('span').textContent, 'En espera');

  serverStateResponse = 'offline';
  await shared.refreshServerStatus();
  assert.equal(statusEl().dataset.state, 'offline');
  assert.equal(statusEl().querySelector('span').textContent, 'Caído');
});

test('M54: un sondeo fallido pasa a "Sin datos" (distinto de En espera/Caído) con la edad del último bueno', async () => {
  serverStateResponse = 'online';
  await shared.refreshServerStatus();
  assert.equal(statusEl().dataset.state, 'online');

  serverStateResponse = null; // la próxima petición falla
  await shared.refreshServerStatus();
  assert.equal(statusEl().dataset.state, 'stale');
  assert.equal(statusEl().querySelector('span').textContent, 'Sin datos');
  assert.notEqual(statusEl().dataset.state, 'standby');
  assert.notEqual(statusEl().dataset.state, 'offline');
  assert.match(statusEl().title, /Última comprobación correcta/);

  // Recuperación: un sondeo bueno vuelve a quitar el aviso de "sin datos".
  serverStateResponse = 'online';
  await shared.refreshServerStatus();
  assert.equal(statusEl().dataset.state, 'online');
  assert.doesNotMatch(statusEl().title, /Última comprobación correcta/);
});

test('M54: dos sondeos a la vez no se solapan (una petición en vuelo bloquea la siguiente)', async () => {
  let inFlight = 0;
  let maxConcurrent = 0;
  serverStateResponse = 'online';
  let resolvePending;
  const slowFetch = async (url) => {
    if (!url.endsWith('/api/server/status')) return { status: 200, body: {} };
    inFlight += 1;
    maxConcurrent = Math.max(maxConcurrent, inFlight);
    await new Promise((resolve) => { resolvePending = resolve; });
    inFlight -= 1;
    return { status: 200, body: { state: 'online' } };
  };
  const originalFetch = global.fetch;
  global.fetch = slowFetch;
  try {
    const first = shared.refreshServerStatus();
    await flush(1);
    const second = shared.refreshServerStatus(); // debe volver enseguida sin llamar a fetch otra vez
    resolvePending();
    await Promise.all([first, second]);
    assert.equal(maxConcurrent, 1, 'no debe haber dos peticiones de estado en vuelo a la vez');
  } finally {
    global.fetch = originalFetch;
  }
});
