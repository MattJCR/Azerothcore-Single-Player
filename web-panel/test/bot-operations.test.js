// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import test from 'node:test';
import assert from 'node:assert/strict';
import { createApp } from '../src/app.js';
import { fakePool, sessionCookieFor, listenTestApp } from './helpers.js';

function fakeAuthDb({ gmLevelsByAccountId = {} } = {}) {
  return fakePool({
    execute: async (sql, params) => {
      if (sql.includes('FROM account_access')) return [[{ gmlevel: gmLevelsByAccountId[params[0]] ?? 0 }]];
      return [[]];
    },
  });
}

// Simula bot_operations_snapshot (una fila fija) y bot_operations_action
// (lista mutable), sólo por el fragmento de SQL que las toca — mismo patrón
// que fakePanelConfigDb en server-config.test.js.
function fakeWorldDb({ snapshotRow = null, actionRows = [] } = {}) {
  let nextId = actionRows.length ? Math.max(...actionRows.map((row) => row.id)) + 1 : 1;
  return fakePool({
    query: async (sql, params) => {
      if (sql.includes('FROM bot_operations_snapshot')) {
        return [snapshotRow ? [snapshotRow] : []];
      }
      if (sql.includes('FROM bot_operations_action ORDER BY id DESC LIMIT')) {
        const [limit] = params;
        return [[...actionRows].sort((a, b) => b.id - a.id).slice(0, limit)];
      }
      if (sql.includes("status = 'pending' ORDER BY id DESC LIMIT 1")) {
        const [action, param] = params;
        const found = [...actionRows].reverse()
          .find((row) => row.action === action && row.param === param && row.status === 'pending');
        return [found ? [{ id: found.id }] : []];
      }
      if (sql.includes('requested_at > (NOW() - INTERVAL')) {
        const [action, param, windowSeconds] = params;
        const cutoffMs = Date.now() - windowSeconds * 1000;
        const found = [...actionRows].reverse()
          .find((row) => row.action === action && row.param === param && row.status !== 'pending' && row.requestedAt.getTime() > cutoffMs);
        return [found ? [{ id: found.id }] : []];
      }
      return [[]];
    },
    execute: async (sql, params) => {
      if (sql.includes('INSERT INTO bot_operations_action')) {
        const [action, param, reason, actorAccount, actorName] = params;
        // Simula la clave única generada `pending_key` (bot_operations.sql):
        // sólo puede existir una fila 'pending' por
        // acción+param, igual que en MySQL/MariaDB real.
        if (actionRows.some((row) => row.action === action && row.param === param && row.status === 'pending')) {
          const error = new Error('Duplicate entry for key uniq_bot_operations_action_pending');
          error.code = 'ER_DUP_ENTRY';
          throw error;
        }
        const id = nextId++;
        actionRows.push({
          id, action, param, reason, actorAccount, actorName,
          status: 'pending', result: null, requestedAt: new Date(), completedAt: null,
        });
        return [{ insertId: id }];
      }
      return [{}];
    },
  });
}

async function startApp(gmlevel, { snapshotRow, actionRows } = {}) {
  const app = createApp({
    authDb: fakeAuthDb({ gmLevelsByAccountId: { 1: gmlevel } }),
    worldDb: fakeWorldDb({ snapshotRow, actionRows }),
    panelDb: fakePool(),
  });
  return listenTestApp(app);
}

const ACTOR = { id: 1, username: 'ADMIN' };
const JSON_HEADERS = { 'Content-Type': 'application/json', 'X-Panel-Request': '1' };

const SNAPSHOT_ROW = {
  reservations: { onlineTotal: 40 }, world_stage: { stage: 2 }, world_pvp: { activeEvents: [] },
  queues: { waitingHumans: 0 }, groups: { groupsFormed: 0 }, quest_mates: { activePairings: 0 },
  guilds: { homeGuilds: 0 },
};

