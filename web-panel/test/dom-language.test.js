// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// El panel real (index.html + app.js) montado en inglés: el HTML estático, lo
// que pintan las vistas por JS, las etiquetas de estado y la cabecera
// X-Panel-Lang con la que el servidor sabe en qué idioma contestar. Ver
// test/dom-helpers.js para por qué todo el fichero comparte un único mountApp().
import test, { after } from 'node:test';
import assert from 'node:assert/strict';
import { mountApp, flush } from './dom-helpers.js';

const requests = [];
const { shared, document } = await mountApp({
  lang: 'en',
  fetchHandler: (url, options) => {
    requests.push({ url, headers: options?.headers || {} });
    if (url.endsWith('/api/me')) return { status: 401, body: {} };
    if (url.endsWith('/api/players')) {
      return {
        status: 200,
        body: {
          updatedAt: '2026-10-07T13:05:09Z',
          players: [
            { guid: 1, name: 'Arthas', race: 1, class: 6, level: 80, role: 'tank', type: 'player' },
            { guid: 2, name: 'Jaina', race: 4, class: 8, level: 55, role: 'dps', type: 'bot' },
          ],
        },
      };
    }
    if (url.endsWith('/api/server/status')) return { status: 200, body: { state: 'standby' } };
    return { status: 404, body: {} };
  },
});

after(() => { shared.stopRefreshPolling(); shared.stopServerStatusPolling(); });

test('el login se ve en inglés y la sesión se pregunta con la cabecera de idioma', () => {
  assert.equal(document.documentElement.lang, 'en');
  assert.equal(document.querySelector('#login-button').textContent, 'Enter the realm');
  assert.equal(document.querySelector('#show-register').textContent, 'No account? Sign up with an invitation');
  const me = requests.find((request) => request.url.endsWith('/api/me'));
  assert.equal(me.headers['X-Panel-Lang'], 'en');
  assert.equal(me.headers['X-Panel-Request'], '1');
});

test('la tabla de jugadores, el menú y los indicadores se pintan en inglés', async () => {
  shared.showApp({ username: 'MATT', gmlevel: 3, isGm: true });
  await flush();

  const rows = [...document.querySelectorAll('#players-body tr')].map((row) => row.textContent.replace(/\s+/g, ' ').trim());
  assert.equal(rows.length, 2);
  assert.match(rows[0], /Arthas Human Death Knight Tank 80 Player/);
  assert.match(rows[1], /Jaina Night Elf Mage DPS 55 Bot/);

  assert.equal(document.querySelector('#account-rank').textContent, 'Game Master · Rank 3');
  assert.equal(document.querySelector('#page-title').textContent, 'Connected players');
  assert.equal(document.querySelector('#server-status span').textContent, 'Standby');
  assert.match(document.querySelector('#server-status').title, /^The worldserver is asleep to save resources/);
  assert.match(document.querySelector('#updated-time').textContent, /^\d\d:\d\d:\d\d$/); // 24 h, sea cual sea la zona horaria

  shared.showView('bot-ops');
  assert.equal(document.querySelector('#page-title').textContent, 'Bot operations');
});

test('los textos de otras vistas y formatos de tiempo salen en inglés', () => {
  assert.equal(shared.formatAgo(5_000), '5 s ago');
  assert.equal(shared.formatAgo(125_000), '2 min ago');
  assert.equal(shared.formatAgo(0), 'just now');
  assert.equal(shared.SERVER_STATUS_LABELS.offline, 'Down');
  assert.equal(shared.CLASSES[11][0], 'Druid');
});
