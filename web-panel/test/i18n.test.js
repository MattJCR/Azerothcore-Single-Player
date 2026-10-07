// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Localización del panel (public/i18n.js + public/locales/en.js): ninguna cadena
// del HTML ni de las llamadas t('…') queda sin traducir, el diccionario no
// acumula claves huérfanas ni marcadores {x} distintos entre idiomas, y en
// inglés el HTML estático se traduce al arrancar.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { JSDOM } from 'jsdom';

const publicDirectory = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', 'public');
const normalize = (text) => text.replace(/\s+/g, ' ').trim();

// Categorías del catálogo de addons: se traducen con t(variable), que el
// escáner de abajo no puede ver. '1c1' sí está en el HTML, pero el escáner
// ignora los textos con una sola letra.
const DYNAMIC_KEYS = new Set([
  '1c1',
  'Interfaz', 'Bandas y combate', 'Utilidades', 'Equipo e inventario', 'Misiones y subida', 'Mapas y exploración',
  'Economía y profesiones', 'Chat y social', 'JcJ', 'Servidor',
]);

function sourceFiles(directory) {
  const files = [];
  for (const entry of fs.readdirSync(directory, { withFileTypes: true })) {
    const full = path.join(directory, entry.name);
    if (entry.isDirectory()) {
      if (!['assets', 'vendor', 'locales'].includes(entry.name)) files.push(...sourceFiles(full));
    } else if (entry.name.endsWith('.js') && entry.name !== 'i18n.js') files.push(full);
  }
  return files;
}

function scriptKeys() {
  const keys = new Map();
  const call = /\bt\(\s*(?:'((?:[^'\\\n]|\\.)*)'|`((?:[^`\\]|\\.)*)`)/g;
  for (const file of sourceFiles(publicDirectory)) {
    const source = fs.readFileSync(file, 'utf8');
    for (const match of source.matchAll(call)) {
      const raw = match[1] ?? match[2];
      assert.ok(!raw.includes('${'), `${path.relative(publicDirectory, file)}: t(\`${raw}\`) interpola con \${…}; usa {marcador} y parámetros`);
      keys.set(raw.replace(/\\(.)/g, (_, char) => (char === 'n' ? '\n' : char)), path.relative(publicDirectory, file));
    }
  }
  return keys;
}

function htmlKeys() {
  const doc = new JSDOM(fs.readFileSync(path.join(publicDirectory, 'index.html'), 'utf8')).window.document;
  const keys = new Map();
  const htmlElements = [...doc.querySelectorAll('[data-i18n-html]')];
  for (const element of htmlElements) keys.set(normalize(element.innerHTML), 'index.html (data-i18n-html)');
  for (const attribute of ['placeholder', 'title', 'aria-label', 'alt']) {
    for (const element of doc.querySelectorAll(`[${attribute}]`)) keys.set(normalize(element.getAttribute(attribute)), `index.html [${attribute}]`);
  }
  const walker = doc.createTreeWalker(doc.body, 4);
  while (walker.nextNode()) {
    const node = walker.currentNode;
    if (['SCRIPT', 'STYLE'].includes(node.parentElement.tagName) || htmlElements.some((element) => element.contains(node))) continue;
    const text = normalize(node.nodeValue);
    if (text && /\p{L}{2,}/u.test(text)) keys.set(text, 'index.html');
  }
  keys.set(normalize(doc.title), 'index.html <title>');
  return keys;
}

const { default: en } = await import('../public/locales/en.js');

test('todo texto del HTML y toda llamada t() tiene traducción al inglés', () => {
  const missing = [...scriptKeys(), ...htmlKeys()].filter(([key]) => !Object.hasOwn(en, key)).map(([key, file]) => `${file}: ${JSON.stringify(key)}`);
  assert.deepEqual(missing, []);
});

