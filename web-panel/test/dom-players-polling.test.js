// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// shared.refresh() pedía /api/map/players en cada sondeo
// aunque el mapa GM estuviera oculto, y players.render() reconstruía toda la
// tabla en cada respuesta aunque los datos no hubieran cambiado. Ninguna
// prueba existente cubría esto (jsdom + mountApp(), test/dom-helpers.js, es
// la "nueva forma de probar" para lo que no depende de datos reales/GM en
// vivo). Un único mountApp() para todo el fichero: los tests
// se leen de arriba abajo como una sola sesión que va cambiando de escenario.
import test, { after } from 'node:test';
import assert from 'node:assert/strict';
import { mountApp, flush } from './dom-helpers.js';

function makePlayer(overrides = {}) {
  return { guid: 1, name: 'Arthas', race: 1, class: 1, gender: 0, level: 60, zone: 1, map: 0, role: 'tank', type: 'player', ...overrides };
}

const calls = [];
let playersResponse = () => [makePlayer()];
let playersAuthorized = true;

const { shared, document, setFetchHandler } = await mountApp({
  fetchHandler: (url) => {
    calls.push(url);
    if (url.endsWith('/api/me')) return { status: 401, body: {} };
    if (url.endsWith('/api/players')) {
      return playersAuthorized
        ? { status: 200, body: { players: playersResponse(), updatedAt: new Date().toISOString() } }
        : { status: 401, body: { error: 'Sesión no válida o caducada' } };
    }
    if (url.endsWith('/api/map/players')) return { status: 200, body: { players: [], updatedAt: new Date().toISOString() } };
    if (url.endsWith('/api/server/status')) return { status: 200, body: { state: 'online' } };
    return { status: 404, body: { error: 'no configurado' } };
  },
});
void setFetchHandler; // no se necesita cambiar de handler completo en este fichero, sólo las variables que lee

after(() => { shared.stopRefreshPolling(); shared.stopServerStatusPolling(); });

test('M53: no pide /api/map/players mientras el mapa GM está oculto, y sí al abrirlo', async () => {
  shared.showApp({ username: 'ADMIN', gmlevel: 3, isGm: true });
  await flush();

  assert.ok(calls.some((url) => url.endsWith('/api/players')), 'debe pedir jugadores');
  assert.ok(!calls.some((url) => url.endsWith('/api/map/players')), 'no debe pedir el mapa con la vista oculta');

  calls.length = 0;
  shared.showView('map');
  await flush();
  assert.ok(calls.some((url) => url.endsWith('/api/map/players')), 'al abrir el mapa debe pedir sus datos ya, no esperar al próximo sondeo');

  calls.length = 0;
  shared.showView('players');
  await shared.runRefresh();
  await flush();
  assert.ok(!calls.some((url) => url.endsWith('/api/map/players')), 'al volver a Jugadores, el sondeo general no debe seguir pidiendo el mapa');
});

test('M53: la tabla no se reconstruye en un sondeo cuyos datos no cambiaron', async () => {
  // viewRegistry.players es el objeto de exports de players.js: sus
  // propiedades son de sólo lectura (namespace de módulo ES), así que no se
  // puede parchear render() in situ — se sustituye la entrada del registro
  // entera, que sí es un objeto mutable normal (`export const viewRegistry = {}`).
  let renderCalls = 0;
  const playersModule = shared.viewRegistry.players;
  shared.viewRegistry.players = { ...playersModule, render: (...args) => { renderCalls += 1; return playersModule.render(...args); } };
  try {
    // Un guid/nivel que no haya salido en un test anterior: la firma de
    // M53 vive en shared.js (todo el fichero comparte un único mountApp()),
    // así que si se reusaran los mismos datos, esta comprobación heredaría
    // el "sin cambios" de otro test en vez de partir de cero.
    playersResponse = () => [makePlayer({ guid: 2, level: 55 })];
    await shared.runRefresh();
    await flush();
    const afterFirst = renderCalls;
    assert.ok(afterFirst >= 1, 'debe haber pintado con estos datos al menos una vez');
    assert.equal(document.querySelector('#players-body').children.length, 1);

    await shared.runRefresh();
    await flush();
    assert.equal(renderCalls, afterFirst, 'un sondeo con los mismos jugadores no debe reconstruir la tabla');

    // Un cambio real (p. ej. subió de nivel) sí debe volver a pintar.
    playersResponse = () => [makePlayer({ guid: 2, level: 56 })];
    await shared.runRefresh();
    await flush();
    assert.equal(renderCalls, afterFirst + 1, 'un cambio real en los datos sí debe volver a pintar');
  } finally {
    shared.viewRegistry.players = playersModule;
  }
});

test('M53: perder la sesión durante un sondeo vuelve al login', async () => {
  playersAuthorized = true;
  shared.showApp({ username: 'JUGADOR', gmlevel: 0, isGm: false });
  await flush();
  assert.ok(!document.querySelector('#app-view').classList.contains('hidden'));

  playersAuthorized = false;
  await shared.runRefresh();
  await flush();
  assert.ok(document.querySelector('#app-view').classList.contains('hidden'), 'un 401 a media sesión debe volver al login');
  assert.ok(!document.querySelector('#login-view').classList.contains('hidden'));
});
