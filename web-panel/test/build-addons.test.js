// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import assert from 'node:assert/strict';
import { execFileSync, spawn } from 'node:child_process';
import crypto from 'node:crypto';
import fs from 'node:fs';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import test, { after } from 'node:test';
import { fileURLToPath } from 'node:url';
import JSZip from 'jszip';

const tool = fileURLToPath(new URL('../tools/build-addons.mjs', import.meta.url));
const sandboxes = [];
after(() => { for (const directory of sandboxes) fs.rmSync(directory, { recursive: true, force: true }); });
const sandbox = () => { const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'build-addons-')); sandboxes.push(directory); return directory; };
const write = (file, text) => { fs.mkdirSync(path.dirname(file), { recursive: true }); fs.writeFileSync(file, text); };
const tocBase = '## Interface: 30300\n## Title: Foo\n## Notes: Foo addon.\nFoo.lua\n';
const sha = (bytes) => crypto.createHash('sha256').update(bytes).digest('hex');

// Se lanza como proceso aparte (asíncrono) para que el servidor HTTP de la
// prueba pueda atender la descarga mientras la herramienta espera.
function run(args) {
  return new Promise((resolve) => {
    const child = spawn(process.execPath, [tool, ...args], { stdio: ['ignore', 'pipe', 'pipe'] });
    let stdout = '';
    let stderr = '';
    child.stdout.on('data', (chunk) => { stdout += chunk; });
    child.stderr.on('data', (chunk) => { stderr += chunk; });
    child.on('close', (status) => resolve({ status, stdout, stderr }));
  });
}

const expectedTree = (directory) => execFileSync(process.execPath, [tool, 'arbol', '--destino', directory], { encoding: 'utf8' });

test('verificar: coincide, ignora CRLF y mayúsculas de rutas, y detecta ausentes y alterados', async () => {
  const root = sandbox();
  const reference = path.join(root, 'ref');
  write(path.join(reference, 'Foo', 'Foo.toc'), tocBase);
  write(path.join(reference, 'Foo', 'Libs', 'a.lua'), 'print(1)\n');
  write(path.join(reference, 'Bar', 'Bar.toc'), tocBase);
  write(path.join(reference, 'Bar', 'notas.textile'), 'uno'+String.fromCharCode(10)+'dos'+String.fromCharCode(10));   // extensión poco común: igual es texto
  fs.writeFileSync(path.join(reference, 'Bar', 'datos.bin'), Buffer.from([0, 13, 10, 1, 13, 10])); // binario: no se toca
  const recipe = path.join(root, 'receta');
  write(path.join(recipe, 'arbol.tsv'), expectedTree(reference));

  const copy = path.join(root, 'copy');
  write(path.join(copy, 'Foo', 'Foo.toc'), tocBase.replace(/\n/g, '\r\n'));
  write(path.join(copy, 'Foo', 'libs', 'a.lua'), 'print(1)\r\n');
  write(path.join(copy, 'Bar', 'Bar.toc'), tocBase);
  write(path.join(copy, 'Bar', 'notas.textile'), 'uno\r\ndos\r\n');
  fs.writeFileSync(path.join(copy, 'Bar', 'datos.bin'), Buffer.from([0, 13, 10, 1, 13, 10]));
  assert.equal((await run(['verificar', '--destino', copy, '--receta', recipe])).status, 0);
  // Un binario con otros bytes sí es un cambio, aunque sólo difiera en los saltos de línea.
  fs.writeFileSync(path.join(copy, 'Bar', 'datos.bin'), Buffer.from([0, 10, 1, 10]));
  assert.equal((await run(['verificar', '--destino', copy, '--receta', recipe])).status, 1);
  fs.writeFileSync(path.join(copy, 'Bar', 'datos.bin'), Buffer.from([0, 13, 10, 1, 13, 10]));

  write(path.join(copy, 'Bar', 'Bar.toc'), `${tocBase}extra\n`);
  const altered = await run(['verificar', '--destino', copy, '--receta', recipe]);
  assert.equal(altered.status, 1);
  assert.match(altered.stdout, /alteradas: Bar/);

  fs.rmSync(path.join(copy, 'Foo'), { recursive: true });
  const missing = await run(['verificar', '--destino', copy, '--receta', recipe]);
  assert.equal(missing.status, 1);
  assert.match(missing.stdout, /ausentes: Foo/);
});

async function packageZip() {
  const zip = new JSZip();
  zip.file('Foo/Foo.toc', tocBase);
  zip.file('Foo/Foo.lua', 'print("foo")\n');
  return zip.generateAsync({ type: 'nodebuffer' });
}

