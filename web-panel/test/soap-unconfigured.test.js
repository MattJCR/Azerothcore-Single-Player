// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import test from 'node:test';
import assert from 'node:assert/strict';

// Sin SOAP_USERNAME/SOAP_PASSWORD (el estado por defecto de .env.example), el
// panel debe negarse a intentar la petición en vez de fallar de forma opaca.
// Fichero aparte porque config.js sólo lee process.env una vez, al importarse.
const { executeCommand, SoapError } = await import('../src/soap.js');

test('sin credenciales SOAP configuradas, no se intenta contactar con el worldserver', async () => {
  const originalFetch = globalThis.fetch;
  let called = false;
  globalThis.fetch = async () => { called = true; throw new Error('no debería llamarse'); };
  try {
    await assert.rejects(executeCommand('server info'), SoapError);
    assert.equal(called, false);
  } finally {
    globalThis.fetch = originalFetch;
  }
});
