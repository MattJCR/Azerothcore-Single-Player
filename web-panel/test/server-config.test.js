// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import test from 'node:test';
import assert from 'node:assert/strict';
import { createApp } from '../src/app.js';
import { CATEGORIES } from '../src/serverConfigCatalog.js';
import { resolveAndValidate } from '../src/serverConfigValidation.js';
import { fakePool, sessionCookieFor, listenTestApp } from './helpers.js';

function fakeAuthDb({ gmLevelsByAccountId = {} } = {}) {
  return fakePool({
    execute: async (sql, params) => {
      if (sql.includes('FROM account_access')) return [[{ gmlevel: gmLevelsByAccountId[params[0]] ?? 0 }]];
      return [[]];
    },
  });
}

// Simula sólo lo que app.js necesita de panel_config_pending y
// panel_config_apply_request: contra qué fragmento de SQL coincide cada
// consulta, igual que fakeAuthDb/fakeCharactersDb en los demás test files.
function fakePanelConfigDb() {
  const pending = [];
  const applyRequests = [];
  let nextId = 1;
  let nextRevision = 1;

  return fakePool({
    query: async (sql, params) => {
      if (sql.includes('SELECT category_id AS categoryId, COUNT(*) AS count FROM panel_config_pending')) {
        const counts = new Map();
        for (const row of pending) if (row.status === 'pending') counts.set(row.categoryId, (counts.get(row.categoryId) || 0) + 1);
        return [[...counts].map(([categoryId, count]) => ({ categoryId, count }))];
      }
      if (sql.includes('SELECT key_name AS keyName, new_value AS newValue FROM panel_config_pending WHERE file_name')) {
        const [file] = params;
        return [pending.filter((row) => row.fileName === file && row.status === 'pending')
          .map((row) => ({ keyName: row.keyName, newValue: row.newValue }))];
      }
      if (sql.includes('SELECT key_name AS keyName, file_name AS fileName, category_id AS categoryId, new_value AS newValue, risk FROM panel_config_pending')) {
        return [pending.filter((row) => row.status === 'pending')
          .map((row) => ({ keyName: row.keyName, fileName: row.fileName, categoryId: row.categoryId, newValue: row.newValue, risk: row.risk }))];
      }
      if (sql.includes('SELECT status, requested_at AS requestedAt, applied_at AS appliedAt, error FROM panel_config_apply_request')) {
        const last = applyRequests.at(-1);
        return [last ? [{ status: last.status, requestedAt: last.requestedAt, appliedAt: last.appliedAt, error: last.error }] : []];
      }
      if (sql.includes("SELECT id FROM panel_config_apply_request WHERE status = 'pending'")) {
        const found = [...applyRequests].reverse().find((row) => row.status === 'pending');
        return [found ? [{ id: found.id }] : []];
      }
      if (sql.includes("SELECT COUNT(*) AS count FROM panel_config_pending WHERE status = 'pending'")) {
        return [[{ count: pending.filter((row) => row.status === 'pending').length }]];
      }
      return [[]];
    },
    execute: async (sql, params) => {
      // Simula panel_config_pending_seq: patrón UPDATE ... SET n =
      // LAST_INSERT_ID(n + 1), expuesto por mysql2 como
      // insertId del resultado del UPDATE.
      if (sql.includes('UPDATE panel_config_pending_seq')) {
        return [{ insertId: nextRevision++ }];
      }
      if (sql.includes('INSERT INTO panel_config_pending')) {
        const [key, file, categoryId, value, revision, risk, createdBy] = params;
        const existing = pending.find((row) => row.keyName === key && row.fileName === file);
        if (existing) Object.assign(existing, { newValue: value, revision, status: 'pending', createdBy, categoryId, risk });
        else pending.push({ keyName: key, fileName: file, categoryId, newValue: value, revision, risk, status: 'pending', createdBy });
        return [{}];
      }
      if (sql.includes("UPDATE panel_config_pending SET status = 'discarded' WHERE key_name")) {
        const [key, file] = params;
        const row = pending.find((entry) => entry.keyName === key && entry.fileName === file);
        if (row) row.status = 'discarded';
        return [{}];
      }
      if (sql.includes("UPDATE panel_config_pending SET status = 'discarded' WHERE status = 'pending'")) {
        pending.forEach((row) => { if (row.status === 'pending') row.status = 'discarded'; });
        return [{}];
      }
      if (sql.includes('INSERT INTO panel_config_apply_request')) {
        // Simula la clave única generada `pending_marker` (install.sh):
        // sólo puede existir una fila 'pending' a la vez en toda la
        // tabla, igual que en MySQL/MariaDB real.
        if (applyRequests.some((row) => row.status === 'pending')) {
          const error = new Error('Duplicate entry for key uniq_panel_config_apply_pending');
          error.code = 'ER_DUP_ENTRY';
          throw error;
        }
        const [requestedBy] = params;
        const id = nextId++;
        applyRequests.push({ id, requestedBy, status: 'pending', requestedAt: new Date(), appliedAt: null, error: null });
        return [{ insertId: id }];
      }
      return [{}];
    },
  });
}

