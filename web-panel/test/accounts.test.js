// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import test from 'node:test';
import assert from 'node:assert/strict';
import { AccountError, buildCredentials, MAX_PASSWORD_LENGTH, MAX_USERNAME_LENGTH, normalizeUsername } from '../src/accounts.js';
import { verifyPassword } from '../src/srp6.js';

test('normalizeUsername exige 3-17 caracteres (límite MAX_ACCOUNT_STR del core) y guarda en mayúsculas', () => {
  assert.equal(normalizeUsername('jugador_1'), 'JUGADOR_1');
  assert.throws(() => normalizeUsername('ab'), AccountError);
  assert.throws(() => normalizeUsername('a'.repeat(MAX_USERNAME_LENGTH + 1)), AccountError);
  assert.throws(() => normalizeUsername('nombre con espacio'), AccountError);
  assert.throws(() => normalizeUsername('usuario;DROP TABLE'), AccountError);
});

test('buildCredentials genera un verifier que valida con la contraseña real y no con otra', () => {
  const credentials = buildCredentials('jugador_1', 'unaClave123');
  assert.equal(credentials.username, 'JUGADOR_1');
  assert.equal(credentials.salt.length, 32);
  assert.equal(credentials.verifier.length, 32);
  assert.ok(verifyPassword(credentials.username, 'unaClave123', credentials.salt, credentials.verifier));
  assert.ok(!verifyPassword(credentials.username, 'otraClave', credentials.salt, credentials.verifier));
});

test('dos registros generan salt distinto aunque la contraseña sea igual', () => {
  const a = buildCredentials('jugador_1', 'unaClave123');
  const b = buildCredentials('jugador_2', 'unaClave123');
  assert.notDeepEqual(a.salt, b.salt);
  assert.notDeepEqual(a.verifier, b.verifier);
});

test(`la contraseña respeta el límite de ${MAX_PASSWORD_LENGTH} caracteres del cliente 3.3.5a`, () => {
  assert.throws(() => buildCredentials('jugador_1', 'corta'), AccountError);
  assert.throws(() => buildCredentials('jugador_1', 'x'.repeat(MAX_PASSWORD_LENGTH + 1)), AccountError);
  assert.doesNotThrow(() => buildCredentials('jugador_1', 'x'.repeat(MAX_PASSWORD_LENGTH)));
});
