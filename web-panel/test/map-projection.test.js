// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import test from 'node:test';
import assert from 'node:assert/strict';
import { projectMapPlayer } from '../public/map-projection.js';

test('proyecta todo el sur de Reinos del Este dentro del mapa', () => {
  const player = projectMapPlayer({ map: 0, x: -14461, y: 491.632 });
  assert.equal(player.displayMap, 0);
  assert.equal(player.onMap, true);
  assert.ok(player.mapX > 0 && player.mapX < 1);
  assert.ok(player.mapY > 0.9 && player.mapY < 1);
});

test('traslada las zonas de elfos de sangre de 530 a Reinos del Este', () => {
  const player = projectMapPlayer({ map: 530, x: 9802.83, y: -7480.19 });
  assert.equal(player.displayMap, 0);
  assert.equal(player.onMap, true);
  assert.ok(player.mapY < 0.2);
});

test('traslada las zonas draenei de 530 a Kalimdor', () => {
  const player = projectMapPlayer({ map: 530, x: -3915.16, y: -11550.8 });
  assert.equal(player.displayMap, 1);
  assert.equal(player.onMap, true);
});

test('mantiene las coordenadas de Terrallende en su propio mapa', () => {
  const player = projectMapPlayer({ map: 530, x: 224.13, y: 4334.08 });
  assert.equal(player.displayMap, 530);
  assert.equal(player.onMap, true);
});

test('no pega al borde las posiciones de mapas sin proyección', () => {
  const player = projectMapPlayer({ map: 33, x: 0, y: 0 });
  assert.equal(player.onMap, false);
});
