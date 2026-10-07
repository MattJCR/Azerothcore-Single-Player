// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Vista "Estado y rendimiento" (bajo Mi cuenta) —
// (13/09/2026): muestreo bajo demanda con caché compartida de 5 minutos (el
// backend ya la aplica, metricsCache.js; aquí sólo se evita pedir de más
// mientras nadie mira la vista) y "no observado" siempre distinto de un
// valor real igual a cero.
import { $, $$, state, api, showLogin, formatAgo, escapeHtml } from '../shared.js';
import { t, formatDateTime } from '../i18n.js';

const METRICS_RESULT_LABELS = { ok: t('Todo correcto'), warn: t('Con avisos'), fail: t('Con fallos') };
// Reutiliza los tonos de .tag que ya existen (verde/ámbar/rojo) en vez de
// añadir clases nuevas: ok→online, warn→pending (ámbar), fail→warn (rojo).
const METRICS_RESULT_TAGS = { ok: 'online', warn: 'pending', fail: 'warn' };
const METRICS_EVENT_LABELS = { install: t('Instalación'), update: t('Actualización'), compile: t('Compilación'), restart: t('Reinicio'), manual: t('Manual') };
const METRICS_MODULE_STATUS_LABELS = { active: t('activo'), disabled: t('desactivado') };
const METRICS_POLL_MS = 5 * 60_000;
let metricsPollTimer = null;

export function enter() {
  loadMetricsStatus();
  startMetricsPolling();
}

export function exit() {
  stopMetricsPolling();
}

function metricsEventLabel(eventType) {
  if (!eventType) return '—';
  if (METRICS_EVENT_LABELS[eventType]) return METRICS_EVENT_LABELS[eventType];
  // "phase-5", "install-failed"... variantes que genera install.sh: se
  // muestran tal cual en vez de forzarlas a una etiqueta que no encaja.
  return eventType;
}

// "sin datos" para null/undefined en vez de dejar pasar un 0 o un NaN: es la
// diferencia entre "no observado" y "un valor real igual a cero" que se pide
// aquí, en un único sitio en vez de repetir la comprobación.
function metricOrGap(value, formatter = (v) => String(v)) {
  return value === null || value === undefined ? t('sin datos') : formatter(value);
}

