// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import fs from 'node:fs';
import { fileURLToPath } from 'node:url';

// Los parches de idioma (patch-<idioma>-4.MPQ) los genera el panel a partir del
// cliente del jugador y viven en el almacén de recursos. Este catálogo sólo
// describe cada uno; tamaño y hash salen del fichero real en cada consulta, así
// que un parche regenerado o sustituido nunca se anuncia con un hash viejo.
const manifestPath = fileURLToPath(new URL('../addons/patches.json', import.meta.url));
const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));

for (const entry of manifest.patches) {
  if (!/^[a-z0-9-]+$/.test(entry.id) || !/^[A-Za-z]{2,4}$/.test(entry.targetDir || '') || !/^patch-[A-Za-z]{4}-4\.mpq$/i.test(entry.file)) {
    throw new Error(`Parche no válido en el catálogo: ${entry.id}`);
  }
}

export function createPatchCatalog(store) {
  const entries = () => store.patchEntries(manifest.patches);
  return {
    patchCatalog() {
      return {
        patches: entries().map(({ filePath: _filePath, ...entry }) => ({
          ...entry,
          downloadUrl: `/api/patches/${entry.id}/download`,
        })),
      };
    },
    findPatch(id) {
      return entries().find((entry) => entry.id === id) || null;
    },
  };
}
