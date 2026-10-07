// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import { spawn } from 'node:child_process';
import { createHash, randomBytes } from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import JSZip from 'jszip';

// Recursos que el servidor genera a partir del cliente del jugador (PUB04-I):
// iconos de la armería y los parches patch-<idioma>-4.MPQ. Ninguno viaja en el
// repositorio público. El navegador del administrador lee los ficheros que hacen
// falta de los MPQ de su cliente (public/mpq.js), los envía aquí y el servidor
// los convierte con los mismos generadores de siempre (Python). Cada resultado
// guarda con qué receta e insumos se hizo, para decidir si sigue valiendo.
export const RECIPE = '1';
export const LANGUAGES = ['esES', 'enUS'];
const ICONS_ID = 'iconos';
const MAPS_ID = 'mapas';
const MAP_FILES = ['0', '1', '530', '571'];
const patchId = (language) => `parche-${language}`;
export const RESOURCE_IDS = [ICONS_ID, ...LANGUAGES.map(patchId), MAPS_ID];

const LIMITS = Object.freeze({
  batchBytes: 64 * 1024 * 1024,
  dbcBytes: 80 * 1024 * 1024,
  blpBytes: 1024 * 1024,
  indexBytes: 2 * 1024 * 1024,
  jobBytes: 400 * 1024 * 1024,
  batchEntries: 2000,
  jobEntries: 12000,
  stepMs: 15 * 60_000,
  logLines: 200,
});

const JOB_ID = /^[0-9a-f]{32}$/;
const SHA = /^[0-9a-f]{64}$/;
const ENTRY_RULES = [
  { pattern: /^dbc\/ItemDisplayInfo\.dbc$/, limit: LIMITS.dbcBytes, magic: 'WDBC' },
  { pattern: /^dbc\/(esES|enUS)\/(Item|Spell)\.dbc$/, limit: LIMITS.dbcBytes, magic: 'WDBC' },
  { pattern: /^iconos\/[0-9]{1,6}\.blp$/, limit: LIMITS.blpBytes, magic: 'BLP' },
  { pattern: /^iconos\/indice\.json$/, limit: LIMITS.indexBytes, magic: null },
];

const sha256 = (data) => createHash('sha256').update(data).digest('hex');
const nowIso = () => new Date().toISOString();

function readJson(file, fallback) {
  try { return JSON.parse(fs.readFileSync(file, 'utf8')); } catch { return fallback; }
}

function writeJsonAtomic(file, value) {
  const temporary = `${file}.${process.pid}.tmp`;
  fs.writeFileSync(temporary, `${JSON.stringify(value, null, 2)}\n`);
  fs.renameSync(temporary, file);
}

function fileSha256(file) {
  const hash = createHash('sha256');
  const descriptor = fs.openSync(file, 'r');
  try {
    const buffer = Buffer.alloc(1024 * 1024);
    for (let read = fs.readSync(descriptor, buffer, 0, buffer.length, null); read > 0; read = fs.readSync(descriptor, buffer, 0, buffer.length, null)) {
      hash.update(buffer.subarray(0, read));
    }
  } finally {
    fs.closeSync(descriptor);
  }
  return hash.digest('hex');
}

// Lo que genera el panel debe poder leerlo el usuario del instalador (doctor, copias de
// seguridad): mkdtemp crea las carpetas 0700 y el rename las conserva así.
function shareReadable(root) {
  fs.chmodSync(root, 0o755);
  for (const entry of fs.readdirSync(root, { withFileTypes: true })) {
    const target = path.join(root, entry.name);
    if (entry.isDirectory()) shareReadable(target);
    else if (entry.isFile()) fs.chmodSync(target, 0o644);
  }
}

function removeQuietly(target) {
  fs.rmSync(target, { recursive: true, force: true });
}

export class ResourceError extends Error {
  constructor(message, status = 400) {
    super(message);
    this.status = status;
  }
}

