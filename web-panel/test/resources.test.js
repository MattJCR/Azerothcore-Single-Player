// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test, { after } from 'node:test';
import JSZip from 'jszip';
import { createApp } from '../src/app.js';
import { RECIPE, createResourceStore } from '../src/resources.js';
import { fakePool, listenTestApp, sessionCookieFor } from './helpers.js';

const temporaries = [];
after(() => { for (const directory of temporaries) fs.rmSync(directory, { recursive: true, force: true }); });
const temporary = (prefix) => { const directory = fs.mkdtempSync(path.join(os.tmpdir(), prefix)); temporaries.push(directory); return directory; };
const python = process.platform === 'win32' ? 'python' : 'python3';
const sha = '0123456789abcdef'.repeat(4);
const sha2 = 'fedcba9876543210'.repeat(4);

// Generadores de mentira (Python): escriben lo que escribirían los reales.
function fakeScripts(root, { failPatch = false } = {}) {
  const icons = path.join(root, 'icons.py');
  fs.writeFileSync(icons, `import argparse, json, pathlib
p = argparse.ArgumentParser(); p.add_argument('--insumos'); p.add_argument('--salida')
a = p.parse_args()
out = pathlib.Path(a.salida); out.mkdir(parents=True)
index = json.loads((pathlib.Path(a.insumos) / 'iconos' / 'indice.json').read_text())
names = sorted(set(index.values()))
for i, n in enumerate(names, 1):
    (out / ('icono-' + n + '.webp')).write_bytes(b'RIFF')
print('Convertidos %d/%d iconos...' % (len(names), len(names)), flush=True)
(out / 'map.json').write_text(json.dumps({'version': 1, 'fallback': 'icono-' + names[0], 'icons': ['icono-' + names[0]]}))
`);
  const patch = path.join(root, 'patch.py');
  fs.writeFileSync(patch, `import argparse, pathlib, sys
p = argparse.ArgumentParser(); p.add_argument('--insumos'); p.add_argument('--idioma'); p.add_argument('--salida'); p.add_argument('--arac')
a = p.parse_args()
${failPatch ? "print('fallo de prueba'); sys.exit(3)" : ''}
d = pathlib.Path(a.salida) / a.idioma; d.mkdir(parents=True)
(d / ('patch-' + a.idioma + '-4.MPQ')).write_bytes(b'MPQ\\x1a' + bytes(4096))
print('hecho', a.idioma)
`);
  return { icons, patch };
}

function makeStore({ failPatch = false, aracDirectory = '' } = {}) {
  const root = temporary('recursos-test-');
  const scripts = fakeScripts(root, { failPatch });
  const store = createResourceStore({ directory: path.join(root, 'datos'), python, iconsScript: scripts.icons, patchScript: scripts.patch, aracDirectory, log: () => {} });
  return { store, root };
}

async function zipOf(files) {
  const zip = new JSZip();
  for (const [name, data] of Object.entries(files)) zip.file(name, data);
  return zip.generateAsync({ type: 'nodebuffer', compression: 'DEFLATE' });
}

const wdbc = () => Buffer.concat([Buffer.from('WDBC'), Buffer.alloc(16)]);
const blp = () => Buffer.concat([Buffer.from('BLP2'), Buffer.alloc(60)]);
const fullInputs = (language = 'esES') => ({
  'dbc/ItemDisplayInfo.dbc': wdbc(),
  'iconos/indice.json': JSON.stringify({ inv_a: '1', inv_b: '2' }),
  'iconos/1.blp': blp(),
  'iconos/2.blp': blp(),
  [`dbc/${language}/Item.dbc`]: wdbc(),
  [`dbc/${language}/Spell.dbc`]: wdbc(),
});

async function waitFor(store, id, state) {
  for (let attempt = 0; attempt < 200; attempt += 1) {
    const job = store.getJob(id);
    if (job.estado === state) return job;
    if (['error', 'cancelado', 'listo'].includes(job.estado) && job.estado !== state) throw new Error(`terminó en ${job.estado}: ${job.mensaje}`);
    await new Promise((resolve) => setTimeout(resolve, 25));
  }
  throw new Error('el trabajo no terminó');
}

