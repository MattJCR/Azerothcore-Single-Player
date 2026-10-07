// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Lista blanca de acciones seguras para "Panel Bots".
// Los cinco primeros nombres son exactamente los que devuelve
// BotOperations::ActionName() en modules/shared/BotOperations.h — si cambian
// ahí, hay que cambiarlos aquí. 'refresh_snapshot' lo resuelve directamente
// mod-bot-operations, sin pasar por ningún otro módulo.
export const BOT_OPERATIONS_ACTIONS = new Set([
  'world_bots_pass',
  'stop_world_pvp',
  'queue_bots_pass',
  'party_here_pass',
  'home_guild_pass',
  'refresh_snapshot',
]);

// Sólo "parar un evento PvP atascado" admite parámetro: el id numérico del
// evento (vacío = para todos los eventos activos).
const ACTIONS_WITH_PARAM = new Set(['stop_world_pvp']);

export class BotOperationsError extends Error {}

export function validateAction(rawAction, rawParam) {
  const action = String(rawAction ?? '');
  if (!BOT_OPERATIONS_ACTIONS.has(action)) throw new BotOperationsError('Acción no reconocida.');

  const hasParam = rawParam !== undefined && rawParam !== null && String(rawParam) !== '';
  if (!hasParam) return { action, param: '' };

  if (!ACTIONS_WITH_PARAM.has(action)) throw new BotOperationsError('Esta acción no admite parámetro.');
  const param = String(rawParam);
  if (!/^[0-9]{1,10}$/.test(param)) throw new BotOperationsError('El parámetro debe ser un identificador numérico.');
  if (BigInt(param) > 4294967295n) throw new BotOperationsError('El identificador de evento supera el máximo permitido.');
  return { action, param };
}

export function validateReason(rawReason) {
  if (rawReason === undefined || rawReason === null || rawReason === '') return '';
  const reason = String(rawReason).trim();
  if (reason.length > 255) throw new BotOperationsError('El motivo es demasiado largo (máximo 255 caracteres).');
  return reason;
}
