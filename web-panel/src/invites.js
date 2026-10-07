// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import { randomBytes, createHash } from 'node:crypto';

// La base sólo guarda el hash: leer panel_invite no da una clave utilizable.
// El texto plano se devuelve una única vez, en la respuesta de creación.
export function generateInviteCode() {
  return randomBytes(15).toString('base64url');
}

export function hashInviteCode(code) {
  return createHash('sha256').update(code).digest('hex');
}
