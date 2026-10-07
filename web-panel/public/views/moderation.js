// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Vista "Moderación": sanciones, buscador de personaje con las 8 acciones de
// GM, objetos a enviar, e invitaciones/cuentas admin (rango 3).
// selectModCharacterByName también lo usan Jugadores/Armería ("Moderar
// personaje"), por eso se exporta.
import { $, $$, state, api, showLogin, showToast, showView, formValues, escapeHtml, icon, RACES, CLASSES, QUALITY_COLORS } from '../shared.js';
import { t, formatDateTime, lowerCase } from '../i18n.js';
import { openArmory } from './armory.js';
import { focusCharacterOnMap } from './map.js';

export function enter() {
  if (!state.sanctions) loadSanctions();
  if (state.user?.gmlevel >= 3 && !state.invites) loadInvites();
}

function formatUnixSeconds(seconds) {
  return seconds ? formatDateTime(seconds * 1000) : '—';
}

function sanctionRowHtml(kind, row) {
  const who = row.username || row.name;
  const until = row.permanent ? t('Permanente') : t('hasta {date}', { date: formatUnixSeconds(row.unbanDate) });
  return `<div class="sanction-row">
    <span class="sanction-who">${escapeHtml(who)}</span>
    <span class="sanction-detail">${escapeHtml(row.reason || '')} · ${escapeHtml(row.bannedBy || '')} · ${until}</span>
    <button class="secondary-button" data-unban="${kind}:${escapeHtml(who)}">${t('Levantar')}</button>
  </div>`;
}

function muteRowHtml(row) {
  const status = row.pending ? t('pendiente al próximo inicio de sesión') : t('hasta {date}', { date: formatUnixSeconds(row.muteUntil) });
  return `<div class="sanction-row">
    <span class="sanction-who">${escapeHtml(row.username)}</span>
    <span class="sanction-detail">${escapeHtml(row.reason || '')} · ${escapeHtml(row.mutedBy || '')} · ${status}</span>
    <button class="secondary-button" data-unmute="${escapeHtml(row.username)}">${t('Quitar silencio')}</button>
  </div>`;
}

function renderSanctions() {
  if (!state.sanctions) return;
  const { accountBans, characterBans, muted } = state.sanctions;
  const total = accountBans.length + characterBans.length + muted.length;
  $('#sanctions-summary').textContent = total ? t('{count} activas', { count: total }) : t('Sin sanciones activas');
  $('#sanctions-list').innerHTML = [
    ...accountBans.map((row) => sanctionRowHtml('account', row)),
    ...characterBans.map((row) => sanctionRowHtml('character', row)),
    ...muted.map(muteRowHtml),
  ].join('');
  if (state.modCharacter) renderModCharacterCard();
}

async function loadSanctions() {
  try {
    state.sanctions = await api('/api/moderation/sanctions');
    renderSanctions();
  } catch (error) {
    if (error.status === 401) return showLogin();
    if (error.status === 403) return; // el gmlevel bajó entre medias; el nav ya lo oculta
    showToast(error.message);
  }
}

// Punto único para las 8 acciones de moderación: envía, avisa por toast con
// la respuesta real del worldserver, refresca la lista de sanciones y
// reactiva el formulario pase lo que pase.
async function moderationSubmit(path, body, { formSelector, refreshSanctions = true, onSuccess } = {}) {
  const form = formSelector ? $(formSelector) : null;
  const button = form?.querySelector('button[type="submit"]');
  if (button) button.disabled = true;
  try {
    const data = await api(path, { method: 'POST', body: JSON.stringify(body) });
    showToast(data.result || t('Hecho.'));
    if (form) form.reset();
    // form.reset() también vacía el campo oculto characterName/name: hay que
    // devolverle el personaje seleccionado o el siguiente envío iría vacío.
    if (form?.closest('#mod-tab-character')) applyModCharacterToForms();
    if (onSuccess) onSuccess(data);
    if (refreshSanctions) loadSanctions();
  } catch (error) {
    if (error.status === 401) return showLogin();
    showToast(error.message);
  } finally {
    if (button) button.disabled = false;
  }
}

