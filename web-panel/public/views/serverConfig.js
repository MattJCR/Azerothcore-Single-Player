// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Vista "Configuración del servidor" (rango 3): edición por lotes con
// aplicación diferida — se guarda en panel_config_pending y
// se aplica de una vez con reinicio avisado.
import { $, state, api, showLogin, showToast, showView, escapeHtml, serverConfigCategorySlot } from '../shared.js';

export function enter() {
  loadServerConfigCategories();
}

const RISK_LABELS = { low: 'Riesgo bajo', medium: 'Riesgo medio', high: 'Riesgo alto' };

function serverConfigRowHtml(param) {
  const modified = param.pending !== null && param.pending !== undefined;
  const displayValue = modified ? param.pending : param.current;
  const locked = param.risk === 'high';
  let control;
  if (locked) {
    control = '<span class="locked-note">Edición manual en el servidor</span>';
  } else if (param.type === 'bool') {
    const on = displayValue === '1' || displayValue === 'true';
    control = `<button type="button" class="toggle ${on ? 'is-on' : ''}" role="switch" aria-checked="${on}" data-config-key="${escapeHtml(param.key)}"><span class="toggle-thumb"></span></button>`;
  } else {
    control = `<input class="config-number" type="text" inputmode="decimal" value="${escapeHtml(displayValue ?? '')}" data-config-key="${escapeHtml(param.key)}">
      <span class="config-default">por defecto ${escapeHtml(param.default ?? '—')}</span>`;
  }
  return `<div class="config-row ${modified ? 'is-modified' : ''} ${locked ? 'is-locked' : ''}" data-config-key="${escapeHtml(param.key)}">
    <div>
      <div class="config-row-title">
        <code>${escapeHtml(param.key)}</code>
        <span class="risk-badge ${param.risk}">${RISK_LABELS[param.risk] || param.risk}</span>
        ${modified ? '<span class="pending-dot" title="Cambio pendiente"></span>' : ''}
      </div>
      <p class="config-row-desc">${escapeHtml(param.description || '')}</p>
    </div>
    <div class="config-row-control">${control}</div>
  </div>`;
}

// antes la búsqueda sólo miraba sc.params (la categoría ya
// cargada); con texto de búsqueda se busca en el catálogo entero (sc.searchIndex,
// sin current/pending: sólo clave+descripción, pedido una vez al entrar) y
// cada resultado salta a su categoría. Sin texto, se conserva el filtrado de
// siempre dentro de la categoría seleccionada.
function serverConfigSearchResultHtml(param) {
  return `<button type="button" class="config-search-result" data-jump-category="${escapeHtml(param.categoryId)}" data-jump-key="${escapeHtml(param.key)}">
    <div class="config-row-title"><code>${escapeHtml(param.key)}</code><span class="tag">${escapeHtml(param.categoryLabel)}</span></div>
    <p class="config-row-desc">${escapeHtml(param.description || '')}</p>
  </button>`;
}

function renderServerConfigParams() {
  const sc = state.serverConfig;
  const search = sc.search.trim().toLocaleLowerCase('es');
  if (search && sc.searchIndex) {
    const results = sc.searchIndex.filter((param) => param.key.toLocaleLowerCase('es').includes(search)
      || param.description.toLocaleLowerCase('es').includes(search));
    $('#server-config-groups').innerHTML = results.map(serverConfigSearchResultHtml).join('');
    $('#server-config-empty').classList.toggle('hidden', results.length > 0);
    return;
  }
  const params = sc.params.filter((param) => !search
    || param.key.toLocaleLowerCase('es').includes(search)
    || (param.description || '').toLocaleLowerCase('es').includes(search));
  $('#server-config-groups').innerHTML = params.map(serverConfigRowHtml).join('');
  $('#server-config-empty').classList.toggle('hidden', params.length > 0);
}

