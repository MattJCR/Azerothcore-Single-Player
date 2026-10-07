// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import { config } from './config.js';

// Cliente mínimo del servicio SOAP de ACSoap.cpp (namespace "urn:AC", elemento
// raíz "ns1:executeCommand"), verificado contra el código fuente del core:
// gSOAP rellena soap->userid/soap->passwd desde la cabecera HTTP estándar
// "Authorization: Basic ...", así que no hace falta WS-Security.
const NAMESPACE = 'urn:AC';

export class SoapError extends Error {}

function escapeXml(value) {
  return String(value)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&apos;');
}

function unescapeXml(value) {
  return value
    .replace(/&lt;/g, '<')
    .replace(/&gt;/g, '>')
    .replace(/&quot;/g, '"')
    .replace(/&apos;/g, "'")
    .replace(/&amp;/g, '&');
}

// Analizador deliberadamente ingenuo: sólo habla con el worldserver en
// 127.0.0.1, nunca con XML de origen externo, así que no hace falta un parser
// XML completo (ni la dependencia que traería).
function extractTag(xml, tag) {
  if (new RegExp(`<(?:[\\w-]+:)?${tag}\\b[^>]*/>`).test(xml)) return '';
  const match = xml.match(new RegExp(`<(?:[\\w-]+:)?${tag}\\b[^>]*>([\\s\\S]*?)</(?:[\\w-]+:)?${tag}>`));
  return match ? unescapeXml(match[1]).trim() : null;
}

function buildEnvelope(command) {
  return `<?xml version="1.0" encoding="UTF-8"?>
<SOAP-ENV:Envelope xmlns:SOAP-ENV="http://schemas.xmlsoap.org/soap/envelope/" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xmlns:xsd="http://www.w3.org/2001/XMLSchema">
<SOAP-ENV:Body>
<ns1:executeCommand xmlns:ns1="${NAMESPACE}">
<command>${escapeXml(command)}</command>
</ns1:executeCommand>
</SOAP-ENV:Body>
</SOAP-ENV:Envelope>`;
}

export async function executeCommand(command) {
  if (!config.soap.configured) throw new SoapError('SOAP no está configurado en este panel');

  let response;
  try {
    response = await fetch(`http://${config.soap.host}:${config.soap.port}/`, {
      method: 'POST',
      headers: {
        'Content-Type': 'text/xml; charset=utf-8',
        Authorization: `Basic ${Buffer.from(`${config.soap.username}:${config.soap.password}`).toString('base64')}`,
        SOAPAction: '""',
      },
      body: buildEnvelope(command),
      signal: AbortSignal.timeout(config.soap.timeoutMs),
    });
  } catch (error) {
    throw new SoapError(`No se pudo contactar con la consola del worldserver: ${error.message}`);
  }

  const text = await response.text();
  if (!response.ok) {
    if (response.status === 401) throw new SoapError('Credenciales SOAP rechazadas por el worldserver');
    if (response.status === 403) throw new SoapError('La cuenta SOAP no tiene gmlevel de administrador');
    const fault = extractTag(text, 'faultstring');
    throw new SoapError(fault || `La consola respondió con el estado ${response.status}`);
  }
  return extractTag(text, 'result') ?? '';
}

export const soapClient = { executeCommand };
