// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import JSZip from 'jszip';

const here = path.dirname(fileURLToPath(import.meta.url));
const panelRoot = path.resolve(here, '..');
const repositoryRoot = path.resolve(panelRoot, '..');
const syncRoot = path.join(panelRoot, '.addon-sync');
const packagesRoot = path.join(syncRoot, 'packages');
const clientAddonsRoot = path.join(repositoryRoot, 'cliente', 'Interface', 'AddOns');
const protectedAddons = new Set(['multibot', 'serverhelp']);
const catalog = JSON.parse(await fs.readFile(path.join(syncRoot, 'catalog.json'), 'utf8'));
const translations = JSON.parse(await fs.readFile(path.join(panelRoot, 'addons', 'descriptions-es.json'), 'utf8'));
const allClasses = ['Guerrero', 'Paladín', 'Cazador', 'Pícaro', 'Sacerdote', 'Caballero de la Muerte', 'Chamán', 'Mago', 'Brujo', 'Druida'];
const universalRecommendations = new Set([
  'atlasloot', 'bartender4', 'cooldowns', 'dbm', 'details', 'gearscore', 'gearscorelite',
  'gtfo', 'needtoknow', 'omen', 'quartz', 'skada', 'tellmewhen', 'tidyplates',
]);
const classRecommendations = {
  magenuggets: ['Mago'],
  pallypower: ['Paladín'],
  fortexorcist: ['Sacerdote', 'Brujo'],
  smarttrack: ['Cazador'],
  decursive: ['Paladín', 'Sacerdote', 'Chamán', 'Mago', 'Druida'],
  healbot: ['Paladín', 'Sacerdote', 'Chamán', 'Druida'],
  vuhdo: ['Paladín', 'Sacerdote', 'Chamán', 'Druida'],
  grid2: ['Paladín', 'Sacerdote', 'Chamán', 'Druida'],
  clique: ['Paladín', 'Sacerdote', 'Chamán', 'Druida'],
  redeemer: ['Paladín', 'Sacerdote', 'Chamán', 'Druida'],
  interruptbar: ['Guerrero', 'Paladín', 'Cazador', 'Pícaro', 'Sacerdote', 'Caballero de la Muerte', 'Chamán', 'Mago', 'Brujo', 'Druida'],
  snowfallkeypress: ['Guerrero', 'Paladín', 'Cazador', 'Pícaro', 'Sacerdote', 'Caballero de la Muerte', 'Chamán', 'Mago', 'Brujo', 'Druida'],
};

async function directorySize(directory) {
  let size = 0;
  for (const entry of await fs.readdir(directory, { withFileTypes: true })) {
    const target = path.join(directory, entry.name);
    if (entry.isDirectory()) size += await directorySize(target);
    else if (entry.isFile()) size += (await fs.stat(target)).size;
  }
  return size;
}

function normalizedEntries(zip) {
  const entries = Object.values(zip.files).filter((entry) => !entry.dir)
    .map((entry) => ({ entry, name: entry.name.replace(/^\.\//, '').replaceAll('\\', '/') }))
    .filter(({ name }) => name && !name.startsWith('__MACOSX/') && !name.endsWith('/.DS_Store'))
    .filter(({ name }) => !name.split('/').some((part) => part === '.git' || part === '.github'));
  if (entries.some(({ name }) => name.startsWith('/') || name.split('/').some((part) => !part || part === '..' || part.includes(':')))) {
    throw new Error('El paquete contiene una ruta no segura');
  }
  const tocPaths = entries.map(({ name }) => name).filter((name) => name.toLowerCase().endsWith('.toc'));
  if (!tocPaths.length) throw new Error('El paquete no contiene archivos .toc');
  const first = entries[0]?.name.split('/')[0];
  const stripWrapper = first && entries.every(({ name }) => name.split('/')[0] === first) && tocPaths.every((name) => name.split('/').length >= 3);
  const unwrapped = entries.map(({ entry, name }) => ({ entry, name: stripWrapper ? name.split('/').slice(1).join('/') : name }));
  const addonRoots = new Set(unwrapped
    .filter(({ name }) => name.toLowerCase().endsWith('.toc'))
    .map(({ name }) => name.split('/')[0].toLowerCase()));
  return unwrapped.filter(({ name }) => addonRoots.has(name.split('/')[0].toLowerCase()));
}

await fs.mkdir(clientAddonsRoot, { recursive: true });
const writtenRoots = new Set();
let writtenFiles = 0;

for (const addon of catalog.addons) {
  if (!translations[addon.id]) throw new Error(`Falta la descripción española de ${addon.id}`);
  addon.description = translations[addon.id];
  addon.classes = universalRecommendations.has(addon.id) ? allClasses : (classRecommendations[addon.id] || []);
  // Ojo: esto es "es uno de nuestros propios addons, léelo tal cual del
  // cliente" (MultiBot/ServerHelp), no "es obligatorio". BugSack y ACP
  // también son obligatorios pero vienen del mirror de NoM0Re como los demás
  // -con su propio archive/sha256 y, en el caso de BugSack, dos carpetas
  // (!BugGrabber + BugSack)-, así que deben seguir la rama normal de abajo.
  if (protectedAddons.has(addon.id)) {
    addon.directories = [addon.name];
    addon.size = await directorySize(path.join(clientAddonsRoot, addon.name));
    delete addon.archive;
    delete addon.sha256;
    continue;
  }
  const zip = await JSZip.loadAsync(await fs.readFile(path.join(packagesRoot, addon.archive)));
  const entries = normalizedEntries(zip);
  addon.directories = [...new Set(entries.map(({ name }) => name.split('/')[0]))]
    .filter((name) => !protectedAddons.has(name.toLowerCase()))
    .sort((a, b) => a.localeCompare(b));
  addon.size = entries.filter(({ name }) => addon.directories.includes(name.split('/')[0]))
    .reduce((sum, { entry }) => sum + (entry._data?.uncompressedSize || 0), 0);
  delete addon.archive;
  delete addon.sha256;
  for (const { entry, name } of entries) {
    const parts = name.split('/');
    const rootName = parts[0];
    if (protectedAddons.has(rootName.toLowerCase())) continue;
    const target = path.join(clientAddonsRoot, ...parts);
    const relative = path.relative(clientAddonsRoot, target);
    if (relative.startsWith('..') || path.isAbsolute(relative)) throw new Error(`Ruta fuera de AddOns: ${name}`);
    await fs.mkdir(path.dirname(target), { recursive: true });
    await fs.writeFile(target, await entry.async('nodebuffer'));
    writtenRoots.add(rootName);
    writtenFiles += 1;
  }
}

await fs.mkdir(path.join(panelRoot, 'addons'), { recursive: true });
await fs.writeFile(path.join(panelRoot, 'addons', 'catalog.json'), `${JSON.stringify(catalog, null, 2)}\n`);
console.log(`Extraídos ${writtenFiles} archivos en ${writtenRoots.size} carpetas; MultiBot y ServerHelp se conservaron.`);