// ── Moderación: buscador de personaje ───────────────────────────────────
// Todas las acciones de la pestaña "Buscar personaje" operan sobre
// state.modCharacter en vez de repetir un campo de nombre en cada formulario.
function modCharResultHtml(character) {
  const [race] = RACES[character.race] || [t('Raza {id}', { id: character.race })];
  const [className, classColor] = CLASSES[character.class] || [t('Clase {id}', { id: character.class }), '#9eb1a7'];
  return `<button class="armory-result" data-mod-guid="${character.guid}">
    <span class="armory-result-identity" style="--icon-color:${classColor}">
      ${icon('class', character.class, className)}
      <span><strong>${escapeHtml(character.name)}</strong><small>${t('Nv. {level}', { level: character.level })} · ${escapeHtml(className)} · ${escapeHtml(race)}${character.guildName ? ` · &lt;${escapeHtml(character.guildName)}&gt;` : ''}</small></span>
    </span>
    <span class="armory-result-status${character.online ? ' online' : ''}">${character.online ? t('En línea') : t('Desconectado')}</span>
  </button>`;
}

async function searchModCharacters() {
  const query = $('#mod-char-search').value.trim();
  if (query.length < 2) {
    state.modSearchResults = [];
    $('#mod-char-results').innerHTML = '';
    return;
  }
  try {
    const data = await api(`/api/characters/search?q=${encodeURIComponent(query)}`);
    state.modSearchResults = data.characters;
    $('#mod-char-results').innerHTML = data.characters.map(modCharResultHtml).join('');
  } catch (error) {
    if (error.status === 401) return showLogin();
    showToast(error.message);
  }
}

function applyModCharacterToForms() {
  const name = state.modCharacter?.name || '';
  $$('#mod-tab-character .mod-char-field').forEach((input) => { input.value = name; });
  $$('#mod-tab-character .mod-char-chip').forEach((chip) => { chip.textContent = name ? `⚑ ${name}` : ''; });
}

function renderModCharacterCard() {
  const character = state.modCharacter;
  if (!character) return;
  const [race] = RACES[character.race] || [t('Raza {id}', { id: character.race })];
  const [className, classColor] = CLASSES[character.class] || [t('Clase {id}', { id: character.class }), '#9eb1a7'];
  const mute = character.accountUsername && state.sanctions
    ? state.sanctions.muted.find((row) => row.username === character.accountUsername) : null;
  const charBan = state.sanctions?.characterBans.find((row) => row.guid === character.guid);
  const acctBan = character.accountUsername && state.sanctions
    ? state.sanctions.accountBans.find((row) => row.username === character.accountUsername) : null;
  const tags = [`<span class="tag ${character.online ? 'online' : 'offline'}">${character.online ? t('En línea') : t('Desconectado')}</span>`];
  if (mute) tags.push(`<span class="tag warn">${mute.pending ? t('Silenciado al próximo inicio de sesión') : t('Silenciado hasta {date}', { date: formatUnixSeconds(mute.muteUntil) })}</span>`);
  if (charBan) tags.push(`<span class="tag warn">${charBan.permanent ? t('Personaje baneado (permanente)') : t('Personaje baneado hasta {date}', { date: formatUnixSeconds(charBan.unbanDate) })}</span>`);
  if (acctBan) tags.push(`<span class="tag warn">${acctBan.permanent ? t('Cuenta baneada (permanente)') : t('Cuenta baneada hasta {date}', { date: formatUnixSeconds(acctBan.unbanDate) })}</span>`);
  $('#mod-char-card').innerHTML = `<div class="char-card" style="--icon-color:${classColor}">
    ${icon('class', character.class, className)}
    <div class="char-card-copy">
      <h3>${escapeHtml(character.name)}</h3>
      <p>${t('Nivel {level}', { level: character.level })} · ${escapeHtml(className)} · ${escapeHtml(race)}${character.guildName ? ` · &lt;${escapeHtml(character.guildName)}&gt;` : ''}</p>
      <div class="char-card-tags">${tags.join('')}</div>
      <div class="char-card-links">
        <button type="button" data-mod-goto="armory">${t('Ver en armería →')}</button>
        <button type="button" data-mod-goto="map">${t('Ver en el mapa →')}</button>
      </div>
    </div>
    <button class="char-clear" type="button" title="${t('Cambiar de personaje')}" aria-label="${t('Cambiar de personaje')}">×</button>
  </div>`;
}

function selectModCharacter(character) {
  state.modCharacter = character;
  state.modSearchResults = [];
  $('#mod-char-results').innerHTML = '';
  $('#mod-char-search').value = '';
  $('#mod-character-panel').classList.remove('hidden');
  $('#mod-no-character').classList.add('hidden');
  applyModCharacterToForms();
  renderModCharacterCard();
  if (!state.sanctions) loadSanctions();
}

export async function selectModCharacterByName(name) {
  try {
    const data = await api(`/api/characters/search?q=${encodeURIComponent(name)}`);
    const exact = data.characters.find((candidate) => lowerCase(candidate.name) === lowerCase(name)) || data.characters[0];
    if (!exact) return showToast(t('No se encontró ese personaje.'));
    selectModCharacter(exact);
  } catch (error) {
    if (error.status === 401) return showLogin();
    showToast(error.message);
  }
}