async function generate(store, language = 'esES', declared = { iconos: sha, [`parche-${language}`]: sha2 }) {
  const { job } = store.createJob({ languages: [language], declared });
  await store.receive(job.id, await zipOf(fullInputs(language)));
  const { done } = store.start(job.id);
  await done;
  return waitFor(store, job.id, 'listo');
}

test('sin nada generado todo está pendiente y el plan lo pide', () => {
  const { store } = makeStore();
  const status = Object.fromEntries(store.status().map((item) => [item.id, item]));
  assert.equal(status.iconos.estado, 'pendiente');
  assert.equal(status['parche-esES'].estado, 'pendiente');
  assert.equal(store.iconManifest(), null);
  const { job, plan } = store.createJob({ languages: ['esES', 'enUS'], declared: { iconos: sha } });
  assert.ok(job.id);
  assert.deepEqual(job.tipos.sort(), ['iconos', 'parche-enUS', 'parche-esES']);
  assert.equal(plan.iconos.necesario, true);
});

test('genera iconos y parche, los publica y la segunda vez con los mismos insumos no hace nada', async () => {
  const { store } = makeStore();
  const job = await generate(store);
  assert.equal(job.estado, 'listo');
  const status = Object.fromEntries(store.status().map((item) => [item.id, item]));
  assert.equal(status.iconos.estado, 'listo');
  assert.equal(status.iconos.origen, 'generado');
  assert.equal(status['parche-esES'].estado, 'listo');
  assert.ok(store.iconManifest().icons.length);
  assert.ok(fs.existsSync(store.patchFile('esES')));
  if (process.platform !== 'win32') {
    // El doctor del instalador (otro usuario) tiene que poder leer lo generado.
    assert.equal(fs.statSync(store.iconsDirectory).mode & 0o755, 0o755);
    assert.equal(fs.statSync(path.join(store.iconsDirectory, 'map.json')).mode & 0o644, 0o644);
    assert.equal(fs.statSync(store.patchFile('esES')).mode & 0o644, 0o644);
  }
  assert.equal(fs.existsSync(path.join(store.root, 'trabajos', job.id, 'entrada')), false, 'los insumos del jugador no se conservan');

  const again = store.createJob({ languages: ['esES'], declared: { iconos: sha, 'parche-esES': sha2 } });
  assert.equal(again.job, null);
  assert.equal(again.plan.iconos.necesario, false);
  const changed = store.createJob({ languages: ['esES'], declared: { iconos: sha, 'parche-esES': sha } });
  assert.equal(changed.plan['parche-esES'].necesario, true);
  assert.match(changed.plan['parche-esES'].razon, /cliente es distinto/);
  store.cancel(changed.job.id);
  const forced = store.createJob({ languages: ['esES'], declared: { iconos: sha, 'parche-esES': sha2 }, force: ['iconos'] });
  assert.deepEqual(forced.job.tipos, ['iconos']);
});

test('una receta nueva invalida lo generado con la anterior', async () => {
  const { store } = makeStore();
  await generate(store);
  const stateFile = path.join(store.root, 'estado.json');
  const state = JSON.parse(fs.readFileSync(stateFile, 'utf8'));
  state.recursos.iconos.receta = `${RECIPE}-vieja`;
  fs.writeFileSync(stateFile, JSON.stringify(state));
  const result = store.createJob({ languages: ['esES'], declared: { iconos: sha, 'parche-esES': sha2 } });
  assert.equal(result.plan.iconos.necesario, true);
  assert.match(result.plan.iconos.razon, /receta/);
});

