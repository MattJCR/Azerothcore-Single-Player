// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import test from 'node:test';
import assert from 'node:assert/strict';
import { createApp } from '../src/app.js';
import { fakePool, sessionCookieFor, listenTestApp } from './helpers.js';

async function startApp({ tier, worldDb }) {
  const authDb = fakePool({ execute: async (sql) => (sql.includes('account_access') ? [[{ gmlevel: tier }]] : [[]]) });
  const app = createApp({ authDb, worldDb, charactersDb: fakePool(), panelDb: fakePool() });
  const { server, base } = await listenTestApp(app);
  return {
    server,
    url: `${base}/api/help`,
    headers: { Cookie: sessionCookieFor({ id: 7, username: 'TEST' }) },
  };
}

function helpWorldDb() {
  return fakePool({
    query: async (sql) => {
      if (sql.includes('server_help_category')) return [[
        { id: 1, name: 'General', sort: 1, minSecurity: 0, enabled: 1 },
        { id: 2, name: 'Comandos de jugador', sort: 2, minSecurity: 0, enabled: 1 },
        { id: 13, name: 'Game Master', sort: 13, minSecurity: 2, enabled: 1 },
        { id: 14, name: 'Moderación', sort: 14, minSecurity: 1, enabled: 1 },
        { id: 15, name: 'Administración', sort: 15, minSecurity: 3, enabled: 1 },
      ]];
      if (sql.includes('server_help_command')) return [[
        { path: 'help', categoryId: 1, title: 'Ayuda', description: '', syntax: '.help', examples: '', keywords: '', minSecurity: 0, enabled: 1 },
        { path: 'announce', categoryId: 1, title: 'Anuncio', description: '', syntax: '.announce', examples: '', keywords: '', minSecurity: 2, enabled: 1 },
        { path: 'gm', categoryId: 13, title: 'GM', description: '', syntax: '.gm', examples: '', keywords: '', minSecurity: 1, enabled: 1 },
      ]];
      if (sql.includes('server_help_article')) return [[
        { id: 1, categoryId: 1, title: 'Pública', body: 'Texto', keywords: '', commandPath: '', minSecurity: 0, sort: 1, enabled: 1, isHot: 0 },
        { id: 2, categoryId: 1, title: 'Solo con announce', body: 'Texto', keywords: '', commandPath: 'announce', minSecurity: 0, sort: 2, enabled: 1, isHot: 0 },
      ]];
      throw new Error(`Consulta inesperada: ${sql}`);
    },
  });
}

test('/api/help no entrega comandos ni artículos ligados por encima del nivel de la cuenta', async (t) => {
  const { server, url, headers } = await startApp({ tier: 0, worldDb: helpWorldDb() });
  t.after(() => server.close());
  const response = await fetch(url, { headers });
  assert.equal(response.status, 200);
  const catalog = await response.json();
  assert.deepEqual(catalog.commands.map((command) => command.path), ['help']);
  assert.deepEqual(catalog.articles.map((article) => article.id), [1]);
});

test('/api/help expone permiso y reclasifica una ficha permitida cuya categoría temática está oculta', async (t) => {
  const { server, url, headers } = await startApp({ tier: 1, worldDb: helpWorldDb() });
  t.after(() => server.close());
  const response = await fetch(url, { headers });
  assert.equal(response.status, 200);
  const catalog = await response.json();
  const gm = catalog.commands.find((command) => command.path === 'gm');
  assert.deepEqual({ categoryId: gm.categoryId, minSecurity: gm.minSecurity }, { categoryId: 14, minSecurity: 1 });
  assert.ok(!catalog.commands.some((command) => command.path === 'announce'));
});
