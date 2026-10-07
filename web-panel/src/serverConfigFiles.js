// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import { readFileSync } from 'node:fs';
import path from 'node:path';
import { config } from './config.js';

const KEY_LINE = /^([A-Za-z0-9_.]+)[ \t]*=[ \t]*(.*?)[ \t]*$/;

function parseConfFile(filePath) {
  let text;
  try {
    text = readFileSync(filePath, 'utf8');
  } catch {
    return null;
  }
  const values = new Map();
  for (const rawLine of text.split(/\r?\n/)) {
    const line = rawLine.trim();
    if (!line || line.startsWith('#')) continue;
    const match = KEY_LINE.exec(line);
    if (!match) continue;
    let value = match[2];
    if (value.length >= 2 && value.startsWith('"') && value.endsWith('"')) value = value.slice(1, -1);
    values.set(match[1], value);
  }
  return values;
}

// Un nombre lógico de fichero ('worldserver.conf', o el .conf de un módulo)
// resuelve a su ruta real en la VM. worldserverConfPath/moduleConfDirectory
// vacíos (fuera de la VM, en tests) hacen que la lectura caiga a null en vez
// de lanzar: el llamador usa entonces el valor por defecto del catálogo.
function resolveFilePath(fileName) {
  if (fileName === 'worldserver.conf') return config.worldserverConfPath || null;
  if (!config.moduleConfDirectory) return null;
  return path.join(config.moduleConfDirectory, fileName);
}

// Sin caché entre llamadas: son ficheros de texto que sólo se leen cuando un
// GM3 abre la pantalla de configuración, no en cada petición de la app.
// Cachear arriesgaría mostrar un valor obsoleto justo después de que el
// script privilegiado aplique un cambio y reinicie.
function loadFile(fileName) {
  const filePath = resolveFilePath(fileName);
  if (!filePath) return null;
  return parseConfFile(filePath);
}

export function readCurrentValue(fileName, key) {
  const values = loadFile(fileName);
  if (!values) return null;
  return values.has(key) ? values.get(key) : null;
}

// Agrupa la lectura de varias claves del mismo fichero en un único parseo.
export function readCurrentValues(fileName, keys) {
  const values = loadFile(fileName);
  const result = new Map();
  for (const key of keys) result.set(key, values?.get(key) ?? null);
  return result;
}
