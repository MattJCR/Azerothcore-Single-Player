// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Vista "Armería": ficha de personaje (equipo, bolsas, banco, hermandad).
// openArmory() también lo usan Jugadores/Moderación al abrir un personaje
// desde su fila o su ficha, por eso se exporta.
import { $, state, api, showLogin, showToast, showView, escapeHtml, icon, RACES, CLASSES, QUALITY_COLORS, itemIconHtml, armorySlot } from '../shared.js';
import { selectModCharacterByName } from './moderation.js';
import { focusCharacterOnMap } from './map.js';

const EQUIP_SLOT_LABELS = ['Cabeza', 'Cuello', 'Hombros', 'Camisa', 'Pecho', 'Cintura', 'Piernas', 'Pies', 'Muñecas', 'Manos', 'Anillo 1', 'Anillo 2', 'Abalorio 1', 'Abalorio 2', 'Espalda', 'Mano principal', 'Mano secundaria', 'A distancia', 'Sobreveste'];
const STAT_LABELS = {
  0: 'Maná', 1: 'Salud', 3: 'Agilidad', 4: 'Fuerza', 5: 'Intelecto', 6: 'Espíritu', 7: 'Aguante',
  12: 'Defensa', 13: 'Esquivar', 14: 'Parada', 15: 'Bloqueo',
  16: 'Golpe (cac)', 17: 'Golpe (distancia)', 18: 'Golpe (hechizos)',
  19: 'Crítico (cac)', 20: 'Crítico (distancia)', 21: 'Crítico (hechizos)',
  28: 'Premura (cac)', 29: 'Premura (distancia)', 30: 'Premura (hechizos)',
  31: 'Golpe', 32: 'Crítico', 35: 'Resiliencia', 36: 'Premura', 37: 'Pericia',
  38: 'Poder de ataque', 39: 'Poder de ataque a distancia',
  44: 'Curación', 45: 'Poder de hechizos', 48: 'Regeneración de maná', 49: 'Regeneración de maná', 50: 'Penetración de armadura',
};

export function enter() {
  if (!state.myCharacters) loadMyCharacters();
}

function armoryResultHtml(character) {
  const [race] = RACES[character.race] || [`Raza ${character.race}`];
  const [className, classColor] = CLASSES[character.class] || [`Clase ${character.class}`, '#9eb1a7'];
  return `<button class="armory-result" data-armory-guid="${character.guid}">
    <span class="armory-result-identity" style="--icon-color:${classColor}">
      ${icon('class', character.class, className)}
      <span><strong>${escapeHtml(character.name)}</strong><small>Nv. ${character.level} · ${escapeHtml(className)} · ${escapeHtml(race)}${character.guildName ? ` · &lt;${escapeHtml(character.guildName)}&gt;` : ''}</small></span>
    </span>
    <span class="armory-result-status${character.online ? ' online' : ''}">${character.online ? 'En línea' : 'Desconectado'}</span>
  </button>`;
}

async function searchArmory() {
  const query = $('#armory-search').value.trim();
  if (query.length < 2) {
    $('#armory-results').innerHTML = '';
    $('#armory-search-empty').classList.add('hidden');
    return;
  }
  try {
    const data = await api(`/api/characters/search?q=${encodeURIComponent(query)}`);
    $('#armory-results').innerHTML = data.characters.map(armoryResultHtml).join('');
    $('#armory-search-empty').classList.toggle('hidden', data.characters.length > 0);
  } catch (error) {
    if (error.status === 401) return showLogin();
    showToast(error.message);
  }
}

async function loadMyCharacters() {
  try {
    const data = await api('/api/account/characters');
    state.myCharacters = data.characters;
    $('#armory-my-characters').innerHTML = data.characters.length ? `<p class="armory-chips-label">Tus personajes</p>
      <div class="armory-chip-list">${data.characters.map((character) => `<button class="armory-chip" data-armory-guid="${character.guid}">${escapeHtml(character.name)}</button>`).join('')}</div>` : '';
  } catch (error) {
    if (error.status === 401) return showLogin();
    showToast(error.message);
  }
}

function itemLineHtml(item) {
  const color = QUALITY_COLORS[item.quality] ?? QUALITY_COLORS[1];
  const statLine = item.stats.map((stat) => `${STAT_LABELS[stat.type] || `Estadística ${stat.type}`} +${stat.value}`).join(' · ');
  const weaponLine = item.weapon ? `${item.weapon.dmgMin}-${item.weapon.dmgMax} daño · ${(item.weapon.delay / 1000).toFixed(1)}s · ${item.weapon.dps} DPS` : '';
  const bits = [
    item.ilvl ? `Nivel objeto ${item.ilvl}` : '',
    item.requiredLevel > 1 ? `Requiere nivel ${item.requiredLevel}` : '',
    item.armor ? `${item.armor} de armadura` : '',
    weaponLine, statLine,
  ].filter(Boolean).join(' · ');
  return `${itemIconHtml(item)}<span class="item-copy"><span class="item-name" style="color:${color}">${escapeHtml(item.name)}</span>${bits ? `<span class="item-detail">${escapeHtml(bits)}</span>` : ''}</span>`;
}

function renderEquippedList(equipped) {
  if (!equipped.length) return '<p class="armory-empty-note">Sin objetos equipados.</p>';
  return `<ul class="item-list">${equipped.map((entry) => `<li class="item-row">
    <span class="item-slot">${escapeHtml(EQUIP_SLOT_LABELS[entry.slot] || `Ranura ${entry.slot}`)}</span>${itemLineHtml(entry.item)}
  </li>`).join('')}</ul>`;
}

