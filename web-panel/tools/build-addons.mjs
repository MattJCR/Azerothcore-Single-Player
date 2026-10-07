#!/usr/bin/env node
// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Construye cliente/Interface/AddOns a partir de fuentes fijadas, sin depender de
// una copia preparada: paquetes de NoM0Re por commit y SHA-256, repositorios de
// addons por commit (addons.lock), parches propios de patches-cliente/ y los
// textos en español de los .toc. El resultado debe coincidir con arbol.tsv.
//
//   node tools/build-addons.mjs build     [--destino DIR] [--cache DIR] [--solo A,B] [--sin-red] [--receta DIR]
//   node tools/build-addons.mjs verificar [--destino DIR] [--estricto]
//   node tools/build-addons.mjs arbol     [--destino DIR] > addons/arbol.tsv
//
// `build` trabaja en una carpeta temporal DENTRO de destino, comprueba el árbol
// esperado y sólo entonces sustituye las carpetas una a una; un fallo de
// descarga, de hash o de parche deja el destino como estaba.
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import JSZip from 'jszip';
import { localizeTocs } from './localize-addon-tocs.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const panelRoot = path.resolve(here, '..');
const repositoryRoot = path.resolve(panelRoot, '..');
let addonsRoot = path.join(panelRoot, 'addons');
// Mismo criterio que Git para decidir si un fichero es texto: sin ningún NUL en sus
// primeros 8000 bytes. Git normaliza CRLF a LF al comitear los textos, así que el mismo
// addon llega con distinto fin de línea según salga de un paquete, de una copia de
// trabajo de Windows o de un `git archive`; el contenido es el mismo.
const isText = (data) => !data.subarray(0, 8000).includes(0);

const [command = '', ...rest] = process.argv.slice(2);
const options = { destino: path.join(repositoryRoot, 'cliente', 'Interface', 'AddOns'), cache: path.join(repositoryRoot, '.instalacion', 'cache', 'addons'), solo: null, sinRed: false, estricto: false };
for (let index = 0; index < rest.length; index += 1) {
  const arg = rest[index];
  if (arg === '--destino') options.destino = path.resolve(rest[++index]);
  else if (arg === '--cache') options.cache = path.resolve(rest[++index]);
  else if (arg === '--solo') options.solo = new Set(rest[++index].split(',').filter(Boolean));
  else if (arg === '--receta') addonsRoot = path.resolve(rest[++index]);
  else if (arg === '--sin-red') options.sinRed = true;
  else if (arg === '--estricto') options.estricto = true;
  else fail(`Opción desconocida: ${arg}`);
}

function fail(message) {
  console.error(`build-addons: ${message}`);
  process.exit(1);
}

const readJson = (file) => JSON.parse(fs.readFileSync(file, 'utf8'));
const sha256 = (data) => crypto.createHash('sha256').update(data).digest('hex');

// ---------------------------------------------------------------- hashes
function listFiles(root) {
  const result = [];
  (function walk(current, relative) {
    for (const entry of fs.readdirSync(current, { withFileTypes: true })) {
      const absolute = path.join(current, entry.name);
      const rel = relative ? `${relative}/${entry.name}` : entry.name;
      if (entry.isDirectory()) walk(absolute, rel);
      else if (entry.isFile()) result.push({ rel, absolute });
    }
  }(root, ''));
  return result;
}

// Hash del contenido de una carpeta de addon. Rutas en minúsculas (Windows no
// distingue Libs/libs, un paquete sí) y CRLF→LF en texto (Git normaliza los
// .lua/.toc al comitear; el resultado funcional es el mismo).
export function directoryHash(root, name) {
  const files = listFiles(path.join(root, name)).map(({ rel, absolute }) => ({ key: `${name}/${rel}`.toLowerCase(), absolute }));
  files.sort((a, b) => (a.key < b.key ? -1 : a.key > b.key ? 1 : 0));
  const hash = crypto.createHash('sha256');
  let bytes = 0;
  for (const { key, absolute } of files) {
    let data = fs.readFileSync(absolute);
    if (isText(data)) data = Buffer.from(data.toString('latin1').replace(/\r\n/g, '\n'), 'latin1');
    hash.update(`${key}\n`);
    hash.update(data);
    hash.update('\n');
    bytes += data.length;
  }
  return { sha256: hash.digest('hex'), files: files.length, bytes };
}

