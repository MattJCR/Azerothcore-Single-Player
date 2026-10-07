// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Mensajes de la API en el idioma del panel: el cliente manda X-Panel-Lang y el
// servidor contesta en inglés; sin la cabecera (curl, pruebas, el propio código)
// el texto sigue siendo el español en que se escribe.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { createApp } from '../src/app.js';
import { localizeBody, translate } from '../src/i18n.js';
import en from '../src/locales/en.js';
import { fakePool, sessionCookieFor, listenTestApp } from './helpers.js';

const sourceDirectory = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', 'src');

test('una frase conocida se traduce entera; una desconocida se deja como está', () => {
  assert.equal(translate('Sesión no válida o caducada', 'en'), 'Invalid or expired session');
  assert.equal(translate('Sesión no válida o caducada', 'es'), 'Sesión no válida o caducada');
  assert.equal(translate('Frase que nadie ha traducido', 'en'), 'Frase que nadie ha traducido');
  assert.equal(translate(undefined, 'en'), undefined);
});

test('las plantillas rellenan sus valores y traducen las etiquetas conocidas que capturan', () => {
  assert.equal(translate('El motivo supera los 255 caracteres', 'en'), 'The reason exceeds 255 characters');
  assert.equal(translate('La cantidad no es un número válido', 'en'), 'The quantity is not a valid number');
  assert.equal(translate('Algo desconocido no puede estar vacío', 'en'), 'Algo desconocido cannot be empty');
  assert.equal(translate('El valor no puede ser menor que 5', 'en'), 'The value cannot be less than 5');
});

test('gana la plantilla con más texto fijo, no la que se declaró antes', () => {
  assert.equal(translate('El valor supera los 64 caracteres', 'en'), 'The value exceeds 64 characters');
  assert.equal(translate('iconos terminó con código 1', 'en'), 'icons exited with code 1');
  assert.equal(translate('Falló la generación: parche esES terminó con código 2', 'en'), 'Generation failed: patch esES exited with code 2');
});

test('la respuesta sólo traduce los campos de texto para el usuario', () => {
  const body = {
    error: 'Trabajo no encontrado',
    trabajo: { mensaje: 'Cancelado', estado: 'cancelado', id: 'Cancelado' },
    recursos: [{ detalle: 'Se genera a partir de tu cliente de WoW' }],
    players: [{ name: 'Cancelado' }],
    description: 'Un texto que no es del diccionario',
  };
  assert.deepEqual(localizeBody(body, 'en'), {
    error: 'Job not found',
    trabajo: { mensaje: 'Cancelled', estado: 'cancelado', id: 'Cancelado' },
    recursos: [{ detalle: 'It is generated from your WoW client' }],
    players: [{ name: 'Cancelado' }],
    description: 'Un texto que no es del diccionario',
  });
  assert.equal(localizeBody(body, 'es'), body);
});

test('«description» sólo se traduce si el texto entero está en el diccionario, nunca por plantilla', () => {
  assert.equal(localizeBody({ description: 'Recibidos 5 ficheros' }, 'en').description, 'Recibidos 5 ficheros');
  assert.equal(localizeBody({ description: 'Parche no encontrado' }, 'en').description, 'Patch not found');
});

test('los placeholders {x} se conservan entre el mensaje y su traducción', () => {
  const placeholders = (text) => [...text.matchAll(/\{(\w+)\}/g)].map((match) => match[1]).sort();
  for (const [key, value] of Object.entries(en)) assert.deepEqual(placeholders(value), placeholders(key), key);
});