function renderBagSection(section) {
  if (!section.loose.length && !section.containers.length) return '<p class="armory-empty-note">Vacío.</p>';
  const looseHtml = section.loose.length ? `<ul class="item-list">${section.loose.map((item) => `<li class="item-row item-row-plain">${itemLineHtml(item)}</li>`).join('')}</ul>` : '';
  const containersHtml = section.containers.map(({ container, items }) => `<div class="item-container">
    <div class="item-container-title">${itemIconHtml(container)}<span>${escapeHtml(container.name)}</span></div>
    ${items.length ? `<ul class="item-list">${items.map((item) => `<li class="item-row item-row-plain">${itemLineHtml(item)}</li>`).join('')}</ul>` : '<p class="armory-empty-note">Vacía.</p>'}
  </div>`).join('');
  return looseHtml + containersHtml;
}

function renderGuildBank(guildBank) {
  if (!guildBank) return '<p class="armory-empty-note">Este personaje no tiene hermandad.</p>';
  return guildBank.tabs.map((tab) => `<div class="item-container">
    <p class="item-container-title">${escapeHtml(tab.tabName)}${!tab.allowed ? ' <span class="tab-locked">(sin permiso de tu rango)</span>' : ''}</p>
    ${tab.allowed ? (tab.items.length ? `<ul class="item-list">${tab.items.map((item) => `<li class="item-row item-row-plain">${itemLineHtml(item)}</li>`).join('')}</ul>` : '<p class="armory-empty-note">Vacía.</p>') : ''}
  </div>`).join('');
}

function renderArmoryDetail() {
  const data = state.currentArmory;
  if (!data) return;
  const { character } = data;
  const [race] = RACES[character.race] || [`Raza ${character.race}`];
  const [className, classColor] = CLASSES[character.class] || [`Clase ${character.class}`, '#9eb1a7'];
  $('#armory-header').innerHTML = `<div class="armory-header-identity" style="--icon-color:${classColor}">
    ${icon('class', character.class, className)}
    <div><h3>${escapeHtml(character.name)}</h3><p>Nivel ${character.level} · ${escapeHtml(className)} · ${escapeHtml(race)}${character.guildName ? ` · &lt;${escapeHtml(character.guildName)}&gt;` : ''} · ${character.online ? 'En línea' : 'Desconectado'}</p></div>
  </div>`;

  const gmLinks = $('#armory-gm-links');
  gmLinks.classList.toggle('hidden', !state.user?.isGm);
  if (state.user?.isGm) {
    gmLinks.innerHTML = `<button type="button" data-armory-goto="moderation">⚑ Moderar este personaje</button>
      <button type="button" data-armory-goto="map">⌖ Ver en el mapa</button>`;
  }

  const tabs = [{ key: 'equipped', label: 'Equipo' }];
  if (character.ownedByMe) tabs.push({ key: 'bags', label: 'Bolsas' }, { key: 'bank', label: 'Banco' }, { key: 'guild', label: 'Hermandad' });
  $('#armory-tabs').innerHTML = tabs.map((tab) => `<button class="${tab.key === state.armoryTab ? 'active' : ''}" data-armory-tab="${tab.key}">${tab.label}</button>`).join('');

  if (state.armoryTab === 'bags' && data.bags) $('#armory-body').innerHTML = renderBagSection(data.bags);
  else if (state.armoryTab === 'bank' && data.bank) $('#armory-body').innerHTML = renderBagSection(data.bank);
  else if (state.armoryTab === 'guild' && character.ownedByMe) $('#armory-body').innerHTML = renderGuildBank(data.guildBank);
  else $('#armory-body').innerHTML = renderEquippedList(data.equipped);
}

export async function openArmory(guid) {
  const requestId = armorySlot.begin();
  try {
    const data = await api(`/api/armory/${guid}`);
    if (!armorySlot.isCurrent(requestId)) return; // una petición más nueva ya ganó (B1)
    state.currentArmory = data;
    state.armoryTab = 'equipped';
    renderArmoryDetail();
    $('#armory-detail').classList.remove('hidden');
    $('#armory-detail').scrollIntoView({ behavior: 'smooth', block: 'start' });
  } catch (error) {
    if (!armorySlot.isCurrent(requestId)) return;
    if (error.status === 401) return showLogin();
    if (error.status === 404) return showToast('Ese personaje no existe.');
    showToast(error.message);
  }
}

$('#armory-search').addEventListener('input', searchArmory);
$('#armory-results').addEventListener('click', (event) => { const button = event.target.closest('[data-armory-guid]'); if (button) openArmory(button.dataset.armoryGuid); });
$('#armory-my-characters').addEventListener('click', (event) => { const button = event.target.closest('[data-armory-guid]'); if (button) openArmory(button.dataset.armoryGuid); });
$('#armory-tabs').addEventListener('click', (event) => {
  const button = event.target.closest('[data-armory-tab]');
  if (!button) return;
  state.armoryTab = button.dataset.armoryTab;
  renderArmoryDetail();
});
$('#armory-back').addEventListener('click', () => $('#armory-detail').classList.add('hidden'));
$('#armory-gm-links').addEventListener('click', (event) => {
  const button = event.target.closest('[data-armory-goto]');
  if (!button || !state.currentArmory) return;
  const { character } = state.currentArmory;
  if (button.dataset.armoryGoto === 'moderation') { showView('moderation'); selectModCharacterByName(character.name); }
  else if (button.dataset.armoryGoto === 'map') focusCharacterOnMap({ guid: character.guid, name: character.name, sourceLabel: 'Armería' });
});
