// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import test from 'node:test';
import assert from 'node:assert/strict';
import { calculateVerifier, verifyPassword, internals } from '../src/srp6.js';

test('calcula el verifier SRP6 de AzerothCore en little-endian', () => {
  const salt = Buffer.from([...Array(32).keys()]);
  const verifier = calculateVerifier('ADMIN', 'PASSWORD', salt);
  assert.equal(verifier.toString('hex'), '8d228fd2f89a39f80b6c27cf32bbcc9218e7026868bb5162fe6916785cb5450b');
  assert.equal(verifyPassword('admin', 'password', salt, verifier), true);
  assert.equal(verifyPassword('admin', 'incorrecta', salt, verifier), false);
});

test('las conversiones BigInt conservan el orden little-endian del core', () => {
  const bytes = Buffer.from('0102030405000000', 'hex');
  const number = internals.littleEndianToBigInt(bytes);
  assert.equal(number, 0x0504030201n);
  assert.deepEqual(internals.bigIntToLittleEndian(number, 8), bytes);
});
