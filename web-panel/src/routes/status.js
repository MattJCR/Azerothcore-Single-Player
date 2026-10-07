// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// routes/status.js — señales de salud del panel: el indicador "En
// vivo/En espera/Caído" (/api/server/status) y "Estado y rendimiento"
// (/api/metrics/status). extraído de app.js con el mismo
// patrón de inyección que routes/serverConfig.js/botOperations.js. Se
// agrupan porque ambas son lecturas de salud/estado, no acciones — a
// diferencia de moderación o configuración, que sí escriben algo.
//
// buildMetricsCache() vive aquí (no en metricsCache.js, que es genérico) para
// que app.js sólo tenga que decidir SI usarla (o un metricsCache de test
// inyectado vía overrides), sin conocer fetchDoctorStatus/fetchSystemStatus/
// persistMetricsSample, que son detalle de este dominio.
import { createMetricsCache } from '../metricsCache.js';
import { sampleSystem } from '../systemMetrics.js';
import { getServerStatus } from '../serverStatus.js';

// Resultado más reciente de `doctor` (lib/doctor.sh) y un historial breve.
// Nunca deja que "las tablas todavía no existen" (ninguna pasada de doctor se
// ha ejecutado nunca, o `modules` es de una versión anterior al ALTER de hoy)
// tumbe la petición: es "todavía no hay datos", no un fallo.
async function fetchDoctorStatus(worldDb) {
  const empty = { available: false, checks: [], modules: [], history: [] };
  let historyRows;
  try {
    [historyRows] = await worldDb.query(
      'SELECT event_type, overall_result, duration_ms, created_at '
      + 'FROM doctor_history ORDER BY id DESC LIMIT 10',
    );
  } catch (error) {
    if (error && error.code === 'ER_NO_SUCH_TABLE') return empty;
    throw error;
  }
  const history = historyRows.map((row) => ({
    eventType: row.event_type,
    overallResult: row.overall_result,
    durationMs: Number(row.duration_ms),
    createdAt: new Date(row.created_at).toISOString(),
  }));

  let statusRows;
  try {
    [statusRows] = await worldDb.query(
      'SELECT event_type, overall_result, duration_ms, checks, modules, installer_commit, updated_at '
      + 'FROM doctor_status WHERE id = 1',
    );
  } catch (error) {
    // ER_BAD_FIELD_ERROR: doctor_status existe pero de antes del ALTER TABLE
    // que añadió `modules` (lib/doctor.sh la agrega sola en su próxima
    // pasada) — se sirve igual, sin inventario de módulos todavía.
    if (error && error.code === 'ER_BAD_FIELD_ERROR') {
      [statusRows] = await worldDb.query(
        'SELECT event_type, overall_result, duration_ms, checks, installer_commit, updated_at '
        + 'FROM doctor_status WHERE id = 1',
      );
    } else if (error && error.code === 'ER_NO_SUCH_TABLE') {
      return { ...empty, history };
    } else {
      throw error;
    }
  }
  if (!statusRows[0]) return { ...empty, history };

  const row = statusRows[0];
  return {
    available: true,
    updatedAt: new Date(row.updated_at).toISOString(),
    eventType: row.event_type,
    overallResult: row.overall_result,
    durationMs: Number(row.duration_ms),
    installerCommit: row.installer_commit,
    checks: row.checks,
    modules: row.modules ?? [],
    history,
  };
}

// Una muestra completa para "Estado y rendimiento": doctor + proceso del
// worldserver (systemMetrics.js) + población actual. Es el `fetcher` de
// metricsCache: sólo se llama cuando la caché compartida ya tiene 5 minutos o
// más, nunca una vez por usuario que abre la vista.
async function fetchSystemStatus({ worldDb, charactersDb, botAccountIdSet }) {
  const [doctor, system] = await Promise.all([fetchDoctorStatus(worldDb), sampleSystem()]);

  const [charRows] = await charactersDb.query(
    'SELECT account FROM characters WHERE online = 1 AND deleteDate IS NULL',
  );
  const botIds = charRows.length ? await botAccountIdSet(charRows.map((row) => Number(row.account))) : new Set();
  const botsOnline = charRows.filter((row) => botIds.has(Number(row.account))).length;

  return {
    sampledAt: new Date().toISOString(),
    doctor,
    unit: system.unit,
    process: system.process,
    slowTicks: system.slowTicks,
    population: { playersOnline: charRows.length - botsOnline, botsOnline },
  };
}