function renderServerConfigRail() {
  const sc = state.serverConfig;
  const pendingByCategory = new Map();
  for (const change of sc.pending?.changes || []) {
    pendingByCategory.set(change.categoryId, (pendingByCategory.get(change.categoryId) || 0) + 1);
  }
  const button = (category) => {
    const pendingCount = pendingByCategory.get(category.id) || 0;
    const badge = pendingCount > 0 ? `<em class="has-pending">${pendingCount}</em>` : `<em>${category.count}</em>`;
    return `<button class="${category.id === sc.selectedCategoryId ? 'active' : ''}" data-config-category="${category.id}">${escapeHtml(category.label)} ${badge}</button>`;
  };
  $('#server-config-rail').innerHTML = `
    <p class="nav-section-label">Núcleo</p>
    ${sc.categories.filter((category) => category.group === 'core').map(button).join('')}
    <p class="nav-section-label">Módulos</p>
    ${sc.categories.filter((category) => category.group === 'module').map(button).join('')}
  `;
}

function renderServerConfigPending() {
  const { changes, apply } = state.serverConfig.pending || { changes: [], apply: null };
  const applying = apply && apply.status === 'pending';
  const hasChanges = changes.length > 0;
  $('#server-config-pending-bar').classList.toggle('pending-bar-active', hasChanges || applying);
  $('#server-config-discard').disabled = !hasChanges || applying;
  $('#server-config-save').disabled = !hasChanges || applying;
  $('#server-config-pending-chips').innerHTML = changes
    .map((change) => `<span class="tag pending">${escapeHtml(change.key)} → ${escapeHtml(change.value)}</span>`)
    .join('');
  if (applying) {
    $('#server-config-pending-title').textContent = 'Aplicando cambios y reiniciando el servidor…';
    $('#server-config-pending-detail').textContent = 'Puede tardar hasta un minuto: avisa a los jugadores conectados y guarda antes de reiniciar.';
  } else if (hasChanges) {
    $('#server-config-pending-title').textContent = `${changes.length} cambio${changes.length === 1 ? '' : 's'} pendiente${changes.length === 1 ? '' : 's'}`;
    $('#server-config-pending-detail').textContent = 'Se guardarán en el fichero de configuración y el servidor se reiniciará por completo (aviso de 60 s + guardado automático).';
  } else {
    $('#server-config-pending-title').textContent = 'Sin cambios pendientes';
    $('#server-config-pending-detail').textContent = 'Modifica un valor editable para habilitar el guardado y reinicio.';
  }
}

async function refreshServerConfigPending() {
  try {
    const data = await api('/api/server-config/pending');
    const previousStatus = state.serverConfig.pending?.apply?.status;
    state.serverConfig.pending = data;
    renderServerConfigPending();
    renderServerConfigRail();
    clearTimeout(state.serverConfig.pollTimer);
    if (data.apply?.status === 'pending') {
      state.serverConfig.pollTimer = setTimeout(refreshServerConfigPending, 3000);
    } else if (previousStatus === 'pending' && data.apply?.status) {
      showToast(data.apply.status === 'applied'
        ? 'Cambios aplicados: el servidor se ha reiniciado.'
        : `No se pudo aplicar la configuración: ${data.apply.error || 'error desconocido'}`);
      if (state.serverConfig.selectedCategoryId) loadServerConfigCategory(state.serverConfig.selectedCategoryId);
      $('#server-config-save').disabled = false;
    }
  } catch (error) {
    if (error.status === 401) showLogin();
  }
}

async function loadServerConfigCategory(id) {
  const requestId = serverConfigCategorySlot.begin();
  try {
    const data = await api(`/api/server-config/category/${id}`);
    if (!serverConfigCategorySlot.isCurrent(requestId)) return; // una petición más nueva ya ganó (B1)
    state.serverConfig.selectedCategoryId = id;
    state.serverConfig.category = data.category;
    state.serverConfig.params = data.params;
    $('#server-config-category-title').textContent = data.category.label;
    $('#server-config-category-subtitle').textContent = `${data.category.file} — ${data.params.length} parámetros`;
    renderServerConfigRail();
    renderServerConfigParams();
  } catch (error) {
    if (!serverConfigCategorySlot.isCurrent(requestId)) return;
    if (error.status === 401) return showLogin();
    showToast(error.message);
  }
}

