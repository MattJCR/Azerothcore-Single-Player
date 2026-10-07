// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import { readFileSync } from 'node:fs';
import test from 'node:test';
import zlib from 'node:zlib';
import { MpqArchive, archivePriority, blobSource, explodePkware, hashString, inflateBzip2 } from '../public/mpq.js';

// ---- Escritor mínimo de MPQ v1 para las pruebas (el real sólo lee) ----------
function encryptWords(bytes, key) {
  const table = []; // la tabla de cifrado sale del lector: se reconstruye igual
  let seed = 0x00100001;
  for (let i1 = 0; i1 < 0x100; i1 += 1) {
    let i2 = i1;
    for (let round = 0; round < 5; round += 1, i2 += 0x100) {
      seed = (seed * 125 + 3) % 0x2AAAAB;
      const high = (seed & 0xFFFF) << 16;
      seed = (seed * 125 + 3) % 0x2AAAAB;
      table[i2] = (high | (seed & 0xFFFF)) >>> 0;
    }
  }
  // Como en un MPQ real: sólo se cifran las palabras enteras; el resto queda en claro.
  const whole = bytes.length - (bytes.length % 4);
  const words = new Uint32Array(bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + whole));
  const tail = bytes.subarray(whole);
  let s = 0xEEEEEEEE;
  let k = key >>> 0;
  for (let i = 0; i < words.length; i += 1) {
    s = (s + table[0x400 + (k & 0xFF)]) >>> 0;
    const plain = words[i];
    words[i] = (plain ^ ((k + s) >>> 0)) >>> 0;
    k = ((((~k) << 0x15) + 0x11111111) | (k >>> 0x0B)) >>> 0;
    s = (plain + s + ((s << 5) >>> 0) + 3) >>> 0;
  }
  const out = new Uint8Array(bytes.length);
  out.set(new Uint8Array(words.buffer), 0);
  out.set(tail, whole);
  return out;
}

const word = (value) => { const b = Buffer.alloc(4); b.writeUInt32LE(value >>> 0); return b; };