// ── Permisos ────────────────────────────────────────────────────────────
test('sin sesión, /snapshot y /actions responden 401', async (t) => {
  const { server, base } = await startApp(0);
  t.after(() => server.close());
  assert.equal((await fetch(`${base}/api/bot-operations/snapshot`)).status, 401);
  assert.equal((await fetch(`${base}/api/bot-operations/actions`)).status, 401);
  const post = await fetch(`${base}/api/bot-operations/actions`, { method: 'POST', headers: JSON_HEADERS, body: '{}' });
  assert.equal(post.status, 401);
});

test('GM1 puede leer, pero no solicitar acciones (reservado a GM3)', async (t) => {
  const { server, base } = await startApp(1, { snapshotRow: { ...SNAPSHOT_ROW, updated_at: new Date() } });
  t.after(() => server.close());
  const snapshot = await fetch(`${base}/api/bot-operations/snapshot`, { headers: { Cookie: sessionCookieFor(ACTOR) } });
  assert.equal(snapshot.status, 200);
  const actions = await fetch(`${base}/api/bot-operations/actions`, { headers: { Cookie: sessionCookieFor(ACTOR) } });
  assert.equal(actions.status, 200);
  const post = await fetch(`${base}/api/bot-operations/actions`, {
    method: 'POST', headers: { ...JSON_HEADERS, Cookie: sessionCookieFor(ACTOR) },
    body: JSON.stringify({ action: 'queue_bots_pass' }),
  });
  assert.equal(post.status, 403);
});

test('un usuario sin GM recibe 403 aunque pida las rutas directamente', async (t) => {
  const { server, base } = await startApp(0);
  t.after(() => server.close());
  const snapshot = await fetch(`${base}/api/bot-operations/snapshot`, { headers: { Cookie: sessionCookieFor(ACTOR) } });
  assert.equal(snapshot.status, 403);
});

test('GM3 sin la cabecera X-Panel-Request no puede solicitar una acción (CSRF)', async (t) => {
  const { server, base } = await startApp(3);
  t.after(() => server.close());
  const response = await fetch(`${base}/api/bot-operations/actions`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', Cookie: sessionCookieFor(ACTOR) },
    body: JSON.stringify({ action: 'queue_bots_pass' }),
  });
  assert.equal(response.status, 403);
});

// ── Payload inválido ────────────────────────────────────────────────────
test('una acción no reconocida se rechaza con 400', async (t) => {
  const { server, base } = await startApp(3);
  t.after(() => server.close());
  const response = await fetch(`${base}/api/bot-operations/actions`, {
    method: 'POST', headers: { ...JSON_HEADERS, Cookie: sessionCookieFor(ACTOR) },
    body: JSON.stringify({ action: 'teletransportar_bots' }),
  });
  assert.equal(response.status, 400);
});

test('un parámetro no numérico en stop_world_pvp se rechaza con 400', async (t) => {
  const { server, base } = await startApp(3);
  t.after(() => server.close());
  const response = await fetch(`${base}/api/bot-operations/actions`, {
    method: 'POST', headers: { ...JSON_HEADERS, Cookie: sessionCookieFor(ACTOR) },
    body: JSON.stringify({ action: 'stop_world_pvp', param: 'DROP TABLE bots' }),
  });
  assert.equal(response.status, 400);
});

test('stop_world_pvp acepta uint32 máximo y rechaza el primer valor fuera de rango', async (t) => {
  const rows = [];
  const { server, base } = await startApp(3, { actionRows: rows });
  t.after(() => server.close());
  const headers = { ...JSON_HEADERS, Cookie: sessionCookieFor(ACTOR) };
  const send = (param) => fetch(`${base}/api/bot-operations/actions`, {
    method: 'POST', headers,
    body: JSON.stringify({ action: 'stop_world_pvp', param }),
  });
  assert.equal((await send('4294967295')).status, 202);
  assert.equal((await send('4294967296')).status, 400);
  assert.equal(rows.length, 1);
});

