#!/usr/bin/env node
// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Informe de actualizaciones de addons de cliente, pensado para que
// lib/addon-versions.sh lo fusione con el de lib/versions.sh (versions.lock)
// antes de escribir el fichero único que lee mod-update-notice. Sin esto,
// mod-update-notice sólo avisaba de core/módulos y nunca de addons.
//
// Reutiliza tal cual la misma comprobación que la pestaña Actualizaciones
// del panel web (web-panel/src/updates.js): checkRepositories/remoteHead
// para addons.lock (PlayerBotManager, con su propio repositorio) y
// checkAddonCatalog para el catálogo grande de terceros (un solo mirror en
// bloque de NoM0Re/WoW-3.3.5a-Addons; sólo pide a la API compare de GitHub
// qué addon concreto cambió cuando el commit fijado está desactualizado).
// No clona nada, sólo red.
//
// Salida en stdout, en un formato deliberadamente simple para que un bucle
// `while read` en bash lo separe sin ambigüedad:
//   COUNT=<n>
//   UNREACHABLE=<n>
//   DETAIL\t[informativo] nombre: detalle
//   ...
//
// Uso: node tools/addon-updates-report.mjs [ruta-al-repo, por defecto el padre de tools/]
import path from 'node:path';
import { readFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { checkAddonCatalog, checkRepositories, parseLockContent } from '../web-panel/src/updates.js';

const root = path.resolve(process.argv[2] || path.join(path.dirname(fileURLToPath(import.meta.url)), '..'));

async function main() {
  const [addonLockContent, catalogContent] = await Promise.all([
    readFile(path.join(root, 'addons.lock'), 'utf8'),
    readFile(path.join(root, 'web-panel', 'addons', 'catalog.json'), 'utf8'),
  ]);
  const catalog = JSON.parse(catalogContent);

  const [addonLockRows, catalogRows] = await Promise.all([
    checkRepositories(parseLockContent(addonLockContent, 'addons.lock')).then((rows) => rows.map((row) => ({ ...row, origin: 'addons.lock' }))),
    checkAddonCatalog(catalog).then((rows) => rows.map((row) => ({ ...row, origin: 'catálogo NoM0Re' }))),
  ]);
  const rows = [...addonLockRows, ...catalogRows];

  const updates = rows.filter((row) => row.status === 'update');
  const unreachable = rows.filter((row) => row.status === 'unreachable');

  process.stdout.write(`COUNT=${updates.length}\n`);
  process.stdout.write(`UNREACHABLE=${unreachable.length}\n`);
  for (const row of updates) {
    process.stdout.write(`DETAIL\t[informativo] ${row.name}: addon de cliente con actualización disponible (${row.origin}; ver tools/revisar-actualizaciones-addons.sh)\n`);
  }
}

main().catch((error) => {
  console.error(`No se pudo comprobar las actualizaciones de addons: ${error.message}`);
  process.exit(1);
});