// entries: [{ name, data, mode: 'sectors'|'single', pack: 'zlib'|'bzip2'|'none'|'implode', crc, encrypted, raw }]
function buildMpq(entries, { sectorShift = 3 } = {}) {
  const sectorSize = 512 << sectorShift;
  const chunks = [Buffer.alloc(32)];
  let position = 32;
  const blocks = [];
  for (const entry of entries) {
    const data = Buffer.from(entry.data);
    let flags = 0x80000000;
    let body;
    const key = hashString(entry.name.split('\\').pop(), 3);
    const pack = (piece) => {
      if (entry.pack === 'zlib') { const z = Buffer.concat([Buffer.from([0x02]), zlib.deflateSync(piece)]); return z.length < piece.length ? z : piece; }
      if (entry.pack === 'bzip2') return Buffer.concat([Buffer.from([0x10]), entry.bz2]);
      if (entry.pack === 'implode') return entry.imploded;
      return piece;
    };
    if (entry.pack && entry.pack !== 'none') flags |= entry.pack === 'implode' ? 0x100 : 0x200;
    if (entry.mode === 'single') {
      flags |= 0x01000000;
      body = pack(data);
      if (entry.encrypted) { flags |= 0x10000; body = Buffer.from(encryptWords(body, key)); }
    } else {
      if (entry.crc) flags |= 0x04000000;
      const sectors = Math.ceil(data.length / sectorSize);
      const parts = [];
      for (let index = 0; index < sectors; index += 1) {
        let piece = pack(data.subarray(index * sectorSize, (index + 1) * sectorSize));
        if (entry.encrypted) piece = Buffer.from(encryptWords(piece, (key + index) >>> 0));
        parts.push(piece);
      }
      const entries2 = sectors + 1 + (entry.crc ? 1 : 0);
      let offset = entries2 * 4;
      const offsets = [offset];
      for (const part of parts) { offset += part.length; offsets.push(offset); }
      if (entry.crc) offsets.push(offset);
      let table = Buffer.concat(offsets.map(word));
      if (entry.encrypted) { flags |= 0x10000; table = Buffer.from(encryptWords(table, (key - 1) >>> 0)); }
      body = Buffer.concat([table, ...parts]);
    }
    blocks.push({ position, packed: body.length, size: data.length, flags });
    chunks.push(body);
    position += body.length;
  }
  let hashSize = 16;
  while (hashSize < entries.length * 2) hashSize <<= 1;
  const hash = new Array(hashSize).fill(null).map(() => [0xFFFFFFFF, 0xFFFFFFFF, 0xFFFF, 0xFFFF, 0xFFFFFFFF]);
  entries.forEach((entry, block) => {
    let index = hashString(entry.name, 0) & (hashSize - 1);
    while (hash[index][4] !== 0xFFFFFFFF) index = (index + 1) & (hashSize - 1);
    hash[index] = [hashString(entry.name, 1), hashString(entry.name, 2), 0, 0, block];
  });
  const hashBytes = Buffer.concat(hash.map(([a, b, locale, platform, block]) => Buffer.concat([word(a), word(b), Buffer.from([locale & 255, locale >> 8, platform & 255, platform >> 8]), word(block)])));
  const blockBytes = Buffer.concat(blocks.flatMap((b) => [word(b.position), word(b.packed), word(b.size), word(b.flags)]));
  const hashEncrypted = Buffer.from(encryptWords(hashBytes, hashString('(hash table)', 3)));
  const blockEncrypted = Buffer.from(encryptWords(blockBytes, hashString('(block table)', 3)));
  const hashPosition = position;
  const blockPosition = hashPosition + hashEncrypted.length;
  const header = Buffer.alloc(32);
  header.write('MPQ\x1a', 0, 'latin1');
  header.writeUInt32LE(32, 4);
  header.writeUInt32LE(blockPosition + blockEncrypted.length, 8);
  header.writeUInt16LE(0, 12);
  header.writeUInt16LE(sectorShift, 14);
  header.writeUInt32LE(hashPosition, 16);
  header.writeUInt32LE(blockPosition, 20);
  header.writeUInt32LE(hashSize, 24);
  header.writeUInt32LE(blocks.length, 28);
  chunks[0] = header;
  return Buffer.concat([...chunks, hashEncrypted, blockEncrypted]);
}

const open = (buffer) => MpqArchive.open(blobSource(new Blob([buffer])), 'prueba.mpq');
const sha = (bytes) => crypto.createHash('sha256').update(bytes).digest('hex');
const patterned = (length, seed = 1) => Buffer.from(Array.from({ length }, (_, i) => (i * seed + (i >> 5)) & 0xFF));

test('lee ficheros por sectores (zlib, con y sin CRC), de una pieza y sin comprimir', async () => {
  const big = patterned(10000);
  const noisy = Buffer.from(crypto.randomBytes(9000)); // incompresible: sectores almacenados tal cual
  const mpq = await open(buildMpq([
    { name: 'DBFilesClient\\Item.dbc', data: big, mode: 'sectors', pack: 'zlib' },
    { name: 'Interface\\Icons\\A.blp', data: big, mode: 'sectors', pack: 'zlib', crc: true },
    { name: 'Interface\\Icons\\B.blp', data: patterned(300, 3), mode: 'single', pack: 'zlib' },
    { name: 'Raw.bin', data: noisy, mode: 'sectors', pack: 'zlib' },
    { name: 'Plain.txt', data: Buffer.from('sin comprimir'), mode: 'sectors', pack: 'none' },
    { name: 'Empty.txt', data: Buffer.alloc(0), mode: 'single', pack: 'none' },
  ]));
  assert.equal(sha(await mpq.read('DBFilesClient\\Item.dbc')), sha(big));
  assert.equal(sha(await mpq.read('interface/icons/a.blp')), sha(big), 'barras y mayúsculas no importan');
  assert.equal(sha(await mpq.read('Interface\\Icons\\B.blp')), sha(patterned(300, 3)));
  assert.equal(sha(await mpq.read('Raw.bin')), sha(noisy));
  assert.equal(Buffer.from(await mpq.read('Plain.txt')).toString(), 'sin comprimir');
  assert.equal((await mpq.read('Empty.txt')).length, 0);
  assert.equal(await mpq.read('No\\Existe.dbc'), null);
  assert.equal(mpq.has('Plain.txt'), true);
  assert.equal(mpq.has('No\\Existe.dbc'), false);
});

