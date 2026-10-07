// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Preparación de recursos desde el cliente de WoW del jugador, en el navegador.
//
// El servidor Ubuntu no puede leer una ruta del PC Windows, así que el navegador
// (que ya tiene la carpeta del cliente por la File System Access API) lee los MPQ,
// extrae sólo lo necesario (ItemDisplayInfo/Item/Spell.dbc y los iconos de objeto,
// unos 100 MB en total frente a varios GB de cliente), lo envía al panel y el panel
// lo convierte con sus generadores. El jugador no ejecuta ningún programa.
import { MpqArchive, archivePriority, blobSource } from './mpq.js';

export const LANGUAGES = ['esES', 'enUS'];
const DBF = 'DBFilesClient\\';
const ICONS = 'Interface\\Icons\\';
const FALLBACK_ICON = 'inv_misc_questionmark';
const OWN_PATCHES = new Set(LANGUAGES.map((language) => `patch-${language.toLowerCase()}-4.mpq`));
const BATCH_BYTES = 24 * 1024 * 1024;
const BATCH_FILES = 800;

const toHex = (buffer) => [...new Uint8Array(buffer)].map((byte) => byte.toString(16).padStart(2, '0')).join('');
export async function sha256Hex(data) {
  return toHex(await crypto.subtle.digest('SHA-256', data));
}

export function iconKey(name) {
  const value = name.toLowerCase();
  return value.endsWith('.blp') || value.endsWith('.tga') ? value.slice(0, -4) : value;
}

// ItemDisplayInfo.dbc de 3.3.5a: los campos 5 y 6 son InventoryIcon[2].
export function parseItemDisplayInfo(bytes) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (bytes.length < 20 || view.getUint32(0, true) !== 0x43424457) throw new Error('ItemDisplayInfo.dbc no tiene el formato esperado');
  const records = view.getInt32(4, true);
  const fields = view.getInt32(8, true);
  const size = view.getInt32(12, true);
  if (fields < 7 || size !== fields * 4 || 20 + records * size > bytes.length) throw new Error('ItemDisplayInfo.dbc no es de WotLK 3.3.5a');
  const strings = bytes.subarray(20 + records * size);
  const text = (offset) => {
    if (offset <= 0 || offset >= strings.length) return '';
    let end = offset;
    while (end < strings.length && strings[end] !== 0) end += 1;
    return String.fromCharCode(...strings.subarray(offset, end));
  };
  const rows = [];
  for (let index = 0; index < records; index += 1) {
    const base = 20 + index * size;
    rows.push({ id: view.getInt32(base, true), names: [5, 6].map((field) => view.getInt32(base + field * 4, true)).filter(Boolean).map(text) });
  }
  return rows;
}

// Los MPQ del cliente ordenados por prioridad, con búsqueda "gana el de más prioridad".
export class ClientData {
  // files: [{ path: 'esES/patch-esES-3.MPQ', blob }] con la ruta relativa a Data/.
  constructor(files) {
    this.files = files.filter((file) => /\.mpq$/i.test(file.path) && !OWN_PATCHES.has(file.path.split('/').pop().toLowerCase()));
    this.archives = new Map();
  }

  languages() {
    return LANGUAGES.filter((language) => this.files.some((file) => file.path.toLowerCase() === `${language.toLowerCase()}/locale-${language.toLowerCase()}.mpq`));
  }

  async archive(file) {
    if (!this.archives.has(file.path)) this.archives.set(file.path, MpqArchive.open(blobSource(file.blob), file.path));
    return this.archives.get(file.path);
  }

  chain(language) {
    const prefix = `${language.toLowerCase()}/`;
    return this.files
      .filter((file) => !file.path.includes('/') || file.path.toLowerCase().startsWith(prefix))
      .map((file) => ({ file, priority: archivePriority(file.path, language) }))
      .sort((a, b) => b.priority - a.priority || (a.file.path < b.file.path ? -1 : 1));
  }

  // Lee `path` del archivo con más prioridad que lo tenga; {data, from} o null.
  async read(path, language) {
    for (const { file } of this.chain(language)) {
      let archive;
      try { archive = await this.archive(file); } catch { continue; } // un MPQ ilegible no impide leer de los demás
      if (archive.has(path)) {
        const data = await archive.read(path);
        if (data) return { data, from: file.path };
      }
    }
    return null;
  }
}

// Comprueba que parece un cliente 3.3.5a antes de leer nada pesado.
export function describeClient(client) {
  const names = new Set(client.files.map((file) => file.path.split('/').pop().toLowerCase()));
  const core = ['common.mpq', 'expansion.mpq', 'lichking.mpq', 'patch.mpq'];
  const missing = core.filter((name) => !names.has(name));
  const languages = client.languages();
  if (missing.length) throw new Error(`No parece un cliente 3.3.5a completo: faltan ${missing.join(', ')} en Data/.`);
  if (!languages.length) throw new Error('No encuentro ninguna carpeta de idioma esES o enUS en Data/.');
  return { languages };
}

