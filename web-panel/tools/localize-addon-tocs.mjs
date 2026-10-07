// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Añade `## Notes-esES:` a los .toc del cliente que no tengan localización en
// español, reutilizando el texto ya traducido de addons/descriptions-es.json
// (la misma fuente que usa el panel web). No toca ningún .toc que ya declare
// Notes-esES: revisar traducciones existentes es cosa de una persona, no de
// este script. Idempotente: se puede volver a ejecutar tras sincronizar
// addons nuevos y sólo tocará los que de verdad no tengan nada en español.
//
// Uso: node tools/localize-addon-tocs.mjs [--dry-run]

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const panelRoot = path.resolve(here, '..');
const repositoryRoot = path.resolve(panelRoot, '..');

const NOTES_ESES_PATTERN = /^##\s*Notes-esES\s*:/i;
const NOTES_PATTERN = /^##\s*Notes\s*:/i;
const TITLE_PATTERN = /^##\s*Title\s*(-[A-Za-z]+)?\s*:/i;

function firstSentence(text) {
  const trimmed = text.trim();
  const match = trimmed.match(/^.{15,}?[.!?](?=\s|$)/);
  const sentence = match ? match[0] : trimmed;
  // Un TOC es una sola línea: nada de saltos de línea en el texto insertado.
  return sentence.replace(/\s+/g, ' ').trim();
}

function localizeToc(tocPath, noteText, dryRun) {
  const original = fs.readFileSync(tocPath, 'utf8');
  if (NOTES_ESES_PATTERN.test(original.replace(/^﻿/, ''))) return 'ya-localizado';
  const lines = original.split(/\r?\n/);
  if (lines.some((line) => NOTES_ESES_PATTERN.test(line))) return 'ya-localizado';
  const newLine = `## Notes-esES: ${noteText}`;
  let insertAt = lines.findIndex((line) => NOTES_PATTERN.test(line));
  if (insertAt !== -1) {
    // Detrás de la última línea Notes-* consecutiva (enUS y luego los demás idiomas).
    let after = insertAt;
    while (after + 1 < lines.length && /^##\s*Notes-[A-Za-z]+\s*:/i.test(lines[after + 1])) after += 1;
    lines.splice(after + 1, 0, newLine);
  } else {
    const titleAt = lines.findIndex((line) => TITLE_PATTERN.test(line));
    if (titleAt !== -1) {
      let after = titleAt;
      while (after + 1 < lines.length && TITLE_PATTERN.test(lines[after + 1])) after += 1;
      lines.splice(after + 1, 0, newLine);
    } else {
      // Sin ## Title ni ## Notes (típico de un submódulo LoadOnDemand sin
      // ficha propia, p. ej. EventHorizon_Warrior): detrás de ## Interface.
      const interfaceAt = lines.findIndex((line) => /^##\s*Interface\s*:/i.test(line));
      if (interfaceAt === -1) return 'sin-metadatos';
      lines.splice(interfaceAt + 1, 0, newLine);
    }
  }
  const usesCRLF = /\r\n/.test(original);
  const rewritten = lines.join(usesCRLF ? '\r\n' : '\n');
  if (!dryRun) fs.writeFileSync(tocPath, rewritten, 'utf8');
  return 'localizado';
}

// Localiza los .toc de `clientAddonsRoot` (la carpeta AddOns o una de trabajo).
// Con `only` (conjunto de nombres de carpeta) sólo toca esas carpetas.
export function localizeTocs({ clientAddonsRoot, catalog, descriptions, dryRun = false, only = null }) {
  let localized = 0;
  let skippedExisting = 0;
  const skippedNoMetadata = [];
  for (const addon of catalog.addons) {
    // playerbotmanager/questradar/guildlevels no pasan por sync-addons.mjs (no
    // vienen del mirror de NoM0Re): no tienen entrada en descriptions-es.json
    // porque su descripción en español ya vive directamente en catalog.json.
    const description = descriptions[addon.id] || addon.description;
    if (!description) throw new Error(`Sin descripción en español para ${addon.id}`);
    const noteText = firstSentence(description);
    for (const directoryName of addon.directories) {
      if (only && !only.has(directoryName)) continue;
      const directoryPath = path.join(clientAddonsRoot, directoryName);
      if (!fs.existsSync(directoryPath)) continue;
      const tocFiles = fs.readdirSync(directoryPath).filter((name) => /\.toc$/i.test(name));
      for (const tocName of tocFiles) {
        const result = localizeToc(path.join(directoryPath, tocName), noteText, dryRun);
        if (result === 'localizado') localized += 1;
        else if (result === 'ya-localizado') skippedExisting += 1;
        else skippedNoMetadata.push(`${directoryName}/${tocName}`);
      }
    }
  }
  return { localized, skippedExisting, skippedNoMetadata };
}

if (process.argv[1] && import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href) {
  const dryRun = process.argv.includes('--dry-run');
  const catalog = JSON.parse(fs.readFileSync(path.join(panelRoot, 'addons', 'catalog.json'), 'utf8'));
  const descriptions = JSON.parse(fs.readFileSync(path.join(panelRoot, 'addons', 'descriptions-es.json'), 'utf8'));
  const clientAddonsRoot = path.join(repositoryRoot, 'cliente', 'Interface', 'AddOns');
  const { localized, skippedExisting, skippedNoMetadata } = localizeTocs({ clientAddonsRoot, catalog, descriptions, dryRun });
  console.log(`${dryRun ? '[dry-run] ' : ''}Localizados ${localized} .toc, ${skippedExisting} ya tenían Notes-esES.`);
  if (skippedNoMetadata.length) {
    console.log(`Sin bloque ## Title/Notes reconocible (revisar a mano): ${skippedNoMetadata.join(', ')}`);
  }
}
