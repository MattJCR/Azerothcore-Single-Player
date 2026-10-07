// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import test from 'node:test';
import assert from 'node:assert/strict';
import { classifyInventory } from '../src/inventory.js';

function item(itemGuid, bag, slot, name) {
  return { bag, slot, itemGuid, name };
}

test('separa equipo, bolsas y banco por rango de slot', () => {
  const rows = [
    item(1, 0, 4, 'Pechera'),               // equipada
    item(2, 0, 19, 'Bolsa de tela'),        // contenedor de bolsa
    item(3, 2, 0, 'Poción'),                // dentro de la bolsa de tela
    item(4, 0, 25, 'Objeto suelto'),        // mochila
    item(5, 0, 40, 'Objeto en el banco'),   // banco suelto
    item(6, 0, 67, 'Bolsa del banco'),      // contenedor del banco
    item(7, 6, 0, 'Reactivo'),              // dentro de la bolsa del banco
  ];

  const result = classifyInventory(rows);

  assert.deepEqual(result.equipped.map((row) => row.name), ['Pechera']);
  assert.deepEqual(result.bags.loose.map((row) => row.name), ['Objeto suelto']);
  assert.equal(result.bags.containers.length, 1);
  assert.equal(result.bags.containers[0].container.name, 'Bolsa de tela');
  assert.deepEqual(result.bags.containers[0].items.map((row) => row.name), ['Poción']);
  assert.deepEqual(result.bank.loose.map((row) => row.name), ['Objeto en el banco']);
  assert.equal(result.bank.containers[0].container.name, 'Bolsa del banco');
  assert.deepEqual(result.bank.containers[0].items.map((row) => row.name), ['Reactivo']);
});

test('incluye todos los slots de objetos y bolsas del banco', () => {
  const result = classifyInventory([
    item(1, 0, 66, 'Último objeto del banco'),
    item(2, 0, 73, 'Última bolsa del banco'),
    item(3, 2, 0, 'Contenido de la última bolsa'),
  ]);
  assert.deepEqual(result.bank.loose.map((row) => row.name), ['Último objeto del banco']);
  assert.equal(result.bank.containers[0].container.name, 'Última bolsa del banco');
  assert.equal(result.bank.containers[0].items[0].name, 'Contenido de la última bolsa');
});

test('ignora slots de devolución de compra, llavero o monedas', () => {
  const rows = [item(1, 0, 90, 'Moneda rara')];
  const result = classifyInventory(rows);
  assert.equal(result.equipped.length, 0);
  assert.equal(result.bags.loose.length, 0);
  assert.equal(result.bank.loose.length, 0);
});

test('un objeto huérfano (contenedor no encontrado) no se muestra ni rompe el cálculo', () => {
  const rows = [item(9, 999, 0, 'Objeto perdido')];
  assert.doesNotThrow(() => classifyInventory(rows));
});
