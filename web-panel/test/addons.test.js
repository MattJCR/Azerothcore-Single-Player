// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import test, { after } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path, { sep } from 'node:path';
import { createHash } from 'node:crypto';
import { addonCatalog, findAddon } from '../src/addons.js';
import { createPatchCatalog } from '../src/patches.js';
import { createResourceStore } from '../src/resources.js';

// Almacén de recursos de prueba, con los dos parches de idioma o sin ninguno.
const patchDirectories = [];
after(() => { for (const directory of patchDirectories) fs.rmSync(directory, { recursive: true, force: true }); });
function patchCatalogWith({ languages }) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'patches-test-'));
  patchDirectories.push(directory);
  for (const language of languages) {
    const file = path.join(directory, 'parches', language, `patch-${language}-4.MPQ`);
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.writeFileSync(file, Buffer.concat([Buffer.from('MPQ', 'latin1'), Buffer.alloc(2048, language.charCodeAt(0))]));
  }
  return createPatchCatalog(createResourceStore({ directory }));
}
const { findPatch, patchCatalog } = patchCatalogWith({ languages: ['esES', 'enUS'] });

test('el catálogo contiene paquetes locales únicos y los obligatorios', () => {
  const catalog = addonCatalog();
  assert.equal(catalog.addons.length, 123);
  assert.equal(new Set(catalog.addons.map((addon) => addon.id)).size, catalog.addons.length);
  assert.deepEqual(catalog.addons.filter((addon) => addon.required).map((addon) => addon.name).sort(),
    ['Addon Control Panel', 'BugSack', 'GuildLevels', 'MultiBot', 'ServerHelp']);
  assert.ok(catalog.addons.every((addon) => addon.downloadUrl.startsWith('/api/addons/')));
});

test('el catálogo expone las carpetas y una versión de contenido estable por addon', () => {
  const catalog = addonCatalog();
  assert.ok(catalog.addons.every((addon) => Array.isArray(addon.directories) && addon.directories.length > 0));
  assert.ok(catalog.addons.every((addon) => /^[0-9a-f]{64}$/.test(addon.contentVersion)));
  assert.equal(new Set(catalog.addons.map((addon) => addon.contentVersion)).size, catalog.addons.length);
  // Determinista: volver a pedir el catálogo en el mismo proceso da el mismo valor.
  assert.deepEqual(addonCatalog().addons.map((addon) => addon.contentVersion), catalog.addons.map((addon) => addon.contentVersion));
});

test('todas las descargas se resuelven desde carpetas locales con TOC', () => {
  for (const listed of addonCatalog().addons) {
    const addon = findAddon(listed.id);
    assert.ok(listed.size > 0, addon.name);
    assert.ok(addon.directoryPaths.length > 0, addon.name);
    for (const item of addon.directoryPaths) {
      assert.ok(fs.readdirSync(item.directoryPath).some((name) => name.toLowerCase().endsWith('.toc')), item.name);
    }
  }
});

test('no permite resolver identificadores ajenos al catálogo', () => {
  assert.equal(findAddon('../catalog.json'), null);
  assert.equal(findAddon('inexistente'), null);
});

test('EraTalents, retirado del proyecto (24/09/2026), no se lista ni se descarga', () => {
  const catalog = addonCatalog();
  assert.ok(!catalog.addons.some((addon) => /era.?talents/i.test(`${addon.id} ${addon.name} ${addon.directories.join(' ')}`)));
  for (const id of ['eratalents', 'era-talents', 'EraTalents']) assert.equal(findAddon(id), null, id);
  assert.equal(patchCatalog().patches.length, 2);
});

test('el catálogo está en español y ofrece recomendaciones para las diez clases', () => {
  const addons = addonCatalog().addons;
  const classes = ['Guerrero', 'Paladín', 'Cazador', 'Pícaro', 'Sacerdote', 'Caballero de la Muerte', 'Chamán', 'Mago', 'Brujo', 'Druida'];
  assert.ok(addons.every((addon) => addon.description.length > 20));
  assert.ok(addons.every((addon) => !/\b(this addon|allows you|your character|with the|the game)\b/i.test(addon.description)));
  for (const playerClass of classes) {
    assert.ok(addons.filter((addon) => addon.classes.includes(playerClass)).length >= 10, playerClass);
  }
});

test('ningún paquete externo puede sustituir los addons propios', () => {
  for (const listed of addonCatalog().addons) {
    const addon = findAddon(listed.id);
    if (listed.id !== 'multibot') assert.ok(!addon.directories.includes('MultiBot'), listed.id);
    if (listed.id !== 'serverhelp') assert.ok(!addon.directories.includes('ServerHelp'), listed.id);
  }
});

test('el catálogo de parches sale del almacén de recursos, con hash real e idioma', () => {
  const catalog = patchCatalog();
  assert.equal(catalog.patches.length, 2);
  assert.ok(catalog.patches.every((p) => p.required && p.available));
  assert.ok(catalog.patches.every((p) => p.downloadUrl.startsWith('/api/patches/')));
  assert.ok(catalog.patches.every((p) => !('filePath' in p)), 'la ruta del servidor no sale al navegador');

  for (const [id, dir] of [['patch-eses-4', 'esES'], ['patch-enus-4', 'enUS']]) {
    const patch = findPatch(id);
    assert.match(patch.description, /raza y clase/i);   // ARAC va fundido aquí
    assert.equal(patch.targetDir, dir);        // va en Data/<idioma>/
    assert.ok(patch.filePath.endsWith(`${dir}${sep}${patch.file}`), id);
    assert.equal(patch.sha256, createHash('sha256').update(fs.readFileSync(patch.filePath)).digest('hex').toUpperCase(), id);
    assert.equal(patch.size, fs.statSync(patch.filePath).size);
  }

  assert.equal(findPatch('patch-arac'), null);
  assert.equal(findPatch('../Patch-Arac.MPQ'), null);
});

test('sin recursos generados el panel lista los parches como pendientes, sin hash ni ruta', () => {
  const pending = patchCatalogWith({ languages: ['esES'] });
  const byId = Object.fromEntries(pending.patchCatalog().patches.map((p) => [p.id, p]));
  assert.equal(byId['patch-eses-4'].available, true);
  assert.equal(byId['patch-enus-4'].available, false);
  assert.equal(byId['patch-enus-4'].sha256, null);
  assert.equal(pending.findPatch('patch-enus-4').filePath, null);
});