test('el diccionario inglés no tiene claves huérfanas ni duplicadas', () => {
  const used = new Set([...scriptKeys().keys(), ...htmlKeys().keys(), ...DYNAMIC_KEYS]);
  assert.deepEqual(Object.keys(en).filter((key) => !used.has(normalize(key)) && !used.has(key)), []);

  const source = fs.readFileSync(path.join(publicDirectory, 'locales', 'en.js'), 'utf8');
  const declared = [...source.matchAll(/^ {2}("(?:[^"\\]|\\.)*"): /gm)].map((match) => JSON.parse(match[1]));
  const duplicated = declared.filter((key, index) => declared.indexOf(key) !== index);
  assert.deepEqual(duplicated, []);
  assert.equal(declared.length, Object.keys(en).length);
});

test('la traducción conserva los marcadores {x} de la clave', () => {
  const placeholders = (text) => [...text.matchAll(/\{(\w+)\}/g)].map((match) => match[1]).sort();
  for (const [key, value] of Object.entries(en)) assert.deepEqual(placeholders(value), placeholders(key), key);
});

test('las categorías de addons del catálogo están traducidas', () => {
  const catalog = JSON.parse(fs.readFileSync(path.join(publicDirectory, '..', 'addons', 'catalog.json'), 'utf8'));
  const classes = new Set(catalog.addons.flatMap((addon) => addon.classes));
  for (const addon of catalog.addons) assert.ok(Object.hasOwn(en, addon.category), `categoría sin traducir: ${addon.category}`);
  for (const name of classes) assert.ok(Object.hasOwn(en, name), `clase sin traducir: ${name}`);
});

// i18n.js fija el idioma al cargarse: este fichero lo carga una sola vez, en inglés.
const dom = new JSDOM(fs.readFileSync(path.join(publicDirectory, 'index.html'), 'utf8'), { url: 'http://localhost/' });
dom.window.localStorage.setItem('panel-lang', 'en');
global.window = dom.window;
global.document = dom.window.document;
const i18n = await import('../public/i18n.js');
const { document } = dom.window;

test('en inglés se traducen textos, atributos y fragmentos con etiquetas del HTML estático', () => {
  assert.equal(i18n.lang(), 'en');
  assert.equal(document.documentElement.lang, 'en');
  assert.equal(document.querySelector('#login-button').textContent, 'Enter the realm');
  assert.equal(document.querySelector('#player-search').getAttribute('placeholder'), 'Search player…');
  assert.equal(document.querySelector('#logout-button').getAttribute('aria-label'), 'Log out');
  assert.equal(document.querySelector('#social-zoom-in').getAttribute('title'), 'Zoom in');
  assert.match(document.querySelector('.certificate-help').innerHTML, /^First time: install the certificate in <strong>Local Computer/);
  // El <span id> del fragmento sigue ahí, para que el código pueda rellenarlo.
  assert.ok(document.querySelector('#bot-ops-cap'));
  assert.equal(document.querySelector('#bot-ops-cap').parentElement.textContent, 'of — allowed');
});

test('en inglés se conservan los espacios que separan el texto de sus etiquetas vecinas', () => {
  assert.equal(document.querySelector('#selected-count').parentElement.textContent.trim(), '2 selected');
  assert.equal(document.querySelector('#map-nav').textContent.replace(/\s+/g, ' ').trim(), '⌖GM map GM');
});

test('t() rellena marcadores y formatea con la configuración regional del idioma', () => {
  assert.equal(i18n.t('hace {n} min', { n: 5 }), '5 min ago');
  assert.equal(i18n.t('Texto que no existe en el diccionario {x}', { x: 1 }), 'Texto que no existe en el diccionario 1');
  assert.equal(i18n.t('{a} y {b}', { a: 1 }), '1 y {b}');
  assert.equal(i18n.locale(), 'en-GB');
  assert.equal(i18n.formatDateTime('2026-10-07T13:05:00Z').includes('2026'), true);
});

test('los selectores de idioma ofrecen español e inglés y reflejan el idioma elegido', () => {
  const selects = [...document.querySelectorAll('select.lang-select')];
  assert.equal(selects.length, 3);
  for (const select of selects) {
    assert.deepEqual([...select.options].map((option) => option.value), ['es', 'en']);
    assert.equal(select.value, 'en');
  }
});
