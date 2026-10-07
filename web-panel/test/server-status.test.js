// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import test from 'node:test';
import assert from 'node:assert/strict';
import { SoapError } from '../src/soap.js';
import { getServerStatus } from '../src/serverStatus.js';

// config.soap.configured es false en los tests (sin SOAP_USERNAME/PASSWORD), y
// config.standby.enabled también (sin WORLDSERVER_STANDBY). Con ese estado por
// defecto el estado del servidor es "unknown": no hay forma de saberlo.
test('sin SOAP ni modo en espera configurados, el estado es "unknown"', async () => {
  const status = await getServerStatus({ executeCommand: async () => { throw new SoapError('no'); } });
  assert.equal(status.state, 'unknown');
});
