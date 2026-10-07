// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import test from 'node:test';
import assert from 'node:assert/strict';
import { inferRole } from '../src/roles.js';

test('infiere roles desde talentos distintivos del grupo activo', () => {
  assert.equal(inferRole(2, [53563]), 'healer');
  assert.equal(inferRole(1, [46968]), 'tank');
  assert.equal(inferRole(8, [44425]), 'dps');
});
