// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import test from 'node:test';
import assert from 'node:assert/strict';
import { createApp } from '../src/app.js';
import { fakePool, sessionCookieFor, listenTestApp } from './helpers.js';

// Cuatro personajes online: dos en cuentas "rndbot" + sólo dígitos (bots),
// uno en una cuenta que empieza igual pero no es sólo dígitos (jugador: un
// humano pudo elegir "rndbotarian" de nombre de cuenta) y uno normal.
function charactersDbForPlayers() {
  return fakePool({
    query: async () => [[
      { guid: 1, name: 'Heroina', race: 1, class: 1, gender: 0, level: 60, zone: 12, map: 0, account: 100, talentSpells: null },
      { guid: 2, name: 'Rndbot1', race: 2, class: 2, gender: 0, level: 55, zone: 14, map: 0, account: 200, talentSpells: null },
      { guid: 3, name: 'Rndbot2', race: 2, class: 3, gender: 0, level: 12, zone: 14, map: 0, account: 201, talentSpells: null },
      { guid: 4, name: 'Rndbotarian', race: 1, class: 4, gender: 0, level: 30, zone: 12, map: 0, account: 300, talentSpells: null },
    ]],
  });
}

function fakeAuthDb() {
  return fakePool({
    execute: async () => [[{ gmlevel: 0 }]],
    query: async (sql, params) => {
      if (sql.includes('FROM account WHERE id IN')) {
        const ids = new Set(params[0]);
        const accounts = [
          { id: 100, username: 'JUGADOR' },
          { id: 200, username: 'rndbot1' },
          { id: 201, username: 'RNDBOT2' },
          { id: 300, username: 'rndbotarian' },
        ];
        return [accounts.filter((account) => ids.has(account.id))];
      }
      return [[]];
    },
  });
}

async function startApp(overrides = {}) {
  const app = createApp({
    authDb: fakeAuthDb(), charactersDb: charactersDbForPlayers(), worldDb: fakePool(), panelDb: fakePool(),
    soap: { executeCommand: async () => '' }, ...overrides,
  });
  return listenTestApp(app);
}

const cookieFor = sessionCookieFor;

const ME = { id: 100, username: 'JUGADOR' };

test('sin sesión, /api/players responde 401', async (t) => {
  const { server, base } = await startApp();
  t.after(() => server.close());
  const response = await fetch(`${base}/api/players`);
  assert.equal(response.status, 401);
});

// una cookie con codificación inválida lanzaba dentro de
// parseCookie() y requireAuth lo derivaba al manejador genérico de errores
// (500) en vez de tratarla como sesión inválida (401). No es sólo un
// problema de session.js en aislado: aquí se comprueba que app.js, con
// requireAuth de verdad en la cadena de middlewares, también responde bien.
test('una cookie con codificación inválida responde 401, no 500', async (t) => {
  const { server, base } = await startApp();
  t.after(() => server.close());
  const response = await fetch(`${base}/api/players`, { headers: { Cookie: 'acore_session=%' } });
  assert.equal(response.status, 401);
});

test('una cookie truncada (token sin firma) responde 401', async (t) => {
  const { server, base } = await startApp();
  t.after(() => server.close());
  const response = await fetch(`${base}/api/players`, { headers: { Cookie: 'acore_session=solo-payload-sin-punto' } });
  assert.equal(response.status, 401);
});

test('clasifica Jugador/Bot por el prefijo de cuenta, no sólo por el nombre de personaje', async (t) => {
  const { server, base } = await startApp();
  t.after(() => server.close());
  const response = await fetch(`${base}/api/players`, { headers: { Cookie: cookieFor(ME) } });
  assert.equal(response.status, 200);
  const body = await response.json();
  const byGuid = Object.fromEntries(body.players.map((player) => [player.guid, player.type]));
  assert.equal(byGuid[1], 'player');
  assert.equal(byGuid[2], 'bot');
  assert.equal(byGuid[3], 'bot');
  // "rndbotarian" empieza como el prefijo pero no es sólo dígitos detrás: jugador.
  assert.equal(byGuid[4], 'player');
});

test('sin personajes online, no consulta cuentas y no revienta', async (t) => {
  let queried = false;
  const emptyCharactersDb = fakePool({ query: async () => [[]] });
  const { server, base } = await startApp({
    charactersDb: emptyCharactersDb,
    authDb: fakePool({
      execute: async () => [[{ gmlevel: 0 }]],
      query: async () => { queried = true; return [[]]; },
    }),
  });
  t.after(() => server.close());
  const response = await fetch(`${base}/api/players`, { headers: { Cookie: cookieFor(ME) } });
  const body = await response.json();
  assert.deepEqual(body.players, []);
  assert.equal(queried, false);
});