test('rechaza nombres ajenos, rutas con .., magia errónea, idiomas no pedidos y ZIP corruptos', async () => {
  const { store } = makeStore();
  const { job } = store.createJob({ languages: ['esES'], declared: { iconos: sha } });
  const reject = async (files, pattern) => assert.rejects(async () => store.receive(job.id, await zipOf(files)), pattern);
  await reject({ '../evil.txt': 'x' }, /no permitido/);
  await reject({ 'dbc/esES/../../Item.dbc': wdbc() }, /no permitido/);
  await reject({ 'iconos/a.blp': blp() }, /no permitido/);
  await reject({ 'dbc/enUS/Item.dbc': wdbc() }, /idioma enUS/);
  await reject({ 'dbc/ItemDisplayInfo.dbc': Buffer.from('NOPE-not-a-dbc') }, /formato esperado/);
  await reject({ 'iconos/1.blp': Buffer.from('GIF89a') }, /formato esperado/);
  await reject({ 'iconos/indice.json': '{"a":"../x"}' }, /identificador/);
  await reject({ 'iconos/indice.json': '[1,2]' }, /objeto/);
  await assert.rejects(store.receive(job.id, Buffer.from('esto no es un zip')), /ZIP/);
  assert.equal(store.getJob(job.id).recibidos, 0, 'un lote inválido no deja nada escrito');
  assert.equal(fs.existsSync(path.join(store.root, 'trabajos', job.id, 'entrada', 'iconos')), false);
});

test('start falla si faltan insumos; la recepción interrumpida se reanuda y un cliente distinto la sustituye', async () => {
  const { store } = makeStore();
  const { job } = store.createJob({ languages: ['esES'], declared: { iconos: sha } });
  await store.receive(job.id, await zipOf({ 'dbc/ItemDisplayInfo.dbc': wdbc() }));
  assert.throws(() => store.start(job.id), /Faltan ficheros/);
  // Una recepción interrumpida se reanuda: mismo trabajo y los ficheros ya recibidos.
  const resumed = store.createJob({ languages: ['esES'], declared: { iconos: sha } });
  assert.equal(resumed.job.id, job.id);
  assert.equal(resumed.reanudado, true);
  assert.deepEqual(resumed.job.ficheros, ['dbc/ItemDisplayInfo.dbc']);
  // Con otro cliente (huellas distintas) el trabajo viejo se descarta y se empieza otro.
  const replaced = store.createJob({ languages: ['esES'], declared: { iconos: sha2 } });
  assert.notEqual(replaced.job.id, job.id);
  assert.equal(store.getJob(job.id), null, 'el trabajo descartado y sus insumos se borran');
  store.cancel(replaced.job.id);
  assert.equal(store.getJob(replaced.job.id).estado, 'cancelado');
  assert.ok(store.createJob({ languages: ['esES'], declared: { iconos: sha } }).job, 'tras cancelar se puede empezar otro');
});

test('un fallo del generador deja lo anterior intacto y lo cuenta', async () => {
  const { store, root } = makeStore();
  await generate(store);
  const before = fs.readFileSync(store.patchFile('esES'));
  // Mismo almacén con un generador de parches que falla.
  const failing = createResourceStore({ directory: store.root, python, iconsScript: path.join(root, 'icons.py'), patchScript: fakeScripts(path.join(root), { failPatch: true }).patch, log: () => {} });
  const { job } = failing.createJob({ languages: ['esES'], declared: { iconos: sha, 'parche-esES': sha }, force: ['parche-esES'] });
  await failing.receive(job.id, await zipOf(fullInputs()));
  await failing.start(job.id).done;
  const result = failing.getJob(job.id);
  assert.equal(result.estado, 'error');
  assert.match(result.mensaje, /código 3/);
  assert.ok(result.registro.some((line) => /fallo de prueba/.test(line)));
  assert.deepEqual(fs.readFileSync(failing.patchFile('esES')), before);
  assert.equal(fs.existsSync(path.join(store.root, 'trabajos', job.id, 'entrada')), false);
});

test('un trabajo que estaba generando al reiniciarse el panel pasa a error y se puede repetir', async () => {
  const { store, root } = makeStore();
  await generate(store);
  const { job } = store.createJob({ languages: ['esES'], declared: { iconos: sha, 'parche-esES': sha }, force: ['parche-esES'] });
  const file = path.join(store.root, 'trabajos', job.id, 'estado.json');
  const saved = JSON.parse(fs.readFileSync(file, 'utf8'));
  saved.estado = 'generando'; // el proceso del panel murió en mitad de la generación
  fs.writeFileSync(file, JSON.stringify(saved));
  const restarted = createResourceStore({ directory: store.root, python, iconsScript: path.join(root, 'icons.py'), patchScript: path.join(root, 'patch.py'), log: () => {} });
  assert.equal(restarted.getJob(job.id).estado, 'error');
  assert.match(restarted.getJob(job.id).mensaje, /se interrumpió/);
  assert.ok(fs.existsSync(restarted.patchFile('esES')), 'lo ya generado sigue ahí');
  const again = restarted.createJob({ languages: ['esES'], declared: { iconos: sha, 'parche-esES': sha }, force: ['parche-esES'] });
  assert.ok(again.job, 'tras el fallo se puede empezar otro');
});

