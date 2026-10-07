// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Vista "Jugadores conectados": tabla filtrable/ordenable y su menú de
// acciones rápidas por fila. Sin enter()/exit(): ya la mantiene al día el
// sondeo general de refresh() (shared.js), como en el app.js original.
import { $, $$, state, RACES, CLASSES, icon, escapeHtml, showView } from '../shared.js';
import { t, lowerCase, compareText, formatTime } from '../i18n.js';
import { openArmory } from './armory.js';
import { selectModCharacterByName } from './moderation.js';
import { focusCharacterOnMap } from './map.js';

const ROLES = { tank: t('Tanque'), healer: t('Sanador'), dps: t('DPS') };

export function render(updatedAt) {
  closeRowMenu();
  const search = lowerCase($('#player-search').value.trim());
  const classFilter = $('#class-filter').value;
  const typeFilter = $('#type-filter').value;
  const filtered = state.players.filter((player) => {
    return (!search || lowerCase(player.name).includes(search))
      && (!classFilter || String(player.class) === classFilter)
      && (!typeFilter || player.type === typeFilter);
  }).sort((a, b) => {
    let left = a[state.sort.key]; let right = b[state.sort.key];
    if (state.sort.key === 'race') { left = RACES[a.race]?.[0] || ''; right = RACES[b.race]?.[0] || ''; }
    if (state.sort.key === 'class') { left = CLASSES[a.class]?.[0] || ''; right = CLASSES[b.class]?.[0] || ''; }
    if (typeof left === 'string') return compareText(left, right) * state.sort.direction;
    return (left - right) * state.sort.direction;
  });

  $('#players-body').innerHTML = filtered.map((player) => {
    const [race] = RACES[player.race] || [t('Raza {id}', { id: player.race })];
    const [className, classColor] = CLASSES[player.class] || [t('Clase {id}', { id: player.class }), '#9eb1a7'];
    const actionsCell = state.user?.isGm
      ? `<td class="actions-cell"><button class="row-menu-btn" data-row-menu="${player.guid}" aria-label="${escapeHtml(t('Acciones de {name}', { name: player.name }))}">⋮</button></td>`
      : '';
    return `<tr>
      <td><div class="player-cell"><i class="player-dot"></i><button class="link-button" data-armory-guid="${player.guid}">${escapeHtml(player.name)}</button></div></td>
      <td><div class="identity-cell">${icon('race', player.race, race)}<span>${escapeHtml(race)}</span></div></td>
      <td><div class="identity-cell" style="--icon-color:${classColor}">${icon('class', player.class, className)}<span>${escapeHtml(className)}</span></div></td>
      <td><div class="role-cell"><span class="role-badge">${icon('role', player.role, ROLES[player.role])}${ROLES[player.role]}</span></div></td>
      <td><span class="level-badge">${player.level}</span></td>
      <td><span class="type-badge ${player.type}">${player.type === 'bot' ? t('Bot') : t('Jugador')}</span></td>
      ${actionsCell}
    </tr>`;
  }).join('');
  $('#empty-state').classList.toggle('hidden', filtered.length > 0);

  // aria-sort vive en el <th>, no en el botón que dispara el
  // orden (así lo pide el patrón ARIA de cabeceras de tabla ordenables).
  $$('.th-sort').forEach((button) => {
    const isActive = button.dataset.sort === state.sort.key;
    button.closest('th').setAttribute('aria-sort', !isActive ? 'none' : state.sort.direction === 1 ? 'ascending' : 'descending');
  });

  const alliance = state.players.filter((player) => RACES[player.race]?.[1] === 'Alliance').length;
  const horde = state.players.filter((player) => RACES[player.race]?.[1] === 'Horde').length;
  const humans = state.players.filter((player) => player.type === 'player').length;
  const bots = state.players.filter((player) => player.type === 'bot').length;
  const average = state.players.length ? Math.round(state.players.reduce((sum, player) => sum + player.level, 0) / state.players.length) : 0;
  $('#online-count').textContent = state.players.length;
  $('#average-level').textContent = average || '—';
  $('#faction-count').textContent = `${alliance} / ${horde}`;
  $('#type-count').textContent = `${humans} / ${bots}`;
  $('#updated-time').textContent = formatTime(updatedAt, { hour: '2-digit', minute: '2-digit', second: '2-digit' });
}

