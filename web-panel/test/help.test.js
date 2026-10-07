// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import test from 'node:test';
import assert from 'node:assert/strict';
import { buildHelpCatalog, securityTier } from '../src/help.js';

const categories = [
  { id: 1, name: 'General', sort: 0, minSecurity: 0, enabled: true },
  { id: 2, name: 'Comandos de jugador', sort: 5, minSecurity: 0, enabled: true },
  { id: 13, name: 'Game Master', sort: 10, minSecurity: 2, enabled: true },
  { id: 14, name: 'Moderación', sort: 15, minSecurity: 1, enabled: true },
  { id: 99, name: 'Deshabilitada', sort: 20, minSecurity: 0, enabled: false },
];
const commands = [
  { path: 'ayuda', categoryId: 1, minSecurity: 0, enabled: true },
  { path: 'ayuda buscar', categoryId: 1, minSecurity: 0, enabled: true },
  { path: 'announce', categoryId: 1, minSecurity: 2, enabled: true },
  { path: 'gm', categoryId: 13, minSecurity: 1, enabled: true },
  { path: 'oculto', categoryId: 99, enabled: true },
  { path: 'deshabilitado', categoryId: 1, enabled: false },
];
const articles = [
  { id: 1, title: 'Bienvenida', categoryId: 1, sort: 0, minSecurity: 0, enabled: true },
  { id: 2, title: 'Guía de GM', categoryId: 13, sort: 0, minSecurity: 2, enabled: true },
  { id: 3, title: 'Anuncios', categoryId: 1, commandPath: 'announce', sort: 1, minSecurity: 0, enabled: true },
];

test('un jugador normal (tier 0) sólo ve el contenido de jugador', () => {
  const catalog = buildHelpCatalog(0, { categories, commands, articles });
  assert.deepEqual(catalog.categories.map((c) => c.id), [1, 2]);
  assert.deepEqual(catalog.commands.map((c) => c.path), ['ayuda', 'ayuda buscar']);
  assert.deepEqual(catalog.articles.map((a) => a.id), [1]);
  assert.equal(catalog.commands[0].isFamily, true);
});

test('un GM (tier 2) ve también las categorías y comandos de GM', () => {
  const catalog = buildHelpCatalog(2, { categories, commands, articles });
  assert.deepEqual(catalog.commands.map((c) => c.path).sort(), ['announce', 'ayuda', 'ayuda buscar', 'gm']);
  assert.deepEqual(catalog.articles.map((a) => a.id).sort(), [1, 2, 3]);
});

test('una categoría deshabilitada oculta sus comandos aunque el nivel encaje', () => {
  const catalog = buildHelpCatalog(3, { categories, commands, articles });
  assert.ok(!catalog.commands.some((c) => c.path === 'oculto'));
});

test('el permiso del comando prevalece sobre una categoría pública', () => {
  const player = buildHelpCatalog(0, { categories, commands, articles });
  const gm = buildHelpCatalog(2, { categories, commands, articles });
  assert.ok(!player.commands.some((command) => command.path === 'announce'));
  assert.ok(gm.commands.some((command) => command.path === 'announce'));
});

test('un comando permitido cae en su categoría de nivel si la temática aún está oculta', () => {
  const moderator = buildHelpCatalog(1, { categories, commands, articles });
  const gm = moderator.commands.find((command) => command.path === 'gm');
  assert.equal(gm.categoryId, 14);
});

test('una ficha antigua sin minSecurity hereda el permiso de la categoría', () => {
  const catalog = buildHelpCatalog(0, { categories, commands: [{ path: 'legado', categoryId: 13, enabled: true }] });
  assert.deepEqual(catalog.commands, []);
});

test('securityTier acota el gmlevel crudo a 0-3', () => {
  assert.equal(securityTier(undefined), 0);
  assert.equal(securityTier(-1), 0);
  assert.equal(securityTier(2), 2);
  assert.equal(securityTier(9), 3);
});