async function startApp(gmlevel) {
  const app = createApp({ authDb: fakeAuthDb({ gmLevelsByAccountId: { 1: gmlevel } }), panelDb: fakePanelConfigDb() });
  return listenTestApp(app);
}

const ACTOR = { id: 1, username: 'ADMIN' };
const JSON_HEADERS = { 'Content-Type': 'application/json', 'X-Panel-Request': '1' };

test('los valores por defecto editables del catálogo pasan el mismo validador que el backend', () => {
  for (const category of CATEGORIES) {
    for (const param of category.params) {
      assert.notEqual(param.default, null, `${category.file}: ${param.key} carece de valor por defecto`);
      if (param.risk === 'high') continue;
      assert.doesNotThrow(
        () => resolveAndValidate(param.key, category.file, param.default),
        `${category.file}: ${param.key} (${param.default})`,
      );
    }
  }
});

test('la validación rechaza números que JavaScript redondearía o convertiría en infinito', () => {
  assert.throws(() => resolveAndValidate('GM.InWhoList.Level', 'worldserver.conf', '9007199254740993'), /rango admitido/);
  assert.throws(() => resolveAndValidate('Rate.Health', 'worldserver.conf', '9'.repeat(400)), /número finito/);
});

test('sin sesión, la lista de categorías responde 401', async (t) => {
  const { server, base } = await startApp(3);
  t.after(() => server.close());
  const response = await fetch(`${base}/api/server-config/categories`);
  assert.equal(response.status, 401);
});

test('gmlevel 2 no llega a administrador: 403 en categorías y en guardar', async (t) => {
  const { server, base } = await startApp(2);
  t.after(() => server.close());
  const categories = await fetch(`${base}/api/server-config/categories`, { headers: { Cookie: sessionCookieFor(ACTOR) } });
  assert.equal(categories.status, 403);
  const save = await fetch(`${base}/api/server-config/save`, { method: 'POST', headers: { ...JSON_HEADERS, Cookie: sessionCookieFor(ACTOR) }, body: '{}' });
  assert.equal(save.status, 403);
});

test('sin la cabecera X-Panel-Request, encolar un cambio se rechaza (CSRF)', async (t) => {
  const { server, base } = await startApp(3);
  t.after(() => server.close());
  const response = await fetch(`${base}/api/server-config/stage`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', Cookie: sessionCookieFor(ACTOR) },
    body: JSON.stringify({ key: 'Rate.Health', file: 'worldserver.conf', value: '1.5' }),
  });
  assert.equal(response.status, 403);
});