// Guarda la muestra recién tomada (nunca una servida desde caché: eso lo
// garantiza metricsCache.js) y recorta lo más viejo de 30 días, para que la
// tabla no crezca sin límite. Un fallo aquí no debe romper la respuesta al
// usuario (metricsCache.js ya envuelve esto en su propio try/catch).
async function persistMetricsSample(panelDb, data, atMs) {
  await panelDb.execute(
    `INSERT INTO panel_metrics_sample
      (sampled_at, cpu_pct, mem_rss_kb, mem_peak_kb, uptime_secs, world_restarts, slow_tick_count, players_online, bots_online)
     VALUES (FROM_UNIXTIME(?), ?, ?, ?, ?, ?, ?, ?, ?)`,
    [
      Math.floor(atMs / 1000),
      data.process?.cpuPct ?? null,
      data.process?.memRssKb ?? null,
      data.process?.memPeakKb ?? null,
      data.process?.uptimeSecs ?? null,
      data.unit.observed ? data.unit.restarts : null,
      data.slowTicks.observed ? data.slowTicks.count : null,
      data.population.playersOnline,
      data.population.botsOnline,
    ],
  );
  await panelDb.execute('DELETE FROM panel_metrics_sample WHERE sampled_at < NOW() - INTERVAL 30 DAY');
}

// Caché compartida de un único vuelo (metricsCache.js): todo el proceso del
// panel comparte esta misma instancia, así que da igual cuánta gente tenga
// "Estado y rendimiento" abierta a la vez — como mucho una consulta real cada
// 5 minutos, nunca una por usuario ni por apertura de la vista. app.js decide
// si usar ésta o un metricsCache de test vía overrides; aquí sólo se sabe
// construirla.
export function buildMetricsCache({ worldDb, charactersDb, panelDb, botAccountIdSet }) {
  return createMetricsCache({
    fetcher: () => fetchSystemStatus({ worldDb, charactersDb, botAccountIdSet }),
    persist: (data, atMs) => persistMetricsSample(panelDb, data, atMs),
  });
}

export function registerStatusRoutes(app, { requireAuth, noStore, soap, panelDb, metricsCache }) {
  app.get('/api/server/status', requireAuth, async (_request, response, next) => {
    noStore(response);
    try {
      response.json(await getServerStatus(soap));
    } catch (error) {
      next(error);
    }
  });

  // "Estado y rendimiento": doctor + métricas en vivo del worldserver +
  // población + inventario de módulos, en una sola respuesta. Visible para
  // cualquier cuenta con sesión (requireAuth a secas, sin requireGm):
  // hay un menú propio bajo Mi cuenta visible para todos los
  // usuarios autenticados. metricsCache.get() es quien decide si hace falta
  // una muestra nueva (5 minutos o más desde la última) o si sirve la que ya
  // hay — un botón "actualizar" del frontend pasa por el mismo camino, sin
  // forzar nada. Los mensajes de cada comprobación los escribe doctor.sh
  // pensados para pantalla — sin rutas internas, contraseñas ni SQL — así
  // que no hace falta filtrar nada aquí aparte de no exponer más tablas.
  app.get('/api/metrics/status', requireAuth, async (_request, response, next) => {
    noStore(response);
    try {
      const { data, ageMs, fromCache } = await metricsCache.get();
      const periodStartedAt = data.doctor.available ? data.doctor.updatedAt : null;

      let periodMax = null;
      if (periodStartedAt) {
        const [maxRows] = await panelDb.query(
          `SELECT MAX(cpu_pct) AS cpuPct, MAX(mem_rss_kb) AS memRssKb, MAX(uptime_secs) AS uptimeSecs,
                  MAX(players_online) AS playersOnline, MAX(bots_online) AS botsOnline
           FROM panel_metrics_sample WHERE sampled_at >= ?`,
          [periodStartedAt],
        );
        const row = maxRows[0] || {};
        periodMax = {
          cpuPct: row.cpuPct === null || row.cpuPct === undefined ? null : Number(row.cpuPct),
          memRssKb: row.memRssKb === null || row.memRssKb === undefined ? null : Number(row.memRssKb),
          uptimeSecs: row.uptimeSecs === null || row.uptimeSecs === undefined ? null : Number(row.uptimeSecs),
          playersOnline: row.playersOnline === null || row.playersOnline === undefined ? null : Number(row.playersOnline),
          botsOnline: row.botsOnline === null || row.botsOnline === undefined ? null : Number(row.botsOnline),
        };
      }

      response.json({
        doctor: {
          available: data.doctor.available,
          overallResult: data.doctor.overallResult ?? null,
          checks: data.doctor.checks ?? [],
          // Commit del instalador: no es secreto (repositorio abierto), y es
          // la "versión" de los módulos propios (no tienen commit propio,
          // ver lib/doctor.sh::_doctor_build_modules_json).
          installerCommit: data.doctor.installerCommit ?? null,
        },
        period: periodStartedAt ? { startedAt: periodStartedAt, eventType: data.doctor.eventType } : null,
        history: data.doctor.history ?? [],
        modules: data.doctor.modules ?? [],
        live: {
          // null = "no observado" (worldserver dormido/caído, o sin systemctl):
          // se distingue a propósito de un 0 real.
          active: data.unit.observed ? data.unit.active : null,
          restarts: data.unit.observed ? data.unit.restarts : null,
          activeSince: data.unit.observed ? data.unit.activeSince : null,
          process: data.process,
          slowTicks: data.slowTicks.observed ? data.slowTicks.count : null,
          population: data.population,
          sampledAt: data.sampledAt,
        },
        periodMax,
        cache: { ageMs, fromCache },
      });
    } catch (error) {
      next(error);
    }
  });
}
