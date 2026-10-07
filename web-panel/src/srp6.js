// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import { createHash, timingSafeEqual } from 'node:crypto';

// Parámetros SRP6 usados por WoW 3.3.5a/AzerothCore.
const GENERATOR = 7n;
const MODULUS = BigInt('0x894B645E89E1535BBDAD5B8B290650530801B18EBFBF5E8FAB3C82872A3E9BB7');

function sha1(data) {
  return createHash('sha1').update(data).digest();
}

function littleEndianToBigInt(buffer) {
  const reversed = Buffer.from(buffer).reverse().toString('hex');
  return reversed ? BigInt(`0x${reversed}`) : 0n;
}

function bigIntToLittleEndian(value, length) {
  let hex = value.toString(16);
  if (hex.length % 2) hex = `0${hex}`;
  const bytes = Buffer.from(hex, 'hex').reverse();
  if (bytes.length > length) throw new Error('El entero SRP6 excede el tamaño esperado');
  return Buffer.concat([bytes, Buffer.alloc(length - bytes.length)]);
}

function modPow(base, exponent, modulus) {
  let result = 1n;
  let factor = base % modulus;
  let power = exponent;
  while (power > 0n) {
    if (power & 1n) result = (result * factor) % modulus;
    power >>= 1n;
    factor = (factor * factor) % modulus;
  }
  return result;
}

// AzerothCore aplica Utf8ToUpperOnlyLatin a ambos valores antes del cálculo.
function coreUpper(value) {
  return value.replace(/[a-zà-öø-ÿ]/g, (character) => character.toUpperCase());
}

export function calculateVerifier(username, password, salt) {
  if (!Buffer.isBuffer(salt) || salt.length !== 32) throw new Error('Salt SRP6 inválido');
  const identity = Buffer.from(`${coreUpper(username)}:${coreUpper(password)}`, 'utf8');
  const x = littleEndianToBigInt(sha1(Buffer.concat([salt, sha1(identity)])));
  return bigIntToLittleEndian(modPow(GENERATOR, x, MODULUS), 32);
}

export function verifyPassword(username, password, salt, storedVerifier) {
  if (!Buffer.isBuffer(storedVerifier) || storedVerifier.length !== 32) return false;
  const calculated = calculateVerifier(username, password, salt);
  return timingSafeEqual(calculated, storedVerifier);
}

export const internals = { littleEndianToBigInt, bigIntToLittleEndian, modPow, coreUpper };