function expectedTree() {
  const rows = new Map();
  for (const line of fs.readFileSync(path.join(addonsRoot, 'arbol.tsv'), 'utf8').split(/\r?\n/)) {
    if (!line || line.startsWith('#')) continue;
    const [name, hash, files] = line.split('\t');
    rows.set(name, { sha256: hash, files: Number(files) });
  }
  return rows;
}

function topDirectories(root) {
  return fs.existsSync(root) ? fs.readdirSync(root, { withFileTypes: true }).filter((entry) => entry.isDirectory() && !entry.name.startsWith('.')).map((entry) => entry.name) : [];
}

// ---------------------------------------------------------------- descargas
function git(args, cwd, extra = {}) {
  return execFileSync('git', ['-c', 'core.autocrlf=false', '-c', 'core.eol=lf', ...args], {
    cwd, stdio: ['ignore', 'pipe', 'pipe'], env: { ...process.env, GIT_TERMINAL_PROMPT: '0', GIT_CEILING_DIRECTORIES: path.dirname(cwd), ...extra },
  });
}

async function download(url, target, expectedSha) {
  if (fs.existsSync(target)) {
    if (sha256(fs.readFileSync(target)) === expectedSha) return;
    fs.rmSync(target, { force: true }); // una caché no sustituye la comprobación del hash
  }
  if (options.sinRed) throw new Error(`Falta ${path.basename(target)} en la caché y --sin-red está activo`);
  let lastError;
  for (let attempt = 1; attempt <= 3; attempt += 1) {
    try {
      const response = await fetch(url, { redirect: 'follow' });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const data = Buffer.from(await response.arrayBuffer());
      if (sha256(data) !== expectedSha) throw new Error('el SHA-256 no coincide con la receta');
      fs.mkdirSync(path.dirname(target), { recursive: true });
      const partial = `${target}.part`;
      fs.writeFileSync(partial, data);
      fs.renameSync(partial, target);
      return;
    } catch (error) {
      lastError = error;
      if (/SHA-256/.test(error.message)) break; // reintentar no arregla un hash distinto
    }
  }
  throw new Error(`No se pudo obtener ${url}: ${lastError.message}`);
}

// bsdtar (libarchive) lee RAR; en Windows es tar.exe de System32, y el tar de
// GNU que pueda haber antes en el PATH no sirve.
const archiveTools = ['bsdtar', ...(process.platform === 'win32' ? [path.join(process.env.SystemRoot || 'C:\Windows', 'System32', 'tar.exe')] : []), 'tar'];

function rarToZip(rarPath, work) {
  const unpacked = fs.mkdtempSync(path.join(work, 'rar-'));
  let ok = false;
  for (const tool of archiveTools) {
    try { execFileSync(tool, ['-xf', rarPath, '-C', unpacked], { stdio: 'pipe' }); ok = true; break; } catch { /* siguiente */ }
  }
  if (!ok) throw new Error(`No se pudo abrir ${path.basename(rarPath)}: instala bsdtar (libarchive-tools)`);
  const zipPath = path.join(work, `${path.basename(rarPath)}.zip`);
  for (const tool of archiveTools) {
    try { execFileSync(tool, ['-a', '-cf', zipPath, '-C', unpacked, '.'], { stdio: 'pipe' }); return zipPath; } catch { /* siguiente */ }
  }
  throw new Error('No se pudo reempaquetar el RAR como ZIP');
}

// Mismas reglas que extract-client-addons.mjs (rutas seguras, sin __MACOSX/.git,
// se quita la carpeta envoltorio y sólo se conservan carpetas con .toc).
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
  const roots = new Set(unwrapped.filter(({ name }) => name.toLowerCase().endsWith('.toc')).map(({ name }) => name.split('/')[0].toLowerCase()));
  return unwrapped.filter(({ name }) => roots.has(name.split('/')[0].toLowerCase()));
}