test('datos preparados: se incorporan una vez y no se anuncian como generados', () => {
  const root = temporary('semilla-test-');
  const seed = path.join(root, 'semilla');
  fs.mkdirSync(path.join(seed, 'iconos'), { recursive: true });
  fs.writeFileSync(path.join(seed, 'iconos', 'a-1.webp'), 'RIFF');
  fs.writeFileSync(path.join(seed, 'iconos', 'map.json'), JSON.stringify({ version: 1, fallback: 'a-1', icons: ['a-1'] }));
  fs.mkdirSync(path.join(seed, 'parches', 'esES'), { recursive: true });
  fs.writeFileSync(path.join(seed, 'parches', 'esES', 'patch-esES-4.MPQ'), Buffer.concat([Buffer.from('MPQ\x1a'), Buffer.alloc(4096)]));
  const store = createResourceStore({ directory: path.join(root, 'datos'), seedDirectory: seed, python, log: () => {} });
  const status = Object.fromEntries(store.status().map((item) => [item.id, item]));
  assert.equal(status.iconos.origen, 'preparado');
  assert.equal(status['parche-esES'].origen, 'preparado');
  assert.equal(status['parche-enUS'].estado, 'pendiente');
  // Valen tal cual: un trabajo con otro cliente no pide regenerar lo preparado, salvo orden expresa.
  const plan = store.createJob({ languages: ['esES'], declared: { iconos: sha, 'parche-esES': sha2 } });
  assert.equal(plan.job, null);
  assert.ok(store.createJob({ languages: ['esES'], declared: {}, force: ['iconos'] }).job);
  // Un conjunto de semilla inválido (falta un WebP) no se incorpora.
  const broken = path.join(root, 'rota');
  fs.mkdirSync(path.join(broken, 'iconos'), { recursive: true });
  fs.writeFileSync(path.join(broken, 'iconos', 'map.json'), JSON.stringify({ version: 1, fallback: 'falta', icons: ['falta'] }));
  const store2 = createResourceStore({ directory: path.join(root, 'datos2'), seedDirectory: broken, python, log: () => {} });
  assert.equal(store2.iconManifest(), null);
});

test('datos preparados: los mapas que faltan se completan en una pasada posterior', () => {
  const root = temporary('semilla-mapas-');
  const seed = path.join(root, 'semilla');
  fs.mkdirSync(path.join(seed, 'mapas'), { recursive: true });
  for (const id of ['0', '530']) fs.writeFileSync(path.join(seed, 'mapas', `${id}.jpg`), `mapa-${id}`);
  const options = { directory: path.join(root, 'datos'), seedDirectory: seed, python, log: () => {} };
  const first = createResourceStore(options);
  assert.equal(first.status().find((item) => item.id === 'mapas').estado, 'pendiente', 'sin los cuatro, pendiente');
  // La instalación siguiente consigue otro mapa: se añade sin tocar los que ya estaban.
  fs.writeFileSync(path.join(seed, 'mapas', '1.jpg'), 'mapa-1');
  fs.writeFileSync(path.join(seed, 'mapas', '0.jpg'), 'otro-contenido');
  const second = createResourceStore(options);
  const dir = path.join(root, 'datos', 'mapas');
  assert.equal(fs.readFileSync(path.join(dir, '1.jpg'), 'utf8'), 'mapa-1');
  assert.equal(fs.readFileSync(path.join(dir, '0.jpg'), 'utf8'), 'mapa-0', 'lo que ya estaba no se pisa');
  assert.ok(second.status().find((item) => item.id === 'mapas'));
});