test('descifra tablas y ficheros cifrados', async () => {
  const data = patterned(5000, 7);
  const mpq = await open(buildMpq([
    { name: 'Dir\\Secreto.dat', data, mode: 'sectors', pack: 'zlib', encrypted: true },
    { name: 'Dir\\Pieza.dat', data: patterned(2000, 5), mode: 'single', pack: 'none', encrypted: true },
  ]));
  assert.equal(sha(await mpq.read('Dir\\Secreto.dat')), sha(data));
  assert.equal(sha(await mpq.read('Dir\\Pieza.dat')), sha(patterned(2000, 5)));
});

test('bzip2: bloques con rachas, texto y datos aleatorios', () => {
  const fixtures = JSON.parse(readFileSync(new URL('./fixtures/bzip2.json', import.meta.url), 'utf8'));
  for (const [name, item] of Object.entries(fixtures)) {
    const plain = inflateBzip2(new Uint8Array(Buffer.from(item.bz2, 'base64')));
    assert.equal(plain.length, item.length, name);
    assert.equal(sha(plain), item.sha256, name);
  }
  assert.throws(() => inflateBzip2(new Uint8Array([1, 2, 3, 4])), /bzip2/);
});

test('bzip2 dentro de un MPQ (máscara 0x10)', async () => {
  const fixtures = JSON.parse(readFileSync(new URL('./fixtures/bzip2.json', import.meta.url), 'utf8'));
  const item = fixtures.texto;
  const plain = inflateBzip2(new Uint8Array(Buffer.from(item.bz2, 'base64')));
  const mpq = await open(buildMpq([{ name: 'Bz.txt', data: Buffer.from(plain), mode: 'single', pack: 'bzip2', bz2: Buffer.from(item.bz2, 'base64') }]));
  assert.equal(sha(await mpq.read('Bz.txt')), item.sha256);
});

test('PKWARE implode: vector de prueba de blast.c', () => {
  const imploded = Uint8Array.from([0x00, 0x04, 0x82, 0x24, 0x25, 0x8f, 0x80, 0x7f]);
  assert.equal(Buffer.from(explodePkware(imploded, 13)).toString('latin1'), 'AIAIAIAIAIAIA');
  assert.throws(() => explodePkware(Uint8Array.from([9, 9, 9]), 4), /PKWARE/);
});

test('rechaza lo que no es un MPQ, los parches delta y las compresiones no soportadas', async () => {
  await assert.rejects(open(Buffer.from('esto no es un mpq, ni de lejos........')), /no es un MPQ/);
  const mpq = await open(buildMpq([{ name: 'X.bin', data: Buffer.from('abcdef'), mode: 'single', pack: 'none' }]));
  assert.equal(await mpq.read('X.bin') !== null, true);
  // Máscara 0x40 (ADPCM) en un sector comprimido: error claro, no basura.
  const adpcm = await open(buildMpq([{ name: 'Y.bin', data: patterned(50), mode: 'single', pack: 'implode', imploded: Buffer.concat([Buffer.from([0x40]), Buffer.from('zzzz')]) }]));
  await assert.rejects(adpcm.read('Y.bin'), /PKWARE|no soportada/);
});

test('prioridad de los MPQ del cliente: base < locale < patch de idioma < letras < zava', () => {
  const order = ['common.MPQ', 'patch-3.MPQ', 'esES/locale-esES.MPQ', 'esES/patch-esES-3.MPQ', 'patch-A.mpq', 'patch-G.mpq', 'patch-Zava.mpq'];
  const ranks = order.map((name) => archivePriority(name, 'esES'));
  assert.deepEqual([...ranks].sort((a, b) => a - b), ranks);
  assert.equal(archivePriority('esES/patch-esES-4.MPQ', 'esES') > 0, true);
});
