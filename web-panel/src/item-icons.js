// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Iconos de objetos de la armería. El índice (displayId -> icono) lo genera el
// panel a partir del cliente del jugador (resources.js); mientras no exista, el
// panel arranca igual y todos los objetos usan un icono genérico propio.
const safeFilename = /^[a-z0-9_.-]+$/;
export const PLACEHOLDER = 'generico';

let source = () => null;

// app.js enlaza aquí el almacén de recursos; los tests ponen el suyo.
export function useIconSource(provider) {
  source = typeof provider === 'function' ? provider : () => null;
}

export function iconForDisplayId(displayId) {
  const manifest = source();
  if (!manifest) return PLACEHOLDER;
  const filename = manifest.icons[Number(displayId)] || manifest.fallback;
  return safeFilename.test(filename) ? filename : manifest.fallback;
}
