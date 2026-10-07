// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// el menú móvil sólo alternaba una clase (sin aria-expanded,
// sin Escape, sin cierre al pulsar fuera) y la ordenación de Jugadores vivía
// en un <th> con manejador de clic, inaccesible por teclado. Ver
// test/dom-helpers.js para por qué todo este fichero comparte un único
// mountApp().
import test, { after } from 'node:test';
import assert from 'node:assert/strict';
import { mountApp, flush } from './dom-helpers.js';

function makePlayer(overrides = {}) {
  return { guid: 1, name: 'Arthas', race: 1, class: 1, gender: 0, level: 60, zone: 1, map: 0, role: 'tank', type: 'player', ...overrides };
}

const { shared, window, document } = await mountApp({
  fetchHandler: (url) => {
    if (url.endsWith('/api/me')) return { status: 401, body: {} };
    if (url.endsWith('/api/players')) return { status: 200, body: { players: [makePlayer(), makePlayer({ guid: 2, name: 'Jaina', level: 55 })], updatedAt: new Date().toISOString() } };
    if (url.endsWith('/api/server/status')) return { status: 200, body: { state: 'online' } };
    return { status: 404, body: {} };
  },
});

after(() => { shared.stopRefreshPolling(); shared.stopServerStatusPolling(); });

function click(el) { el.dispatchEvent(new window.MouseEvent('click', { bubbles: true, cancelable: true })); }
function keydown(target, key) { target.dispatchEvent(new window.KeyboardEvent('keydown', { key, bubbles: true, cancelable: true })); }

test('M56: el menú móvil refleja su estado en aria-expanded y se abre/cierra al pulsar el botón', async () => {
  shared.showApp({ username: 'JUGADOR', gmlevel: 0, isGm: false });
  await flush();

  const menuButton = document.querySelector('#menu-button');
  const sidebar = document.querySelector('#sidebar');
  assert.equal(menuButton.getAttribute('aria-expanded'), 'false');
  assert.equal(menuButton.getAttribute('aria-controls'), 'sidebar');
  assert.ok(!sidebar.classList.contains('open'));

  click(menuButton);
  assert.ok(sidebar.classList.contains('open'));
  assert.equal(menuButton.getAttribute('aria-expanded'), 'true');

  click(menuButton);
  assert.ok(!sidebar.classList.contains('open'));
  assert.equal(menuButton.getAttribute('aria-expanded'), 'false');
});

test('M56: Escape cierra el menú abierto y devuelve el foco al botón', async () => {
  const menuButton = document.querySelector('#menu-button');
  const sidebar = document.querySelector('#sidebar');
  click(menuButton);
  assert.ok(sidebar.classList.contains('open'));

  keydown(document, 'Escape');
  assert.ok(!sidebar.classList.contains('open'));
  assert.equal(menuButton.getAttribute('aria-expanded'), 'false');
  assert.equal(document.activeElement, menuButton);
});

test('M56: pulsar fuera del menú abierto lo cierra', async () => {
  const menuButton = document.querySelector('#menu-button');
  const sidebar = document.querySelector('#sidebar');
  click(menuButton);
  assert.ok(sidebar.classList.contains('open'));

  click(document.querySelector('#page-title'));
  assert.ok(!sidebar.classList.contains('open'));
});

test('M56: cambiar de vista también cierra el menú y limpia aria-expanded', async () => {
  const menuButton = document.querySelector('#menu-button');
  const sidebar = document.querySelector('#sidebar');
  click(menuButton);
  assert.ok(sidebar.classList.contains('open'));

  shared.showView('account');
  assert.ok(!sidebar.classList.contains('open'));
  assert.equal(menuButton.getAttribute('aria-expanded'), 'false');
  shared.showView('players');
});

test('M56: las cabeceras ordenables son botones (activables por teclado) y mueven aria-sort al th', async () => {
  await flush();
  const nameButton = document.querySelector('.th-sort[data-sort="name"]');
  const levelButton = document.querySelector('.th-sort[data-sort="level"]');
  assert.equal(nameButton.tagName, 'BUTTON');
  // Por defecto se ordena por nivel descendente (state.sort inicial en shared.js).
  assert.equal(levelButton.closest('th').getAttribute('aria-sort'), 'descending');
  assert.equal(nameButton.closest('th').getAttribute('aria-sort'), 'none');

  // Un <button> ya es activable con Enter/Espacio de forma nativa en un
  // navegador real; aquí se comprueba lo que sí depende de nuestro código:
  // que el clic (lo que despacha el user-agent al activar el botón) cambia
  // el orden y el aria-sort se mueve a la columna correcta.
  click(nameButton);
  assert.equal(nameButton.closest('th').getAttribute('aria-sort'), 'ascending');
  assert.equal(levelButton.closest('th').getAttribute('aria-sort'), 'none');

  click(nameButton); // mismo criterio: invierte a descendente
  assert.equal(nameButton.closest('th').getAttribute('aria-sort'), 'descending');
});
