// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import test from 'node:test';
import assert from 'node:assert/strict';

process.env.SOAP_USERNAME = 'admin';
process.env.SOAP_PASSWORD = 'secreto';
process.env.SOAP_HOST = '127.0.0.1';
process.env.SOAP_PORT = '7878';

const { executeCommand, SoapError } = await import('../src/soap.js');

async function withFakeFetch(handler, run) {
  const original = globalThis.fetch;
  globalThis.fetch = handler;
  try {
    return await run();
  } finally {
    globalThis.fetch = original;
  }
}

test('monta el sobre con el namespace urn:AC, escapa el comando y manda Basic Auth', async () => {
  let capturedBody;
  let capturedHeaders;
  await withFakeFetch(async (_url, options) => {
    capturedBody = options.body;
    capturedHeaders = options.headers;
    return new Response(
      '<SOAP-ENV:Envelope xmlns:SOAP-ENV="http://schemas.xmlsoap.org/soap/envelope/"><SOAP-ENV:Body><ns1:executeCommandResponse xmlns:ns1="urn:AC"><result>ok</result></ns1:executeCommandResponse></SOAP-ENV:Body></SOAP-ENV:Envelope>',
      { status: 200 },
    );
  }, () => executeCommand('announce <hola> & "adiós"'));

  assert.match(capturedBody, /xmlns:ns1="urn:AC"/);
  assert.match(capturedBody, /<ns1:executeCommand[^>]*>/);
  assert.match(capturedBody, /<command>announce &lt;hola&gt; &amp; &quot;adiós&quot;<\/command>/);
  assert.match(capturedHeaders.Authorization, /^Basic /);
  assert.equal(Buffer.from(capturedHeaders.Authorization.replace('Basic ', ''), 'base64').toString(), 'admin:secreto');
});

test('devuelve el contenido de <result>', async () => {
  const result = await withFakeFetch(async () => new Response(
    '<Envelope><Body><ns1:executeCommandResponse><result>Comando ejecutado</result></ns1:executeCommandResponse></Body></Envelope>',
    { status: 200 },
  ), () => executeCommand('server info'));
  assert.equal(result, 'Comando ejecutado');
});

test('un <result/> autocerrado (comando sin salida) no lanza y devuelve cadena vacía', async () => {
  const result = await withFakeFetch(async () => new Response(
    '<Envelope><Body><ns1:executeCommandResponse><result/></ns1:executeCommandResponse></Body></Envelope>',
    { status: 200 },
  ), () => executeCommand('kick Testigo'));
  assert.equal(result, '');
});

test('un fallo SOAP (500 + faultstring) se traduce en SoapError con ese texto', async () => {
  await assert.rejects(
    withFakeFetch(async () => new Response(
      '<Envelope><Body><SOAP-ENV:Fault><faultstring>Player not found</faultstring></SOAP-ENV:Fault></Body></Envelope>',
      { status: 500 },
    ), () => executeCommand('kick Nadie')),
    (error) => error instanceof SoapError && error.message === 'Player not found',
  );
});

test('401/403 directos del worldserver se traducen a mensajes claros', async () => {
  await assert.rejects(
    withFakeFetch(async () => new Response('', { status: 401 }), () => executeCommand('server info')),
    (error) => error instanceof SoapError && /credenciales/i.test(error.message),
  );
  await assert.rejects(
    withFakeFetch(async () => new Response('', { status: 403 }), () => executeCommand('server info')),
    (error) => error instanceof SoapError && /administrador/i.test(error.message),
  );
});

test('un fallo de red se envuelve en SoapError sin tumbar el proceso', async () => {
  await assert.rejects(
    withFakeFetch(async () => { throw new Error('ECONNREFUSED'); }, () => executeCommand('server info')),
    (error) => error instanceof SoapError && /no se pudo contactar/i.test(error.message),
  );
});
