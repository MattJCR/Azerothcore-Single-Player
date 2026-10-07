// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Vista "Operaciones de bots" (mod-bot-operations). Lee
// bot_operations_snapshot/bot_operations_action tal cual (JSON ya parseado
// por mysql2 en el backend): esta vista sólo pinta lo que el servidor ya
// agregó, nunca interpreta nada por su cuenta.
import { $, $$, state, api, showLogin, showToast, formValues, formatAgo, escapeHtml } from '../shared.js';

export function enter() {
  loadBotOps();
}

export async function load() {
  await loadBotOps();
}

// Igual que formatAgo pero sin el "hace" (para una duración futura, p.ej. lo
// que queda de enfriamiento de un punto caliente de Guerra de mundo).
function formatDuration(ms) {
  if (!Number.isFinite(ms) || ms < 1000) return 'menos de 1 min';
  const seconds = Math.floor(ms / 1000);
  if (seconds < 60) return `${seconds} s`;
  const minutes = Math.floor(seconds / 60);
  if (minutes < 60) return `${minutes} min`;
  return `${Math.floor(minutes / 60)} h`;
}

// attackerTeam/defenderTeam: 0 Alianza, 1 Horda, 2 cualquiera (mismo valor en
// los dos = punto de duelos si no es 2,2 — ver world_bots_pvp_hotspot.sql).
function pvpTeamsLabel(attackerTeam, defenderTeam) {
  const TEAM_NAMES = ['Alianza', 'Horda', 'cualquiera'];
  if (attackerTeam === defenderTeam) {
    return attackerTeam === 2 ? 'cualquiera contra cualquiera' : `duelos (${TEAM_NAMES[attackerTeam]})`;
  }
  return `${TEAM_NAMES[attackerTeam]} ataca / ${TEAM_NAMES[defenderTeam]} defiende`;
}

