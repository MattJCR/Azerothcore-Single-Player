#!/usr/bin/env node
// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Deja en --salida los cuatro mapas del panel (<id>.jpg) verificados por SHA-256.
//   node tools/fetch-maps.mjs --salida DIR [--desde DIR]...
// --desde: carpetas con mapas ya preparados (por ejemplo public/assets/maps de un
// repositorio con datos); se usan sólo si su hash cuadra. Lo que falta se descarga.
// Un mapa que no se consigue con el hash exacto no se instala: se avisa y el panel
// lo muestra pendiente (fondo neutro) en vez de usar una versión distinta.
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const recipe = JSON.parse(fs.readFileSync(path.join(here, '..', 'addons', 'mapas.json'), 'utf8'));
let output = '';
const sources = [];
const args = process.argv.slice(2);
for (let index = 0; index < args.length; index += 1) {
  if (args[index] === '--salida') output = path.resolve(args[++index]);
  else if (args[index] === '--desde') sources.push(path.resolve(args[++index]));
  else { console.error(`Opción desconocida: ${args[index]}`); process.exit(2); }
}
if (!output) { console.error('Uso: fetch-maps.mjs --salida DIR [--desde DIR]...'); process.exit(2); }

const pause = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const sha256 = (data) => crypto.createHash('sha256').update(data).digest('hex');
fs.mkdirSync(output, { recursive: true });
let missing = 0;
for (const map of recipe.mapas) {
  const target = path.join(output, `${map.id}.jpg`);
  if (fs.existsSync(target) && sha256(fs.readFileSync(target)) === map.sha256) { console.log(`mapa ${map.id}: ya está y cuadra`); continue; }
  let data = null;
  for (const directory of sources) {
    const file = path.join(directory, `${map.id}.jpg`);
    if (fs.existsSync(file) && sha256(fs.readFileSync(file)) === map.sha256) { data = fs.readFileSync(file); break; }
  }
  let origin = 'preparado';
  if (!data) {
    origin = 'descargado';
    // La CDN de la wiki sirve a veces una variante recomprimida (otros bytes) según la ruta de caché
    // y el momento: se prueba la URL tal cual y después con un parámetro de caché distinto, y sólo se
    // acepta el fichero con el SHA-256 fijado. Si ninguna variante lo da, el mapa queda pendiente.
    let others = 0;
    for (let round = 1; round <= 3 && !data; round += 1) {
      for (const suffix of recipe.variantes || ['']) {
        for (let attempt = 1; attempt <= 2 && !data; attempt += 1) {
          try {
            const response = await fetch(`${recipe.base}${map.ruta}${suffix}`, { headers: { 'User-Agent': recipe.userAgent }, redirect: 'follow' });
            if (!response.ok) continue;
            const bytes = Buffer.from(await response.arrayBuffer());
            if (sha256(bytes) === map.sha256) data = bytes;
            else { others += 1; break; }
          } catch { /* reintento */ }
        }
        if (data) break;
        await pause(1000);
      }
      // La variante servida cambia con el tiempo (también entre pasadas de unos segundos).
      if (!data && round < 3) await pause(8000);
    }
    if (!data && others) console.error(`mapa ${map.id}: la fuente entrega otros bytes (SHA-256 distinto) en ${others} intentos; no se instala`);
  }
  if (!data) { console.error(`mapa ${map.id} (${map.nombre}): PENDIENTE`); missing += 1; continue; }
  fs.writeFileSync(`${target}.parte`, data);
  fs.renameSync(`${target}.parte`, target);
  console.log(`mapa ${map.id}: ${origin} y verificado`);
}
process.exit(missing ? 3 : 0);
