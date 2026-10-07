// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// PUB04-I: el panel «Recursos generados desde tu cliente» sólo lo ve un administrador, enseña
// qué está pendiente y los parches sin generar no ofrecen descarga. Ver test/dom-helpers.js para
// por qué todo este fichero comparte un único mountApp().
import test, { after } from 'node:test';
import assert from 'node:assert/strict';
import { mountApp, flush } from './dom-helpers.js';

const PATCHES = [
  { id: 'patch-eses-4', name: 'patch-esES-4.MPQ', file: 'patch-esES-4.MPQ', targetDir: 'esES', description: 'Objetos propios.', required: true, available: false, size: 0, sha256: null, downloadUrl: '/api/patches/patch-eses-4/download' },
  { id: 'patch-enus-4', name: 'patch-enUS-4.MPQ', file: 'patch-enUS-4.MPQ', targetDir: 'enUS', description: 'Objetos propios.', required: true, available: true, size: 4096, sha256: 'AB', downloadUrl: '/api/patches/patch-enus-4/download' },
];
const RESOURCES = {
  recursos: [
    { id: 'iconos', estado: 'pendiente', origen: null, actualizado: null, detalle: 'Se generan a partir de tu cliente de WoW' },
    { id: 'parche-esES', estado: 'pendiente', origen: null, actualizado: null, detalle: 'Se genera a partir de tu cliente de WoW' },
    { id: 'parche-enUS', estado: 'listo', origen: 'preparado', actualizado: '2026-10-05T10:00:00.000Z', detalle: null },
    { id: 'mapas', estado: 'pendiente', origen: null, actualizado: null, detalle: 'Faltan los mapas 571' },
  ],
  trabajo: { id: 'a'.repeat(32), estado: 'error', mensaje: 'Falló la generación: de prueba' },
};
let resourcesStatus = 200;

const { shared, document } = await mountApp({
  fetchHandler: (url) => {
    if (url.endsWith('/api/me')) return { status: 401, body: {} };
    if (url.endsWith('/api/players')) return { status: 200, body: { players: [], updatedAt: new Date().toISOString() } };
    if (url.endsWith('/api/server/status')) return { status: 200, body: { state: 'online' } };
    if (url.endsWith('/api/addons')) return { status: 200, body: { sourceCommit: '0123456789abcdef', mirroredAt: '2026-10-01T00:00:00Z', addons: [], patches: PATCHES } };
    if (url.endsWith('/api/recursos')) return { status: resourcesStatus, body: resourcesStatus === 200 ? RESOURCES : { error: 'Se requiere una cuenta de administrador' } };
    return { status: 404, body: {} };
  },
});

after(() => { shared.stopRefreshPolling(); shared.stopServerStatusPolling(); });

test('un administrador ve los recursos, lo pendiente y el último intento fallido', async () => {
  shared.showApp({ username: 'ADMIN', gmlevel: 3, isGm: true });
  shared.showView('addons');
  await flush(8);
  const panel = document.querySelector('#resources-panel');
  assert.equal(panel.classList.contains('hidden'), false);
  const items = [...document.querySelectorAll('#resource-list .resource-item')].map((item) => item.textContent.replace(/\s+/g, ' ').trim());
  assert.equal(items.length, 5, 'cuatro recursos más el último intento');
  assert.match(items[0], /Iconos de la armería.*Pendiente/);
  assert.match(items[2], /patch-enUS-4\.MPQ.*Listo.*Preparado en el servidor/);
  assert.match(items[3], /Mapas del panel.*Faltan los mapas 571/);
  assert.match(items[4], /Falló.*de prueba/);
});

test('los parches sin generar se marcan pendientes y no ofrecen descarga; los listos sí', () => {
  const rows = [...document.querySelectorAll('#patch-list .patch-item')];
  assert.equal(rows.length, 2);
  assert.ok(rows[0].classList.contains('patch-pending'));
  assert.equal(rows[0].querySelector('a'), null, 'sin enlace de descarga');
  assert.match(rows[0].textContent, /Pendiente de generar/);
  assert.ok(rows[1].querySelector('a[download]'));
});

test('un jugador (no administrador) no ve el panel de recursos', async () => {
  resourcesStatus = 403;
  shared.showApp({ username: 'JUGADOR', gmlevel: 0, isGm: false });
  shared.showView('addons');
  await flush(8);
  assert.equal(document.querySelector('#resources-panel').classList.contains('hidden'), true);
});