test('gmlevel 3 ve el catálogo, incluida la categoría GM y seguridad', async (t) => {
  const { server, base } = await startApp(3);
  t.after(() => server.close());
  const response = await fetch(`${base}/api/server-config/categories`, { headers: { Cookie: sessionCookieFor(ACTOR) } });
  assert.equal(response.status, 200);
  const data = await response.json();
  assert.ok(data.categories.length > 30);
  assert.ok(data.categories.some((category) => category.id === 'gm-security'));
  assert.ok(data.categories.some((category) => category.id === 'skills-professions'));
  assert.ok(data.categories.some((category) => category.group === 'module'));
});

// índice plano para la búsqueda global de la vista — sin
// tocar disco ni BD, así que no hace falta simular panel_config_pending.
test('el índice de búsqueda cubre todo el catálogo, no sólo una categoría', async (t) => {
  const { server, base } = await startApp(3);
  t.after(() => server.close());
  const response = await fetch(`${base}/api/server-config/search-index`, { headers: { Cookie: sessionCookieFor(ACTOR) } });
  assert.equal(response.status, 200);
  const data = await response.json();
  const totalParams = CATEGORIES.reduce((sum, category) => sum + category.params.length, 0);
  assert.equal(data.params.length, totalParams);
  assert.ok(data.params.some((param) => param.key === 'Rate.Health' && param.categoryId === 'rates-combat'));
  for (const param of data.params) {
    assert.equal(typeof param.key, 'string');
    assert.equal(typeof param.categoryLabel, 'string');
  }
});

test('gmlevel 2 no llega a administrador: 403 en el índice de búsqueda', async (t) => {
  const { server, base } = await startApp(2);
  t.after(() => server.close());
  const response = await fetch(`${base}/api/server-config/search-index`, { headers: { Cookie: sessionCookieFor(ACTOR) } });
  assert.equal(response.status, 403);
});

test('encolar una clave que no existe en el catálogo responde 400', async (t) => {
  const { server, base } = await startApp(3);
  t.after(() => server.close());
  const response = await fetch(`${base}/api/server-config/stage`, {
    method: 'POST', headers: { ...JSON_HEADERS, Cookie: sessionCookieFor(ACTOR) },
    body: JSON.stringify({ key: 'No.Existe', file: 'worldserver.conf', value: '1' }),
  });
  assert.equal(response.status, 400);
});

test('un parámetro de riesgo alto no se puede encolar aunque sea administrador (GM3)', async (t) => {
  const { server, base } = await startApp(3);
  t.after(() => server.close());
  const response = await fetch(`${base}/api/server-config/stage`, {
    method: 'POST', headers: { ...JSON_HEADERS, Cookie: sessionCookieFor(ACTOR) },
    body: JSON.stringify({ key: 'GM.StartLevel', file: 'worldserver.conf', value: '3' }),
  });
  assert.equal(response.status, 400);
  const body = await response.json();
  assert.match(body.error, /riesgo alto/);
});

test('un valor fuera de rango responde 400 y no llega a encolarse', async (t) => {
  const { server, base } = await startApp(3);
  t.after(() => server.close());
  const stage = await fetch(`${base}/api/server-config/stage`, {
    method: 'POST', headers: { ...JSON_HEADERS, Cookie: sessionCookieFor(ACTOR) },
    body: JSON.stringify({ key: 'DurabilityLoss.OnDeath', file: 'worldserver.conf', value: '150' }),
  });
  assert.equal(stage.status, 400);
  const pending = await fetch(`${base}/api/server-config/pending`, { headers: { Cookie: sessionCookieFor(ACTOR) } });
  assert.deepEqual((await pending.json()).changes, []);
});