// ---------------------------------------------------------------- construcción
// Permisos de lectura para todos: el panel corre como DynamicUser y las carpetas que
// salen de un checkout temporal (mkdtemp crea 0700) o de un ZIP no los traían.
function normalizeModes(root) {
  fs.chmodSync(root, 0o755);
  for (const entry of fs.readdirSync(root, { withFileTypes: true })) {
    const target = path.join(root, entry.name);
    if (entry.isDirectory()) normalizeModes(target);
    else if (entry.isFile()) fs.chmodSync(target, 0o644);
  }
}

function applyPatches(stage, name, patches) {
  for (const patch of patches || []) {
    const file = path.join(repositoryRoot, patch);
    if (!fs.existsSync(file)) throw new Error(`Falta el parche ${patch}`);
    const directory = path.join(stage, name);
    try {
      git(['apply', '-p3', '--unsafe-paths', '--whitespace=nowarn', file], directory);
    } catch (error) {
      throw new Error(`El parche ${patch} no aplica sobre ${name}: ${error.stderr?.toString().trim() || error.message}`);
    }
  }
}

async function build() {
  const recipe = readJson(path.join(addonsRoot, 'fuentes.json'));
  const expected = expectedTree();
  const wanted = (name) => !options.solo || options.solo.has(name);
  fs.mkdirSync(options.destino, { recursive: true });
  const stage = fs.mkdtempSync(path.join(options.destino, '.build-'));
  const work = fs.mkdtempSync(path.join(stage, '.work-'));
  const built = new Set();
  const started = Date.now();
  try {
    // 1. Paquetes de NoM0Re
    for (const item of recipe.paquetes) {
      const dirs = item.directorios.filter(wanted);
      if (!dirs.length) continue;
      const url = `${recipe.origen.raw}/${item.commit}/${recipe.origen.ruta}/${encodeURIComponent(item.archivo)}`;
      const cached = path.join(options.cache, `${item.commit.slice(0, 12)}-${item.archivo}`);
      await download(url, cached, item.sha256);
      let archive = cached;
      if (/\.rar$/i.test(item.archivo)) archive = rarToZip(cached, work);
      const zip = await JSZip.loadAsync(fs.readFileSync(archive));
      for (const { entry, name } of normalizedEntries(zip)) {
        const parts = name.split('/');
        if (!dirs.includes(parts[0])) continue;
        const target = path.join(stage, ...parts);
        if (path.relative(stage, target).startsWith('..')) throw new Error(`Ruta fuera de AddOns: ${name}`);
        fs.mkdirSync(path.dirname(target), { recursive: true });
        fs.writeFileSync(target, await entry.async('nodebuffer'));
      }
      for (const dir of dirs) built.add(dir);
    }
    // 2. Repositorios de addons (commit fijado en addons.lock)
    const lock = new Map(fs.readFileSync(path.join(repositoryRoot, 'addons.lock'), 'utf8').split(/\r?\n/)
      .filter((line) => line && !line.startsWith('#')).map((line) => { const [name, , commit, , url] = line.split('\t'); return [name, { commit, url }]; }));
    for (const repo of recipe.repos) {
      if (!wanted(repo.nombre)) continue;
      const source = lock.get(repo.nombre);
      if (!source) throw new Error(`${repo.nombre} no está en addons.lock`);
      if (options.sinRed) throw new Error(`${repo.nombre} necesita red (--sin-red)`);
      const checkout = fs.mkdtempSync(path.join(work, 'repo-'));
      git(['init', '-q', '.'], checkout);
      git(['fetch', '-q', '--depth', '1', '--no-tags', source.url, source.commit], checkout);
      const head = git(['rev-parse', 'FETCH_HEAD^{commit}'], checkout).toString().trim();
      if (head !== source.commit) throw new Error(`${repo.nombre}: se obtuvo ${head}, addons.lock fija ${source.commit}`);
      git(['checkout', '-q', 'FETCH_HEAD'], checkout);
      const from = path.join(checkout, repo.desde || '.');
      const target = path.join(stage, repo.nombre);
      fs.cpSync(from, target, { recursive: true, filter: (src) => {
        const rel = path.relative(from, src).replaceAll('\\', '/');
        return rel !== '.git' && !rel.startsWith('.git/') && !(repo.excluir || []).some((skip) => rel === skip || rel.startsWith(`${skip}/`));
      } });
      built.add(repo.nombre);
    }
    // 3. Parches propios
    for (const name of built) applyPatches(stage, name, recipe.parches[name]);
    // 4. Notas en español de los .toc
    const catalog = readJson(path.join(addonsRoot, 'catalog.json'));
    const descriptions = readJson(path.join(addonsRoot, 'descriptions-es.json'));
    localizeTocs({ clientAddonsRoot: stage, catalog, descriptions, only: built });
    for (const name of built) normalizeModes(path.join(stage, name));
    // 5. Comprobar contra el árbol esperado antes de tocar el destino
    const problems = [];
    for (const name of built) {
      const want = expected.get(name);
      const got = directoryHash(stage, name);
      if (!want) problems.push(`${name}: no figura en arbol.tsv`);
      else if (want.sha256 !== got.sha256) problems.push(`${name}: el contenido construido no coincide con arbol.tsv`);
    }
    if (problems.length) throw new Error(`El resultado no coincide con la receta:\n  ${problems.join('\n  ')}`);
    // 6. Publicar carpeta a carpeta (la anterior se conserva hasta el final)
    const replaced = [];
    try {
      for (const name of built) {
        const final = path.join(options.destino, name);
        const old = path.join(stage, `.old-${name}`);
        if (fs.existsSync(final)) { fs.renameSync(final, old); replaced.push([final, old]); }
        else replaced.push([final, null]);
        fs.renameSync(path.join(stage, name), final);
      }
    } catch (error) {
      for (const [final, old] of replaced.reverse()) {
        fs.rmSync(final, { recursive: true, force: true });
        if (old && fs.existsSync(old)) fs.renameSync(old, final);
      }
      throw error;
    }
    console.log(`build-addons: ${built.size} addons construidos y verificados en ${Math.round((Date.now() - started) / 1000)} s.`);
  } finally {
    fs.rmSync(stage, { recursive: true, force: true });
  }
}

