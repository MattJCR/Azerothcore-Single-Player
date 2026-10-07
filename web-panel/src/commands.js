// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Lista blanca de comandos GM: ninguna cadena escrita por el usuario llega a
// formar un comando sin pasar por un validador con forma fija. Formatos
// verificados contra el código fuente del core (cs_ban.cpp, cs_misc.cpp,
// cs_send.cpp, BanMgr.cpp): `ban`/`unban` toman "account"|"character",
// TimeStringToSecs acepta "30m"/"1h"/"1d"/"7d"/"30d" y trata "0" como
// permanente; `mute` exige una duración positiva (sin "permanente"); `send
// items`/`send money` esperan el asunto y el texto entre comillas dobles.

export class CommandError extends Error {}

const CHARACTER_NAME = /^[A-Za-zÀ-ÖØ-öø-ÿ]{2,12}$/;
const ACCOUNT_NAME = /^[A-Za-z0-9_]{3,32}$/;

// "30m"|"1h"|"1d"|"7d"|"30d"|"perm" → literal que entiende TimeStringToSecs.
// "perm" se traduce a "0": BanMgr trata una duración no positiva como permanente.
const BAN_DURATIONS = { '30m': '30m', '1h': '1h', '1d': '1d', '7d': '7d', '30d': '30d', perm: '0' };
// El core rechaza un silencio con duración <= 0: no hay opción "perm" aquí.
const MUTE_DURATIONS = { '30m': '30m', '1h': '1h', '1d': '1d', '7d': '7d' };

function requireMatch(value, pattern, label) {
  const trimmed = typeof value === 'string' ? value.trim() : '';
  if (!pattern.test(trimmed)) throw new CommandError(`${label} no es válido`);
  return trimmed;
}

function characterName(value) {
  return requireMatch(value, CHARACTER_NAME, 'El nombre del personaje');
}

function accountName(value) {
  return requireMatch(value, ACCOUNT_NAME, 'El nombre de la cuenta');
}

function targetName(type, value) {
  if (type !== 'account' && type !== 'character') throw new CommandError('El tipo de objetivo no es válido');
  return type === 'account' ? accountName(value) : characterName(value);
}

// Fuera comillas, control y saltos de línea: mismo criterio que
// lib/notify-gm.sh al inyectar texto por screen, aplicado aquí a SOAP.
function sanitizeText(value, { max, label, allowEmpty = false }) {
  const cleaned = typeof value === 'string'
    ? value.replace(/["'`]/g, '').replace(/[\r\n\t\x00-\x1f\x7f]/g, ' ').trim().replace(/\s+/g, ' ')
    : '';
  if (!allowEmpty && !cleaned) throw new CommandError(`${label} no puede estar vacío`);
  if (cleaned.length > max) throw new CommandError(`${label} supera los ${max} caracteres`);
  return cleaned;
}

function fromList(value, list, label) {
  const key = typeof value === 'string' ? value.trim() : '';
  if (!Object.prototype.hasOwnProperty.call(list, key)) throw new CommandError(`${label} no es una opción válida`);
  return list[key];
}

function positiveInt(value, { max, label }) {
  const n = Number.parseInt(value, 10);
  if (!Number.isInteger(n) || n <= 0 || n > max || String(n) !== String(value).trim()) {
    throw new CommandError(`${label} no es un número válido`);
  }
  return n;
}

// Red de seguridad final: pase lo que pase arriba, el comando montado no
// puede llevar retorno de carro ni salto de línea, ni una longitud absurda.
function assertSafeCommand(command) {
  if (/[\r\n]/.test(command) || command.length > 1000) throw new CommandError('El comando generado no es válido');
  return command;
}

export function kickCommand({ characterName: name, reason }) {
  const target = characterName(name);
  const cleanReason = sanitizeText(reason, { max: 255, label: 'El motivo', allowEmpty: true });
  return assertSafeCommand(cleanReason ? `kick ${target} ${cleanReason}` : `kick ${target}`);
}

export function muteCommand({ characterName: name, duration, reason }) {
  const target = characterName(name);
  const durationValue = fromList(duration, MUTE_DURATIONS, 'La duración');
  const cleanReason = sanitizeText(reason, { max: 255, label: 'El motivo' });
  return assertSafeCommand(`mute ${target} ${durationValue} ${cleanReason}`);
}

export function unmuteCommand({ characterName: name }) {
  return assertSafeCommand(`unmute ${characterName(name)}`);
}

export function banCommand({ type, name, duration, reason }) {
  const target = targetName(type, name);
  const durationValue = fromList(duration, BAN_DURATIONS, 'La duración');
  const cleanReason = sanitizeText(reason, { max: 255, label: 'El motivo' });
  return assertSafeCommand(`ban ${type} ${target} ${durationValue} ${cleanReason}`);
}

export function unbanCommand({ type, name }) {
  return assertSafeCommand(`unban ${type} ${targetName(type, name)}`);
}

export function announceCommand({ text }) {
  const cleanText = sanitizeText(text, { max: 500, label: 'El anuncio' });
  return assertSafeCommand(`announce ${cleanText}`);
}

export function sendItemsCommand({ characterName: name, subject, text, itemEntry, count }) {
  const target = characterName(name);
  const cleanSubject = sanitizeText(subject, { max: 100, label: 'El asunto' });
  const cleanText = sanitizeText(text, { max: 500, label: 'El texto' });
  const entry = positiveInt(itemEntry, { max: 999999, label: 'El objeto' });
  const quantity = positiveInt(count, { max: 1000, label: 'La cantidad' });
  return assertSafeCommand(`send items ${target} "${cleanSubject}" "${cleanText}" ${entry}:${quantity}`);
}

export function sendMoneyCommand({ characterName: name, subject, text, copper }) {
  const target = characterName(name);
  const cleanSubject = sanitizeText(subject, { max: 100, label: 'El asunto' });
  const cleanText = sanitizeText(text, { max: 500, label: 'El texto' });
  const amount = positiveInt(copper, { max: 100_000_000, label: 'La cantidad de cobre' });
  return assertSafeCommand(`send money ${target} "${cleanSubject}" "${cleanText}" ${amount}`);
}