// Lee todo lo que el panel necesita y calcula la huella de insumos de cada recurso.
export async function collectInputs(client, languages, onProgress = () => {}) {
  const inputs = new Map();
  const declared = {};
  const first = languages[0];

  onProgress({ fase: 'leyendo', texto: 'Leyendo ItemDisplayInfo.dbc' });
  const display = await client.read(`${DBF}ItemDisplayInfo.dbc`, first);
  if (!display) throw new Error('No encuentro ItemDisplayInfo.dbc en el cliente.');
  inputs.set('dbc/ItemDisplayInfo.dbc', display.data);
  const rows = parseItemDisplayInfo(display.data);

  const keys = new Map();
  for (const { names } of rows) for (const name of names) keys.set(iconKey(name), keys.get(iconKey(name)) || name);
  if (!keys.has(FALLBACK_ICON)) keys.set(FALLBACK_ICON, 'INV_Misc_QuestionMark');
  const sortedKeys = [...keys.keys()].sort();

  const index = {};
  const iconHashes = [];
  let found = 0;
  let number = 0;
  for (const key of sortedKeys) {
    const name = keys.get(key);
    const result = await client.read(`${ICONS}${key}.blp`, first) || await client.read(`${ICONS}${name}`, first);
    number += 1;
    if (number % 200 === 0) onProgress({ fase: 'leyendo', texto: `Leyendo iconos ${number}/${sortedKeys.length}`, hecho: number, total: sortedKeys.length });
    if (!result) continue;
    found += 1;
    const id = String(found);
    inputs.set(`iconos/${id}.blp`, result.data);
    index[key] = id;
    iconHashes.push(`${key}:${await sha256Hex(result.data)}`);
  }
  inputs.set('iconos/indice.json', new TextEncoder().encode(JSON.stringify(index)));
  declared.iconos = await sha256Hex(new TextEncoder().encode(`${await sha256Hex(display.data)}\n${iconHashes.join('\n')}`));

  for (const language of languages) {
    const parts = [];
    for (const name of ['Item', 'Spell']) {
      onProgress({ fase: 'leyendo', texto: `Leyendo ${name}.dbc (${language})` });
      const result = await client.read(`${DBF}${name}.dbc`, language);
      if (!result) throw new Error(`No encuentro ${name}.dbc para ${language} en el cliente.`);
      inputs.set(`dbc/${language}/${name}.dbc`, result.data);
      parts.push(await sha256Hex(result.data));
    }
    declared[`parche-${language}`] = await sha256Hex(new TextEncoder().encode(parts.join('\n')));
  }
  return { inputs, declared, summary: { apariencias: rows.length, iconos: found, sinArte: sortedKeys.length - found } };
}

// Qué ficheros hacen falta según el plan que devolvió el servidor.
export function selectInputs(inputs, plan, languages, alreadySent = new Set()) {
  const chosen = new Map();
  const needed = (id) => plan[id]?.necesario;
  for (const [name, data] of inputs) {
    if (alreadySent.has(name)) continue;
    if (name === 'dbc/ItemDisplayInfo.dbc' || name.startsWith('iconos/')) { if (needed('iconos')) chosen.set(name, data); continue; }
    const language = /^dbc\/(esES|enUS)\//.exec(name)?.[1];
    if (language && languages.includes(language) && needed(`parche-${language}`)) chosen.set(name, data);
  }
  return chosen;
}

export function* batches(files) {
  let current = [];
  let bytes = 0;
  for (const entry of files) {
    if (current.length && (bytes + entry[1].length > BATCH_BYTES || current.length >= BATCH_FILES)) {
      yield current;
      current = [];
      bytes = 0;
    }
    current.push(entry);
    bytes += entry[1].length;
  }
  if (current.length) yield current;
}

async function zipBatch(entries, JSZip) {
  const zip = new JSZip();
  for (const [name, data] of entries) zip.file(name, data, { binary: true });
  return zip.generateAsync({ type: 'uint8array', compression: 'DEFLATE', compressionOptions: { level: 3 } });
}

const wait = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

// Ciclo completo. `http` ofrece json(method, path, body) y put(path, bytes).
export async function prepareResources({ client, http, JSZip, languages, force = [], onProgress = () => {}, signal }) {
  const { inputs, declared, summary } = await collectInputs(client, languages, onProgress);
  onProgress({ fase: 'consultando', texto: 'Comprobando qué recursos hacen falta' });
  const created = await http.json('POST', '/api/recursos/trabajos', { idiomas: languages, entradas: declared, regenerar: force });
  if (!created.trabajo) return { estado: 'alDia', plan: created.plan, resumen: summary };
  const job = created.trabajo;
  const sent = new Set(job.ficheros || []);
  const chosen = selectInputs(inputs, created.plan, languages, sent);
  let done = 0;
  for (const entries of batches(chosen)) {
    if (signal?.aborted) throw new Error('Preparación cancelada');
    const bytes = await zipBatch(entries, JSZip);
    await http.put(`/api/recursos/trabajos/${job.id}/lote`, bytes);
    done += entries.length;
    onProgress({ fase: 'enviando', texto: `Enviando al servidor ${done}/${chosen.size} ficheros`, hecho: done, total: chosen.size });
  }
  if (signal?.aborted) throw new Error('Preparación cancelada');
  await http.json('POST', `/api/recursos/trabajos/${job.id}/iniciar`);
  for (;;) {
    await wait(1000);
    if (signal?.aborted) { await http.json('DELETE', `/api/recursos/trabajos/${job.id}`); throw new Error('Preparación cancelada'); }
    const { trabajo } = await http.json('GET', `/api/recursos/trabajos/${job.id}`);
    onProgress({ fase: 'generando', texto: trabajo.mensaje, ...(trabajo.progreso || {}) });
    if (trabajo.estado === 'listo') return { estado: 'listo', plan: created.plan, resumen: summary };
    if (trabajo.estado === 'error' || trabajo.estado === 'cancelado') throw new Error(trabajo.mensaje);
  }
}
