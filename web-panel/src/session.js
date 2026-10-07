// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import { createHmac, timingSafeEqual } from 'node:crypto';

const COOKIE_NAME = 'acore_session';

function encode(value) {
  return Buffer.from(value).toString('base64url');
}

function signature(payload, secret) {
  return createHmac('sha256', secret).update(payload).digest('base64url');
}

export function createSession(account, secret, ttlSeconds) {
  const now = Math.floor(Date.now() / 1000);
  const payload = encode(JSON.stringify({ sub: account.id, username: account.username, iat: now, exp: now + ttlSeconds }));
  return `${payload}.${signature(payload, secret)}`;
}

export function readSession(token, secret) {
  if (typeof token !== 'string') return null;
  const [payload, suppliedSignature, extra] = token.split('.');
  if (!payload || !suppliedSignature || extra) return null;
  const expected = Buffer.from(signature(payload, secret));
  const supplied = Buffer.from(suppliedSignature);
  if (expected.length !== supplied.length || !timingSafeEqual(expected, supplied)) return null;
  try {
    const session = JSON.parse(Buffer.from(payload, 'base64url').toString('utf8'));
    // Validación explícita de los campos temporales: sin
    // Number.isFinite(), un `exp` que no fuera un número (payload corrupto o
    // manipulado a mano tras romper la firma) hacía que `session.exp <= ...`
    // se comparase con NaN, que siempre da false — la sesión nunca caducaba.
    if (!Number.isInteger(session.sub) || typeof session.username !== 'string') return null;
    if (!Number.isFinite(session.iat) || !Number.isFinite(session.exp) || session.exp <= Date.now() / 1000) return null;
    return session;
  } catch {
    return null;
  }
}

export function parseCookie(header = '') {
  for (const part of header.split(';')) {
    const separator = part.indexOf('=');
    if (separator < 0) continue;
    if (part.slice(0, separator).trim() === COOKIE_NAME) {
      try {
        // decodeURIComponent lanza URIError con una cookie
        // truncada o mal codificada (p.ej. "acore_session=%"); antes eso
        // subía sin capturar hasta requireAuth y el manejador genérico de
        // errores lo convertía en un 500 — una cookie inválida es una sesión
        // inválida, no un fallo del servidor.
        return decodeURIComponent(part.slice(separator + 1).trim());
      } catch {
        return null;
      }
    }
  }
  return null;
}

export function sessionCookie(token, ttlSeconds, secure) {
  const parts = [`${COOKIE_NAME}=${encodeURIComponent(token)}`, 'Path=/', 'HttpOnly', 'SameSite=Strict', `Max-Age=${ttlSeconds}`];
  if (secure) parts.push('Secure');
  return parts.join('; ');
}

export function clearSessionCookie(secure) {
  return sessionCookie('', 0, secure);
}