function clearModCharacter() {
  state.modCharacter = null;
  $('#mod-character-panel').classList.add('hidden');
  $('#mod-no-character').classList.remove('hidden');
  applyModCharacterToForms();
}

function itemResultHtml(item) {
  const color = QUALITY_COLORS[item.quality] ?? QUALITY_COLORS[1];
  return `<button class="item-result" type="button" data-item-entry="${item.entry}">
    <img src="/assets/item-icons/${escapeHtml(item.icon)}.webp" alt="" loading="lazy" decoding="async">
    <span style="color:${color}">${escapeHtml(item.name)}</span>
  </button>`;
}

async function searchItems() {
  const query = $('#item-search').value.trim();
  if (query.length < 2) {
    state.itemResults = [];
    $('#item-results').innerHTML = '';
    return;
  }
  try {
    const data = await api(`/api/items/search?q=${encodeURIComponent(query)}`);
    state.itemResults = data.items;
    $('#item-results').innerHTML = data.items.map(itemResultHtml).join('');
  } catch (error) {
    if (error.status === 401) return showLogin();
    showToast(error.message);
  }
}

function selectItem(entry) {
  const item = state.itemResults.find((candidate) => candidate.entry === Number(entry));
  if (!item) return;
  state.selectedItem = item;
  const color = QUALITY_COLORS[item.quality] ?? QUALITY_COLORS[1];
  $('#send-items-selected').innerHTML = `<img src="/assets/item-icons/${escapeHtml(item.icon)}.webp" alt=""><span style="color:${color}">${escapeHtml(item.name)}</span>`;
  $('#send-items-form [name="itemEntry"]').value = item.entry;
  $('#send-items-form').classList.remove('hidden');
}

function inviteRowHtml(invite) {
  const expired = invite.expiresAt && new Date(invite.expiresAt) < new Date();
  const status = invite.usedAt ? t('usada por la cuenta {account}', { account: invite.usedByAccount }) : (expired ? t('caducada') : t('disponible'));
  return `<div class="sanction-row">
    <span class="sanction-who">${escapeHtml(invite.id)}…</span>
    <span class="sanction-detail">${escapeHtml(invite.note || t('sin nota'))} · ${t('creada por {name}', { name: escapeHtml(invite.createdBy) })} · ${status}</span>
  </div>`;
}

function renderInvites() {
  if (!state.invites) return;
  $('#invite-list').innerHTML = state.invites.length ? state.invites.map(inviteRowHtml).join('') : `<p class="armory-empty-note">${t('Sin invitaciones todavía.')}</p>`;
}

async function loadInvites() {
  try {
    const data = await api('/api/admin/invites');
    state.invites = data.invites;
    renderInvites();
  } catch (error) {
    if (error.status === 401) return showLogin();
    showToast(error.message);
  }
}

function selectModerationTab(name, { focus = false } = {}) {
  state.modTab = name;
  $$('.mod-tabs button[data-mod-tab]').forEach((tab) => {
    const selected = tab.dataset.modTab === name;
    tab.classList.toggle('active', selected);
    tab.setAttribute('aria-selected', String(selected));
    tab.tabIndex = selected ? 0 : -1;
    if (selected && focus) tab.focus();
  });
  $('#mod-tab-character').classList.toggle('hidden', name !== 'character');
  $('#mod-tab-general').classList.toggle('hidden', name !== 'general');
}

