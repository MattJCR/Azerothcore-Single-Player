// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import test from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { createApp } from '../src/app.js';
import { calculateVerifier } from '../src/srp6.js';

function fakePool(overrides = {}) {
  return {
    execute: overrides.execute || (async () => [[]]),
    query: overrides.query || (async () => [[{ 1: 1 }]]),
    getConnection: overrides.getConnection || (async () => { throw new Error('getConnection no implementado en este mock'); }),
  };
}

async function startApp(overrides) {
  const app = createApp({
    authDb: fakePool(), charactersDb: fakePool(), worldDb: fakePool(), panelDb: fakePool(), ...overrides,
  });
  const server = app.listen(0);
  await new Promise((resolve) => server.once('listening', resolve));
  return { server, base: `http://127.0.0.1:${server.address().port}` };
}

test('login: contraseña incorrecta responde 401 y no crea cookie de sesión', async (t) => {
  const salt = randomBytes(32);
  const verifier = calculateVerifier('ADMIN', 'correcta', salt);
  const authDb = fakePool({
    execute: async (sql) => {
      if (sql.includes('FROM account WHERE username')) return [[{ id: 1, username: 'ADMIN', salt, verifier, locked: 0, last_ip: '127.0.0.1' }]];
      return [[]];
    },
  });
  const { server, base } = await startApp({ authDb });
  t.after(() => server.close());

  const response = await fetch(`${base}/api/login`, {
    method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ username: 'admin', password: 'incorrecta' }),
  });
  assert.equal(response.status, 401);
  assert.equal(response.headers.get('set-cookie'), null);
});

test('login: contraseña correcta crea una cookie firmada y devuelve el gmlevel', async (t) => {
  const salt = randomBytes(32);
  const verifier = calculateVerifier('ADMIN', 'correcta', salt);
  const authDb = fakePool({
    execute: async (sql) => {
      if (sql.includes('FROM account WHERE username')) return [[{ id: 1, username: 'ADMIN', salt, verifier, locked: 0, last_ip: '127.0.0.1' }]];
      if (sql.includes('FROM account_access')) return [[{ gmlevel: 3 }]];
      return [[]];
    },
  });
  const { server, base } = await startApp({ authDb });
  t.after(() => server.close());

  const response = await fetch(`${base}/api/login`, {
    method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ username: 'admin', password: 'correcta' }),
  });
  assert.equal(response.status, 200);
  assert.match(response.headers.get('set-cookie') || '', /acore_session=.+HttpOnly/);
  const data = await response.json();
  assert.deepEqual(data.user, { username: 'ADMIN', gmlevel: 3, isGm: true });
});

test('rate limit: la undécima petición en 15 minutos responde 429', async (t) => {
  const { server, base } = await startApp({});
  t.after(() => server.close());

  let lastStatus;
  for (let i = 0; i < 11; i += 1) {
    const response = await fetch(`${base}/api/login`, {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ username: 'nadie', password: 'x'.repeat(8) }),
    });
    lastStatus = response.status;
  }
  assert.equal(lastStatus, 429);
});

test('/api/me sin cookie responde 401', async (t) => {
  const { server, base } = await startApp({});
  t.after(() => server.close());
  const response = await fetch(`${base}/api/me`);
  assert.equal(response.status, 401);
});

test('logout limpia la cookie de sesión', async (t) => {
  const { server, base } = await startApp({});
  t.after(() => server.close());
  const response = await fetch(`${base}/api/logout`, { method: 'POST' });
  assert.equal(response.status, 204);
  assert.match(response.headers.get('set-cookie') || '', /acore_session=;.*Max-Age=0/);
});
