// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import crypto from 'node:crypto';
import fs from 'node:fs/promises';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const panelRoot = path.resolve(here, '..');
const repositoryRoot = path.resolve(panelRoot, '..');
const sourceRoot = path.resolve(process.argv[2] || '');
const sourcePackages = path.join(sourceRoot, 'src', 'Addons');
const sourceCatalog = path.join(sourceRoot, 'docs', 'wotlk_addons.json');
const targetRoot = path.join(panelRoot, '.addon-sync');
const targetPackages = path.join(targetRoot, 'packages');

if (!process.argv[2]) {
  console.error('Uso: node tools/sync-addons.mjs /ruta/WoW-3.3.5a-Addons');
  process.exit(2);
}

const normalize = (value) => value.toLowerCase()
  .replace(/wotlk|warmane|enhanced|improved|backport|addons?|\b3[.]?3[.]?5a?\b/g, '')
  .replace(/[^a-z0-9]/g, '');
const slug = (value) => value.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '');
const cleanDescription = (value) => String(value || '')
  .replace(/\*\*/g, '').replace(/\s+/g, ' ').trim();

function categoryFor(name) {
  const value = name.toLowerCase();
  if (/quest|story|turnin|immersion|level/.test(value)) return 'Misiones y subida';
  if (/map|atlas|gather|carbonite|minimap|carto/.test(value)) return 'Mapas y exploración';
  if (/raid|dbm|boss|omen|skada|details|threat|halion|gtfo|fails|plagued|corporeality/.test(value)) return 'Bandas y combate';
  if (/arena|gladius|afflicted|losecontrol|interrupt|spellalert|spy/.test(value)) return 'JcJ';
  if (/auction|trade|market|recipe|profession|postal|gold/.test(value)) return 'Economía y profesiones';
  if (/bag|inventory|gear|item|loot|dress|mog|outfitter|equip/.test(value)) return 'Equipo e inventario';
  if (/chat|friend|ignore|wim|emote|flood|invite/.test(value)) return 'Chat y social';
  if (/ui|bar|plate|frame|tooltip|cooldown|media|domino|bartender|elv|vuhdo|healbot|grid|quartz|opie/.test(value)) return 'Interfaz';
  return 'Utilidades';
}

function runTar(args) {
  try {
    execFileSync('tar', args, { stdio: 'pipe' });
  } catch (error) {
    throw new Error(`No se pudo convertir el RAR con bsdtar (${args.join(' ')}): ${error.stderr?.toString() || error.message}`);
  }
}

async function convertRar(source, target) {
  const work = await fs.mkdtemp(path.join(await fs.realpath(process.env.TEMP || process.env.TMPDIR || '/tmp'), 'addon-rar-'));
  try {
    runTar(['-xf', source, '-C', work]);
    runTar(['-a', '-cf', target, '-C', work, '.']);
  } finally {
    await fs.rm(work, { recursive: true, force: true });
  }
}

const overrides = {
  'Auctionator+.rar': ['Auctionator completo', 'Versión completa de Auctionator para gestionar compras, ventas y búsquedas en la casa de subastas.'],
  'Auctionator_WatchList.rar': ['Auctionator WatchList', 'Añade listas de seguimiento de precios y objetos a Auctionator.'],
  'EnsidiaFails.zip': ['EnsidiaFails', 'Detecta y comunica errores evitables de los jugadores durante encuentros de banda.'],
  'MarketWatcher+.rar': ['MarketWatcher+', 'Registra precios de la casa de subastas y permite consultar su evolución.'],
  'MerfinPlus.zip': ['MerfinPlus', 'Colección de sonidos, voces, fuentes y barras para addons compatibles con LibSharedMedia.'],
  'Plagued.zip': ['Plagued', 'Ayuda de encuentro para gestionar mecánicas de plaga y avisos durante el combate.'],
  'Plugins.zip': ['Plugins para ElvUI', 'Colección de complementos opcionales para ampliar y personalizar ElvUI.'],
  'RaitingBuster.zip': ['RatingBuster', 'Compara estadísticas y muestra en los tooltips cuánto mejora o empeora una pieza de equipo.'],
};
const idOverrides = {
  'Auctionator+.rar': 'auctionator-completo',
  'Auctionator_WatchList.rar': 'auctionator-watchlist',
};

