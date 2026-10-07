// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import test from 'node:test';
import assert from 'node:assert/strict';
import { createApp } from '../src/app.js';
import { fakePool, sessionCookieFor, listenTestApp } from './helpers.js';

// authDb sirve tanto de tabla de gmlevel (account_access) como, para los
// baneos de tipo "account", de tabla de cuentas por nombre.
function fakeAuthDb({ gmLevelsByAccountId = {}, accountIdsByUsername = {} } = {}) {
  return fakePool({
    execute: async (sql, params) => {
      if (sql.includes('FROM account_access')) return [[{ gmlevel: gmLevelsByAccountId[params[0]] ?? 0 }]];
      if (sql.includes('FROM account WHERE username')) {
        const id = accountIdsByUsername[params[0]];
        return [id === undefined ? [] : [{ id }]];
      }
      return [[]];
    },
  });
}

function fakeCharactersDb({ accountIdsByName = {} } = {}) {
  return fakePool({
    execute: async (sql, params) => {
      if (sql.includes('FROM characters WHERE LOWER(name)')) {
        const accountId = accountIdsByName[String(params[0]).toLowerCase()];
        return [accountId === undefined ? [] : [{ account: accountId }]];
      }
      return [[]];
    },
  });
}

async function startApp(overrides) {
  const soapCalls = [];
  const soap = { executeCommand: async (command) => { soapCalls.push(command); return 'Comando ejecutado con éxito.'; } };
  const app = createApp({
    authDb: fakeAuthDb(), charactersDb: fakeCharactersDb(), worldDb: fakePool(), panelDb: fakePool(), soap, ...overrides,
  });
  const { server, base } = await listenTestApp(app);
  return { server, base, soapCalls };
}

const ACTOR = { id: 1, username: 'MODERADOR' };
const JSON_HEADERS = { 'Content-Type': 'application/json', 'X-Panel-Request': '1' };

test('sin sesión, cualquier endpoint de moderación responde 401', async (t) => {
  const { server, base } = await startApp({});
  t.after(() => server.close());
  const response = await fetch(`${base}/api/moderation/kick`, { method: 'POST', headers: JSON_HEADERS, body: '{}' });
  assert.equal(response.status, 401);
});

test('sin la cabecera X-Panel-Request, una petición con cookie válida se rechaza (CSRF)', async (t) => {
  const { server, base } = await startApp({ authDb: fakeAuthDb({ gmLevelsByAccountId: { 1: 3 } }) });
  t.after(() => server.close());
  const response = await fetch(`${base}/api/moderation/announce`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', Cookie: sessionCookieFor(ACTOR) },
    body: JSON.stringify({ text: 'hola' }),
  });
  assert.equal(response.status, 403);
});

test('gmlevel 0 no puede expulsar (403) y el SOAP falso no se llama', async (t) => {
  const { server, base, soapCalls } = await startApp({ authDb: fakeAuthDb({ gmLevelsByAccountId: { 1: 0 } }) });
  t.after(() => server.close());
  const response = await fetch(`${base}/api/moderation/kick`, {
    method: 'POST', headers: { ...JSON_HEADERS, Cookie: sessionCookieFor(ACTOR) },
    body: JSON.stringify({ characterName: 'Testigo', reason: '' }),
  });
  assert.equal(response.status, 403);
  assert.deepEqual(soapCalls, []);
});

test('gmlevel 1 no alcanza para banear (exige 2): 403 y el SOAP falso no se llama', async (t) => {
  const { server, base, soapCalls } = await startApp({ authDb: fakeAuthDb({ gmLevelsByAccountId: { 1: 1 } }) });
  t.after(() => server.close());
  const response = await fetch(`${base}/api/moderation/ban`, {
    method: 'POST', headers: { ...JSON_HEADERS, Cookie: sessionCookieFor(ACTOR) },
    body: JSON.stringify({ type: 'character', name: 'Testigo', duration: '1d', reason: 'trampas' }),
  });
  assert.equal(response.status, 403);
  assert.deepEqual(soapCalls, []);
});

test('un parámetro inválido responde 400 antes de tocar la base o el SOAP falso', async (t) => {
  const executedQueries = [];
  const authDb = fakeAuthDb({ gmLevelsByAccountId: { 1: 3 } });
  const charactersDb = fakePool({ execute: async (sql, params) => { executedQueries.push([sql, params]); return [[]]; } });
  const { server, base, soapCalls } = await startApp({ authDb, charactersDb });
  t.after(() => server.close());
  const response = await fetch(`${base}/api/moderation/kick`, {
    method: 'POST', headers: { ...JSON_HEADERS, Cookie: sessionCookieFor(ACTOR) },
    body: JSON.stringify({ characterName: 'Test123', reason: '' }),
  });
  assert.equal(response.status, 400);
  assert.deepEqual(soapCalls, []);
  assert.deepEqual(executedQueries, []);
});

