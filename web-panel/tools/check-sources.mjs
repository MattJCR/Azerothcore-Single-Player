// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// check-sources.mjs — descubre TODOS los .js propios (src/ y public/) y les
// pasa `node --check`, en vez de la lista manual que tenía `npm run check`
//: una fuente nueva podía quedar fuera del chequeo de
// sintaxis sin que nada lo avisara. Al escribir este script, la lista
// manual ya se había quedado corta de verdad: config.js, roles.js,
// serverStatus.js, session.js, srp6.js y public/map-projection.js no
// estaban.
import fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';

const here = path.dirname(fileURLToPath(import.meta.url));
const panelRoot = path.resolve(here, '..');

async function collectJsFiles(dir) {
  const found = [];
  let entries;
  try {
    entries = await fs.readdir(dir, { withFileTypes: true });
  } catch (error) {
    if (error.code === 'ENOENT') return found;
    throw error;
  }
  for (const entry of entries) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) {
      found.push(...await collectJsFiles(full));
    } else if (entry.isFile() && entry.name.endsWith('.js')) {
      found.push(full);
    }
  }
  return found;
}

const targets = [
  ...await collectJsFiles(path.join(panelRoot, 'src')),
  ...await collectJsFiles(path.join(panelRoot, 'public')),
].sort();

if (targets.length === 0) {
  console.error('check-sources.mjs: no se encontró ningún .js en src/ ni public/ — ¿ruta equivocada?');
  process.exit(2);
}

const failed = [];
for (const file of targets) {
  const result = spawnSync(process.execPath, ['--check', file], { stdio: 'inherit' });
  if (result.status !== 0) failed.push(file);
}

if (failed.length > 0) {
  console.error(`\ncheck-sources.mjs: ${failed.length}/${targets.length} ficheros con error de sintaxis.`);
  process.exit(1);
}

console.log(`check-sources.mjs: ${targets.length} ficheros .js (src/ + public/) sin errores de sintaxis.`);