await fs.mkdir(targetPackages, { recursive: true });
const upstream = JSON.parse(await fs.readFile(sourceCatalog, 'utf8'));
const archives = (await fs.readdir(sourcePackages)).filter((name) => /\.(zip|rar)$/i.test(name)).sort((a, b) => a.localeCompare(b));
const entries = [];

for (const archive of archives) {
  const source = path.join(sourcePackages, archive);
  const outputName = archive.replace(/\.rar$/i, '-rar.zip');
  const target = path.join(targetPackages, outputName);
  if (/\.rar$/i.test(archive)) await convertRar(source, target);
  else await fs.copyFile(source, target);

  const direct = upstream.find((item) => {
    try { return decodeURIComponent(new URL(item.primary_download).pathname.split('/').pop()).toLowerCase() === archive.toLowerCase(); }
    catch { return false; }
  });
  const base = normalize(archive.replace(/\.(zip|rar)$/i, ''));
  const inferred = upstream
    .map((item) => ({ item, key: normalize(item.name) }))
    .filter(({ key }) => key && (key.includes(base) || base.includes(key)))
    .sort((a, b) => Math.abs(a.key.length - base.length) - Math.abs(b.key.length - base.length))[0]?.item;
  const metadata = direct || inferred;
  const [overrideName, overrideDescription] = overrides[archive] || [];
  const name = overrideName || metadata?.name || archive.replace(/\.(zip|rar)$/i, '');
  const description = overrideDescription || cleanDescription(metadata?.description_text) || `Addon ${name} compatible con World of Warcraft 3.3.5a.`;
  const bytes = await fs.readFile(target);
  entries.push({
    id: idOverrides[archive] || slug(archive.replace(/\.(zip|rar)$/i, '')),
    name,
    description,
    category: categoryFor(`${name} ${description}`),
    required: false,
    archive: outputName,
    size: bytes.length,
    sha256: crypto.createHash('sha256').update(bytes).digest('hex'),
    sourceUrl: metadata?.source?.[0]?.url || 'https://github.com/NoM0Re/WoW-3.3.5a-Addons',
  });
}

for (const addon of [
  ['MultiBot', 'Interfaz obligatoria para controlar y organizar los bots del servidor.', true],
  ['ServerHelp', 'Ayuda obligatoria del servidor con guías, comandos e información para el jugador.', true],
  ['PlayerBotManager', 'Gestión recomendada de equipo, GearScore y composición de banda para tu lista de playerbots. Complementa a MultiBot, no lo sustituye.', false],
]) {
  const [name, description, required] = addon;
  const sourceDirectory = path.join(repositoryRoot, 'cliente', 'Interface', 'AddOns', name);
  const target = path.join(targetPackages, `${name}.zip`);
  runTar(['-a', '-cf', target, '-C', path.dirname(sourceDirectory), name]);
  const bytes = await fs.readFile(target);
  entries.unshift({
    id: slug(name), name, description, category: 'Servidor', required,
    archive: `${name}.zip`, size: bytes.length,
    sha256: crypto.createHash('sha256').update(bytes).digest('hex'),
    sourceUrl: null,
  });
}

let sourceCommit = 'desconocido';
try { sourceCommit = execFileSync('git', ['-C', sourceRoot, 'rev-parse', 'HEAD'], { encoding: 'utf8' }).trim(); } catch {}
const catalog = {
  version: 1,
  sourceCommit,
  sourceUrl: 'https://github.com/NoM0Re/WoW-3.3.5a-Addons',
  mirroredAt: new Date().toISOString(),
  addons: entries,
};
await fs.writeFile(path.join(targetRoot, 'catalog.json'), `${JSON.stringify(catalog, null, 2)}\n`);
execFileSync(process.execPath, [path.join(here, 'extract-client-addons.mjs')], { stdio: 'inherit' });
await fs.rm(targetRoot, { recursive: true, force: true });
console.log(`Sincronizados ${entries.length} addons (${entries.filter((item) => item.required).length} obligatorios) en cliente/Interface/AddOns.`);
