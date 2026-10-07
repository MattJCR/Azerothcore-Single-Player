// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Vista "Comandos y ayuda": catálogo de comandos y artículos con filtro por
// categoría y buscador.
import { $, state, api, showLogin, showToast, escapeHtml } from '../shared.js';
import { t, lowerCase } from '../i18n.js';

export function enter() {
  loadHelp();
}

function helpEntryHtml(kind, entry) {
  if (kind === 'command') {
    const levelNames = [t('Jugador'), t('Moderador'), t('Game Master'), t('Administrador')];
    return `<details class="help-entry">
      <summary><span class="help-kind">.${escapeHtml(entry.path)}</span><strong>${escapeHtml(entry.title || entry.path)}</strong><span class="help-badges">${entry.isFamily ? `<em class="help-family">${t('Familia')}</em>` : ''}<em class="help-level help-level-${entry.minSecurity}">${levelNames[entry.minSecurity] || t('Nivel {level}', { level: entry.minSecurity })}</em></span></summary>
      <div class="help-entry-body">
        ${entry.description ? `<p>${escapeHtml(entry.description).replaceAll('\n', '<br>')}</p>` : ''}
        ${entry.syntax ? `<p class="help-label">${t('Uso')}</p><code>${escapeHtml(entry.syntax)}</code>` : ''}
        ${entry.examples ? `<p class="help-label">${t('Ejemplos')}</p><code>${escapeHtml(entry.examples).replaceAll('\n', '<br>')}</code>` : ''}
      </div>
    </details>`;
  }
  return `<details class="help-entry">
    <summary>${entry.isHot ? '<span class="help-hot">★</span>' : ''}<strong>${escapeHtml(entry.title)}</strong></summary>
    <div class="help-entry-body"><p>${escapeHtml(entry.body).replaceAll('\n', '<br>')}</p></div>
  </details>`;
}

function renderHelp() {
  if (!state.help) return;
  const search = lowerCase($('#help-search').value.trim());
  const categoryFilter = state.helpCategory;
  const matches = (text) => !search || lowerCase(text || '').includes(search);

  const searchedCommands = state.help.commands.filter((command) => matches(command.path) || matches(command.title) || matches(command.description) || matches(command.keywords) || matches(command.syntax) || matches(command.examples));
  const searchedArticles = state.help.articles.filter((article) => matches(article.title) || matches(article.body) || matches(article.keywords));

  // Barra siempre visible con el recuento por categoría, en vez de un
  // desplegable que esconde de un vistazo cuánto hay en cada una.
  $('#help-rail').innerHTML = [{ id: '', name: t('Todas') }, ...state.help.categories].map((category) => {
    const count = category.id === ''
      ? searchedCommands.length + searchedArticles.length
      : searchedCommands.filter((command) => command.categoryId === category.id).length
        + searchedArticles.filter((article) => article.categoryId === category.id).length;
    return `<button class="${String(categoryFilter) === String(category.id) ? 'active' : ''}" data-help-category="${category.id}">${escapeHtml(category.name)} <em>${count}</em></button>`;
  }).join('');

  const commands = searchedCommands.filter((command) => !categoryFilter || String(command.categoryId) === String(categoryFilter));
  const articles = searchedArticles.filter((article) => !categoryFilter || String(article.categoryId) === String(categoryFilter));

  const groups = state.help.categories
    .map((category) => ({
      category,
      articles: articles.filter((article) => article.categoryId === category.id),
      commands: commands.filter((command) => command.categoryId === category.id),
    }))
    .filter((group) => group.articles.length || group.commands.length);

  $('#help-list').innerHTML = groups.map((group) => `<div class="help-group">
    <h4>${escapeHtml(group.category.name)}</h4>
    ${group.articles.map((article) => helpEntryHtml('article', article)).join('')}
    ${group.commands.map((command) => helpEntryHtml('command', command)).join('')}
  </div>`).join('');
  $('#help-empty').classList.toggle('hidden', groups.length > 0);
}

async function loadHelp() {
  if (state.help) return renderHelp();
  try {
    state.help = await api('/api/help');
    renderHelp();
  } catch (error) {
    if (error.status === 401) return showLogin();
    showToast(error.message);
  }
}

$('#help-search').addEventListener('input', renderHelp);
$('#help-rail').addEventListener('click', (event) => {
  const button = event.target.closest('[data-help-category]');
  if (!button) return;
  state.helpCategory = button.dataset.helpCategory;
  renderHelp();
});