function verify() {
  const expected = expectedTree();
  const present = new Set(topDirectories(options.destino));
  const missing = [];
  const altered = [];
  let ok = 0;
  for (const [name, want] of expected) {
    if (!present.has(name)) { missing.push(name); continue; }
    if (directoryHash(options.destino, name).sha256 === want.sha256) ok += 1;
    else altered.push(name);
  }
  const extra = [...present].filter((name) => !expected.has(name));
  console.log(`build-addons: ${ok}/${expected.size} carpetas coinciden con arbol.tsv; ausentes ${missing.length}, alteradas ${altered.length}, sobrantes ${extra.length}.`);
  if (missing.length) console.log(`  ausentes: ${missing.join(', ')}`);
  if (altered.length) console.log(`  alteradas: ${altered.join(', ')}`);
  if (extra.length) console.log(`  sobrantes: ${extra.join(', ')}`);
  process.exit(missing.length || altered.length || (options.estricto && extra.length) ? 1 : 0);
}

function tree() {
  const names = topDirectories(options.destino).sort((a, b) => (a < b ? -1 : 1));
  const lines = ['# directorio\tsha256\tarchivos\tbytes — contenido esperado de cliente/Interface/AddOns (build-addons.mjs arbol)'];
  for (const name of names) {
    const { sha256: hash, files, bytes } = directoryHash(options.destino, name);
    lines.push(`${name}\t${hash}\t${files}\t${bytes}`);
  }
  process.stdout.write(`${lines.join('\n')}\n`);
}

if (command === 'build') await build();
else if (command === 'verificar') verify();
else if (command === 'arbol') tree();
else fail('Uso: build-addons.mjs build|verificar|arbol [--destino DIR] [--cache DIR] [--solo A,B] [--sin-red] [--estricto]');