// Menú de acciones rápidas por fila (sólo GM): un único elemento flotante
// reutilizado y reposicionado, en vez de un desplegable por fila, para que no
// quede atrapado por el overflow del contenedor de la tabla.
export function closeRowMenu() {
  $('#players-row-menu').classList.add('hidden');
  $$('.row-menu-btn.is-open').forEach((button) => button.classList.remove('is-open'));
}

export function openRowMenu(button, player) {
  const wasOpenForThisRow = button.classList.contains('is-open');
  closeRowMenu();
  if (wasOpenForThisRow) return;
  button.classList.add('is-open');
  const menu = $('#players-row-menu');
  menu.innerHTML = `<p class="row-menu-label">${escapeHtml(player.name)} · ${t('Nv. {level}', { level: player.level })}</p>
    <button class="row-menu-action" data-row-action="armory"><span class="menu-icon">⛨</span>${t('Ver en armería')}</button>
    <button class="row-menu-action" data-row-action="moderation"><span class="menu-icon">⚑</span>${t('Moderar personaje')}</button>
    <button class="row-menu-action" data-row-action="map"><span class="menu-icon">⌖</span>${t('Ver en el mapa')}</button>`;
  menu.dataset.guid = player.guid;
  menu.dataset.name = player.name;
  menu.classList.remove('hidden');
  const rect = button.getBoundingClientRect();
  menu.style.top = `${Math.min(rect.bottom + 6, window.innerHeight - 160)}px`;
  menu.style.left = `${Math.max(8, Math.min(rect.right - 206, window.innerWidth - 216))}px`;
}

$('#player-search').addEventListener('input', () => render(new Date().toISOString()));
$('#class-filter').addEventListener('change', () => render(new Date().toISOString()));
$('#type-filter').addEventListener('change', () => render(new Date().toISOString()));
// Botones en vez de manejador de clic en el <th>: un <th> no es interactivo
// por teclado por defecto y un <button> ya lo es, sin añadir
// tabindex ni un keydown propio.
$$('.th-sort').forEach((button) => button.addEventListener('click', () => {
  const key = button.dataset.sort;
  state.sort.direction = state.sort.key === key ? state.sort.direction * -1 : 1;
  state.sort.key = key;
  render(new Date().toISOString());
}));

$('#players-body').addEventListener('click', (event) => {
  const armoryButton = event.target.closest('[data-armory-guid]');
  if (armoryButton) { showView('armory'); openArmory(armoryButton.dataset.armoryGuid); return; }
  const menuButton = event.target.closest('[data-row-menu]');
  if (!menuButton) return;
  const player = state.players.find((candidate) => String(candidate.guid) === menuButton.dataset.rowMenu);
  if (player) openRowMenu(menuButton, player);
});
$('#players-row-menu').addEventListener('click', (event) => {
  const action = event.target.closest('[data-row-action]');
  if (!action) return;
  const menu = $('#players-row-menu');
  const guid = Number(menu.dataset.guid);
  const name = menu.dataset.name;
  closeRowMenu();
  if (action.dataset.rowAction === 'armory') { showView('armory'); openArmory(guid); }
  else if (action.dataset.rowAction === 'moderation') { showView('moderation'); selectModCharacterByName(name); }
  else if (action.dataset.rowAction === 'map') { focusCharacterOnMap({ guid, name, sourceLabel: t('Jugadores') }); }
});
document.addEventListener('click', (event) => {
  if (!event.target.closest('.row-menu, .row-menu-btn')) closeRowMenu();
});

for (const [id, [name]] of Object.entries(CLASSES)) $('#class-filter').insertAdjacentHTML('beforeend', `<option value="${id}">${name}</option>`);