// Mensajes que el código escribe para el usuario: errores de las rutas, de
// las clases de error propias y de los validadores. Los literales con ${…} se
// comparan por su forma (cada ${…} cuenta como un marcador).
test('cada mensaje de error que escribe el código tiene su traducción', () => {
  const shape = (text) => text.replace(/\$\{[^}]*\}|\{\w+\}/g, '{}');
  const known = new Set(Object.keys(en).map(shape));
  const files = fs.readdirSync(sourceDirectory, { recursive: true, withFileTypes: true })
    .filter((entry) => entry.isFile() && entry.name.endsWith('.js'))
    .map((entry) => path.join(entry.parentPath, entry.name))
    // Errores de arranque o internos: nunca llegan al panel.
    .filter((file) => !/[\\/](config|srp6|metricsCache|addons|patches|i18n)\.js$|locales/.test(file));
  const patterns = [
    /\berror:\s*(?:'((?:[^'\\\n]|\\.)*)'|`((?:[^`\\]|\\.)*)`)/g,
    /new (?:Account|Command|BotOperations|Resource|ServerConfig|UpdateCheck|Soap)Error\(\s*(?:'((?:[^'\\\n]|\\.)*)'|`((?:[^`\\]|\\.)*)`)/g,
    /\blabel:\s*'((?:[^'\\\n]|\\.)*)'(?=[,\s}])/g,
    /\b(?:job\.mensaje|active\.mensaje)\s*=\s*(?:'((?:[^'\\\n]|\\.)*)'|`((?:[^`\\]|\\.)*)`)/g,
    /requireGmLevel\([^,]+,\s*'((?:[^'\\\n]|\\.)*)'\)/g,
  ];
  const missing = [];
  let scanned = 0;
  for (const file of files) {
    const source = fs.readFileSync(file, 'utf8');
    for (const pattern of patterns) {
      for (const match of source.matchAll(pattern)) {
        const text = (match[1] ?? match[2]).replace(/\\(.)/g, '$1');
        // `error: error.message`-style o textos que ya son una variable: sin letras no hay nada que traducir.
        if (!/\p{L}{3,}/u.test(text.replace(/\$\{[^}]*\}/g, ''))) continue;
        // Etiquetas de la tabla de parámetros del servidor (label: de serverConfigCatalog.js): las de módulos son nombres propios.
        if (/serverConfigCatalog\.js$/.test(file) && /^mod-[\w-]+$/.test(text)) continue;
        scanned += 1;
        if (!known.has(shape(text))) missing.push(`${path.relative(sourceDirectory, file)}: ${text}`);
      }
    }
  }
  assert.deepEqual(missing, []);
  assert.ok(scanned > 80, `el escáner sólo vio ${scanned} mensajes: ¿cambió la forma de escribirlos?`);
});

async function startApp(overrides = {}) {
  const authDb = fakePool({ execute: async (sql) => (sql.includes('FROM account_access') ? [[{ gmlevel: 3 }]] : [[]]) });
  const soap = { executeCommand: async () => 'ok' };
  const app = createApp({ authDb, charactersDb: fakePool(), worldDb: fakePool(), panelDb: fakePool(), soap, ...overrides });
  return listenTestApp(app);
}

test('la API contesta en inglés con X-Panel-Lang: en y en español sin la cabecera', async (t) => {
  const { server, base } = await startApp();
  t.after(() => server.close());

  const spanish = await fetch(`${base}/api/me`);
  assert.equal(spanish.status, 401);
  assert.deepEqual(await spanish.json(), { error: 'Sesión no válida o caducada' });

  const english = await fetch(`${base}/api/me`, { headers: { 'X-Panel-Lang': 'en' } });
  assert.deepEqual(await english.json(), { error: 'Invalid or expired session' });

  const unknown = await fetch(`${base}/api/me`, { headers: { 'X-Panel-Lang': 'fr' } });
  assert.deepEqual(await unknown.json(), { error: 'Sesión no válida o caducada' });

  const missing = await fetch(`${base}/api/inexistente`, { headers: { 'X-Panel-Lang': 'en' } });
  assert.deepEqual(await missing.json(), { error: 'Endpoint not found' });
  const file = await fetch(`${base}/falta.png`, { headers: { 'X-Panel-Lang': 'en' } });
  assert.equal(await file.text(), 'Resource not found');
});

test('un error de validación con valores sale traducido, con la cuenta del peticionario', async (t) => {
  const { server, base } = await startApp();
  t.after(() => server.close());
  const headers = { 'Content-Type': 'application/json', 'X-Panel-Request': '1', 'X-Panel-Lang': 'en', Cookie: sessionCookieFor({ id: 1, username: 'ADMIN' }) };
  const response = await fetch(`${base}/api/moderation/kick`, {
    method: 'POST', headers, body: JSON.stringify({ characterName: 'Testigo', reason: 'x'.repeat(300) }),
  });
  assert.equal(response.status, 400);
  assert.deepEqual(await response.json(), { error: 'The reason exceeds 255 characters' });
});

test('el interruptor de idioma no cambia las respuestas correctas', async (t) => {
  const { server, base } = await startApp();
  t.after(() => server.close());
  const headers = { 'X-Panel-Request': '1', Cookie: sessionCookieFor({ id: 1, username: 'ADMIN' }) };
  const spanish = await (await fetch(`${base}/api/items/search?q=a`, { headers })).json();
  const english = await (await fetch(`${base}/api/items/search?q=a`, { headers: { ...headers, 'X-Panel-Lang': 'en' } })).json();
  assert.deepEqual(english, spanish);
});

// ── Catálogos y comprobaciones de salud ─────────────────────────────────────

test('cada parámetro del catálogo de configuración tiene su descripción en inglés, y ninguna sobra', async () => {
  const { allCategories } = await import('../src/serverConfigCatalog.js');
  const { default: descriptions } = await import('../src/locales/config-en.js');
  const keys = allCategories().flatMap((category) => category.params.map((param) => param.key));
  assert.deepEqual(keys.filter((key) => !descriptions[key]), []);
  assert.deepEqual(Object.keys(descriptions).filter((key) => !keys.includes(key)), []);
  assert.equal(new Set(keys).size, keys.length, 'las claves del catálogo deben ser únicas: la traducción se indexa por clave');
});

