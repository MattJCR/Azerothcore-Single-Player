// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import test from 'node:test';
import assert from 'node:assert/strict';
import { createApp } from '../src/app.js';
import { fakePool, sessionCookieFor, listenTestApp } from './helpers.js';

function fakeAuthDb({ gmLevelsByAccountId = {} } = {}) {
  return fakePool({
    execute: async (sql, params) => {
      if (sql.includes('FROM account_access')) return [[{ gmlevel: gmLevelsByAccountId[params[0]] ?? 0 }]];
      return [[]];
    },
  });
}

// La ruta consulta panelDb sólo para el máximo del periodo (panel_metrics_sample).
function fakePanelDb({ maxRow = null } = {}) {
  return fakePool({
    query: async (sql) => {
      if (sql.includes('FROM panel_metrics_sample')) return [[maxRow || {}]];
      return [[]];
    },
  });
}

const ACTOR = { id: 1, username: 'ADMIN' };

const AVAILABLE_DOCTOR = {
  available: true,
  updatedAt: '2026-09-13T10:00:00.000Z',
  eventType: 'install',
  overallResult: 'ok',
  durationMs: 900,
  installerCommit: 'a'.repeat(40),
  checks: [{ name: 'mirrors', status: 'ok', detail: 'todo coincide.' }],
  modules: [{ name: 'mod-playerbots', kind: 'third-party', commit: 'abc123', status: 'active' }],
  history: [{ eventType: 'install', overallResult: 'ok', durationMs: 900, createdAt: '2026-09-13T10:00:00.000Z' }],
};

function fakeMetricsCache(data, { ageMs = 0, fromCache = false } = {}) {
  return { get: async () => ({ data, ageMs, fromCache }) };
}

async function startApp({ gmlevel = 0, metricsCache, maxRow } = {}) {
  const app = createApp({
    authDb: fakeAuthDb({ gmLevelsByAccountId: { 1: gmlevel } }),
    panelDb: fakePanelDb({ maxRow }),
    metricsCache,
  });
  return listenTestApp(app);
}

test('sin sesión, /api/metrics/status responde 401', async (t) => {
  const { server, base } = await startApp({ metricsCache: fakeMetricsCache({ doctor: { available: false }, unit: { observed: false }, process: null, slowTicks: { observed: false }, population: { playersOnline: 0, botsOnline: 0 }, sampledAt: 'x' }) });
  t.after(() => server.close());
  assert.equal((await fetch(`${base}/api/metrics/status`)).status, 401);
});

// Diferencia clave con /api/bot-operations/snapshot: cualquier cuenta con
// sesión lo ve, no sólo GM.
test('una cuenta sin GM (gmlevel 0) puede leer el estado', async (t) => {
  const data = {
    doctor: AVAILABLE_DOCTOR,
    unit: { observed: true, pid: 4242, active: true, restarts: 0, activeSince: '2026-09-13T10:00:00.000Z' },
    process: { cpuPct: 12.3, memRssKb: 500_000, memPeakKb: 600_000, uptimeSecs: 3600 },
    slowTicks: { observed: true, count: 0 },
    population: { playersOnline: 2, botsOnline: 150 },
    sampledAt: '2026-09-13T11:00:00.000Z',
  };
  const { server, base } = await startApp({
    gmlevel: 0,
    metricsCache: fakeMetricsCache(data, { ageMs: 30_000, fromCache: true }),
    maxRow: { cpuPct: 20.1, memRssKb: 650_000, uptimeSecs: 7200, playersOnline: 5, botsOnline: 160 },
  });
  t.after(() => server.close());

  const response = await fetch(`${base}/api/metrics/status`, { headers: { Cookie: sessionCookieFor(ACTOR) } });
  assert.equal(response.status, 200);
  const body = await response.json();

  assert.equal(body.doctor.available, true);
  assert.equal(body.doctor.overallResult, 'ok');
  assert.deepEqual(body.doctor.checks, AVAILABLE_DOCTOR.checks);
  assert.deepEqual(body.modules, AVAILABLE_DOCTOR.modules);
  assert.deepEqual(body.history, AVAILABLE_DOCTOR.history);

  assert.equal(body.period.eventType, 'install');
  assert.equal(body.period.startedAt, AVAILABLE_DOCTOR.updatedAt);

  assert.equal(body.live.active, true);
  assert.equal(body.live.restarts, 0);
  assert.deepEqual(body.live.process, data.process);
  assert.equal(body.live.slowTicks, 0);
  assert.deepEqual(body.live.population, { playersOnline: 2, botsOnline: 150 });

  assert.equal(body.periodMax.cpuPct, 20.1);
  assert.equal(body.periodMax.playersOnline, 5);
  assert.equal(body.periodMax.botsOnline, 160);

  assert.equal(body.cache.ageMs, 30_000);
  assert.equal(body.cache.fromCache, true);
});

test('sin ninguna pasada de doctor todavía, no hay periodo ni máximos', async (t) => {
  const data = {
    doctor: { available: false, checks: [], modules: [], history: [] },
    unit: { observed: false, pid: null, active: false, restarts: null, activeSince: null },
    process: null,
    slowTicks: { observed: false, count: null },
    population: { playersOnline: 0, botsOnline: 0 },
    sampledAt: '2026-09-13T11:00:00.000Z',
  };
  const { server, base } = await startApp({ metricsCache: fakeMetricsCache(data) });
  t.after(() => server.close());

  const response = await fetch(`${base}/api/metrics/status`, { headers: { Cookie: sessionCookieFor(ACTOR) } });
  const body = await response.json();

  assert.equal(body.doctor.available, false);
  assert.equal(body.period, null);
  assert.equal(body.periodMax, null);
});

// "no observado" (worldserver dormido/caído) es distinto de un cero real:
// active/restarts/activeSince deben llegar en null, no en false/0.
test('con el worldserver dormido, las métricas en vivo llegan como "no observado"', async (t) => {
  const data = {
    doctor: AVAILABLE_DOCTOR,
    unit: { observed: false, pid: null, active: false, restarts: null, activeSince: null },
    process: null,
    slowTicks: { observed: false, count: null },
    population: { playersOnline: 0, botsOnline: 0 },
    sampledAt: '2026-09-13T11:00:00.000Z',
  };
  const { server, base } = await startApp({ metricsCache: fakeMetricsCache(data), maxRow: {} });
  t.after(() => server.close());

  const response = await fetch(`${base}/api/metrics/status`, { headers: { Cookie: sessionCookieFor(ACTOR) } });
  const body = await response.json();

  assert.equal(body.live.active, null);
  assert.equal(body.live.restarts, null);
  assert.equal(body.live.activeSince, null);
  assert.equal(body.live.process, null);
  assert.equal(body.live.slowTicks, null);
  // periodMax con filas NULL (agregado MySQL sin muestras en el periodo)
  // también debe distinguirse de 0.
  assert.equal(body.periodMax.cpuPct, null);
  assert.equal(body.periodMax.playersOnline, null);
});

test('un fallo real de metricsCache.get() se propaga como error, no como 200 vacío', async (t) => {
  const { server, base } = await startApp({
    metricsCache: { get: async () => { throw new Error('systemctl no disponible'); } },
  });
  t.after(() => server.close());
  const response = await fetch(`${base}/api/metrics/status`, { headers: { Cookie: sessionCookieFor(ACTOR) } });
  assert.equal(response.status, 500);
});
