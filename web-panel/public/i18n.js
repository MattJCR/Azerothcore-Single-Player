// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Localización del panel (español / inglés). La clave de cada texto es el propio
// texto en español, como en gettext: el español es el idioma de origen (el que
// se edita) y locales/en.js sólo guarda la traducción al inglés. Lo que no
// tenga traducción se muestra en español en vez de quedar vacío.
//
// - En JS: t('Texto en español con {marcador}', { marcador: valor }).
// - En el HTML estático no hace falta marcar nada: translateDocument() recorre
//   los nodos de texto y los atributos placeholder/title/aria-label/alt. Un
//   elemento con texto mezclado con etiquetas (<strong>, <span id>…) lleva
//   data-i18n-html y se traduce entero, con su HTML interno como clave.
// - El idioma se elige una vez por carga de página (selector + localStorage) y
//   al cambiarlo la página se recarga: ninguna vista tiene que repintarse sola.
//
// Sin DOM (pruebas de módulos sueltos en Node) el idioma es siempre el español.
import en from './locales/en.js';

export const LANGUAGES = { es: 'Español', en: 'English' };
const STORAGE_KEY = 'panel-lang';
const LOCALES = { es: 'es-ES', en: 'en-GB' };

function detectLanguage() {
  const win = globalThis.window;
  if (!win) return 'es';
  try {
    const saved = win.localStorage?.getItem(STORAGE_KEY);
    if (Object.hasOwn(LANGUAGES, saved)) return saved;
  } catch { /* almacenamiento bloqueado: se usa el idioma del navegador */ }
  return /^es(\b|-)/i.test(win.navigator?.language || '') ? 'es' : 'en';
}

const currentLanguage = detectLanguage();

export const lang = () => currentLanguage;
export const locale = () => LOCALES[currentLanguage];

export function setLanguage(code) {
  if (!Object.hasOwn(LANGUAGES, code) || code === currentLanguage) return;
  try { globalThis.window?.localStorage?.setItem(STORAGE_KEY, code); } catch { /* sin almacenamiento no se recuerda, pero se recarga igual */ }
  globalThis.window?.location?.reload();
}

export function t(key, params) {
  let text = currentLanguage === 'es' ? key : (en[key] ?? key);
  if (params) text = text.replace(/\{(\w+)\}/g, (placeholder, name) => (Object.hasOwn(params, name) ? String(params[name]) : placeholder));
  return text;
}

// Formatos con la configuración regional del idioma elegido.
export const formatDateTime = (value) => new Date(value).toLocaleString(locale());
export const formatDate = (value) => new Date(value).toLocaleDateString(locale());
export const formatTime = (value, options) => new Date(value).toLocaleTimeString(locale(), options);
export const formatNumber = (value, options) => Number(value).toLocaleString(locale(), options);
// Para ordenar y buscar sin distinguir mayúsculas.
export const compareText = (a, b) => String(a).localeCompare(String(b), locale());
export const lowerCase = (value) => String(value).toLocaleLowerCase(currentLanguage);

const normalize = (text) => text.replace(/\s+/g, ' ').trim();
let normalizedEntries = null;
function lookupNormalized(text) {
  if (!normalizedEntries) normalizedEntries = new Map(Object.entries(en).map(([key, value]) => [normalize(key), value]));
  return normalizedEntries.get(normalize(text));
}

const TRANSLATED_ATTRIBUTES = ['placeholder', 'title', 'aria-label', 'alt'];
const SKIPPED_TAGS = new Set(['SCRIPT', 'STYLE']);

// Traduce el HTML estático. Sólo hace algo en inglés: el HTML ya está en español.
export function translateDocument(doc = globalThis.document) {
  if (!doc) return;
  doc.documentElement.lang = currentLanguage;
  if (currentLanguage === 'es') return;

  for (const element of doc.querySelectorAll('[data-i18n-html]')) {
    const translated = lookupNormalized(element.innerHTML);
    if (translated !== undefined) element.innerHTML = translated;
  }
  for (const attribute of TRANSLATED_ATTRIBUTES) {
    for (const element of doc.querySelectorAll(`[${attribute}]`)) {
      const translated = lookupNormalized(element.getAttribute(attribute));
      if (translated !== undefined) element.setAttribute(attribute, translated);
    }
  }
  const walker = doc.createTreeWalker(doc.body, 4 /* NodeFilter.SHOW_TEXT */);
  const nodes = [];
  while (walker.nextNode()) nodes.push(walker.currentNode);
  for (const node of nodes) {
    if (SKIPPED_TAGS.has(node.parentElement?.tagName)) continue;
    const original = node.nodeValue;
    if (!original.trim()) continue;
    const translated = lookupNormalized(original);
    if (translated === undefined) continue;
    // Se conserva el espacio de los extremos: separa el texto de las etiquetas vecinas.
    node.nodeValue = original.replace(original.trim(), () => translated);
  }
  const title = lookupNormalized(doc.title);
  if (title !== undefined) doc.title = title;
}

// Los <select class="lang-select"> (login, registro y menú lateral) comparten
// idioma: elegir uno lo guarda y recarga la página.
export function initLanguageSelectors(doc = globalThis.document) {
  if (!doc) return;
  for (const select of doc.querySelectorAll('select.lang-select')) {
    select.innerHTML = Object.entries(LANGUAGES).map(([code, name]) => `<option value="${code}">${name}</option>`).join('');
    select.value = currentLanguage;
    select.addEventListener('change', () => setLanguage(select.value));
  }
}

translateDocument();
initLanguageSelectors();
