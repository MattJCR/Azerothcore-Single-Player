// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Vista "Mapa" (social): mismo visor que el mapa GM, pero sólo con personajes
// que tengan un vínculo social con uno de tus personajes. El punto lleva el
// color de la clase y el aro exterior el color de la relación (grupo azul,
// banda naranja, hermandad verde, amigos dorado); tú apareces con el aro
// blanco, como en el mapa GM.
import { $, state, CLASSES, escapeHtml, api, showLogin, showToast, sizeMapView, createMapController, socialMapSlot } from '../shared.js';

const RELATION_COLORS = { self: '#ffffff', party: '#3f9bff', raid: '#ff8a3d', guild: '#5fce8f', friend: '#f2c94c' };
const RELATION_LABELS = { self: 'Tú', party: 'Tu grupo', raid: 'Tu banda', guild: 'Tu hermandad', friend: 'Amigo' };
const RELATION_ORDER = ['party', 'raid', 'guild', 'friend'];

const socialMap = createMapController({
  viewport: $('#social-world-map'), stage: $('#social-map-stage'), image: $('#social-map-image'), markers: $('#social-map-markers'),
  tabs: $('#social-map-tabs'), zoomIn: $('#social-zoom-in'), zoomOut: $('#social-zoom-out'), zoomReset: $('#social-zoom-reset'),
  zoomLevel: $('#social-zoom-level'), summary: $('#social-map-summary'), instances: $('#social-instance-players'),
}, {
  instancesLabel: 'De tu círculo, en instancias u otros mapas',
  buildMarker(player) {
    const [className, classColor] = CLASSES[player.class] || [`Clase ${player.class}`, '#d9ae62'];
    const relation = player.relation || 'guild';
    const relationColor = RELATION_COLORS[relation] || RELATION_COLORS.guild;
    const relationLabel = RELATION_LABELS[relation] || '';
    const inside = relation === 'self'
      ? `<span class="marker-label">${escapeHtml(player.name)}<small>${relationLabel}</small></span>`
      : `<span class="marker-tip"><strong>${escapeHtml(player.name)}</strong> · Nv. ${player.level}<br><small>${escapeHtml(className)} · ${escapeHtml(relationLabel)}</small></span>`;
    return `<button class="marker${relation === 'self' ? ' you' : ''}" data-map-x="${player.mapX}" data-map-y="${player.mapY}" data-relation="${relation}" style="--class-color:${classColor};--relation-color:${relationColor}" aria-label="${escapeHtml(player.name)} · ${escapeHtml(relationLabel)}">${inside}</button>`;
  },
});

export function fit() {
  socialMap.fit();
}

export function enter() {
  load(state.socialMap.character);
  sizeMapView($('#social-map-view'));
  requestAnimationFrame(() => socialMap.fit());
}

function renderSocialCharacterPicker(data) {
  const select = $('#social-character');
  const signature = data.characters.map((character) => `${character.guid}:${character.online ? 1 : 0}`).join(',');
  if (select.dataset.signature !== signature) {
    select.dataset.signature = signature;
    select.innerHTML = data.characters.map((character) => {
      const [className] = CLASSES[character.class] || [`Clase ${character.class}`];
      return `<option value="${character.guid}">${escapeHtml(character.name)} — Nv. ${character.level} ${escapeHtml(className)}${character.online ? ' · conectado' : ''}</option>`;
    }).join('');
  }
  const selected = state.socialMap.character || data.focus?.guid;
  if (selected) select.value = String(selected);
}

function renderSocialLegend(data) {
  const counts = {};
  for (const player of data.players) counts[player.relation] = (counts[player.relation] || 0) + 1;
  $('#social-legend').innerHTML = RELATION_ORDER
    .map((relation) => `<span class="social-legend-item"><i style="--relation-color:${RELATION_COLORS[relation]}"></i>${RELATION_LABELS[relation]} <em>${counts[relation] || 0}</em></span>`)
    .join('');
}

function renderSocialMap() {
  const data = state.socialMap.data;
  if (!data) return;
  const hasCharacters = data.characters.length > 0;
  const hasFocus = Boolean(data.focus);
  $('#social-no-characters').classList.toggle('hidden', hasCharacters);
  $('#social-picker-row').classList.toggle('hidden', !hasCharacters);
  $('#social-map-panel').classList.toggle('hidden', !hasFocus);
  if (hasCharacters) renderSocialCharacterPicker(data);

  // Ningún personaje conectado y ninguno elegido aún: se carga el primero de la
  // lista para que el mapa aparezca con su hermandad y amigos conectados. La
  // hermandad y los amigos no dependen de que tú estés en línea; el grupo sí.
  if (hasCharacters && !hasFocus) {
    $('#social-no-focus').classList.remove('hidden');
    if (!state.socialMap.character) {
      state.socialMap.character = data.characters[0].guid;
      load(state.socialMap.character);
    }
    return;
  }
  $('#social-no-focus').classList.add('hidden');
  if (!hasFocus) return;

  renderSocialLegend(data);
  socialMap.render({ players: data.players, updatedAt: data.updatedAt });
  const others = data.players.filter((player) => player.relation !== 'self').length;
  const focusOffline = data.focus && !data.players.some((player) => player.relation === 'self');
  $('#social-map-caption').textContent = others === 0
    ? `Ahora mismo no hay nadie de tu grupo, banda, hermandad ni amigos conectado${focusOffline ? ' (y este personaje tampoco lo está)' : ''}.`
    : `${others} de tu círculo ${others === 1 ? 'conectado' : 'conectados'}${focusOffline ? ' · este personaje no está conectado' : ''}.`;
}

export async function load(characterGuid) {
  const requestId = socialMapSlot.begin();
  try {
    const query = characterGuid ? `?character=${encodeURIComponent(characterGuid)}` : '';
    const data = await api(`/api/social/map${query}`);
    if (!socialMapSlot.isCurrent(requestId)) return; // una petición más nueva ya ganó (B1)
    state.socialMap.data = data;
    if (data.focus) state.socialMap.character = data.focus.guid;
    renderSocialMap();
  } catch (error) {
    if (!socialMapSlot.isCurrent(requestId)) return;
    if (error.status === 401) return showLogin();
    showToast(error.message);
  }
}

$('#social-character').addEventListener('change', (event) => {
  state.socialMap.character = Number(event.target.value);
  load(state.socialMap.character);
});
