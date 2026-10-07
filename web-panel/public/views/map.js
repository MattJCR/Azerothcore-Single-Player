// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Vista "Mapa del mundo" (GM): posiciones de todo el reino. focusCharacterOnMap
// también lo usan Jugadores/Armería/Moderación ("Ver en el mapa"), por eso se
// exporta en vez de quedar privado a esta vista.
import { $, state, api, CLASSES, escapeHtml, showView, showLogin, showToast, demoteFromGm, sizeMapView, createMapController } from '../shared.js';

const gmMap = createMapController({
  viewport: $('#world-map'), stage: $('#map-stage'), image: $('#map-image'), markers: $('#map-markers'),
  tabs: $('#map-tabs'), zoomIn: $('#zoom-in'), zoomOut: $('#zoom-out'), zoomReset: $('#zoom-reset'),
  zoomLevel: $('#zoom-level'), summary: $('#map-summary'), instances: $('#instance-players'),
}, {
  beforeRender(projected, available) {
    if (state.mapFocus && !state.mapFocus.jumped) {
      const focusPlayer = projected.find((player) => Number(player.guid) === state.mapFocus.guid);
      if (focusPlayer?.onMap && available.includes(focusPlayer.displayMap)) gmMap.mapId = focusPlayer.displayMap;
      state.mapFocus.jumped = true;
    }
  },
  buildMarker(player) {
    const [className, classColor] = CLASSES[player.class] || [`Clase ${player.class}`, '#d9ae62'];
    const isFocus = state.mapFocus && Number(player.guid) === state.mapFocus.guid;
    const label = isFocus
      ? `<span class="marker-label">${escapeHtml(player.name)}<small>Nv. ${player.level} · ${escapeHtml(className)}</small></span>`
      : `<span class="marker-tip"><strong>${escapeHtml(player.name)}</strong> · Nv. ${player.level}<br><small>${escapeHtml(className)} · ${player.x.toFixed(0)}, ${player.y.toFixed(0)}</small></span>`;
    return `<button class="marker${isFocus ? ' you' : ''}" data-map-x="${player.mapX}" data-map-y="${player.mapY}" style="--class-color:${classColor}" aria-label="${escapeHtml(player.name)}">${label}</button>`;
  },
  afterRender(projected, visible, { centerOnMarker }) {
    if (state.mapFocus && !state.mapFocus.centered) {
      const focusPlayer = visible.find((player) => Number(player.guid) === state.mapFocus.guid);
      if (focusPlayer) {
        state.mapFocus.centered = true;
        centerOnMarker(focusPlayer.mapX, focusPlayer.mapY);
      }
    }
    updateMapContextBanner(projected);
  },
});

export function render(updatedAt) {
  gmMap.render({ players: state.mapPlayers, updatedAt });
}

// antes esta vista se limitaba a pintar lo que shared.refresh()
// ya había pedido en cada sondeo, estuviera abierta o no; ahora pide sus
// propios datos, sólo mientras está abierta, igual que social-map/bot-ops.
export async function load() {
  try {
    const mapData = await api('/api/map/players');
    state.mapPlayers = mapData.players;
    render(mapData.updatedAt);
  } catch (error) {
    if (error.status === 401) return showLogin();
    if (error.status === 403) return demoteFromGm();
    showToast(error.message);
  }
}

export function fit() {
  gmMap.fit();
}

export function enter() {
  sizeMapView($('#map-view'));
  requestAnimationFrame(() => gmMap.fit());
  load();
}

// Personaje resaltado al llegar desde Jugadores/Armería/Moderación con "Ver
// en el mapa": salta a su continente, lo centra una vez y mantiene su
// etiqueta siempre visible (no sólo al pasar el ratón, como el resto).
export function focusCharacterOnMap({ guid, name, sourceLabel }) {
  state.mapFocus = { guid: Number(guid), name, sourceLabel, jumped: false, centered: false };
  if ($('#map-view').classList.contains('hidden')) showView('map');
  // Se difiere un frame: justo tras quitar "hidden" el contenedor del mapa
  // aún puede medir 0 de ancho/alto, igual que ya hace showView con gmMap.fit().
  requestAnimationFrame(() => render(new Date().toISOString()));
}

function updateMapContextBanner(projectedPlayers) {
  const banner = $('#map-context-banner');
  const wasHidden = banner.classList.contains('hidden');
  if (!state.mapFocus) {
    banner.classList.add('hidden');
  } else {
    const player = projectedPlayers.find((candidate) => Number(candidate.guid) === state.mapFocus.guid);
    const sourceText = state.mapFocus.sourceLabel ? ` · llegaste desde ${escapeHtml(state.mapFocus.sourceLabel)}` : '';
    $('#map-context-text').innerHTML = player?.onMap
      ? `Mostrando la posición de <strong>${escapeHtml(state.mapFocus.name)}</strong>${sourceText}`
      : `<strong>${escapeHtml(state.mapFocus.name)}</strong> no está en el mapa ahora mismo${sourceText}`;
    banner.classList.remove('hidden');
  }
  // El aviso ocupa parte del alto fijo de #map-view (sizeMapView()): al
  // aparecer o desaparecer, el mapa cambia de tamaño y hay que reencuadrar.
  if (wasHidden !== banner.classList.contains('hidden')) requestAnimationFrame(() => gmMap.fit());
}

document.addEventListener('click', (event) => {
  if (!event.target.closest('.map-goto')) $('#map-goto-results')?.classList.add('hidden');
});
$('#map-goto-input').addEventListener('input', () => {
  const query = $('#map-goto-input').value.trim().toLocaleLowerCase('es');
  const results = $('#map-goto-results');
  if (!query) { results.innerHTML = ''; results.classList.add('hidden'); return; }
  const matches = state.mapPlayers.filter((player) => player.name.toLocaleLowerCase('es').includes(query)).slice(0, 8);
  results.innerHTML = matches.length
    ? matches.map((player) => `<button type="button" data-map-goto-guid="${player.guid}">${escapeHtml(player.name)} <small>Nv. ${player.level}</small></button>`).join('')
    : '<button type="button" disabled>Sin coincidencias</button>';
  results.classList.remove('hidden');
});
$('#map-goto-results').addEventListener('click', (event) => {
  const button = event.target.closest('[data-map-goto-guid]');
  if (!button) return;
  const player = state.mapPlayers.find((candidate) => String(candidate.guid) === button.dataset.mapGotoGuid);
  $('#map-goto-input').value = '';
  $('#map-goto-results').classList.add('hidden');
  if (player) focusCharacterOnMap({ guid: player.guid, name: player.name, sourceLabel: null });
});
$('#map-context-clear').addEventListener('click', () => {
  state.mapFocus = null;
  render(new Date().toISOString());
});