async function packageServer(bytes) {
  const server = http.createServer((_request, response) => { response.end(bytes); });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  return server;
}

function recipeFor(root, port, archiveSha) {
  const recipe = path.join(root, 'receta');
  write(path.join(recipe, 'fuentes.json'), JSON.stringify({
    version: 1,
    origen: { raw: `http://127.0.0.1:${port}`, ruta: 'src' },
    paquetes: [{ archivo: 'Foo.zip', commit: 'a'.repeat(40), sha256: archiveSha, directorios: ['Foo'] }],
    repos: [], parches: {}, propios: [],
  }));
  write(path.join(recipe, 'catalog.json'), JSON.stringify({ addons: [{ id: 'foo', directories: ['Foo'] }] }));
  write(path.join(recipe, 'descriptions-es.json'), JSON.stringify({ foo: 'Addon de prueba para el cliente.' }));
  // Contenido esperado = lo que sale tras localizar el .toc.
  const expected = path.join(root, 'esperado');
  write(path.join(expected, 'Foo', 'Foo.toc'), tocBase.replace('Foo addon.\n', 'Foo addon.\n## Notes-esES: Addon de prueba para el cliente.\n'));
  write(path.join(expected, 'Foo', 'Foo.lua'), 'print("foo")\n');
  write(path.join(recipe, 'arbol.tsv'), expectedTree(expected));
  return recipe;
}

test('build: descarga, verifica el SHA-256, localiza el .toc y publica', async () => {
  const root = sandbox();
  const bytes = await packageZip();
  const server = await packageServer(bytes);
  try {
    const recipe = recipeFor(root, server.address().port, sha(bytes));
    const destination = path.join(root, 'AddOns');
    write(path.join(destination, 'Foo', 'viejo.lua'), 'anterior\n');
    const result = await run(['build', '--destino', destination, '--cache', path.join(root, 'cache'), '--receta', recipe]);
    assert.equal(result.status, 0, result.stderr);
    assert.match(fs.readFileSync(path.join(destination, 'Foo', 'Foo.toc'), 'utf8'), /Notes-esES: Addon de prueba/);
    assert.equal(fs.existsSync(path.join(destination, 'Foo', 'viejo.lua')), false);
    assert.deepEqual(fs.readdirSync(destination).filter((name) => name.startsWith('.')), []);
    if (process.platform !== 'win32') {
      // El panel corre como otro usuario: carpetas y ficheros legibles para todos.
      assert.equal(fs.statSync(path.join(destination, 'Foo')).mode & 0o755, 0o755);
      assert.equal(fs.statSync(path.join(destination, 'Foo', 'Foo.toc')).mode & 0o644, 0o644);
    }
  } finally {
    server.close();
  }
});

test('build: un SHA-256 distinto detiene la construcción y no toca el destino', async () => {
  const root = sandbox();
  const bytes = await packageZip();
  const server = await packageServer(bytes);
  try {
    const recipe = recipeFor(root, server.address().port, '0'.repeat(64));
    const destination = path.join(root, 'AddOns');
    write(path.join(destination, 'Foo', 'viejo.lua'), 'anterior\n');
    const result = await run(['build', '--destino', destination, '--cache', path.join(root, 'cache'), '--receta', recipe]);
    assert.notEqual(result.status, 0);
    assert.match(result.stderr, /SHA-256/);
    assert.equal(fs.readFileSync(path.join(destination, 'Foo', 'viejo.lua'), 'utf8'), 'anterior\n');
    assert.equal(fs.existsSync(path.join(destination, 'Foo', 'Foo.toc')), false);
    assert.deepEqual(fs.readdirSync(destination).filter((name) => name.startsWith('.')), []);
  } finally {
    server.close();
  }
});

test('build: un resultado que no coincide con arbol.tsv no se publica', async () => {
  const root = sandbox();
  const bytes = await packageZip();
  const server = await packageServer(bytes);
  try {
    const recipe = recipeFor(root, server.address().port, sha(bytes));
    write(path.join(recipe, 'arbol.tsv'), `Foo\t${'0'.repeat(64)}\t2\t10\n`);
    const destination = path.join(root, 'AddOns');
    const result = await run(['build', '--destino', destination, '--cache', path.join(root, 'cache'), '--receta', recipe]);
    assert.notEqual(result.status, 0);
    assert.match(result.stderr, /no coincide con arbol\.tsv/);
    assert.equal(fs.existsSync(path.join(destination, 'Foo')), false);
  } finally {
    server.close();
  }
});