export function createResourceStore(options) {
  const root = path.resolve(options.directory);
  const seedDirectory = options.seedDirectory ? path.resolve(options.seedDirectory) : '';
  const aracDirectory = options.aracDirectory ? path.resolve(options.aracDirectory) : '';
  const python = options.python || 'python3';
  const iconsScript = options.iconsScript;
  const patchScript = options.patchScript;
  const stepTimeoutMs = options.stepTimeoutMs || LIMITS.stepMs;
  const stateFile = path.join(root, 'estado.json');
  const iconsDirectory = path.join(root, 'iconos');
  const patchesDirectory = path.join(root, 'parches');
  const mapsDirectory = path.join(root, 'mapas');
  const jobsDirectory = path.join(root, 'trabajos');
  const log = options.log || ((message) => console.log(`[recursos] ${message}`));

  let running = null; // { id, child }

  fs.mkdirSync(jobsDirectory, { recursive: true });
  fs.mkdirSync(patchesDirectory, { recursive: true });

  // ------------------------------------------------------------ estado
  function readState() {
    const state = readJson(stateFile, null);
    return state && state.version === 1 && state.recursos ? state : { version: 1, recursos: {} };
  }

  function updateState(mutator) {
    const state = readState();
    mutator(state.recursos);
    writeJsonAtomic(stateFile, state);
  }

  function aracDigest() {
    if (!aracDirectory) return null;
    const hash = createHash('sha256');
    for (const name of ['CharBaseInfo.dbc', 'CharStartOutfit.dbc', 'SkillRaceClassInfo.dbc']) {
      const file = path.join(aracDirectory, name);
      if (!fs.existsSync(file)) return null;
      hash.update(`${name}\n${fileSha256(file)}\n`);
    }
    return hash.digest('hex');
  }

  // ------------------------------------------------------------ iconos
  let iconCache = { mtime: -1, manifest: null };
  function iconManifest() {
    const file = path.join(iconsDirectory, 'map.json');
    let stat;
    try { stat = fs.statSync(file); } catch { iconCache = { mtime: -1, manifest: null }; return null; }
    if (iconCache.mtime === stat.mtimeMs) return iconCache.manifest;
    const manifest = readJson(file, null);
    const valid = manifest && manifest.version === 1 && Array.isArray(manifest.icons) && /^[a-z0-9_.-]+$/.test(manifest.fallback || '');
    iconCache = { mtime: stat.mtimeMs, manifest: valid ? manifest : null };
    return iconCache.manifest;
  }

  function validIconSet(directory) {
    const manifestFile = path.join(directory, 'map.json');
    const manifest = readJson(manifestFile, null);
    if (!manifest || manifest.version !== 1 || !Array.isArray(manifest.icons) || manifest.icons.length < 1) return 'map.json no es válido';
    const names = new Set([manifest.fallback, ...manifest.icons]);
    for (const name of names) {
      if (!/^[a-z0-9_.-]+$/.test(String(name))) return `nombre de icono no válido: ${name}`;
      if (!fs.existsSync(path.join(directory, `${name}.webp`))) return `falta ${name}.webp`;
    }
    return null;
  }

  // ------------------------------------------------------------ parches
  function patchFile(language) {
    return path.join(patchesDirectory, language, `patch-${language}-4.MPQ`);
  }

  function validPatchFile(file) {
    try {
      const descriptor = fs.openSync(file, 'r');
      try {
        const head = Buffer.alloc(4);
        fs.readSync(descriptor, head, 0, 4, 0);
        return head.toString('latin1') === 'MPQ\x1a' && fs.fstatSync(descriptor).size > 1024;
      } finally { fs.closeSync(descriptor); }
    } catch { return false; }
  }

  function mapFile(id) {
    return MAP_FILES.includes(String(id)) ? path.join(mapsDirectory, `${id}.jpg`) : null;
  }

  // ------------------------------------------------------------ semilla (datos ya preparados)
  // Una instalación con datos preparados (el repositorio de trabajo los trae) los
  // copia aquí la primera vez; se anotan como `preparado`, sin insumos de cliente
  // conocidos, y valen hasta que el administrador pida regenerarlos.
  function seed() {
    if (!seedDirectory || !fs.existsSync(seedDirectory)) return;
    const state = readState().recursos;
    const prepared = [];
    if (!state[ICONS_ID] || !iconManifest()) {
      const source = path.join(seedDirectory, 'iconos');
      if (fs.existsSync(source) && !validIconSet(source)) {
        removeQuietly(iconsDirectory);
        fs.cpSync(source, iconsDirectory, { recursive: true });
        prepared.push([ICONS_ID, { origen: 'preparado', receta: RECIPE, actualizado: nowIso() }]);
      }
    }
    for (const language of LANGUAGES) {
      const source = path.join(seedDirectory, 'parches', language, `patch-${language}-4.MPQ`);
      if ((!state[patchId(language)] || !validPatchFile(patchFile(language))) && validPatchFile(source)) {
        fs.mkdirSync(path.dirname(patchFile(language)), { recursive: true });
        fs.copyFileSync(source, patchFile(language));
        prepared.push([patchId(language), { origen: 'preparado', receta: RECIPE, sha256: fileSha256(patchFile(language)), actualizado: nowIso() }]);
      }
    }
    // Los mapas se completan fichero a fichero: una instalación puede haber conseguido sólo
    // algunos (la fuente sirve a veces otros bytes) y una pasada posterior trae el resto.
    const mapSource = path.join(seedDirectory, 'mapas');
    if (fs.existsSync(mapSource)) {
      let copied = 0;
      for (const id of MAP_FILES) {
        const from = path.join(mapSource, `${id}.jpg`);
        const to = path.join(mapsDirectory, `${id}.jpg`);
        if (fs.existsSync(from) && !fs.existsSync(to)) {
          fs.mkdirSync(mapsDirectory, { recursive: true });
          fs.copyFileSync(from, to);
          copied += 1;
        }
      }
      if (copied || (!state[MAPS_ID] && MAP_FILES.some((id) => fs.existsSync(path.join(mapsDirectory, `${id}.jpg`))))) {
        prepared.push([MAPS_ID, { origen: 'preparado', receta: RECIPE, actualizado: nowIso() }]);
      }
    }
    if (prepared.length) {
      updateState((recursos) => { for (const [id, value] of prepared) recursos[id] = value; });
      log(`datos preparados incorporados: ${prepared.map(([id]) => id).join(', ')}`);
    }
  }

  // ------------------------------------------------------------ estado público
  function describe(id, entry, present, detail) {
    return {
      id,
      estado: present ? 'listo' : 'pendiente',
      origen: present ? (entry?.origen || 'generado') : null,
      actualizado: present ? entry?.actualizado || null : null,
      detalle: detail || null,
    };
  }

  function status() {
    const state = readState().recursos;
    const list = [];
    list.push(describe(ICONS_ID, state[ICONS_ID], Boolean(iconManifest()), iconManifest() ? null : 'Se generan a partir de tu cliente de WoW'));
    for (const language of LANGUAGES) {
      const present = validPatchFile(patchFile(language));
      list.push(describe(patchId(language), state[patchId(language)], present, present ? null : 'Se genera a partir de tu cliente de WoW'));
    }
    const missingMaps = MAP_FILES.filter((id) => !fs.existsSync(path.join(mapsDirectory, `${id}.jpg`)));
    list.push(describe(MAPS_ID, state[MAPS_ID], !missingMaps.length,
      missingMaps.length ? `Faltan los mapas ${missingMaps.join(', ')}: la fuente no entregó el fichero con el hash fijado` : null));
    return list;
  }

  function patchEntries(manifestPatches) {
    return manifestPatches.map((entry) => {
      const language = entry.targetDir;
      const file = LANGUAGES.includes(language) && entry.file === `patch-${language}-4.MPQ` ? patchFile(language) : null;
      const available = Boolean(file && validPatchFile(file));
      return {
        ...entry,
        filePath: available ? file : null,
        available,
        size: available ? fs.statSync(file).size : 0,
        sha256: available ? fileSha256(file).toUpperCase() : null,
      };
    });
  }

  // ------------------------------------------------------------ trabajos
  const jobDirectory = (id) => path.join(jobsDirectory, id);
  const jobStateFile = (id) => path.join(jobDirectory(id), 'estado.json');

  function readJob(id) {
    if (!JOB_ID.test(id)) return null;
    return readJson(jobStateFile(id), null);
  }

  function saveJob(job) {
    job.actualizado = nowIso();
    writeJsonAtomic(jobStateFile(job.id), job);
  }

  function publicJob(job) {
    if (!job) return null;
    const { recibidos, ...rest } = job;
    return { ...rest, recibidos: Object.keys(recibidos).length, ficheros: Object.keys(recibidos) };
  }

  function activeJob() {
    for (const name of fs.readdirSync(jobsDirectory)) {
      const job = readJob(name);
      if (job && (job.estado === 'recibiendo' || job.estado === 'generando')) return job;
    }
    return null;
  }

  function neededFor(declared, language) {
    const state = readState().recursos;
    const id = language ? patchId(language) : ICONS_ID;
    const entry = state[id];
    const present = language ? validPatchFile(patchFile(language)) : Boolean(iconManifest());
    if (!present) return { necesario: true, razon: 'no existe todavía' };
    if (!entry || entry.origen === 'preparado') return { necesario: false, razon: 'preparado en el servidor' };
    if (entry.receta !== RECIPE) return { necesario: true, razon: 'la receta ha cambiado' };
    if (entry.entradaCliente !== declared) return { necesario: true, razon: 'el cliente es distinto del usado antes' };
    if (language && entry.entradaServidor !== aracDigest()) return { necesario: true, razon: 'cambiaron los DBC de ARAC del servidor' };
    return { necesario: false, razon: 'ya generado con estos insumos' };
  }

  // `declared`: { iconos: <sha>, 'parche-esES': <sha>, ... } calculados por el navegador sobre lo que leyó.
  // `force`: ids a regenerar aunque estén al día.
  function createJob({ languages, declared = {}, force = [] }) {
    if (!Array.isArray(languages) || !languages.length || languages.some((language) => !LANGUAGES.includes(language))) {
      throw new ResourceError('Idiomas no válidos');
    }
    for (const [id, value] of Object.entries(declared)) {
      if (!RESOURCE_IDS.includes(id) || !SHA.test(String(value))) throw new ResourceError('Huella de insumos no válida');
    }
    const plan = {};
    const todo = [];
    const wantIcons = neededFor(declared[ICONS_ID], null);
    plan[ICONS_ID] = force.includes(ICONS_ID) ? { necesario: true, razon: 'regeneración pedida' } : wantIcons;
    if (plan[ICONS_ID].necesario) todo.push(ICONS_ID);
    for (const language of [...new Set(languages)]) {
      const id = patchId(language);
      plan[id] = force.includes(id) ? { necesario: true, razon: 'regeneración pedida' } : neededFor(declared[id], language);
      if (plan[id].necesario) todo.push(id);
    }
    if (!todo.length) return { job: null, plan };
    const active = activeJob();
    if (active) {
      // Una recepción interrumpida (red, navegador cerrado) se reanuda si pide lo mismo;
      // si el cliente cambió se descarta. Lo que ya está generando no se toca.
      const same = active.estado === 'recibiendo' && JSON.stringify(active.declarado) === JSON.stringify(declared)
        && JSON.stringify([...active.idiomas].sort()) === JSON.stringify([...new Set(languages)].sort())
        && JSON.stringify([...active.tipos].sort()) === JSON.stringify([...todo].sort());
      if (same) return { job: publicJob(active), plan, reanudado: true };
      if (active.estado !== 'recibiendo') throw new ResourceError('Ya hay una preparación de recursos en curso', 409);
      active.estado = 'cancelado';
      active.mensaje = 'Sustituido por un trabajo nuevo';
      saveJob(active);
      removeQuietly(path.join(jobDirectory(active.id), 'entrada'));
    }
    for (const name of fs.readdirSync(jobsDirectory)) removeQuietly(jobDirectory(name)); // trabajos viejos
    const id = randomBytes(16).toString('hex');
    fs.mkdirSync(path.join(jobDirectory(id), 'entrada'), { recursive: true });
    const job = {
      id, estado: 'recibiendo', idiomas: [...new Set(languages)], tipos: todo, declarado: declared,
      recibidos: {}, bytes: 0, creado: nowIso(), mensaje: 'Esperando los ficheros del cliente', progreso: null, registro: [],
    };
    saveJob(job);
    return { job: publicJob(job), plan };
  }

  async function receive(id, buffer) {
    const job = readJob(id);
    if (!job) throw new ResourceError('Trabajo no encontrado', 404);
    if (job.estado !== 'recibiendo') throw new ResourceError('El trabajo ya no admite ficheros', 409);
    if (buffer.length > LIMITS.batchBytes) throw new ResourceError('Lote demasiado grande', 413);
    let zip;
    try { zip = await JSZip.loadAsync(buffer, { checkCRC32: true }); } catch { throw new ResourceError('El lote no es un ZIP válido'); }
    const entries = Object.values(zip.files).filter((entry) => !entry.dir);
    if (!entries.length || entries.length > LIMITS.batchEntries) throw new ResourceError('Número de ficheros del lote no válido');
    if (Object.keys(job.recibidos).length + entries.length > LIMITS.jobEntries) throw new ResourceError('Demasiados ficheros en el trabajo', 413);
    // Primero se valida todo el lote (nombres y tamaños declarados); sólo si es válido se escribe.
    const planned = [];
    let batchBytes = 0;
    for (const entry of entries) {
      const rule = ENTRY_RULES.find(({ pattern }) => pattern.test(entry.name));
      if (!rule) throw new ResourceError(`Fichero no permitido: ${entry.name.slice(0, 80)}`);
      const language = /^dbc\/(esES|enUS)\//.exec(entry.name)?.[1];
      if (language && !job.idiomas.includes(language)) throw new ResourceError(`El trabajo no incluye el idioma ${language}`);
      const declaredSize = entry._data?.uncompressedSize;
      if (!Number.isInteger(declaredSize) || declaredSize > rule.limit) throw new ResourceError(`${entry.name} supera el tamaño permitido`, 413);
      batchBytes += declaredSize;
      planned.push({ entry, rule });
    }
    if (job.bytes + batchBytes > LIMITS.jobBytes) throw new ResourceError('El trabajo supera el tamaño máximo', 413);
    const written = {};
    for (const { entry, rule } of planned) {
      const data = await entry.async('nodebuffer');
      if (rule.magic && data.subarray(0, rule.magic.length).toString('latin1') !== rule.magic) throw new ResourceError(`${entry.name} no tiene el formato esperado`);
      if (entry.name.endsWith('indice.json')) {
        let index;
        try { index = JSON.parse(data.toString('utf8')); } catch { throw new ResourceError('indice.json no es JSON'); }
        if (!index || typeof index !== 'object' || Array.isArray(index)) throw new ResourceError('indice.json no es un objeto');
        for (const value of Object.values(index)) if (!/^[0-9]{1,6}$/.test(String(value))) throw new ResourceError('indice.json contiene un identificador no válido');
      }
      const target = path.join(jobDirectory(id), 'entrada', ...entry.name.split('/'));
      fs.mkdirSync(path.dirname(target), { recursive: true });
      fs.writeFileSync(target, data);
      written[entry.name] = sha256(data);
      job.bytes += data.length;
    }
    Object.assign(job.recibidos, written);
    job.mensaje = `Recibidos ${Object.keys(job.recibidos).length} ficheros`;
    saveJob(job);
    return publicJob(job);
  }

  function missingInputs(job) {
    const missing = [];
    const has = (name) => name in job.recibidos;
    if (job.tipos.includes(ICONS_ID)) {
      if (!has('dbc/ItemDisplayInfo.dbc')) missing.push('dbc/ItemDisplayInfo.dbc');
      if (!has('iconos/indice.json')) missing.push('iconos/indice.json');
      if (!Object.keys(job.recibidos).some((name) => name.startsWith('iconos/') && name.endsWith('.blp'))) missing.push('iconos/*.blp');
    }
    for (const language of job.idiomas) {
      if (!job.tipos.includes(patchId(language))) continue;
      for (const name of ['Item', 'Spell']) if (!has(`dbc/${language}/${name}.dbc`)) missing.push(`dbc/${language}/${name}.dbc`);
    }
    return missing;
  }

  function run(job, args, label) {
    return new Promise((resolve, reject) => {
      const child = spawn(python, args, { cwd: jobDirectory(job.id), env: { ...process.env, PYTHONIOENCODING: 'utf-8', PYTHONUNBUFFERED: '1' } });
      running = { id: job.id, child };
      let buffer = '';
      const push = (chunk) => {
        buffer += chunk.toString('utf8');
        let newline = buffer.indexOf('\n');
        while (newline >= 0) {
          const line = buffer.slice(0, newline).trim();
          buffer = buffer.slice(newline + 1);
          if (line) {
            job.registro.push(line.slice(0, 300));
            if (job.registro.length > LIMITS.logLines) job.registro.splice(0, job.registro.length - LIMITS.logLines);
            const progress = /Convertidos (\d+)\/(\d+)/.exec(line);
            if (progress) job.progreso = { paso: label, hecho: Number(progress[1]), total: Number(progress[2]) };
            saveJob(job);
          }
          newline = buffer.indexOf('\n');
        }
      };
      child.stdout.on('data', push);
      child.stderr.on('data', push);
      const timer = setTimeout(() => child.kill('SIGKILL'), stepTimeoutMs);
      child.on('error', (error) => { clearTimeout(timer); running = null; reject(error); });
      child.on('close', (code, signal) => {
        clearTimeout(timer);
        running = null;
        if (code === 0) resolve();
        else reject(new Error(signal ? `${label} interrumpido (${signal})` : `${label} terminó con código ${code}`));
      });
    });
  }

  function publishIcons(outputDirectory) {
    const problem = validIconSet(outputDirectory);
    if (problem) throw new Error(`Los iconos generados no son válidos: ${problem}`);
    shareReadable(outputDirectory);
    const previous = `${iconsDirectory}.anterior`;
    removeQuietly(previous);
    if (fs.existsSync(iconsDirectory)) fs.renameSync(iconsDirectory, previous);
    try {
      fs.renameSync(outputDirectory, iconsDirectory);
    } catch (error) {
      if (fs.existsSync(previous)) fs.renameSync(previous, iconsDirectory);
      throw error;
    }
    removeQuietly(previous);
  }

  function publishPatch(language, generated) {
    if (!validPatchFile(generated)) throw new Error(`El parche generado para ${language} no es válido`);
    const target = patchFile(language);
    fs.mkdirSync(path.dirname(target), { recursive: true });
    const temporary = `${target}.nuevo`;
    fs.copyFileSync(generated, temporary);
    fs.chmodSync(temporary, 0o644);
    fs.renameSync(temporary, target);
  }

  async function execute(job) {
    const output = path.join(jobDirectory(job.id), 'salida');
    const input = path.join(jobDirectory(job.id), 'entrada');
    fs.mkdirSync(output, { recursive: true });
    try {
      if (job.tipos.includes(ICONS_ID)) {
        job.mensaje = 'Generando los iconos de la armería';
        saveJob(job);
        await run(job, [iconsScript, '--insumos', input, '--salida', path.join(output, 'iconos')], 'iconos');
        publishIcons(path.join(output, 'iconos'));
        const dbcHash = job.recibidos['dbc/ItemDisplayInfo.dbc'];
        updateState((recursos) => {
          recursos[ICONS_ID] = {
            origen: 'generado', receta: RECIPE, actualizado: nowIso(),
            entradaCliente: job.declarado[ICONS_ID] || null, entradas: { 'ItemDisplayInfo.dbc': dbcHash },
          };
        });
      }
      for (const language of job.idiomas) {
        if (!job.tipos.includes(patchId(language))) continue;
        job.mensaje = `Generando patch-${language}-4.MPQ`;
        job.progreso = null;
        saveJob(job);
        const args = [patchScript, '--insumos', input, '--idioma', language, '--salida', path.join(output, 'parches')];
        if (aracDirectory) args.push('--arac', aracDirectory);
        await run(job, args, `parche ${language}`);
        const generated = path.join(output, 'parches', language, `patch-${language}-4.MPQ`);
        publishPatch(language, generated);
        updateState((recursos) => {
          recursos[patchId(language)] = {
            origen: 'generado', receta: RECIPE, actualizado: nowIso(), sha256: fileSha256(patchFile(language)),
            entradaCliente: job.declarado[patchId(language)] || null, entradaServidor: aracDigest(),
            entradas: { 'Item.dbc': job.recibidos[`dbc/${language}/Item.dbc`], 'Spell.dbc': job.recibidos[`dbc/${language}/Spell.dbc`] },
          };
        });
      }
      job.estado = 'listo';
      job.mensaje = 'Recursos preparados';
      job.progreso = null;
    } catch (error) {
      job.estado = job.estado === 'cancelado' ? 'cancelado' : 'error';
      job.mensaje = job.estado === 'cancelado' ? 'Cancelado' : `Falló la generación: ${error.message}`;
      log(`trabajo ${job.id}: ${job.mensaje}`);
    } finally {
      removeQuietly(input);
      removeQuietly(output);
      saveJob(job);
    }
  }

  function start(id) {
    const job = readJob(id);
    if (!job) throw new ResourceError('Trabajo no encontrado', 404);
    if (job.estado !== 'recibiendo') throw new ResourceError('El trabajo ya no está en recepción', 409);
    const missing = missingInputs(job);
    if (missing.length) throw new ResourceError(`Faltan ficheros: ${missing.slice(0, 5).join(', ')}`, 409);
    job.estado = 'generando';
    job.mensaje = 'Generando recursos';
    saveJob(job);
    const done = execute(job);
    return { job: publicJob(job), done };
  }

  function cancel(id) {
    const job = readJob(id);
    if (!job) throw new ResourceError('Trabajo no encontrado', 404);
    if (job.estado === 'recibiendo') {
      job.estado = 'cancelado';
      job.mensaje = 'Cancelado';
      saveJob(job);
      removeQuietly(path.join(jobDirectory(id), 'entrada'));
    } else if (job.estado === 'generando' && running?.id === id) {
      job.estado = 'cancelado';
      saveJob(job);
      running.child.kill('SIGKILL');
    }
    return publicJob(readJob(id));
  }

  // Un trabajo que quedó a medias al reiniciar el panel no puede continuar.
  function recoverInterrupted() {
    for (const name of fs.readdirSync(jobsDirectory)) {
      const job = readJob(name);
      if (job && job.estado === 'generando') {
        job.estado = 'error';
        job.mensaje = 'La generación se interrumpió al reiniciar el panel; vuelve a preparar los recursos';
        saveJob(job);
      }
    }
  }

  recoverInterrupted();
  seed();

  return {
    root, iconsDirectory, mapsDirectory, status, iconManifest, patchEntries, mapFile, patchFile,
    createJob, receive, start, cancel,
    getJob: (id) => publicJob(readJob(id)),
    currentJob: () => publicJob(activeJob()),
    lastJob: () => {
      const jobs = fs.readdirSync(jobsDirectory).map((name) => readJob(name)).filter(Boolean);
      jobs.sort((a, b) => String(b.creado).localeCompare(String(a.creado)));
      return publicJob(jobs[0] || null);
    },
    aracDigest,
  };
}
