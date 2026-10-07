// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import { findParam } from './serverConfigCatalog.js';

// Mismo criterio que commands.js: nada de lo que escribe el usuario llega al
// fichero de configuración sin pasar por un validador con forma fija.
export class ServerConfigError extends Error {}

const MAX_STRING_LENGTH = 255;

function normalizeFloat(raw, param) {
  const str = String(raw).trim();
  if (!/^-?\d+(\.\d+)?$/.test(str)) throw new ServerConfigError('El valor debe ser un número');
  const n = Number.parseFloat(str);
  if (!Number.isFinite(n)) throw new ServerConfigError('El valor debe ser un número finito');
  if (param.min !== null && n < param.min) throw new ServerConfigError(`El valor no puede ser menor que ${param.min}`);
  if (param.max !== null && n > param.max) throw new ServerConfigError(`El valor no puede ser mayor que ${param.max}`);
  return str;
}

function normalizeInt(raw, param) {
  const str = String(raw).trim();
  if (!/^-?\d+$/.test(str)) throw new ServerConfigError('El valor debe ser un número entero');
  const n = Number.parseInt(str, 10);
  if (!Number.isSafeInteger(n)) throw new ServerConfigError('El valor entero está fuera del rango admitido');
  if (param.min !== null && n < param.min) throw new ServerConfigError(`El valor no puede ser menor que ${param.min}`);
  if (param.max !== null && n > param.max) throw new ServerConfigError(`El valor no puede ser mayor que ${param.max}`);
  return String(n);
}

function normalizeBool(raw) {
  if (raw === true || raw === '1' || raw === 1 || raw === 'true') return '1';
  if (raw === false || raw === '0' || raw === 0 || raw === 'false') return '0';
  throw new ServerConfigError('El valor debe ser verdadero o falso');
}

function normalizeEnum(raw, param) {
  const str = String(raw).trim();
  const options = param.options || [];
  if (!options.includes(str)) throw new ServerConfigError('El valor no es una opción válida');
  return str;
}

function normalizeString(raw, param) {
  const str = typeof raw === 'string' ? raw.trim() : '';
  if (!str) throw new ServerConfigError('El valor no puede estar vacío');
  if (str.length > MAX_STRING_LENGTH) throw new ServerConfigError(`El valor supera los ${MAX_STRING_LENGTH} caracteres`);
  if (/[\r\n\t]/.test(str)) throw new ServerConfigError('El valor no puede contener saltos de línea ni tabulaciones');
  return str;
}

// Busca el parámetro en el catálogo y normaliza `rawValue` a la forma exacta
// que se escribirá en el fichero de configuración. Nunca deja pasar un
// parámetro de riesgo alto: esos se quedan de sólo lectura para todo el
// mundo, sea cual sea el gmlevel de quien llama (ver REFERENCIA_CONFIG_PANEL.md).
export function resolveAndValidate(key, file, rawValue) {
  const param = findParam(key, file);
  if (!param) throw new ServerConfigError('Parámetro no reconocido');
  if (param.risk === 'high') throw new ServerConfigError('Este parámetro es de riesgo alto: sólo se edita a mano en el servidor');

  let value;
  switch (param.type) {
    case 'float':
      value = normalizeFloat(rawValue, param);
      break;
    case 'int':
      value = normalizeInt(rawValue, param);
      break;
    case 'bool':
      value = normalizeBool(rawValue);
      break;
    case 'enum':
      value = normalizeEnum(rawValue, param);
      break;
    default:
      value = normalizeString(rawValue, param);
      break;
  }
  return { param, value };
}