test('un parámetro en una acción que no lo admite se rechaza con 400', async (t) => {
  const { server, base } = await startApp(3);
  t.after(() => server.close());
  const response = await fetch(`${base}/api/bot-operations/actions`, {
    method: 'POST', headers: { ...JSON_HEADERS, Cookie: sessionCookieFor(ACTOR) },
    body: JSON.stringify({ action: 'queue_bots_pass', param: '5' }),
  });
  assert.equal(response.status, 400);
});

test('un motivo demasiado largo se rechaza con 400', async (t) => {
  const { server, base } = await startApp(3);
  t.after(() => server.close());
  const response = await fetch(`${base}/api/bot-operations/actions`, {
    method: 'POST', headers: { ...JSON_HEADERS, Cookie: sessionCookieFor(ACTOR) },
    body: JSON.stringify({ action: 'queue_bots_pass', reason: 'x'.repeat(300) }),
  });
  assert.equal(response.status, 400);
});

// ── Caso feliz, idempotencia y enfriamiento ────────────────────────────
test('GM3 solicita una acción válida: 202 con id, y queda "pending"', async (t) => {
  const actionRows = [];
  const { server, base } = await startApp(3, { actionRows });
  t.after(() => server.close());
  const response = await fetch(`${base}/api/bot-operations/actions`, {
    method: 'POST', headers: { ...JSON_HEADERS, Cookie: sessionCookieFor(ACTOR) },
    body: JSON.stringify({ action: 'world_bots_pass', reason: 'zona vacía' }),
  });
  assert.equal(response.status, 202);
  const body = await response.json();
  assert.equal(typeof body.id, 'number');
  assert.equal(actionRows.length, 1);
  assert.equal(actionRows[0].status, 'pending');
  assert.equal(actionRows[0].reason, 'zona vacía');
});

test('una acción repetida mientras la anterior sigue pendiente devuelve el mismo id (idempotencia)', async (t) => {
  const actionRows = [];
  const { server, base } = await startApp(3, { actionRows });
  t.after(() => server.close());
  const first = await fetch(`${base}/api/bot-operations/actions`, {
    method: 'POST', headers: { ...JSON_HEADERS, Cookie: sessionCookieFor(ACTOR) },
    body: JSON.stringify({ action: 'queue_bots_pass' }),
  });
  const second = await fetch(`${base}/api/bot-operations/actions`, {
    method: 'POST', headers: { ...JSON_HEADERS, Cookie: sessionCookieFor(ACTOR) },
    body: JSON.stringify({ action: 'queue_bots_pass' }),
  });
  assert.equal(first.status, 202);
  assert.equal(second.status, 202);
  const firstBody = await first.json();
  const secondBody = await second.json();
  assert.equal(firstBody.id, secondBody.id);
  assert.equal(actionRows.length, 1);   // no se duplicó la fila
});

// la prueba de arriba manda las dos peticiones en SERIE. Ésta
// las dispara con Promise.all, sin esperar entre medias, contra el mismo
// camino HTTP/Express real — ejercita la restricción `pending_key` de A3
// frente a un doble clic genuino, no sólo secuencial.
test('dos peticiones simultáneas de verdad de la misma acción comparten un único id', async (t) => {
  const actionRows = [];
  const { server, base } = await startApp(3, { actionRows });
  t.after(() => server.close());
  const [first, second] = await Promise.all([
    fetch(`${base}/api/bot-operations/actions`, {
      method: 'POST', headers: { ...JSON_HEADERS, Cookie: sessionCookieFor(ACTOR) },
      body: JSON.stringify({ action: 'queue_bots_pass' }),
    }),
    fetch(`${base}/api/bot-operations/actions`, {
      method: 'POST', headers: { ...JSON_HEADERS, Cookie: sessionCookieFor(ACTOR) },
      body: JSON.stringify({ action: 'queue_bots_pass' }),
    }),
  ]);
  assert.equal(first.status, 202);
  assert.equal(second.status, 202);
  const firstBody = await first.json();
  const secondBody = await second.json();
  assert.equal(firstBody.id, secondBody.id);
  assert.equal(actionRows.length, 1);
});

