// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Vista "Actualizaciones" (rango 3): compara los módulos fijados en
// versions.lock contra sus ramas remotas de GitHub.
import { $, $$, state, api, showLogin, escapeHtml } from '../shared.js';
import { t, formatDateTime } from '../i18n.js';

function updateStatus(status) {
  return {
    current: { label: t('Al día'), className: 'is-current' },
    update: { label: t('Actualización disponible'), className: 'has-update' },
    unreachable: { label: t('Sin acceso'), className: 'is-unreachable' },
  }[status] || { label: t('Desconocido'), className: 'is-unreachable' };
}

function updateRepositoryRow(repository) {
  const status = updateStatus(repository.status);
  const remote = repository.remoteCommit ? repository.remoteCommit.slice(0, 10) : '?';
  const action = repository.compareUrl
    ? `<a href="${escapeHtml(repository.compareUrl)}" target="_blank" rel="noreferrer">${t('Ver cambios')}</a>`
    : `<a href="${escapeHtml(repository.sourceUrl)}" target="_blank" rel="noreferrer">${t('Repositorio')}</a>`;
  return `<tr>
    <td><a class="update-repository" href="${escapeHtml(repository.sourceUrl)}" target="_blank" rel="noreferrer">${escapeHtml(repository.name)}</a><small>${t('rama {branch}', { branch: escapeHtml(repository.branch) })}</small></td>
    <td>${escapeHtml(repository.pinnedDate)}</td>
    <td class="update-commits"><code>${escapeHtml(repository.pinnedCommit.slice(0, 10))}</code><span>→</span><code>${escapeHtml(remote)}</code></td>
    <td><span class="update-status ${status.className}">${status.label}</span></td>
    <td class="update-action">${action}</td>
  </tr>`;
}

function updateGroupTabCount(group) {
  if (group.summary.updates) return { label: group.summary.updates === 1 ? t('1 pendiente') : t('{count} pendientes', { count: group.summary.updates }), className: 'has-update' };
  if (group.summary.unreachable) return { label: t('{count} sin comprobar', { count: group.summary.unreachable }), className: 'is-unreachable' };
  return { label: t('Al día'), className: '' };
}

function renderUpdateGroup(group) {
  return `<section class="update-group">
    <div class="update-group-heading"><div><h4>${escapeHtml(group.label)}</h4><p>${escapeHtml(group.source)} · ${t('{count} seguidos', { count: group.summary.total })}</p></div></div>
    <div class="table-wrap"><table class="updates-table">
      <thead><tr><th>${t('Repositorio')}</th><th>${t('Fijado')}</th><th>${t('Commit fijado → remoto')}</th><th>${t('Estado')}</th><th></th></tr></thead>
      <tbody>${group.repositories.map(updateRepositoryRow).join('')}</tbody>
    </table></div>
  </section>`;
}

function selectUpdateGroup(id, { focus = false } = {}) {
  state.updates.activeGroup = id;
  $$('#updates-tabs button[data-update-group]').forEach((tab) => {
    const selected = tab.dataset.updateGroup === id;
    tab.classList.toggle('active', selected);
    tab.setAttribute('aria-selected', String(selected));
    tab.tabIndex = selected ? 0 : -1;
    if (selected && focus) tab.focus();
  });
  const group = state.updates.result?.groups.find((candidate) => candidate.id === id);
  $('#updates-results').innerHTML = group ? renderUpdateGroup(group) : '';
}

function renderUpdateResults(data) {
  const { summary } = data;
  $('#updates-last-check').textContent = t('Última comprobación: {date}', { date: formatDateTime(data.checkedAt) });
  $('#updates-summary')?.remove();
  const summaryHtml = `<div id="updates-summary" class="update-summary">
    <article><span>${t('Actualizaciones')}</span><strong class="${summary.updates ? 'has-updates' : ''}">${summary.updates}</strong><small>${t('disponibles')}</small></article>
    <article><span>${t('Al día')}</span><strong>${summary.current}</strong><small>${t('repositorios')}</small></article>
    <article><span>${t('Sin acceso')}</span><strong class="${summary.unreachable ? 'has-warnings' : ''}">${summary.unreachable}</strong><small>${t('no comprobados')}</small></article>
    <article><span>${t('Total')}</span><strong>${summary.total}</strong><small>${t('seguidos')}</small></article>
  </div>`;
  $('#updates-tabs').insertAdjacentHTML('beforebegin', summaryHtml);

  const tabs = $('#updates-tabs');
  tabs.classList.toggle('hidden', !data.groups.length);
  tabs.innerHTML = data.groups.map((group, index) => {
    const count = updateGroupTabCount(group);
    const active = state.updates.activeGroup ? group.id === state.updates.activeGroup : index === 0;
    return `<button id="updates-tab-${escapeHtml(group.id)}-button" class="${active ? 'active' : ''}" type="button" role="tab"
      aria-selected="${active}" aria-controls="updates-results" tabindex="${active ? 0 : -1}" data-update-group="${escapeHtml(group.id)}">
      ${escapeHtml(group.label)} <span class="update-group-count ${count.className}">${escapeHtml(count.label)}</span>
    </button>`;
  }).join('');
  if (!data.groups.some((group) => group.id === state.updates.activeGroup)) state.updates.activeGroup = data.groups[0]?.id || null;
  selectUpdateGroup(state.updates.activeGroup);
}

async function checkForUpdates() {
  if (state.updates.checking) return;
  const button = $('#updates-check');
  const feedback = $('#updates-feedback');
  state.updates.checking = true;
  button.disabled = true;
  button.textContent = t('Comprobando…');
  feedback.textContent = t('Consultando las ramas remotas de GitHub. Puede tardar unos segundos…');
  feedback.className = 'updates-feedback';
  try {
    const data = await api('/api/updates/check', { method: 'POST', body: '{}' });
    state.updates.result = data;
    renderUpdateResults(data);
    feedback.classList.add('hidden');
  } catch (error) {
    if (error.status === 401) return showLogin();
    feedback.textContent = error.message;
    feedback.className = 'updates-feedback is-error';
  } finally {
    state.updates.checking = false;
    button.disabled = false;
    button.textContent = t('Comprobar de nuevo');
  }
}

$('#updates-check').addEventListener('click', checkForUpdates);
$('#updates-tabs').addEventListener('click', (event) => {
  const button = event.target.closest('button[data-update-group]');
  if (button) selectUpdateGroup(button.dataset.updateGroup);
});
$('#updates-tabs').addEventListener('keydown', (event) => {
  if (!['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(event.key)) return;
  event.preventDefault();
  const tabs = $$('#updates-tabs button[data-update-group]');
  if (!tabs.length) return;
  const current = tabs.findIndex((tab) => tab.dataset.updateGroup === state.updates.activeGroup);
  const next = event.key === 'Home' ? 0 : event.key === 'End' ? tabs.length - 1 : (current + (event.key === 'ArrowRight' ? 1 : -1) + tabs.length) % tabs.length;
  selectUpdateGroup(tabs[next].dataset.updateGroup, { focus: true });
});
