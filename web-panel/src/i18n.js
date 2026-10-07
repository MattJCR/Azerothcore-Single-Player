// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Localización de los mensajes de la API. Igual que en el cliente (public/i18n.js),
// el español es el idioma de origen: el código sigue escribiendo sus errores en
// español y locales/en.js sólo guarda la traducción al inglés. El cliente manda
// su idioma en la cabecera X-Panel-Lang; sin ella (curl, pruebas) se contesta en
// español, que es lo que escribe el código.
//
// Las claves con {marcador} son plantillas: sirven para los mensajes que
// llevan un valor dentro (`El valor no puede ser menor que 5`). Cada valor
// capturado se traduce a su vez si es una frase conocida (`El motivo`), así una
// sola plantilla `{label} no puede estar vacío` cubre todos los campos.
import en from './locales/en.js';
import configDescriptions from './locales/config-en.js';

export const DEFAULT_LANGUAGE = 'es';

// Campos de una respuesta JSON cuyo valor es un texto para mostrar al usuario.
const TRANSLATED_KEYS = new Set(['error', 'mensaje', 'detalle', 'detail', 'label', 'categoryLabel', 'pinnedDate']);
// Campos de texto libre que también pueden venir de la base de datos: sólo se
// traducen si el texto entero está en el diccionario, sin plantillas.
const EXACT_ONLY_KEYS = new Set(['description']);
const MAX_DEPTH = 6;

const escapeRegex = (text) => text.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');

const exact = new Map();
const templates = [];
for (const [spanish, english] of Object.entries(en)) {
  if (!/\{\w+\}/.test(spanish)) {
    exact.set(spanish, english);
    continue;
  }
  const pattern = escapeRegex(spanish).replace(/\\\{(\w+)\\\}/g, '(?<$1>.+?)');
  templates.push({ regex: new RegExp(`^${pattern}$`, 's'), english, specificity: spanish.replace(/\{\w+\}/g, '').length });
}
// La plantilla con más texto fijo gana: «El valor supera los {max} caracteres»
// antes que «{label} supera los {max} caracteres».
templates.sort((a, b) => b.specificity - a.specificity);

export function requestLanguage(request) {
  return request.get('X-Panel-Lang') === 'en' ? 'en' : DEFAULT_LANGUAGE;
}

export function translate(message, lang) {
  if (lang === DEFAULT_LANGUAGE || typeof message !== 'string') return message;
  const direct = exact.get(message);
  if (direct !== undefined) return direct;
  for (const { regex, english } of templates) {
    const match = regex.exec(message);
    if (!match) continue;
    return english.replace(/\{(\w+)\}/g, (placeholder, name) => (
      match.groups?.[name] === undefined ? placeholder : translate(match.groups[name], lang)
    ));
  }
  return message;
}

const translateExact = (message, lang) => (lang === DEFAULT_LANGUAGE ? message : (exact.get(message) ?? message));

function localizeValue(value, lang, depth) {
  if (depth > MAX_DEPTH || value === null || typeof value !== 'object') return value;
  if (Array.isArray(value)) return value.map((item) => localizeValue(item, lang, depth + 1));
  if (Object.getPrototypeOf(value) !== Object.prototype) return value;
  const result = {};
  for (const [key, item] of Object.entries(value)) {
    if (typeof item === 'string' && TRANSLATED_KEYS.has(key)) result[key] = translate(item, lang);
    else if (typeof item === 'string' && EXACT_ONLY_KEYS.has(key)) result[key] = translateExact(item, lang);
    else result[key] = localizeValue(item, lang, depth + 1);
  }
  return result;
}

export const localizeBody = (body, lang) => (lang === DEFAULT_LANGUAGE ? body : localizeValue(body, lang, 0));

// Fija request.lang y hace que response.json() traduzca los textos de la respuesta.
export function languageMiddleware(request, response, next) {
  request.lang = requestLanguage(request);
  if (request.lang !== DEFAULT_LANGUAGE) {
    const json = response.json.bind(response);
    response.json = (body) => json(localizeBody(body, request.lang));
  }
  next();
}

// Descripción de un parámetro del catálogo de configuración, en el idioma de la
// petición (la clave del .conf es única en todo el catálogo).
export const paramDescription = (param, request) => (request.lang === 'en' ? configDescriptions[param.key] ?? param.description : param.description) || '';

// item_template_locale no tiene filas enUS: sin coincidencia el nombre sale de
// item_template.name, que es el original en inglés.
export const itemLocale = (request) => (request.lang === 'en' ? 'enUS' : 'esES');
