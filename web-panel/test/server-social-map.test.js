// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import test from 'node:test';
import assert from 'node:assert/strict';
import { createApp } from '../src/app.js';
import { fakePool, sessionCookieFor, listenTestApp } from './helpers.js';

// account 1 tiene dos personajes: 10 (conectado) y 11 (desconectado).
// Personaje 10: grupo con 20; hermandad con 20, 30 y 31; amigo de 30 y 40.
// Conectados ahora mismo: 10, 20, 30 (40 y 31 desconectados).
function charactersDbForSocial({ groupRows = [{ guid: 20, groupType: 0 }] } = {}) {
  return fakePool({
    query: async (sql, params) => {
      if (sql.includes('FROM characters WHERE account = ?')) {
        return [[
          { guid: 10, name: 'Titular', race: 1, class: 1, level: 80, online: 1 },
          { guid: 11, name: 'Alterno', race: 1, class: 8, level: 42, online: 0 },
        ]];
      }
      if (sql.includes('FROM group_member gmMe')) return [groupRows];
      if (sql.includes('FROM guild_member me')) return [[{ guid: 20 }, { guid: 30 }, { guid: 31 }]];
      if (sql.includes('FROM character_social')) return [[{ guid: 30 }, { guid: 40 }]];
      if (sql.includes('WHERE guid IN (?)')) {
        const guids = params[0];
        const all = {
          10: { guid: 10, name: 'Titular', race: 1, class: 1, level: 80, map: 0, zone: 12, position_x: -8900, position_y: -130, position_z: 80, orientation: 0 },
          20: { guid: 20, name: 'Companero', race: 1, class: 2, level: 80, map: 0, zone: 12, position_x: -8901, position_y: -131, position_z: 80, orientation: 0 },
          30: { guid: 30, name: 'Amiga', race: 3, class: 4, level: 74, map: 1, zone: 14, position_x: 100, position_y: 200, position_z: 10, orientation: 0 },
        };
        return [guids.filter((guid) => all[guid]).map((guid) => all[guid])];
      }
      return [[]];
    },
  });
}

function fakeAuthDb() {
  return fakePool({ execute: async () => [[{ gmlevel: 0 }]], query: async () => [[]] });
}

async function startApp(overrides = {}) {
  const app = createApp({
    authDb: fakeAuthDb(), charactersDb: charactersDbForSocial(), worldDb: fakePool(), panelDb: fakePool(),
    soap: { executeCommand: async () => '' }, ...overrides,
  });
  return listenTestApp(app);
}

const cookieFor = sessionCookieFor;

const ME = { id: 1, username: 'JUGADOR' };

test('sin sesión, /api/social/map responde 401', async (t) => {
  const { server, base } = await startApp();
  t.after(() => server.close());
  const response = await fetch(`${base}/api/social/map`);
  assert.equal(response.status, 401);
});

test('elige automáticamente el personaje conectado y marca las relaciones', async (t) => {
  const { server, base } = await startApp();
  t.after(() => server.close());
  const response = await fetch(`${base}/api/social/map`, { headers: { Cookie: cookieFor(ME) } });
  assert.equal(response.status, 200);
  const body = await response.json();
  assert.equal(body.focus.guid, 10);
  assert.equal(body.raid, false);
  const byGuid = Object.fromEntries(body.players.map((player) => [player.guid, player.relation]));
  assert.equal(byGuid[10], 'self');
  assert.equal(byGuid[20], 'party');   // grupo gana a hermandad
  assert.equal(byGuid[30], 'friend');  // amigo gana a hermandad
  assert.equal(body.players.some((player) => player.guid === 40), false); // amigo desconectado
  assert.equal(body.players.some((player) => player.guid === 31), false); // hermandad desconectada
});

test('el flag de banda cambia la relación del grupo a "raid"', async (t) => {
  const { server, base } = await startApp({
    charactersDb: charactersDbForSocial({ groupRows: [{ guid: 20, groupType: 2 }] }),
  });
  t.after(() => server.close());
  const response = await fetch(`${base}/api/social/map`, { headers: { Cookie: cookieFor(ME) } });
  const body = await response.json();
  assert.equal(body.raid, true);
  assert.equal(body.players.find((player) => player.guid === 20).relation, 'raid');
});

test('un character ajeno o inexistente no se usa: cae al personaje conectado', async (t) => {
  const { server, base } = await startApp();
  t.after(() => server.close());
  const response = await fetch(`${base}/api/social/map?character=999`, { headers: { Cookie: cookieFor(ME) } });
  const body = await response.json();
  assert.equal(body.focus.guid, 10);
});

test('sin personaje conectado ni elegido, devuelve el roster y focus nulo', async (t) => {
  const offlineRoster = fakePool({
    query: async (sql) => {
      if (sql.includes('FROM characters WHERE account = ?')) return [[{ guid: 11, name: 'Alterno', race: 1, class: 8, level: 42, online: 0 }]];
      return [[]];
    },
  });
  const { server, base } = await startApp({ charactersDb: offlineRoster });
  t.after(() => server.close());
  const response = await fetch(`${base}/api/social/map`, { headers: { Cookie: cookieFor(ME) } });
  const body = await response.json();
  assert.equal(body.focus, null);
  assert.equal(body.players.length, 0);
  assert.deepEqual(body.characters.map((character) => character.guid), [11]);
});
