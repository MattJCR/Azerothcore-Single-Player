// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// test/helpers.js — utilidades comunes de arranque/cierre para las pruebas
// HTTP del panel.
//
// Antes, cada fichero de test repetía su propia copia de fakePool(),
// sessionCookieFor() y el arranque de createApp() + listen(0) + esperar a
// "listening". Aquí sólo vive lo genérico: el doble mínimo de un pool mysql2
// (execute/query/getConnection) y el ciclo de vida del servidor de pruebas.
// Los dobles SQL específicos de cada dominio (qué fila devuelve cada
// consulta) siguen viviendo en su propio fichero de test — mezclarlos aquí
// sólo cambiaría dónde está la duplicación, no la reduciría.
import { config } from '../src/config.js';
import { createSession, sessionCookie } from '../src/session.js';

export function fakePool(overrides = {}) {
  return {
    execute: overrides.execute || (async () => [[]]),
    query: overrides.query || (async () => [[]]),
    getConnection: overrides.getConnection || (async () => { throw new Error('getConnection no implementado en este mock'); }),
  };
}

export function sessionCookieFor(account) {
  const token = createSession(account, config.sessionSecret, config.sessionTtl);
  return sessionCookie(token, config.sessionTtl, config.cookieSecure).split(';')[0];
}

// Arranca una `app` de Express ya construida (con createApp({...})) en un
// puerto libre y espera a que esté escuchando de verdad antes de devolver la
// URL base — evita el "connection refused" ocasional de pedir a un servidor
// que todavía no aceptaba conexiones.
export async function listenTestApp(app) {
  const server = app.listen(0);
  await new Promise((resolve) => server.once('listening', resolve));
  return { server, base: `http://127.0.0.1:${server.address().port}` };
}