test('una acción resuelta hace poco se rechaza por enfriamiento (429)', async (t) => {
  const actionRows = [{
    id: 1, action: 'party_here_pass', param: '', reason: '', actorAccount: 1, actorName: 'ADMIN',
    status: 'done', result: 'ok', requestedAt: new Date(), completedAt: new Date(),
  }];
  const { server, base } = await startApp(3, { actionRows });
  t.after(() => server.close());
  const response = await fetch(`${base}/api/bot-operations/actions`, {
    method: 'POST', headers: { ...JSON_HEADERS, Cookie: sessionCookieFor(ACTOR) },
    body: JSON.stringify({ action: 'party_here_pass' }),
  });
  assert.equal(response.status, 429);
  assert.equal(actionRows.length, 1);   // no se insertó una nueva
});

test('pasado el enfriamiento, la misma acción se puede volver a pedir', async (t) => {
  const oldEnough = new Date(Date.now() - 60_000);   // más que el enfriamiento (15 s)
  const actionRows = [{
    id: 1, action: 'party_here_pass', param: '', reason: '', actorAccount: 1, actorName: 'ADMIN',
    status: 'done', result: 'ok', requestedAt: oldEnough, completedAt: oldEnough,
  }];
  const { server, base } = await startApp(3, { actionRows });
  t.after(() => server.close());
  const response = await fetch(`${base}/api/bot-operations/actions`, {
    method: 'POST', headers: { ...JSON_HEADERS, Cookie: sessionCookieFor(ACTOR) },
    body: JSON.stringify({ action: 'party_here_pass' }),
  });
  assert.equal(response.status, 202);
  assert.equal(actionRows.length, 2);
});

test('dos parámetros distintos de la misma acción no se pisan (enfriamiento por acción+param)', async (t) => {
  const actionRows = [{
    id: 1, action: 'stop_world_pvp', param: '7', reason: '', actorAccount: 1, actorName: 'ADMIN',
    status: 'done', result: 'ok', requestedAt: new Date(), completedAt: new Date(),
  }];
  const { server, base } = await startApp(3, { actionRows });
  t.after(() => server.close());
  const response = await fetch(`${base}/api/bot-operations/actions`, {
    method: 'POST', headers: { ...JSON_HEADERS, Cookie: sessionCookieFor(ACTOR) },
    body: JSON.stringify({ action: 'stop_world_pvp', param: '9' }),
  });
  assert.equal(response.status, 202);
});

// ── Instantánea caducada ────────────────────────────────────────────────
test('sin ninguna instantánea todavía, available es false y stale es true', async (t) => {
  const { server, base } = await startApp(1, { snapshotRow: null });
  t.after(() => server.close());
  const response = await fetch(`${base}/api/bot-operations/snapshot`, { headers: { Cookie: sessionCookieFor(ACTOR) } });
  const body = await response.json();
  assert.equal(body.available, false);
  assert.equal(body.stale, true);
});

test('una instantánea reciente no está caducada', async (t) => {
  const { server, base } = await startApp(1, { snapshotRow: { ...SNAPSHOT_ROW, updated_at: new Date() } });
  t.after(() => server.close());
  const response = await fetch(`${base}/api/bot-operations/snapshot`, { headers: { Cookie: sessionCookieFor(ACTOR) } });
  const body = await response.json();
  assert.equal(body.available, true);
  assert.equal(body.stale, false);
});

test('una instantánea vieja (worldserver en espera o mod-bot-operations caído) se marca caducada', async (t) => {
  const old = new Date(Date.now() - 5 * 60_000);   // 5 min: muy por encima del umbral de 30 s
  const { server, base } = await startApp(1, { snapshotRow: { ...SNAPSHOT_ROW, updated_at: old } });
  t.after(() => server.close());
  const response = await fetch(`${base}/api/bot-operations/snapshot`, { headers: { Cookie: sessionCookieFor(ACTOR) } });
  const body = await response.json();
  assert.equal(body.available, true);
  assert.equal(body.stale, true);
});
