// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import test from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { createApp } from '../src/app.js';
import { calculateVerifier, verifyPassword } from '../src/srp6.js';
import { hashInviteCode } from '../src/invites.js';
import { fakePool, sessionCookieFor, listenTestApp } from './helpers.js';

// Simula acore_auth con invite de un solo uso y tabla account: con estado
// real (no fijo) para poder probar que el SEGUNDO intento con la misma clave
// falla, igual que en producción.
function fakeAuthDbWithRegistration({ invites, accounts }) {
  let nextId = 100;
  return fakePool({
    getConnection: async () => ({
      async beginTransaction() {},
      async commit() {},
      async rollback() {},
      release() {},
      async execute(sql, params) {
        if (sql.includes('FOR UPDATE')) {
          const invite = invites.get(params[0]);
          const valid = invite && !invite.usedAt && (!invite.expiresAt || invite.expiresAt > new Date());
          return [valid ? [{ code_hash: params[0] }] : []];
        }
        if (sql.includes('SELECT id FROM account WHERE username')) {
          return [accounts.has(params[0]) ? [{ id: 1 }] : []];
        }
        if (sql.startsWith('INSERT INTO account')) {
          accounts.add(params[0]);
          return [{ insertId: nextId++ }];
        }
        if (sql.includes('INSERT IGNORE INTO realmcharacters')) return [{}];
        if (sql.includes('UPDATE') && sql.includes('panel_invite')) {
          const invite = invites.get(params[1]);
          if (invite && !invite.usedAt) {
            invite.usedAt = new Date().toISOString();
            return [{ affectedRows: 1 }];
          }
          return [{ affectedRows: 0 }];
        }
        return [[]];
      },
    }),
  });
}

async function startApp(overrides) {
  const app = createApp({
    authDb: fakePool(), charactersDb: fakePool(), worldDb: fakePool(), panelDb: fakePool(), ...overrides,
  });
  return listenTestApp(app);
}

test('registro: la clave de invitación funciona una vez y la segunda falla', async (t) => {
  const invites = new Map([[hashInviteCode('CLAVE-DE-UN-SOLO-USO'), { usedAt: null, expiresAt: null }]]);
  const accounts = new Set();
  const authDb = fakeAuthDbWithRegistration({ invites, accounts });
  const { server, base } = await startApp({ authDb });
  t.after(() => server.close());

  const body = JSON.stringify({ inviteCode: 'CLAVE-DE-UN-SOLO-USO', username: 'jugador_1', password: 'unaClave123' });
  const first = await fetch(`${base}/api/register`, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body });
  assert.equal(first.status, 201);
  assert.match(first.headers.get('set-cookie') || '', /acore_session=/);

  const second = await fetch(`${base}/api/register`, {
    method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ inviteCode: 'CLAVE-DE-UN-SOLO-USO', username: 'jugador_2', password: 'otraClave123' }),
  });
  assert.equal(second.status, 400);
  const data = await second.json();
  assert.match(data.error, /invitación/i);
});

test('registro: una clave inexistente responde 400 sin crear cuenta', async (t) => {
  const authDb = fakeAuthDbWithRegistration({ invites: new Map(), accounts: new Set() });
  const { server, base } = await startApp({ authDb });
  t.after(() => server.close());
  const response = await fetch(`${base}/api/register`, {
    method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ inviteCode: 'no-existe', username: 'jugador_1', password: 'unaClave123' }),
  });
  assert.equal(response.status, 400);
});

test('registro: un nombre de cuenta ya existente responde 409', async (t) => {
  const invites = new Map([[hashInviteCode('CLAVE-VALIDA'), { usedAt: null, expiresAt: null }]]);
  const authDb = fakeAuthDbWithRegistration({ invites, accounts: new Set(['JUGADOR_1']) });
  const { server, base } = await startApp({ authDb });
  t.after(() => server.close());
  const response = await fetch(`${base}/api/register`, {
    method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ inviteCode: 'CLAVE-VALIDA', username: 'jugador_1', password: 'unaClave123' }),
  });
  assert.equal(response.status, 409);
});

test('cambio de contraseña: la actual incorrecta responde 401', async (t) => {
  const salt = randomBytes(32);
  const verifier = calculateVerifier('JUGADOR_1', 'actualCorrecta', salt);
  const authDb = fakePool({
    execute: async (sql) => {
      if (sql.includes('SELECT username, salt, verifier')) return [[{ username: 'JUGADOR_1', salt, verifier }]];
      return [[]];
    },
  });
  const account = { id: 7, username: 'JUGADOR_1' };
  const { server, base } = await startApp({ authDb });
  t.after(() => server.close());
  const response = await fetch(`${base}/api/account/password`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', 'X-Panel-Request': '1', Cookie: sessionCookieFor(account) },
    body: JSON.stringify({ currentPassword: 'incorrecta', newPassword: 'nuevaClave12' }),
  });
  assert.equal(response.status, 401);
});

test('cambio de contraseña: con la actual correcta, el verifier nuevo valida con la nueva contraseña y no con la vieja', async (t) => {
  const salt = randomBytes(32);
  const verifier = calculateVerifier('JUGADOR_1', 'actualCorrecta', salt);
  let updateParams;
  const authDb = fakePool({
    execute: async (sql, params) => {
      if (sql.includes('SELECT username, salt, verifier')) return [[{ username: 'JUGADOR_1', salt, verifier }]];
      if (sql.startsWith('UPDATE account SET salt')) { updateParams = params; return [{}]; }
      return [[]];
    },
  });
  const account = { id: 7, username: 'JUGADOR_1' };
  const { server, base } = await startApp({ authDb });
  t.after(() => server.close());
  const response = await fetch(`${base}/api/account/password`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', 'X-Panel-Request': '1', Cookie: sessionCookieFor(account) },
    body: JSON.stringify({ currentPassword: 'actualCorrecta', newPassword: 'nuevaClave12' }),
  });
  assert.equal(response.status, 204);
  const [newSalt, newVerifier] = updateParams;
  assert.ok(verifyPassword('JUGADOR_1', 'nuevaClave12', newSalt, newVerifier));
  assert.ok(!verifyPassword('JUGADOR_1', 'actualCorrecta', newSalt, newVerifier));
});

test('cambio de contraseña: sin la cabecera X-Panel-Request se rechaza (CSRF)', async (t) => {
  const account = { id: 7, username: 'JUGADOR_1' };
  const { server, base } = await startApp({});
  t.after(() => server.close());
  const response = await fetch(`${base}/api/account/password`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', Cookie: sessionCookieFor(account) },
    body: JSON.stringify({ currentPassword: 'x', newPassword: 'nuevaClave12' }),
  });
  assert.equal(response.status, 403);
});