test('no se puede actuar sobre una cuenta de rango igual o superior (403)', async (t) => {
  const authDb = fakeAuthDb({ gmLevelsByAccountId: { 1: 2, 5: 2 } });
  const charactersDb = fakeCharactersDb({ accountIdsByName: { testigo: 5 } });
  const { server, base, soapCalls } = await startApp({ authDb, charactersDb });
  t.after(() => server.close());
  const response = await fetch(`${base}/api/moderation/kick`, {
    method: 'POST', headers: { ...JSON_HEADERS, Cookie: sessionCookieFor(ACTOR) },
    body: JSON.stringify({ characterName: 'Testigo', reason: '' }),
  });
  assert.equal(response.status, 403);
  assert.deepEqual(soapCalls, []);
});

test('un objetivo inexistente responde 404 sin llamar al SOAP falso', async (t) => {
  const authDb = fakeAuthDb({ gmLevelsByAccountId: { 1: 3 } });
  const charactersDb = fakeCharactersDb({ accountIdsByName: {} });
  const { server, base, soapCalls } = await startApp({ authDb, charactersDb });
  t.after(() => server.close());
  const response = await fetch(`${base}/api/moderation/kick`, {
    method: 'POST', headers: { ...JSON_HEADERS, Cookie: sessionCookieFor(ACTOR) },
    body: JSON.stringify({ characterName: 'Fantasma', reason: '' }),
  });
  assert.equal(response.status, 404);
  assert.deepEqual(soapCalls, []);
});

test('una expulsión válida llega al SOAP falso con el comando exacto', async (t) => {
  const authDb = fakeAuthDb({ gmLevelsByAccountId: { 1: 1, 5: 0 } });
  const charactersDb = fakeCharactersDb({ accountIdsByName: { testigo: 5 } });
  const { server, base, soapCalls } = await startApp({ authDb, charactersDb });
  t.after(() => server.close());
  const response = await fetch(`${base}/api/moderation/kick`, {
    method: 'POST', headers: { ...JSON_HEADERS, Cookie: sessionCookieFor(ACTOR) },
    body: JSON.stringify({ characterName: 'Testigo', reason: 'motivo de prueba' }),
  });
  assert.equal(response.status, 200);
  assert.deepEqual(soapCalls, ['kick Testigo motivo de prueba']);
});

test('un baneo de cuenta válido (gmlevel 2) llega al SOAP falso con el comando exacto', async (t) => {
  const authDb = fakeAuthDb({ gmLevelsByAccountId: { 1: 2, 9: 0 }, accountIdsByUsername: { jugador_1: 9 } });
  const { server, base, soapCalls } = await startApp({ authDb });
  t.after(() => server.close());
  const response = await fetch(`${base}/api/moderation/ban`, {
    method: 'POST', headers: { ...JSON_HEADERS, Cookie: sessionCookieFor(ACTOR) },
    body: JSON.stringify({ type: 'account', name: 'jugador_1', duration: '7d', reason: 'trampas' }),
  });
  assert.equal(response.status, 200);
  assert.deepEqual(soapCalls, ['ban account jugador_1 7d trampas']);
});

test('anunciar (sin objetivo) no comprueba jerarquía y llega tal cual', async (t) => {
  const authDb = fakeAuthDb({ gmLevelsByAccountId: { 1: 2 } });
  const { server, base, soapCalls } = await startApp({ authDb });
  t.after(() => server.close());
  const response = await fetch(`${base}/api/moderation/announce`, {
    method: 'POST', headers: { ...JSON_HEADERS, Cookie: sessionCookieFor(ACTOR) },
    body: JSON.stringify({ text: 'El servidor reinicia en 5 minutos' }),
  });
  assert.equal(response.status, 200);
  assert.deepEqual(soapCalls, ['announce El servidor reinicia en 5 minutos']);
});

test('si el SOAP falla, responde 503 con el motivo', async (t) => {
  const { SoapError } = await import('../src/soap.js');
  const authDb = fakeAuthDb({ gmLevelsByAccountId: { 1: 2 } });
  const soap = { executeCommand: async () => { throw new SoapError('No se pudo contactar con la consola del worldserver'); } };
  const { server, base } = await startApp({ authDb, soap });
  t.after(() => server.close());
  const response = await fetch(`${base}/api/moderation/announce`, {
    method: 'POST', headers: { ...JSON_HEADERS, Cookie: sessionCookieFor(ACTOR) },
    body: JSON.stringify({ text: 'hola' }),
  });
  assert.equal(response.status, 503);
  const data = await response.json();
  assert.match(data.error, /no se pudo contactar/i);
});