function renderBotOpsSnapshot(data) {
  $('#bot-ops-stale').classList.toggle('hidden', !data.stale);
  const r = data.reservations || {};
  const ws = data.worldStage || {};
  const wp = data.worldPvp || {};
  const q = data.queues || {};
  const g = data.groups || {};
  const gu = data.guilds || {};
  const qm = data.questMates || {};
  const dash = (value) => (value === undefined || value === null ? '—' : value);

  $('#bot-ops-online').textContent = dash(r.onlineTotal);
  $('#bot-ops-cap').textContent = dash(r.globalCap);
  $('#bot-ops-pending').textContent = dash(r.pendingTotal);
  $('#bot-ops-stage').textContent = ws.stageName || '—';
  $('#bot-ops-stage-by').textContent = ws.decidedBy ? `decidida por ${escapeHtml(ws.decidedBy)}` : 'sin nadie que la fuerce';
  $('#bot-ops-updated').textContent = data.available ? formatAgo(Date.now() - new Date(data.updatedAt).getTime()) : 'sin datos';

  const stuck = (wp.activeEvents || []).filter((event) => event.ending && Date.now() - event.startedAtMs > 10 * 60_000);
  // cada sección trae `enabled` y, desde el 24/09/2026, `stale` (el
  // módulo no la ha publicado en las últimas instantáneas). Una sección
  // apagada o sin datos recientes no debe leerse como "cero".
  const sections = [
    ['Mundo y etapa (world-bots)', ws], ['Guerra de mundo', wp], ['Colas (queue-bots)', q],
    ['Grupos (party-here)', g], ['Hermandades (home-guild)', gu], ['Compañeros de misión', qm],
  ];
  const moduleNotes = data.available ? sections.flatMap(([label, section]) => {
    if (section.enabled === false) return [`<p><strong>${label}</strong>: módulo desactivado.</p>`];
    if (section.stale === true) return [`<p><strong>${label}</strong>: sin datos recientes (módulo no cargado o parado).</p>`];
    return [];
  }) : [];
  const alerts = [
    ...stuck.map((event) => `<p><strong>${escapeHtml(event.label)}</strong> (evento #${event.id}) lleva terminando más de 10 min. Puedes pararlo desde la pestaña Acciones.</p>`),
    ...moduleNotes,
  ];
  $('#bot-ops-alerts').innerHTML = alerts.length
    ? alerts.join('')
    : '<span>✓</span><h3>Sin alertas</h3><p>No hay eventos PvP de mundo pendientes de cerrar.</p>';

  $('#bot-ops-claims-body').innerHTML = (r.claimsByOwner || []).map((row) => `<tr><td>${escapeHtml(row.module)}</td><td>${row.count}</td></tr>`).join('')
    || '<tr><td colspan="2">Sin bots reservados ahora mismo</td></tr>';
  $('#bot-ops-pending-alliance').textContent = dash(r.pendingAlliance);
  $('#bot-ops-pending-horde').textContent = dash(r.pendingHorde);
  $('#bot-ops-pending-range-body').innerHTML = (r.pendingByRange || []).map((row) => `<tr><td>${row.minLevel}-${row.maxLevel}</td><td>${row.count}</td></tr>`).join('')
    || '<tr><td colspan="2">Sin reservas pendientes</td></tr>';

  $('#bot-ops-ws-stage').textContent = ws.stageName || '—';
  $('#bot-ops-ws-by').textContent = ws.decidedBy ? `decidida por ${escapeHtml(ws.decidedBy)}` : 'sin nadie que la fuerce';
  $('#bot-ops-ws-cap').textContent = dash(ws.levelCap);
  $('#bot-ops-ws-maps').textContent = (ws.maps || []).length ? `mapas ${ws.maps.join(', ')}` : 'todos los mapas';
  $('#bot-ops-ws-zones').textContent = dash(ws.zonesPopulated);
  $('#bot-ops-ws-moved').textContent = `${ws.relocatedTotal ?? 0} / ${ws.rerolledTotal ?? 0}`;
  $('#bot-ops-pvp-summary').textContent = `${(wp.activeEvents || []).length} evento(s) activos, ${wp.hotspotCount ?? 0} puntos calientes`;
  const events = wp.activeEvents || [];
  $('#bot-ops-pvp-body').innerHTML = events.map((event) => `<tr>
    <td>${event.id}</td><td>${escapeHtml(event.label)}</td><td>${event.zoneId}</td>
    <td>${event.attackers}</td><td>${event.defenders}</td>
    <td>${formatAgo(Date.now() - event.startedAtMs)}</td>
    <td>${event.ending ? 'terminando' : 'activo'}</td>
  </tr>`).join('');
  $('#bot-ops-pvp-empty').classList.toggle('hidden', events.length > 0);

  const activeHotspotLabels = new Set(events.map((event) => event.label));
  $('#bot-ops-hotspots-body').innerHTML = (wp.hotspots || []).map((h) => {
    let status;
    if (activeHotspotLabels.has(h.label)) status = 'activo ahora';
    else if (!h.enabled) status = 'desactivado';
    else if (h.cooldownRemainingMs > 0) status = `enfriando (${formatDuration(h.cooldownRemainingMs)})`;
    else status = 'listo';
    return `<tr>
      <td>${escapeHtml(h.label)}</td>
      <td>${pvpTeamsLabel(h.attackerTeam, h.defenderTeam)}</td>
      <td>${h.minLevel}-${h.maxLevel}</td>
      <td>${status}</td>
    </tr>`;
  }).join('') || '<tr><td colspan="4">Sin puntos configurados</td></tr>';

  $('#bot-ops-q-waiting').textContent = dash(q.waitingHumans);
  $('#bot-ops-q-1v1').textContent = dash(q.arena1v1Missing);
  $('#bot-ops-q-bg').textContent = dash(q.battlegroundMissing);
  $('#bot-ops-q-arena').textContent = dash(q.arenaMissing);
  $('#bot-ops-q-dungeon').textContent = dash(q.dungeonMissing);
  $('#bot-ops-q-raid').textContent = dash(q.raidMissing);
  $('#bot-ops-groups').textContent = `${g.groupsFormed ?? 0} / ${g.raidsFormed ?? 0}`;
  $('#bot-ops-groups-bots').textContent = g.botsInGroups ?? 0;
  $('#bot-ops-guilds').textContent = dash(gu.homeGuilds);
  $('#bot-ops-guilds-bots').textContent = gu.botsManaged ?? 0;
  $('#bot-ops-mates').textContent = dash(qm.humansWithMates);
  $('#bot-ops-mates-pairs').textContent = qm.activePairings ?? 0;
}