$$('.mod-tabs button[data-mod-tab]').forEach((button) => button.addEventListener('click', () => selectModerationTab(button.dataset.modTab)));
$('.mod-tabs').addEventListener('keydown', (event) => {
  if (!['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(event.key)) return;
  event.preventDefault();
  const tabs = $$('.mod-tabs button[data-mod-tab]');
  const current = tabs.findIndex((tab) => tab.dataset.modTab === state.modTab);
  const next = event.key === 'Home' ? 0 : event.key === 'End' ? tabs.length - 1 : (current + (event.key === 'ArrowRight' ? 1 : -1) + tabs.length) % tabs.length;
  selectModerationTab(tabs[next].dataset.modTab, { focus: true });
});

$('#kick-form').addEventListener('submit', (event) => {
  event.preventDefault();
  moderationSubmit('/api/moderation/kick', formValues(event.target), { formSelector: '#kick-form', refreshSanctions: false });
});
$('#mute-form').addEventListener('submit', (event) => {
  event.preventDefault();
  moderationSubmit('/api/moderation/mute', formValues(event.target), { formSelector: '#mute-form' });
});
$('#unmute-form').addEventListener('submit', (event) => {
  event.preventDefault();
  moderationSubmit('/api/moderation/unmute', formValues(event.target), { formSelector: '#unmute-form' });
});
$('#ban-form').addEventListener('submit', (event) => {
  event.preventDefault();
  moderationSubmit('/api/moderation/ban', formValues(event.target), { formSelector: '#ban-form' });
});
$('#unban-form').addEventListener('submit', (event) => {
  event.preventDefault();
  moderationSubmit('/api/moderation/unban', formValues(event.target), { formSelector: '#unban-form' });
});
$('#ban-account-form').addEventListener('submit', (event) => {
  event.preventDefault();
  moderationSubmit('/api/moderation/ban', formValues(event.target), { formSelector: '#ban-account-form' });
});
$('#unban-account-form').addEventListener('submit', (event) => {
  event.preventDefault();
  moderationSubmit('/api/moderation/unban', formValues(event.target), { formSelector: '#unban-account-form' });
});
$('#announce-form').addEventListener('submit', (event) => {
  event.preventDefault();
  moderationSubmit('/api/moderation/announce', formValues(event.target), { formSelector: '#announce-form', refreshSanctions: false });
});
$('#send-money-form').addEventListener('submit', (event) => {
  event.preventDefault();
  moderationSubmit('/api/moderation/send-money', formValues(event.target), { formSelector: '#send-money-form', refreshSanctions: false });
});
$('#send-items-form').addEventListener('submit', (event) => {
  event.preventDefault();
  if (!state.selectedItem) return showToast(t('Elige un objeto de la lista primero.'));
  moderationSubmit('/api/moderation/send-items', formValues(event.target), {
    formSelector: '#send-items-form', refreshSanctions: false,
    onSuccess: () => { $('#send-items-form').classList.add('hidden'); state.selectedItem = null; },
  });
});
$('#sanctions-list').addEventListener('click', (event) => {
  const unban = event.target.closest('[data-unban]');
  if (unban) {
    const [type, name] = unban.dataset.unban.split(':');
    return moderationSubmit('/api/moderation/unban', { type, name });
  }
  const unmute = event.target.closest('[data-unmute]');
  if (unmute) moderationSubmit('/api/moderation/unmute', { characterName: unmute.dataset.unmute });
});
$('#item-search').addEventListener('input', searchItems);
$('#item-results').addEventListener('click', (event) => {
  const button = event.target.closest('[data-item-entry]');
  if (button) selectItem(button.dataset.itemEntry);
});

$('#mod-char-search').addEventListener('input', searchModCharacters);
$('#mod-char-results').addEventListener('click', (event) => {
  const button = event.target.closest('[data-mod-guid]');
  if (!button) return;
  const character = state.modSearchResults.find((candidate) => String(candidate.guid) === button.dataset.modGuid);
  if (character) selectModCharacter(character);
});
$('#mod-char-card').addEventListener('click', (event) => {
  if (event.target.closest('.char-clear')) return clearModCharacter();
  const goto = event.target.closest('[data-mod-goto]');
  if (!goto || !state.modCharacter) return;
  if (goto.dataset.modGoto === 'armory') { showView('armory'); openArmory(state.modCharacter.guid); }
  else if (goto.dataset.modGoto === 'map') focusCharacterOnMap({ guid: state.modCharacter.guid, name: state.modCharacter.name, sourceLabel: t('Moderación') });
});
$('#invite-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  const button = event.target.querySelector('button[type="submit"]');
  button.disabled = true;
  try {
    const data = await api('/api/admin/invites', { method: 'POST', body: JSON.stringify(formValues(event.target)) });
    $('#invite-result').classList.remove('hidden');
    $('#invite-result').textContent = t('Clave (cópiala ahora, no se vuelve a mostrar): {code}', { code: data.code });
    event.target.reset();
    loadInvites();
  } catch (error) {
    if (error.status === 401) return showLogin();
    showToast(error.message);
  } finally {
    button.disabled = false;
  }
});
$('#admin-account-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  const button = event.target.querySelector('button[type="submit"]');
  button.disabled = true;
  try {
    const data = await api('/api/admin/accounts', { method: 'POST', body: JSON.stringify(formValues(event.target)) });
    showToast(t('Cuenta {name} creada.', { name: data.username }));
    event.target.reset();
  } catch (error) {
    if (error.status === 401) return showLogin();
    showToast(error.message);
  } finally {
    button.disabled = false;
  }
});