// ------------------------------------------------------------------ HTTP
async function startApp(store, level) {
  const authDb = fakePool({ execute: async (sql) => (sql.includes('FROM account_access') ? [[{ gmlevel: level }]] : [[]]) });
  const app = createApp({ authDb, charactersDb: fakePool(), worldDb: fakePool(), panelDb: fakePool(), soap: { executeCommand: async () => '' }, resources: store });
  return listenTestApp(app);
}
const cookie = () => sessionCookieFor({ id: 7, username: 'ADMIN' });
const headers = (extra = {}) => ({ Cookie: cookie(), 'X-Panel-Request': '1', ...extra });

test('rutas HTTP: sólo administradores, con cabecera CSRF, y el ciclo completo', async (t) => {
  const { store } = makeStore();
  const player = await startApp(store, 0);
  t.after(() => player.server.close());
  assert.equal((await fetch(`${player.base}/api/recursos`, { headers: { Cookie: cookie() } })).status, 403);
  assert.equal((await fetch(`${player.base}/api/recursos`)).status, 401);

  const admin = await startApp(store, 3);
  t.after(() => admin.server.close());
  assert.equal((await fetch(`${admin.base}/api/recursos/trabajos`, { method: 'POST', headers: { Cookie: cookie(), 'Content-Type': 'application/json' }, body: '{}' })).status, 403, 'sin cabecera CSRF');

  const created = await fetch(`${admin.base}/api/recursos/trabajos`, {
    method: 'POST', headers: headers({ 'Content-Type': 'application/json' }), body: JSON.stringify({ idiomas: ['esES'], entradas: { iconos: sha, 'parche-esES': sha2 } }),
  });
  assert.equal(created.status, 201);
  const { trabajo } = await created.json();

  const lote = await fetch(`${admin.base}/api/recursos/trabajos/${trabajo.id}/lote`, { method: 'PUT', headers: headers({ 'Content-Type': 'application/zip' }), body: await zipOf(fullInputs()) });
  assert.equal(lote.status, 200);
  const bad = await fetch(`${admin.base}/api/recursos/trabajos/${trabajo.id}/lote`, { method: 'PUT', headers: headers({ 'Content-Type': 'application/zip' }), body: await zipOf({ 'etc/passwd': 'x' }) });
  assert.equal(bad.status, 400);
  assert.equal((await fetch(`${admin.base}/api/recursos/trabajos/${'a'.repeat(32)}`, { headers: { Cookie: cookie() } })).status, 404);

  const started = await fetch(`${admin.base}/api/recursos/trabajos/${trabajo.id}/iniciar`, { method: 'POST', headers: headers() });
  assert.equal(started.status, 202);
  await waitFor(store, trabajo.id, 'listo');
  const list = await (await fetch(`${admin.base}/api/recursos`, { headers: { Cookie: cookie() } })).json();
  assert.equal(list.recursos.find((item) => item.id === 'iconos').estado, 'listo');

  // Los iconos generados se sirven y lo que falta cae al SVG genérico (no a una imagen rota).
  const icon = await fetch(`${admin.base}/assets/item-icons/icono-1.webp`);
  assert.equal(icon.status, 200);
  const missing = await fetch(`${admin.base}/assets/item-icons/no-existe.webp`);
  assert.equal(missing.headers.get('content-type'), 'image/svg+xml; charset=utf-8');
});

test('los parches pendientes no se descargan y los generados sí', async (t) => {
  const { store } = makeStore();
  const { server, base } = await startApp(store, 0);
  t.after(() => server.close());
  const pending = await fetch(`${base}/api/patches/patch-eses-4/download`, { headers: { Cookie: cookie() } });
  assert.equal(pending.status, 409);
  const listed = await (await fetch(`${base}/api/addons`, { headers: { Cookie: cookie() } })).json();
  assert.ok(listed.patches.every((patch) => patch.available === false));
  await generate(store);
  const ready = await fetch(`${base}/api/patches/patch-eses-4/download`, { headers: { Cookie: cookie() } });
  assert.equal(ready.status, 200);
  assert.equal((await ready.arrayBuffer()).byteLength, fs.statSync(store.patchFile('esES')).size);
});