test('las etiquetas de categoría del catálogo de configuración están traducidas', async () => {
  const { allCategories } = await import('../src/serverConfigCatalog.js');
  const missing = allCategories().map((category) => category.label)
    .filter((label) => !/^mod-[\w-]+$/.test(label) && translate(label, 'en') === label);
  assert.deepEqual(missing, []);
});

test('la descripción de un parámetro sale en el idioma de la petición', async (t) => {
  const { server, base } = await startApp();
  t.after(() => server.close());
  const headers = { 'X-Panel-Request': '1', Cookie: sessionCookieFor({ id: 1, username: 'ADMIN' }) };
  const ask = async (lang) => (await (await fetch(`${base}/api/server-config/search-index`, { headers: lang ? { ...headers, 'X-Panel-Lang': lang } : headers })).json()).params.find((param) => param.key === 'Rate.Health');
  assert.equal((await ask()).description, 'Multiplicador de la vida máxima de personajes y criaturas.');
  assert.equal((await ask('en')).description, 'Multiplier for the maximum health of characters and creatures.');
  assert.equal((await ask('en')).categoryLabel, 'Combat and regeneration rates');
});

test('cada mensaje de lib/doctor.sh tiene su traducción', () => {
  const doctor = fs.readFileSync(path.join(sourceDirectory, '..', '..', 'lib', 'doctor.sh'), 'utf8');
  // $(…), ${…} y $VAR cuentan como un marcador, igual que las {x} del diccionario.
  const shape = (text) => text.replace(/\$\{[^}]*\}|\$\([^)]*\)|\$[A-Za-z_]+|\{\w+\}/g, '{}');
  const known = new Set(Object.keys(en).map(shape));
  const missing = [];
  let scanned = 0;
  for (const match of doctor.matchAll(/_doctor_check "[\w-]+" (?:ok|warn|fail) "((?:[^"\\]|\\.)*)"/g)) {
    const text = match[1].replace(/\\(.)/g, '$1');
    // Una sustitución $(…) con comillas dentro corta la captura: esas dos (la salida de build-addons y la versión
    // de los datos del cliente) llevan su plantilla a mano en el diccionario.
    if (text.includes('$(') && !/\$\([^)]*\)/.test(text)) continue;
    if (!/\p{L}{3,}/u.test(text.replace(/\$\{[^}]*\}|\$\([^)]*\)/g, ''))) continue; // sólo variables: no hay nada que traducir
    scanned += 1;
    if (!known.has(shape(text))) missing.push(text);
  }
  assert.deepEqual(missing, []);
  assert.ok(scanned > 25, `sólo se vieron ${scanned} mensajes de doctor.sh`);
});

test('los detalles de las comprobaciones se traducen con sus valores', () => {
  const body = { doctor: { checks: [
    { name: 'disk-space', status: 'ok', detail: 'partición de /home/acore/azerothcore al 45% de uso.' },
    { name: 'services', status: 'ok', detail: 'activos: worldserver authserver.' },
    { name: 'pinned-versions', status: 'warn', detail: '2 de 31 repositorios NO están en el commit de versions.lock.' },
    { name: 'client-data', status: 'ok', detail: 'datos del cliente completos ().' },
    { name: 'addons', status: 'ok', detail: '309/309 carpetas coinciden con arbol.tsv; ausentes 0, alteradas 0, sobrantes 0.' },
    { name: 'slow-ticks', status: 'warn', detail: '1 avisos de tick lento acumulados en Server.log.' },
  ] } };
  assert.deepEqual(localizeBody(body, 'en').doctor.checks.map((check) => check.detail), [
    'partition of /home/acore/azerothcore at 45% usage.',
    'active: worldserver authserver.',
    '2 of 31 repositories are NOT at the commit in versions.lock.',
    'client data complete.',
    '309/309 folders match arbol.tsv; missing 0, altered 0, extra 0.',
    '1 slow-tick warning accumulated in Server.log.',
  ]);
});

test('cada addon del catálogo tiene su descripción en inglés, y ninguna sobra', () => {
  const addonsDirectory = path.join(sourceDirectory, '..', 'addons');
  const catalog = JSON.parse(fs.readFileSync(path.join(addonsDirectory, 'catalog.json'), 'utf8'));
  const english = JSON.parse(fs.readFileSync(path.join(addonsDirectory, 'descriptions-en.json'), 'utf8'));
  const ids = catalog.addons.map((addon) => addon.id);
  assert.deepEqual(ids.filter((id) => !english[id]?.trim()), []);
  assert.deepEqual(Object.keys(english).filter((id) => !ids.includes(id)), []);
});