async function loadServerConfigCategories() {
  try {
    const data = await api('/api/server-config/categories');
    state.serverConfig.categories = data.categories;
    if (!state.serverConfig.selectedCategoryId && data.categories.length) state.serverConfig.selectedCategoryId = data.categories[0].id;
    renderServerConfigRail();
    if (state.serverConfig.selectedCategoryId) await loadServerConfigCategory(state.serverConfig.selectedCategoryId);
    await refreshServerConfigPending();
    if (!state.serverConfig.searchIndex) loadServerConfigSearchIndex(); // una sola vez por sesión: catálogo estático
  } catch (error) {
    if (error.status === 401) return showLogin();
    if (error.status === 403) return showView('players');
    showToast(error.message);
  }
}

async function loadServerConfigSearchIndex() {
  try {
    const data = await api('/api/server-config/search-index');
    state.serverConfig.searchIndex = data.params;
    if (state.serverConfig.search.trim()) renderServerConfigParams();
  } catch (error) {
    if (error.status === 401) showLogin(); // sin toast: la búsqueda global es un extra, no bloquea la vista
  }
}

// Un resultado de búsqueda global lleva a su categoría y limpia la búsqueda;
// resaltar la fila exacta requiere esperar a que loadServerConfigCategory()
// pinte esa categoría (misma petición en vuelo que si se hiciera a mano
// desde el riel).
async function jumpToServerConfigResult(categoryId, key) {
  $('#server-config-search').value = '';
  state.serverConfig.search = '';
  await loadServerConfigCategory(categoryId);
  window.scrollTo(0, 0);
  const row = $(`#server-config-groups .config-row[data-config-key="${CSS.escape(key)}"]`);
  if (row) {
    row.scrollIntoView({ block: 'center' });
    row.classList.add('is-highlighted');
    setTimeout(() => row.classList.remove('is-highlighted'), 2000);
  }
}

async function stageServerConfigChange(key, value) {
  const file = state.serverConfig.category?.file;
  if (!file) return;
  try {
    await api('/api/server-config/stage', { method: 'POST', body: JSON.stringify({ key, file, value }) });
    const param = state.serverConfig.params.find((entry) => entry.key === key);
    if (param) param.pending = String(value);
    renderServerConfigParams();
    await refreshServerConfigPending();
  } catch (error) {
    if (error.status === 401) return showLogin();
    showToast(error.message);
    renderServerConfigParams();
  }
}

$('#server-config-rail').addEventListener('click', (event) => {
  const button = event.target.closest('[data-config-category]');
  if (!button) return;
  loadServerConfigCategory(button.dataset.configCategory);
  // La lista de categorías puede ser más larga que la pantalla: sin esto, al
  // elegir una de más abajo la vista se queda donde estaba el clic en vez de
  // subir a ver la cabecera de la categoría recién cargada.
  window.scrollTo(0, 0);
});
$('#server-config-search').addEventListener('input', (event) => {
  state.serverConfig.search = event.target.value;
  renderServerConfigParams();
});
$('#server-config-groups').addEventListener('click', (event) => {
  const jump = event.target.closest('[data-jump-category]');
  if (jump) return jumpToServerConfigResult(jump.dataset.jumpCategory, jump.dataset.jumpKey);
  const toggle = event.target.closest('.toggle[data-config-key]');
  if (!toggle) return;
  const on = toggle.classList.contains('is-on');
  stageServerConfigChange(toggle.dataset.configKey, on ? '0' : '1');
});
$('#server-config-groups').addEventListener('change', (event) => {
  const input = event.target.closest('.config-number[data-config-key]');
  if (input) stageServerConfigChange(input.dataset.configKey, input.value.trim());
});
$('#server-config-discard').addEventListener('click', async () => {
  try {
    await api('/api/server-config/discard', { method: 'POST', body: JSON.stringify({}) });
    await refreshServerConfigPending();
    if (state.serverConfig.selectedCategoryId) loadServerConfigCategory(state.serverConfig.selectedCategoryId);
  } catch (error) {
    if (error.status === 401) return showLogin();
    showToast(error.message);
  }
});
$('#server-config-save').addEventListener('click', async () => {
  const button = $('#server-config-save');
  button.disabled = true;
  try {
    await api('/api/server-config/save', { method: 'POST', body: JSON.stringify({}) });
    await refreshServerConfigPending();
  } catch (error) {
    if (error.status === 401) return showLogin();
    showToast(error.message);
    button.disabled = false;
  }
});
