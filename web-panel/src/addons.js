// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import fs from 'node:fs';
import path from 'node:path';
import { createHash } from 'node:crypto';
import { fileURLToPath } from 'node:url';
import archiver from 'archiver';
import { config } from './config.js';

const directory = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', 'addons');
const catalog = JSON.parse(fs.readFileSync(path.join(directory, 'catalog.json'), 'utf8'));
const clientAddonsDirectory = path.resolve(config.addonsDirectory);
// Descripciones en inglés por id de addon (catalog.json lleva las españolas). Si falta una, se sirve la española.
const descriptionsEn = JSON.parse(fs.readFileSync(path.join(directory, 'descriptions-en.json'), 'utf8'));

if (!Array.isArray(catalog.addons)) throw new Error('El catálogo de addons no es válido');

// Recorre un directorio y devuelve sus archivos con ruta relativa en POSIX,
// en orden estable (mismo criterio de comparación de cadenas que usa el
// navegador con Array.prototype.sort por defecto), para que el hash de
// contenido salga igual sin importar el sistema de archivos.
function listFilesSorted(rootPath) {
  const results = [];
  (function walk(currentPath, relativePath) {
    for (const entry of fs.readdirSync(currentPath, { withFileTypes: true })) {
      const absolutePath = path.join(currentPath, entry.name);
      const entryRelativePath = relativePath ? `${relativePath}/${entry.name}` : entry.name;
      if (entry.isDirectory()) walk(absolutePath, entryRelativePath);
      else if (entry.isFile()) results.push({ relativePath: entryRelativePath, absolutePath });
    }
  }(rootPath, ''));
  results.sort((a, b) => (a.relativePath < b.relativePath ? -1 : a.relativePath > b.relativePath ? 1 : 0));
  return results;
}

// contentVersion identifica el contenido real instalado en cliente/Interface/AddOns
// en este arranque del panel: el cliente del jugador recorre sus propias carpetas
// con el mismo esquema (nombre de carpeta + ruta relativa + salto de línea +
// contenido de cada archivo) para saber, sin manifiesto propio, si lo que tiene
// instalado sigue siendo exactamente esto o si hay una actualización pendiente.
function computeContentVersion(directoryPaths) {
  const hash = createHash('sha256');
  for (const { name, directoryPath } of directoryPaths) {
    for (const { relativePath, absolutePath } of listFilesSorted(directoryPath)) {
      hash.update(`${name}/${relativePath}\n`);
      hash.update(fs.readFileSync(absolutePath));
      hash.update('\n');
    }
  }
  return hash.digest('hex');
}

const byId = new Map();
for (const addon of catalog.addons) {
  if (!addon.id || byId.has(addon.id) || !Array.isArray(addon.directories) || !addon.directories.length) {
    throw new Error(`Entrada de addon no válida o duplicada: ${addon.id || '(sin id)'}`);
  }
  const directoryPaths = addon.directories.map((name) => {
    if (path.basename(name) !== name || name === '.' || name === '..') throw new Error(`Carpeta de addon no válida: ${name}`);
    const directoryPath = path.join(clientAddonsDirectory, name);
    if (!fs.statSync(directoryPath).isDirectory()) throw new Error(`Falta la carpeta de addon: ${name}`);
    return { name, directoryPath };
  });
  const contentVersion = computeContentVersion(directoryPaths);
  byId.set(addon.id, { ...addon, directoryPaths, contentVersion });
}

export function addonCatalog(lang = 'es') {
  return {
    version: catalog.version,
    sourceCommit: catalog.sourceCommit,
    sourceUrl: catalog.sourceUrl,
    mirroredAt: catalog.mirroredAt,
    addons: catalog.addons.map((addon) => ({
      ...addon,
      description: (lang === 'en' && descriptionsEn[addon.id]) || addon.description,
      contentVersion: byId.get(addon.id).contentVersion,
      downloadUrl: `/api/addons/${encodeURIComponent(addon.id)}/download`,
    })),
  };
}

export function findAddon(id) {
  return byId.get(id) || null;
}

export function sendAddonZip(addon, response, next) {
  response.attachment(`${addon.id}.zip`);
  response.set('Cache-Control', 'private, max-age=3600');
  const zip = archiver('zip', { zlib: { level: 6 } });
  zip.on('warning', (error) => console.warn(`Aviso creando ${addon.id}.zip`, error));
  zip.on('error', next);
  response.on('close', () => { if (!response.writableEnded) zip.abort(); });
  zip.pipe(response);
  for (const item of addon.directoryPaths) zip.directory(item.directoryPath, item.name);
  zip.finalize();
}