function formatUptime(seconds) {
  const hours = Math.floor(seconds / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  return hours > 0 ? `${hours} h ${minutes} min` : `${minutes} min`;
}

function formatMib(kb) {
  return `${Math.round(kb / 1024)} MiB`;
}

function renderMetricsStatus(body) {
  $('#metrics-loading').classList.add('hidden');
  $('#metrics-error').classList.add('hidden');

  const badge = $('#metrics-status-badge');
  const historyBody = $('#metrics-history-body');
  const history = body.history || [];
  historyBody.innerHTML = history.map((row) => `<tr>
    <td>${formatDateTime(row.createdAt)}</td>
    <td>${escapeHtml(metricsEventLabel(row.eventType))}</td>
    <td><span class="tag ${METRICS_RESULT_TAGS[row.overallResult] || 'offline'}">${METRICS_RESULT_LABELS[row.overallResult] || escapeHtml(row.overallResult)}</span></td>
    <td>${row.durationMs} ms</td>
  </tr>`).join('') || `<tr><td colspan="4">${t('Sin historial todavía')}</td></tr>`;

  if (!body.doctor.available) {
    $('#metrics-empty').classList.remove('hidden');
    $('#metrics-body').classList.add('hidden');
    badge.textContent = t('Sin datos');
    badge.className = 'tag offline';
    $('#metrics-period-summary').textContent = t('Todavía no se ha ejecutado ninguna comprobación.');
    return;
  }

  $('#metrics-empty').classList.add('hidden');
  $('#metrics-body').classList.remove('hidden');

  badge.textContent = METRICS_RESULT_LABELS[body.doctor.overallResult] || body.doctor.overallResult;
  badge.className = `tag ${METRICS_RESULT_TAGS[body.doctor.overallResult] || 'offline'}`;

  const period = body.period;
  $('#metrics-period-summary').textContent = period
    ? t('{event} · {date} — los picos de esta pestaña se cuentan desde aquí.', { event: metricsEventLabel(period.eventType), date: formatDateTime(period.startedAt) })
    : t('Sin periodo todavía.');

  // Aviso de "puede que el muestreo se haya detenido" con un margen bastante
  // mayor que los 5 min normales de la caché: sólo salta si de verdad hace
  // tiempo que no hay una muestra nueva, no en cada apertura de la vista.
  const sampleAgeMs = Date.now() - new Date(body.live.sampledAt).getTime();
  const stale = sampleAgeMs > 20 * 60_000;
  $('#metrics-stale-note').classList.toggle('hidden', !stale);
  if (stale) $('#metrics-stale-detail').textContent = t('Última muestra {ago}: comprueba que el muestreo automático sigue activo.', { ago: formatAgo(sampleAgeMs) });

  $('#metrics-checks-body').innerHTML = (body.doctor.checks || []).map((check) => `<tr>
    <td>${escapeHtml(check.name)}</td>
    <td><span class="tag ${METRICS_RESULT_TAGS[check.status] || 'offline'}">${METRICS_RESULT_LABELS[check.status] || escapeHtml(check.status)}</span></td>
    <td>${escapeHtml(check.detail)}</td>
  </tr>`).join('') || `<tr><td colspan="3">${t('Sin comprobaciones registradas')}</td></tr>`;

  const live = body.live;
  $('#metrics-live-state').textContent = live.active === null ? t('no observado') : (live.active ? t('activo') : t('inactivo'));
  $('#metrics-live-since').textContent = live.activeSince ? t('desde {date}', { date: formatDateTime(live.activeSince) }) : '';
  $('#metrics-restarts').textContent = live.restarts === null ? t('reinicios: no observado') : t('{count} reinicios registrados por systemd', { count: live.restarts });

  const proc = live.process; // null = worldserver dormido/caído ahora mismo: no observado, no un cero
  // "en espera" cuando sabemos que el motivo es que el proceso no está
  // arrancado (modo en espera u otro reinicio) — "sin datos" se reserva para
  // cuando de verdad no se pudo saber (systemctl no observado, o el proceso
  // desapareció justo al muestrear): 14/09/2026,
  // ver también metrics-live-state un poco más abajo.
  const procGap = live.active === false ? t('en espera') : t('sin datos');
  $('#metrics-cpu').textContent = proc ? `${proc.cpuPct.toFixed(1)} %` : procGap;
  $('#metrics-mem').textContent = proc ? formatMib(proc.memRssKb) : procGap;
  $('#metrics-mem-peak').textContent = proc ? formatMib(proc.memPeakKb) : '—';
  $('#metrics-uptime').textContent = proc ? formatUptime(proc.uptimeSecs) : procGap;
  $('#metrics-cpu-max').textContent = metricOrGap(body.periodMax?.cpuPct, (v) => `${v.toFixed(1)} %`);

  $('#metrics-slow-ticks').textContent = metricOrGap(live.slowTicks);

  $('#metrics-players').textContent = String(live.population.playersOnline);
  $('#metrics-bots').textContent = String(live.population.botsOnline);
  $('#metrics-players-max').textContent = metricOrGap(body.periodMax?.playersOnline);
  $('#metrics-bots-max').textContent = metricOrGap(body.periodMax?.botsOnline);

  $('#metrics-modules-body').innerHTML = (body.modules || []).map((mod) => {
    const commit = mod.commit || (mod.kind === 'own' && body.doctor.installerCommit ? body.doctor.installerCommit.slice(0, 12) : '');
    return `<tr>
      <td>${escapeHtml(mod.name)}</td>
      <td>${mod.kind === 'own' ? t('propio') : t('terceros')}</td>
      <td>${commit ? escapeHtml(commit) : '—'}</td>
      <td>${METRICS_MODULE_STATUS_LABELS[mod.status] || escapeHtml(mod.status)}</td>
    </tr>`;
  }).join('') || `<tr><td colspan="4">${t('Sin inventario todavía')}</td></tr>`;
}

async function loadMetricsStatus() {
  try {
    const body = await api('/api/metrics/status');
    renderMetricsStatus(body);
  } catch (error) {
    if (error.status === 401) { stopMetricsPolling(); return showLogin(); }
    $('#metrics-loading').classList.add('hidden');
    $('#metrics-error').classList.remove('hidden');
    $('#metrics-error-detail').textContent = error.message;
  }
}

function stopMetricsPolling() {
  if (metricsPollTimer) {
    clearInterval(metricsPollTimer);
    metricsPollTimer = null;
  }
}

// Como mucho una petición cada 5 minutos MIENTRAS la vista siga abierta; el
// listener de visibilitychange (app.js) para/retoma esto al ocultar o
// recuperar la pestaña, así que aquí no hay que mirar document.hidden.
function startMetricsPolling() {
  stopMetricsPolling();
  metricsPollTimer = setInterval(loadMetricsStatus, METRICS_POLL_MS);
}

// "Perder la conexión": el navegador ya avisa de esto sin necesitar
// comprobar cada fetch a mano.
window.addEventListener('offline', stopMetricsPolling);

const METRICS_TAB_NAMES = ['summary', 'checks', 'modules', 'history'];

function selectMetricsTab(name, { focus = false } = {}) {
  state.metricsTab = name;
  $$('#metrics-view .mod-tabs button[data-metrics-tab]').forEach((tab) => {
    const selected = tab.dataset.metricsTab === name;
    tab.classList.toggle('active', selected);
    tab.setAttribute('aria-selected', String(selected));
    tab.tabIndex = selected ? 0 : -1;
    if (selected && focus) tab.focus();
  });
  METRICS_TAB_NAMES.forEach((tabName) => {
    $(`#metrics-tab-${tabName}`).classList.toggle('hidden', tabName !== name);
  });
}

$$('#metrics-view .mod-tabs button[data-metrics-tab]').forEach((button) => button.addEventListener('click', () => selectMetricsTab(button.dataset.metricsTab)));
$('#metrics-view .mod-tabs').addEventListener('keydown', (event) => {
  if (!['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(event.key)) return;
  event.preventDefault();
  const tabs = $$('#metrics-view .mod-tabs button[data-metrics-tab]');
  const current = tabs.findIndex((tab) => tab.dataset.metricsTab === state.metricsTab);
  const next = event.key === 'Home' ? 0 : event.key === 'End' ? tabs.length - 1 : (current + (event.key === 'ArrowRight' ? 1 : -1) + tabs.length) % tabs.length;
  selectMetricsTab(tabs[next].dataset.metricsTab, { focus: true });
});