test('encolar, ver en pendientes, descartar y volver a ver vacío', async (t) => {
  const { server, base } = await startApp(3);
  t.after(() => server.close());
  const headers = { ...JSON_HEADERS, Cookie: sessionCookieFor(ACTOR) };

  const stage = await fetch(`${base}/api/server-config/stage`, {
    method: 'POST', headers, body: JSON.stringify({ key: 'Rate.Health', file: 'worldserver.conf', value: '1.5' }),
  });
  assert.equal(stage.status, 200);

  const pendingAfterStage = await fetch(`${base}/api/server-config/pending`, { headers: { Cookie: headers.Cookie } });
  const dataAfterStage = await pendingAfterStage.json();
  assert.equal(dataAfterStage.changes.length, 1);
  assert.equal(dataAfterStage.changes[0].key, 'Rate.Health');
  assert.equal(dataAfterStage.changes[0].value, '1.5');

  const discard = await fetch(`${base}/api/server-config/discard`, { method: 'POST', headers, body: '{}' });
  assert.equal(discard.status, 204);

  const pendingAfterDiscard = await fetch(`${base}/api/server-config/pending`, { headers: { Cookie: headers.Cookie } });
  assert.deepEqual((await pendingAfterDiscard.json()).changes, []);
});

test('guardar sin cambios pendientes responde 400', async (t) => {
  const { server, base } = await startApp(3);
  t.after(() => server.close());
  const response = await fetch(`${base}/api/server-config/save`, { method: 'POST', headers: { ...JSON_HEADERS, Cookie: sessionCookieFor(ACTOR) }, body: '{}' });
  assert.equal(response.status, 400);
});

test('guardar con cambios pendientes encola una única solicitud aunque se pulse dos veces', async (t) => {
  const { server, base } = await startApp(3);
  t.after(() => server.close());
  const headers = { ...JSON_HEADERS, Cookie: sessionCookieFor(ACTOR) };
  await fetch(`${base}/api/server-config/stage`, {
    method: 'POST', headers, body: JSON.stringify({ key: 'Rate.Health', file: 'worldserver.conf', value: '1.5' }),
  });

  const firstSave = await fetch(`${base}/api/server-config/save`, { method: 'POST', headers, body: '{}' });
  assert.equal(firstSave.status, 202);
  const { id: firstId } = await firstSave.json();

  const secondSave = await fetch(`${base}/api/server-config/save`, { method: 'POST', headers, body: '{}' });
  assert.equal(secondSave.status, 202);
  const { id: secondId } = await secondSave.json();
  assert.equal(firstId, secondId);

  const pending = await fetch(`${base}/api/server-config/pending`, { headers: { Cookie: headers.Cookie } });
  const data = await pending.json();
  assert.equal(data.apply.status, 'pending');
});

// la prueba de arriba pulsa "Guardar" dos veces en SERIE (la
// segunda espera a que la primera responda). Ésta las dispara de verdad a
// la vez con Promise.all, sin esperar entre medias, para ejercitar A3
// (restricción `pending_marker`) contra el mismo camino HTTP/Express real
// que usaría un doble clic genuino, no sólo secuencialmente.
test('guardar con cambios pendientes: dos peticiones simultáneas de verdad comparten una única solicitud', async (t) => {
  const { server, base } = await startApp(3);
  t.after(() => server.close());
  const headers = { ...JSON_HEADERS, Cookie: sessionCookieFor(ACTOR) };
  await fetch(`${base}/api/server-config/stage`, {
    method: 'POST', headers, body: JSON.stringify({ key: 'Rate.Health', file: 'worldserver.conf', value: '1.5' }),
  });

  const [firstSave, secondSave] = await Promise.all([
    fetch(`${base}/api/server-config/save`, { method: 'POST', headers, body: '{}' }),
    fetch(`${base}/api/server-config/save`, { method: 'POST', headers, body: '{}' }),
  ]);
  assert.equal(firstSave.status, 202);
  assert.equal(secondSave.status, 202);
  const { id: firstId } = await firstSave.json();
  const { id: secondId } = await secondSave.json();
  assert.equal(firstId, secondId);

  const pending = await fetch(`${base}/api/server-config/pending`, { headers: { Cookie: headers.Cookie } });
  const data = await pending.json();
  assert.equal(data.apply.status, 'pending');
});
