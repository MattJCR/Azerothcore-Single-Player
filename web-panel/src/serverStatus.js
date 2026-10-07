// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import { readFileSync } from 'node:fs';
import { config } from './config.js';
import { SoapError } from './soap.js';

// Estado del worldserver para la cabecera del panel. Tres estados:
//   online   — responde por SOAP (está cargado y aceptando conexiones)
//   standby  — en modo en espera y dormido (systemd tiene el puerto 8085, el
//              proceso no existe); la próxima conexión de un cliente lo arranca
//   offline  — ni responde ni está dormido a propósito (caído, o parado a mano)
//   unknown  — no se puede determinar (SOAP no configurado y sin modo en espera)
//
// "online" es autoritativo: si SOAP responde, está arriba. Sólo cuando SOAP no
// responde se mira el fichero de estado que dejan las unidades systemd
// (ExecStartPost/ExecStopPost) y, como último recurso, si el modo en espera
// está activado en la config.

function readStateFile() {
  if (!config.standby.enabled) return null;
  try {
    return readFileSync(config.standby.statePath, 'utf8').trim().toLowerCase() || null;
  } catch {
    return null;
  }
}

export async function getServerStatus(soap = null) {
  const client = soap || (await import('./soap.js')).soapClient;

  if (config.soap.configured) {
    try {
      await client.executeCommand('server info');
      return { state: 'online' };
    } catch (error) {
      if (!(error instanceof SoapError)) throw error;
      // SOAP configurado pero sin respuesta: el worldserver no está aceptando.
    }
  }

  const fileState = readStateFile();
  if (fileState === 'running') {
    // El fichero dice "arrancado" pero SOAP no responde: o está cargando
    // todavía, o SOAP tarda. Lo damos por "arrancando/online".
    return { state: 'online' };
  }
  if (fileState === 'standby') return { state: 'standby' };
  if (fileState === 'down') return { state: 'offline' };

  if (config.standby.enabled) return { state: 'standby' };
  if (config.soap.configured) return { state: 'offline' };
  return { state: 'unknown' };
}
