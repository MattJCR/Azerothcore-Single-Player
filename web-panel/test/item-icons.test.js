// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import test from 'node:test';
import assert from 'node:assert/strict';
import { PLACEHOLDER, iconForDisplayId, useIconSource } from '../src/item-icons.js';

test('sin índice generado todos los objetos usan el icono genérico propio', () => {
  useIconSource(() => null);
  assert.equal(iconForDisplayId(1234), PLACEHOLDER);
  assert.equal(iconForDisplayId(0), PLACEHOLDER);
});

test('con índice: apariencia conocida, desconocida y nombres no seguros', () => {
  useIconSource(() => ({ version: 1, fallback: 'inv_misc_questionmark-aaaa', icons: ['inv_misc_questionmark-aaaa', 'inv_sword_04-bbbb', '../evil'] }));
  assert.equal(iconForDisplayId(1), 'inv_sword_04-bbbb');
  assert.equal(iconForDisplayId(99), 'inv_misc_questionmark-aaaa');
  assert.equal(iconForDisplayId(2), 'inv_misc_questionmark-aaaa');
  assert.equal(iconForDisplayId('x'), 'inv_misc_questionmark-aaaa');
  useIconSource(null);
});
