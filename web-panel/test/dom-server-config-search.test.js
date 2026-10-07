// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// la búsqueda de "Configuración del servidor" sólo filtraba
// dentro de la categoría ya cargada. Ahora, con texto de búsqueda, mira el
// catálogo entero (vía /api/server-config/search-index) y cada resultado
// salta a su categoría; sin texto, se conserva el filtrado de siempre dentro
// de la categoría seleccionada. Ver test/dom-helpers.js para por qué todo
// este fichero comparte un único mountApp().
import test, { after } from 'node:test';
import assert from 'node:assert/strict';
import { mountApp, flush } from './dom-helpers.js';

const CATEGORIES = [
  { id: 'rates-combat', label: 'Tasas de combate', group: 'core', module: null, file: 'worldserver.conf', count: 2, pendingCount: 0 },
  { id: 'gm-security', label: 'GM y seguridad', group: 'core', module: null, file: 'worldserver.conf', count: 1, pendingCount: 0 },
];
const PARAMS_BY_CATEGORY = {
  'rates-combat': [
    { key: 'Rate.Health', type: 'float', min: 0, max: null, risk: 'low', description: 'Multiplicador de vida.', default: '1.0', current: '1.0', pending: null },
    { key: 'Rate.Mana', type: 'float', min: 0, max: null, risk: 'low', description: 'Multiplicador de maná.', default: '1.0', current: '1.0', pending: null },
  ],
  'gm-security': [
    { key: 'GM.AllowInvite', type: 'bool', min: null, max: null, risk: 'medium', description: 'Permite invitar a GMs a grupo.', default: '0', current: '0', pending: null },
  ],
};
const SEARCH_INDEX = CATEGORIES.flatMap((category) => PARAMS_BY_CATEGORY[category.id].map((param) => ({
  key: param.key, description: param.description, categoryId: category.id, categoryLabel: category.label,
})));

const { shared, window, document } = await mountApp({
  fetchHandler: (url) => {
    if (url.endsWith('/api/me')) return { status: 401, body: {} };
    if (url.endsWith('/api/players')) return { status: 200, body: { players: [], updatedAt: new Date().toISOString() } };
    if (url.endsWith('/api/server/status')) return { status: 200, body: { state: 'online' } };
    if (url.endsWith('/api/server-config/categories')) return { status: 200, body: { categories: CATEGORIES } };
    if (url.endsWith('/api/server-config/search-index')) return { status: 200, body: { params: SEARCH_INDEX } };
    if (url.endsWith('/api/server-config/pending')) return { status: 200, body: { changes: [], apply: null } };
    const categoryMatch = url.match(/\/api\/server-config\/category\/([\w-]+)/);
    if (categoryMatch) {
      const category = CATEGORIES.find((entry) => entry.id === categoryMatch[1]);
      return { status: 200, body: { category, params: PARAMS_BY_CATEGORY[categoryMatch[1]] } };
    }
    return { status: 404, body: {} };
  },
});

after(() => { shared.stopRefreshPolling(); shared.stopServerStatusPolling(); });

function click(el) { el.dispatchEvent(new window.MouseEvent('click', { bubbles: true, cancelable: true })); }
function type(el, value) { el.value = value; el.dispatchEvent(new window.Event('input', { bubbles: true })); }

test('M57: sin texto de búsqueda, se conserva el filtrado dentro de la categoría seleccionada', async () => {
  shared.showApp({ username: 'ADMIN', gmlevel: 3, isGm: true });
  shared.showView('server-config');
  await flush();

  const rows = () => [...document.querySelectorAll('#server-config-groups .config-row')];
  assert.equal(rows().length, 2, 'la categoría por defecto (rates-combat) tiene 2 parámetros');
  assert.ok(rows().every((row) => row.dataset.configKey.startsWith('Rate.')));
});

test('M57: con texto de búsqueda, los resultados vienen de TODAS las categorías, no sólo la abierta', async () => {
  type(document.querySelector('#server-config-search'), 'GM');
  await flush();

  const results = [...document.querySelectorAll('#server-config-groups .config-search-result')];
  assert.ok(results.length >= 1, 'debe encontrar GM.AllowInvite aunque la categoría abierta sea rates-combat');
  assert.ok(results.some((result) => result.dataset.jumpKey === 'GM.AllowInvite'));
});

test('M57: pulsar un resultado salta a su categoría y limpia la búsqueda', async () => {
  const result = document.querySelector('#server-config-groups [data-jump-key="GM.AllowInvite"]');
  click(result);
  await flush();

  assert.equal(document.querySelector('#server-config-search').value, '');
  const rows = [...document.querySelectorAll('#server-config-groups .config-row')];
  assert.equal(rows.length, 1);
  assert.equal(rows[0].dataset.configKey, 'GM.AllowInvite');
});