test('el catálogo de addons y los parches salen en el idioma de la petición', async (t) => {
  const { server, base } = await startApp();
  t.after(() => server.close());
  const headers = { 'X-Panel-Request': '1', Cookie: sessionCookieFor({ id: 1, username: 'ADMIN' }) };
  const ask = async (lang) => (await fetch(`${base}/api/addons`, { headers: lang ? { ...headers, 'X-Panel-Lang': lang } : headers })).json();
  const spanish = await ask();
  const english = await ask('en');
  const find = (catalog, id) => catalog.addons.find((addon) => addon.id === id);
  assert.match(find(spanish, 'autorepair').description, /^Repara automáticamente/);
  assert.match(find(english, 'autorepair').description, /^Automatically repairs/);
  assert.equal(find(english, 'autorepair').contentVersion, find(spanish, 'autorepair').contentVersion);
  assert.match(english.patches[0].description, /^The server's own items/);
});

// ── Ayuda de comandos (server_help_*) ───────────────────────────────────────

test('cada ficha y artículo de la semilla de mod-server-help tiene su traducción al inglés', () => {
  const sql = fs.readFileSync(path.join(sourceDirectory, '..', '..', 'modules', 'mod-server-help', 'data', 'sql', 'db-world', 'base', 'server_help.sql'), 'utf8');
  const seeded = new Set([...sql.matchAll(/^\('([a-z0-9 ]+)',\s*\d+,\s*'/gm)].map((match) => match[1]));
  const translated = new Set([...sql.matchAll(/UPDATE `server_help_command` SET `title_en`=.*? WHERE `command_path`='([^']+)';/g)].map((match) => match[1]));
  assert.ok(seeded.size > 200, `sólo se vieron ${seeded.size} fichas en la semilla`);
  assert.deepEqual([...seeded].filter((command) => !translated.has(command)), []);
  assert.deepEqual([...translated].filter((command) => !seeded.has(command)), []);
  const articles = [...sql.matchAll(/^\((\d+), \d+, '/gm)].map((match) => match[1]);
  const translatedArticles = new Set([...sql.matchAll(/UPDATE `server_help_article` SET `title_en`=.*? WHERE `id`=(\d+);/g)].map((match) => match[1]));
  assert.deepEqual(articles.filter((id) => !translatedArticles.has(id)), []);
});

test('la ayuda de comandos sale en inglés si la columna está rellena y en español si no', async (t) => {
  const worldDb = fakePool({
    query: async (sql) => {
      if (sql.includes('FROM server_help_category')) return [[{ id: 1, name: 'General', nameEn: 'General', sort: 1, minSecurity: 0, enabled: 1 }, { id: 2, name: 'Grupo', nameEn: 'Group', sort: 2, minSecurity: 0, enabled: 1 }]];
      if (sql.includes('FROM server_help_command')) {
        return [[
          { path: 'save', categoryId: 1, title: 'Guardar el personaje', titleEn: 'Save the character', description: 'Fuerza el guardado.', descriptionEn: 'Forces a save.', syntax: '.save', examples: '', keywords: 'guardar', minSecurity: 0, enabled: 1 },
          { path: 'grupo', categoryId: 2, title: 'Grupo de bots', titleEn: '', description: 'Forma un grupo.', descriptionEn: null, syntax: '.grupo', examples: '', keywords: 'grupo', minSecurity: 0, enabled: 1 },
        ]];
      }
      return [[{ id: 1, categoryId: 1, title: 'Cómo funciona', titleEn: 'How it works', body: 'Texto.', bodyEn: 'Text.', keywords: '', commandPath: '', minSecurity: 0, sort: 1, enabled: 1, isHot: 0 }]];
    },
  });
  const { server, base } = await startApp({ worldDb });
  t.after(() => server.close());
  const headers = { 'X-Panel-Request': '1', Cookie: sessionCookieFor({ id: 1, username: 'ADMIN' }) };
  const ask = async (lang) => (await fetch(`${base}/api/help`, { headers: lang ? { ...headers, 'X-Panel-Lang': lang } : headers })).json();

  const spanish = await ask();
  assert.equal(spanish.commands.find((command) => command.path === 'save').title, 'Guardar el personaje');
  assert.equal(spanish.categories.find((category) => category.id === 2).name, 'Grupo');

  const english = await ask('en');
  assert.equal(english.commands.find((command) => command.path === 'save').title, 'Save the character');
  assert.equal(english.commands.find((command) => command.path === 'save').description, 'Forces a save.');
  assert.equal(english.commands.find((command) => command.path === 'grupo').title, 'Grupo de bots', 'sin traducción cae en el español');
  assert.equal(english.articles[0].body, 'Text.');
  assert.equal(english.categories.find((category) => category.id === 2).name, 'Group');
});
