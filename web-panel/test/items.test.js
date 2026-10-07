// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import test from 'node:test';
import assert from 'node:assert/strict';
import { buildWeaponStats, formatItemRow, nonZeroStats } from '../src/items.js';
import { useIconSource } from '../src/item-icons.js';

test('calcula el DPS de un arma a partir del daño y la cadencia', () => {
  const weapon = buildWeaponStats({ itemClass: 2, dmgMin: 100, dmgMax: 200, delay: 3000 });
  assert.deepEqual(weapon, { dmgMin: 100, dmgMax: 200, delay: 3000, dps: 50 });
});

test('un objeto que no es arma no lleva estadísticas de arma', () => {
  assert.equal(buildWeaponStats({ itemClass: 4, dmgMin: 0, dmgMax: 0, delay: 0 }), null);
});

test('un arma sin cadencia no calcula DPS', () => {
  assert.equal(buildWeaponStats({ itemClass: 2, dmgMin: 10, dmgMax: 20, delay: 0 }), null);
});

test('sólo viajan las estadísticas con valor distinto de cero', () => {
  const stats = nonZeroStats({ statType1: 4, statValue1: 20, statType2: 0, statValue2: 0, statType3: 7, statValue3: 15, statType4: 0, statValue4: 0, statType5: 0, statValue5: 0 });
  assert.deepEqual(stats, [{ type: 4, value: 20 }, { type: 7, value: 15 }]);
});

test('formatItemRow compone el objeto que se envía al cliente', () => {
  const item = formatItemRow({
    itemGuid: '5', entry: '25', name: 'Espada', quality: '4', count: '1', ilvl: '80',
    requiredLevel: '70', inventoryType: '13', armor: '0', itemClass: 2,
    displayId: '1',
    dmgMin: 50, dmgMax: 90, delay: 2800,
    statType1: 4, statValue1: 10, statType2: 0, statValue2: 0,
    statType3: 0, statValue3: 0, statType4: 0, statValue4: 0, statType5: 0, statValue5: 0,
  });
  assert.equal(item.itemGuid, 5);
  assert.equal(item.name, 'Espada');
  assert.equal(item.quality, 4);
  assert.equal(item.displayId, 1);
  assert.match(item.icon, /^[a-z0-9_.-]+$/);
  assert.deepEqual(item.stats, [{ type: 4, value: 10 }]);
  assert.ok(item.weapon.dps > 0);
});

test('usa el icono genérico cuando el displayid no existe', () => {
  useIconSource(() => ({ version: 1, fallback: 'inv_misc_questionmark-0123456789', icons: ['inv_misc_questionmark-0123456789'] }));
  try {
    const item = formatItemRow({ displayId: 999999999 });
    assert.match(item.icon, /^inv_misc_questionmark-[a-f0-9]{10}$/);
  } finally {
    useIconSource(null);
  }
});

test('sin iconos generados todavía, el objeto lleva el icono genérico propio', () => {
  useIconSource(null);
  assert.equal(formatItemRow({ displayId: 1 }).icon, 'generico');
});