const BOT_OPS_STATUS_LABELS = { pending: 'pendiente', done: 'hecha', failed: 'fallida', expired: 'caducada' };

function renderBotOpsHistory(actions) {
  $('#bot-ops-history-body').innerHTML = actions.map((row) => `<tr>
    <td>${new Date(row.requestedAt).toLocaleString('es-ES')}</td>
    <td>${escapeHtml(row.action)}</td>
    <td>${row.param ? escapeHtml(row.param) : '—'}</td>
    <td>${row.actorName ? escapeHtml(row.actorName) : '—'}</td>
    <td>${BOT_OPS_STATUS_LABELS[row.status] || escapeHtml(row.status)}</td>
    <td>${row.result ? escapeHtml(row.result) : '—'}</td>
  </tr>`).join('') || '<tr><td colspan="6">Sin solicitudes todavía</td></tr>';
}

async function loadBotOps() {
  try {
    const data = await api('/api/bot-operations/snapshot');
    state.botOps.snapshot = data;
    renderBotOpsSnapshot(data);
  } catch (error) {
    if (error.status === 401) return showLogin();
    if (error.status !== 403) showToast(error.message);
  }
}

async function loadBotOpsActions() {
  try {
    const data = await api('/api/bot-operations/actions');
    state.botOps.actions = data.actions;
    renderBotOpsHistory(data.actions);
  } catch (error) {
    if (error.status === 401) return showLogin();
    if (error.status !== 403) showToast(error.message);
  }
}

function selectBotOpsTab(name, { focus = false } = {}) {
  state.botOpsTab = name;
  $$('#bot-ops-view .mod-tabs button[data-bot-ops-tab]').forEach((tab) => {
    const selected = tab.dataset.botOpsTab === name;
    tab.classList.toggle('active', selected);
    tab.setAttribute('aria-selected', String(selected));
    tab.tabIndex = selected ? 0 : -1;
    if (selected && focus) tab.focus();
  });
  ['summary', 'reservations', 'world-stage', 'queues', 'actions'].forEach((tabName) => {
    $(`#bot-ops-tab-${tabName}`).classList.toggle('hidden', tabName !== name);
  });
  if (name === 'actions' && !state.botOps.actions) loadBotOpsActions();
}

$$('#bot-ops-view .mod-tabs button[data-bot-ops-tab]').forEach((button) => button.addEventListener('click', () => selectBotOpsTab(button.dataset.botOpsTab)));
$('#bot-ops-view .mod-tabs').addEventListener('keydown', (event) => {
  if (!['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(event.key)) return;
  event.preventDefault();
  const tabs = $$('#bot-ops-view .mod-tabs button[data-bot-ops-tab]');
  const current = tabs.findIndex((tab) => tab.dataset.botOpsTab === state.botOpsTab);
  const next = event.key === 'Home' ? 0 : event.key === 'End' ? tabs.length - 1 : (current + (event.key === 'ArrowRight' ? 1 : -1) + tabs.length) % tabs.length;
  selectBotOpsTab(tabs[next].dataset.botOpsTab, { focus: true });
});

$$('.bot-ops-action-form').forEach((form) => {
  form.addEventListener('submit', async (event) => {
    event.preventDefault();
    const button = form.querySelector('button[type="submit"]');
    button.disabled = true;
    try {
      const values = formValues(form);
      await api('/api/bot-operations/actions', {
        method: 'POST',
        body: JSON.stringify({ action: form.dataset.action, param: values.param || undefined, reason: values.reason || undefined }),
      });
      form.reset();
      showToast('Solicitud enviada.');
      await Promise.all([loadBotOpsActions(), loadBotOps()]);
    } catch (error) {
      if (error.status === 401) return showLogin();
      showToast(error.message);
    } finally {
      button.disabled = false;
    }
  });
});
