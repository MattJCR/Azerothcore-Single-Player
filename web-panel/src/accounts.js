// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import { randomBytes } from 'node:crypto';
import { calculateVerifier } from './srp6.js';

// Límites del propio core (AccountMgr.h: MAX_ACCOUNT_STR = 17, MAX_PASS_STR =
// 16); una cuenta creada fuera de ellos no se podría usar desde el cliente.
export const MAX_USERNAME_LENGTH = 17;
export const MAX_PASSWORD_LENGTH = 16;
const MIN_USERNAME_LENGTH = 3;
const MIN_PASSWORD_LENGTH = 8;

const USERNAME_PATTERN = new RegExp(`^[A-Za-z0-9_]{${MIN_USERNAME_LENGTH},${MAX_USERNAME_LENGTH}}$`);

export class AccountError extends Error {}

export function normalizeUsername(value) {
  const trimmed = typeof value === 'string' ? value.trim() : '';
  if (!USERNAME_PATTERN.test(trimmed)) {
    throw new AccountError(`El nombre de cuenta debe tener entre ${MIN_USERNAME_LENGTH} y ${MAX_USERNAME_LENGTH} letras, números o guiones bajos`);
  }
  // Mismo criterio que AccountMgr::CreateAccount y lib/utils.sh: la cuenta se
  // guarda en mayúsculas. La comparación en el login no depende de esto (la
  // columna usa collation _ci), pero mantiene la coherencia con el resto del
  // proyecto y con las cuentas que crea el instalador.
  return trimmed.toUpperCase();
}

export function normalizePassword(value) {
  if (typeof value !== 'string' || value.length < MIN_PASSWORD_LENGTH || value.length > MAX_PASSWORD_LENGTH) {
    throw new AccountError(`La contraseña debe tener entre ${MIN_PASSWORD_LENGTH} y ${MAX_PASSWORD_LENGTH} caracteres (límite del cliente 3.3.5a)`);
  }
  return value;
}

// calculateVerifier aplica Utf8ToUpperOnlyLatin a usuario y contraseña por su
// cuenta (src/srp6.js): no hay que mayuscular la contraseña aquí.
export function buildCredentials(username, password) {
  const normalizedUsername = normalizeUsername(username);
  const normalizedPassword = normalizePassword(password);
  const salt = randomBytes(32);
  const verifier = calculateVerifier(normalizedUsername, normalizedPassword, salt);
  return { username: normalizedUsername, salt, verifier };
}
