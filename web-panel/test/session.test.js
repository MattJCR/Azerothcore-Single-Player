// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import test from 'node:test';
import assert from 'node:assert/strict';
import { createHmac } from 'node:crypto';
import { createSession, readSession, parseCookie, sessionCookie } from '../src/session.js';

const secret = '12345678901234567890123456789012';

// Firma un payload a mano con el mismo esquema que session.js (HMAC-SHA256,
// base64url), para construir tokens con campos que createSession() nunca
// produciría.
function signToken(payloadObject, signingSecret = secret) {
  const payload = Buffer.from(JSON.stringify(payloadObject)).toString('base64url');
  const sig = createHmac('sha256', signingSecret).update(payload).digest('base64url');
  return `${payload}.${sig}`;
}

test('la sesión firmada se crea, se lee y detecta manipulaciones', () => {
  const token = createSession({ id: 42, username: 'ADMIN' }, secret, 600);
  assert.deepEqual(readSession(token, secret).sub, 42);
  assert.equal(readSession(`${token.slice(0, -1)}x`, secret), null);
});

test('la cookie usa defensas de navegador y se puede extraer', () => {
  const cookie = sessionCookie('abc.def', 600, true);
  assert.match(cookie, /HttpOnly/);
  assert.match(cookie, /SameSite=Strict/);
  assert.match(cookie, /Secure/);
  assert.equal(parseCookie(`foo=1; ${cookie}`), 'abc.def');
});

// decodeURIComponent (dentro de parseCookie) lanzaba
// URIError sin capturar ante una cookie truncada/mal codificada, y subía
// hasta el manejador genérico de errores de app.js (500) en vez de tratarse
// como sesión inválida.
test('una cookie con codificación inválida no lanza: se trata como ausente', () => {
  assert.equal(parseCookie('acore_session=%'), null);
  assert.equal(parseCookie('acore_session=%E0%A4%A'), null); // secuencia UTF-8 truncada
});

test('una cookie truncada (sin "=") se ignora sin lanzar', () => {
  assert.equal(parseCookie('acore_session'), null);
  assert.equal(parseCookie(''), null);
});

test('un token con exp no numérico se rechaza (antes: NaN nunca caduca)', () => {
  const token = signToken({ sub: 42, username: 'ADMIN', iat: 1000, exp: 'nunca' });
  assert.equal(readSession(token, secret), null);
});

test('un token sin campo exp se rechaza en vez de tratarse como sesión eterna', () => {
  const token = signToken({ sub: 42, username: 'ADMIN', iat: 1000 });
  assert.equal(readSession(token, secret), null);
});

test('un token con iat no numérico se rechaza', () => {
  const future = Math.floor(Date.now() / 1000) + 600;
  const token = signToken({ sub: 42, username: 'ADMIN', iat: 'ayer', exp: future });
  assert.equal(readSession(token, secret), null);
});
