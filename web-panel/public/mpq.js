// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Lector de archivos MPQ (formato de World of Warcraft 3.3.5a), sólo lectura.
//
// Sirve en el navegador (Blob/File de la File System Access API) y en Node
// (pruebas): trabaja contra una "fuente" { size, read(offset, length) } y no
// carga el archivo entero, sólo las tablas de hash y de bloques y los sectores
// del fichero pedido. Un MPQ del cliente pesa varios GB; esto lee unos MB.
//
// Soporta MPQ v1-v4 (cabecera de usuario incluida), ficheros por sectores y de
// una pieza, CRC de sector, cifrado y las compresiones zlib, PKWARE (implode) y
// bzip2 que usan los DBC y los BLP del cliente. Otras (Huffman, ADPCM, parches
// delta) lanzan un error claro en vez de devolver datos erróneos.

const FLAG_IMPLODE = 0x00000100;
const FLAG_COMPRESS = 0x00000200;
const FLAG_ENCRYPTED = 0x00010000;
const FLAG_FIX_KEY = 0x00020000;
const FLAG_PATCH_FILE = 0x00100000;
const FLAG_SINGLE_UNIT = 0x01000000;
const FLAG_DELETE_MARKER = 0x02000000;
const FLAG_SECTOR_CRC = 0x04000000;
const FLAG_EXISTS = 0x80000000;

// ---------------------------------------------------------------- cifrado y hash
const cryptTable = (() => {
  const table = new Uint32Array(0x500);
  let seed = 0x00100001;
  for (let index1 = 0; index1 < 0x100; index1 += 1) {
    let index2 = index1;
    for (let round = 0; round < 5; round += 1, index2 += 0x100) {
      seed = (seed * 125 + 3) % 0x2AAAAB;
      const high = (seed & 0xFFFF) << 16;
      seed = (seed * 125 + 3) % 0x2AAAAB;
      table[index2] = (high | (seed & 0xFFFF)) >>> 0;
    }
  }
  return table;
})();

export function hashString(name, type) {
  let seed1 = 0x7FED7FED;
  let seed2 = 0xEEEEEEEE;
  for (let index = 0; index < name.length; index += 1) {
    let char = name.charCodeAt(index);
    if (char === 0x2F) char = 0x5C;
    if (char >= 0x61 && char <= 0x7A) char -= 0x20;
    seed1 = (cryptTable[(type << 8) + char] ^ ((seed1 + seed2) >>> 0)) >>> 0;
    seed2 = (char + seed1 + seed2 + ((seed2 << 5) >>> 0) + 3) >>> 0;
  }
  return seed1;
}

function decryptWords(words, key) {
  let seed = 0xEEEEEEEE;
  let current = key >>> 0;
  for (let index = 0; index < words.length; index += 1) {
    seed = (seed + cryptTable[0x400 + (current & 0xFF)]) >>> 0;
    const value = (words[index] ^ ((current + seed) >>> 0)) >>> 0;
    words[index] = value;
    current = ((((~current) << 0x15) + 0x11111111) | (current >>> 0x0B)) >>> 0;
    seed = (value + seed + ((seed << 5) >>> 0) + 3) >>> 0;
  }
}

function decryptBytes(bytes, key) {
  const whole = bytes.length - (bytes.length % 4);
  const words = new Uint32Array(bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + whole));
  decryptWords(words, key);
  const out = new Uint8Array(bytes.length);
  out.set(new Uint8Array(words.buffer), 0);
  out.set(bytes.subarray(whole), whole);
  return out;
}

// ---------------------------------------------------------------- compresiones
async function inflateZlib(bytes) {
  const stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream('deflate'));
  return new Uint8Array(await new Response(stream).arrayBuffer());
}

// PKWARE DCL "explode" (port de blast.c, Mark Adler, zlib).
const LITLEN_BASE = [3, 2, 4, 5, 6, 7, 8, 9, 10, 12, 16, 24, 40, 72, 136, 264];
const LITLEN_EXTRA = [0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8];
const LITLEN_REP = [11, 124, 8, 7, 28, 7, 188, 13, 76, 4, 10, 8, 12, 10, 12, 10, 8, 23, 8, 9, 7, 6, 7, 8, 7, 6, 55, 8, 23, 24, 12, 11, 7, 9, 11, 12, 6, 7, 22, 5, 7, 24, 6, 11, 9, 6, 7, 22, 7, 11, 38, 7, 9, 8, 25, 11, 8, 11, 9, 12, 8, 12, 5, 38, 5, 38, 5, 11, 7, 5, 6, 21, 6, 10, 53, 8, 7, 24, 10, 27, 44, 253, 253, 253, 252, 252, 252, 13, 12, 45, 12, 45, 12, 61, 12, 45, 44, 173];
const LENGTH_REP = [2, 35, 36, 53, 38, 23];
const DISTANCE_REP = [2, 20, 53, 230, 247, 151, 248];

function buildHuffman(repeats) {
  const lengths = [];
  for (const rep of repeats) {
    const times = (rep >> 4) + 1;
    for (let index = 0; index < times; index += 1) lengths.push(rep & 15);
  }
  const count = new Array(16).fill(0);
  for (const len of lengths) count[len] += 1;
  const offs = new Array(16).fill(0);
  for (let len = 1; len < 15; len += 1) offs[len + 1] = offs[len] + count[len];
  const symbol = [];
  lengths.forEach((len, sym) => { if (len) { symbol[offs[len]] = sym; offs[len] += 1; } });
  return { count, symbol };
}
const LITCODE = buildHuffman(LITLEN_REP);
const LENCODE = buildHuffman(LENGTH_REP);
const DISTCODE = buildHuffman(DISTANCE_REP);

export function explodePkware(input, expectedSize) {
  let position = 0;
  let bitBuffer = 0;
  let bitCount = 0;
  const out = new Uint8Array(expectedSize);
  let outPosition = 0;
  const need = () => { if (position >= input.length) throw new Error('PKWARE: datos truncados'); };
  const bits = (n) => {
    let value = bitBuffer;
    while (bitCount < n) {
      need();
      value |= input[position++] << bitCount;
      bitCount += 8;
    }
    bitBuffer = value >> n;
    bitCount -= n;
    return value & ((1 << n) - 1);
  };
  const decode = (huffman) => {
    let code = 0; let first = 0; let index = 0;
    let bitBufferLocal = bitBuffer; let left = bitCount;
    for (let len = 1; ; len += 1) {
      if (left === 0) { need(); bitBufferLocal = input[position++]; left = 8; }
      code |= (bitBufferLocal & 1) ^ 1;
      bitBufferLocal >>= 1; left -= 1;
      const count = huffman.count[len];
      if (code < first + count) { bitBuffer = bitBufferLocal; bitCount = left; return huffman.symbol[index + (code - first)]; }
      index += count; first += count; first <<= 1; code <<= 1;
      if (len >= 15) throw new Error('PKWARE: código no válido');
    }
  };
  const literalMode = bits(8);
  const dictionaryBits = bits(8);
  if (literalMode > 1 || dictionaryBits < 4 || dictionaryBits > 6) throw new Error('PKWARE: cabecera no válida');
  while (outPosition < expectedSize) {
    if (bits(1)) {
      let symbol = decode(LENCODE);
      const length = LITLEN_BASE[symbol] + bits(LITLEN_EXTRA[symbol]);
      if (length === 519) break;
      symbol = length === 2 ? 2 : dictionaryBits;
      const distance = (decode(DISTCODE) << symbol) + bits(symbol) + 1;
      if (distance > outPosition) throw new Error('PKWARE: distancia fuera de rango');
      for (let index = 0; index < length && outPosition < expectedSize; index += 1) {
        out[outPosition] = out[outPosition - distance];
        outPosition += 1;
      }
    } else {
      out[outPosition++] = literalMode ? decode(LITCODE) : bits(8);
    }
  }
  return outPosition === expectedSize ? out : out.subarray(0, outPosition);
}

// bzip2 (decodificador compacto).
export function inflateBzip2(input) {
  let position = 0;
  let bitBuffer = 0;
  let bitCount = 0;
  const readBits = (n) => {
    while (bitCount < n) {
      if (position >= input.length) throw new Error('bzip2: datos truncados');
      bitBuffer = bitBuffer * 256 + input[position++];
      bitCount += 8;
    }
    const shift = 2 ** (bitCount - n);
    const value = Math.floor(bitBuffer / shift);
    bitBuffer -= value * shift;
    bitCount -= n;
    return value;
  };
  if (readBits(8) !== 0x42 || readBits(8) !== 0x5A || readBits(8) !== 0x68) throw new Error('bzip2: cabecera no válida');
  const level = readBits(8) - 0x30;
  if (level < 1 || level > 9) throw new Error('bzip2: tamaño de bloque no válido');
  const blockSize = level * 100000;
  const chunks = [];
  let total = 0;
  for (;;) {
    const magicHigh = readBits(24);
    const magicLow = readBits(24);
    if (magicHigh === 0x177245 && magicLow === 0x385090) { readBits(32); break; }
    if (magicHigh !== 0x314159 || magicLow !== 0x265359) throw new Error('bzip2: bloque no válido');
    readBits(32); // CRC
    if (readBits(1)) throw new Error('bzip2: bloques aleatorizados no soportados');
    const origPointer = readBits(24);
    const usedGroups = readBits(16);
    const seqToUnseq = [];
    for (let group = 0; group < 16; group += 1) {
      if (usedGroups & (0x8000 >> group)) {
        const used = readBits(16);
        for (let bit = 0; bit < 16; bit += 1) if (used & (0x8000 >> bit)) seqToUnseq.push(group * 16 + bit);
      }
    }
    const alphaSize = seqToUnseq.length + 2;
    const groupCount = readBits(3);
    const selectorCount = readBits(15);
    if (groupCount < 2 || groupCount > 6 || selectorCount < 1) throw new Error('bzip2: tablas no válidas');
    const mtf = [];
    for (let index = 0; index < groupCount; index += 1) mtf.push(index);
    const selectors = [];
    for (let index = 0; index < selectorCount; index += 1) {
      let jump = 0;
      while (readBits(1)) { jump += 1; if (jump >= groupCount) throw new Error('bzip2: selector no válido'); }
      const value = mtf.splice(jump, 1)[0];
      mtf.unshift(value);
      selectors.push(value);
    }
    const tables = [];
    for (let group = 0; group < groupCount; group += 1) {
      const lengths = [];
      let current = readBits(5);
      for (let symbol = 0; symbol < alphaSize; symbol += 1) {
        for (;;) {
          if (current < 1 || current > 20) throw new Error('bzip2: longitud de código no válida');
          if (!readBits(1)) break;
          current += readBits(1) ? -1 : 1;
        }
        lengths.push(current);
      }
      let minLen = 32; let maxLen = 0;
      for (const len of lengths) { if (len > maxLen) maxLen = len; if (len < minLen) minLen = len; }
      const limit = new Array(maxLen + 2).fill(0);
      const base = new Array(maxLen + 2).fill(0);
      const perm = [];
      let pp = 0;
      for (let len = minLen; len <= maxLen; len += 1) for (let symbol = 0; symbol < alphaSize; symbol += 1) if (lengths[symbol] === len) perm[pp++] = symbol;
      const count = new Array(maxLen + 2).fill(0);
      for (const len of lengths) count[len] += 1;
      let code = 0; let index = 0;
      for (let len = minLen; len <= maxLen; len += 1) {
        base[len] = index - code;
        code += count[len];
        index += count[len];
        limit[len] = code - 1;
        code <<= 1;
      }
      tables.push({ minLen, maxLen, limit, base, perm });
    }
    // Decodificación Huffman + MTF + RLE2
    const endOfBlock = alphaSize - 1;
    const symbolsMtf = [];
    for (let index = 0; index < seqToUnseq.length; index += 1) symbolsMtf.push(index);
    const tt = new Uint32Array(blockSize);
    const unzftab = new Array(256).fill(0);
    let nblock = 0; let selectorIndex = 0; let groupPosition = 0; let table = null;
    let runLength = 0; let runBit = 1;
    const flushRun = () => {
      if (runLength) {
        const byte = seqToUnseq[symbolsMtf[0]];
        unzftab[byte] += runLength;
        if (nblock + runLength > blockSize) throw new Error('bzip2: bloque demasiado grande');
        tt.fill(byte, nblock, nblock + runLength);
        nblock += runLength; runLength = 0; runBit = 1;
      }
    };
    for (;;) {
      if (groupPosition === 0) {
        if (selectorIndex >= selectors.length) throw new Error('bzip2: faltan selectores');
        table = tables[selectors[selectorIndex++]];
        groupPosition = 50;
      }
      groupPosition -= 1;
      let len = table.minLen;
      let code = readBits(len);
      while (code > table.limit[len]) {
        len += 1;
        if (len > table.maxLen) throw new Error('bzip2: código Huffman no válido');
        code = (code << 1) | readBits(1);
      }
      const symbol = table.perm[code + table.base[len]];
      if (symbol === 0 || symbol === 1) { runLength += runBit << symbol; runBit <<= 1; continue; }
      flushRun();
      if (symbol === endOfBlock) break;
      const slot = symbol - 1;
      const value = symbolsMtf.splice(slot, 1)[0];
      symbolsMtf.unshift(value);
      const byte = seqToUnseq[value];
      unzftab[byte] += 1;
      if (nblock >= blockSize) throw new Error('bzip2: bloque demasiado grande');
      tt[nblock++] = byte;
    }
    // BWT inversa
    const cftab = new Array(257).fill(0);
    for (let index = 0; index < 256; index += 1) cftab[index + 1] = cftab[index] + unzftab[index];
    for (let index = 0; index < nblock; index += 1) {
      const byte = tt[index] & 0xFF;
      tt[cftab[byte]] |= index << 8;
      cftab[byte] += 1;
    }
    let pointer = tt[origPointer] >>> 8;
    let block = new Uint8Array(Math.max(1024, nblock + (nblock >> 2)));
    let blockLength = 0;
    const ensure = (extra) => {
      if (blockLength + extra <= block.length) return;
      const bigger = new Uint8Array(Math.max(block.length * 2, blockLength + extra));
      bigger.set(block.subarray(0, blockLength));
      block = bigger;
    };
    // RLE1
    let previous = -1; let repeat = 0;
    for (let index = 0; index < nblock; index += 1) {
      const entry = tt[pointer];
      const byte = entry & 0xFF;
      pointer = entry >>> 8;
      if (repeat === 4) {
        ensure(byte);
        block.fill(previous, blockLength, blockLength + byte);
        blockLength += byte;
        repeat = 0; previous = -1;
        continue;
      }
      if (byte === previous) repeat += 1; else { repeat = 1; previous = byte; }
      ensure(1);
      block[blockLength++] = byte;
    }
    chunks.push(block.subarray(0, blockLength));
    total += blockLength;
    // los bloques siguen alineados a bit: no hay relleno entre ellos
  }
  const out = new Uint8Array(total);
  let offset = 0;
  for (const chunk of chunks) { out.set(chunk, offset); offset += chunk.length; }
  return out;
}

async function decompressSector(raw, expected) {
  if (raw.length >= expected) return raw.subarray(0, expected);
  const mask = raw[0];
  let data = raw.subarray(1);
  const known = 0x02 | 0x08 | 0x10;
  if (mask & ~known) throw new Error(`Compresión MPQ no soportada (máscara 0x${mask.toString(16)})`);
  // Orden de aplicación inverso al de compresión: bzip2, PKWARE, zlib.
  if (mask & 0x10) data = inflateBzip2(data);
  if (mask & 0x08) data = explodePkware(data, expected);
  if (mask & 0x02) data = await inflateZlib(data);
  if (data.length !== expected) throw new Error(`Sector descomprimido de ${data.length} bytes, se esperaban ${expected}`);
  return data;
}

// ---------------------------------------------------------------- archivo
const view = (bytes) => new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);

export class MpqArchive {
  constructor(source, name, header, hashTable, blockTable) {
    this.source = source;
    this.name = name;
    this.header = header;
    this.hashTable = hashTable;
    this.blockTable = blockTable;
  }

  static async open(source, name = '') {
    // Cabecera (o cabecera de usuario 'MPQ\x1B' que apunta a la real).
    let base = 0;
    let head = await source.read(0, 44);
    let dv = view(head);
    if (dv.getUint32(0, true) === 0x1B51504D) {
      base = dv.getUint32(8, true);
      head = await source.read(base, 44);
      dv = view(head);
    }
    if (dv.getUint32(0, true) !== 0x1A51504D) throw new Error(`${name || 'El archivo'} no es un MPQ`);
    const formatVersion = dv.getUint16(12, true);
    const sectorShift = dv.getUint16(14, true);
    let hashPosition = dv.getUint32(16, true);
    let blockPosition = dv.getUint32(20, true);
    const hashSize = dv.getUint32(24, true);
    const blockSize = dv.getUint32(28, true);
    let hiBlockPosition = 0;
    if (formatVersion >= 1) {
      hiBlockPosition = Number(dv.getBigUint64(32, true));
      hashPosition += dv.getUint16(40, true) * 2 ** 32;
      blockPosition += dv.getUint16(42, true) * 2 ** 32;
    }
    if (!hashSize || (hashSize & (hashSize - 1)) || hashSize > 1 << 24 || blockSize > 1 << 24) throw new Error('Tablas MPQ no válidas');
    const hashTable = await MpqArchive.readTable(source, base + hashPosition, hashSize * 16, '(hash table)');
    const blockTable = await MpqArchive.readTable(source, base + blockPosition, blockSize * 16, '(block table)');
    let hiBlocks = null;
    if (hiBlockPosition) {
      const hi = await source.read(base + hiBlockPosition, blockSize * 2);
      hiBlocks = new Uint16Array(hi.buffer.slice(hi.byteOffset, hi.byteOffset + hi.byteLength));
    }
    return new MpqArchive(source, name, { base, sectorSize: 512 << sectorShift, formatVersion, hashSize, blockSize, hiBlocks }, hashTable, blockTable);
  }

  static async readTable(source, position, length, key) {
    const bytes = await source.read(position, length);
    if (bytes.length !== length) throw new Error('Tabla MPQ truncada');
    const plain = decryptBytes(bytes, hashString(key, 3));
    return new Uint32Array(plain.buffer.slice(plain.byteOffset, plain.byteOffset + plain.byteLength));
  }

  // Devuelve el índice de bloque de `path` (neutro o el primero que case) o -1.
  find(path) {
    const mask = this.header.hashSize - 1;
    const start = hashString(path, 0) & mask;
    const nameA = hashString(path, 1);
    const nameB = hashString(path, 2);
    let found = -1;
    for (let step = 0, index = start; step < this.header.hashSize; step += 1, index = (index + 1) & mask) {
      const base = index * 4;
      const block = this.hashTable[base + 3];
      if (block === 0xFFFFFFFF) break;
      if (block === 0xFFFFFFFE) continue;
      if (this.hashTable[base] === nameA && this.hashTable[base + 1] === nameB) {
        const locale = this.hashTable[base + 2] & 0xFFFF;
        if (locale === 0) return block;
        if (found < 0) found = block;
      }
    }
    return found;
  }

  has(path) {
    const block = this.find(path);
    return block >= 0 && block < this.header.blockSize && (this.blockTable[block * 4 + 3] & FLAG_EXISTS) !== 0
      && (this.blockTable[block * 4 + 3] & FLAG_DELETE_MARKER) === 0;
  }

  // Lee el fichero completo; null si no está en este archivo.
  async read(path) {
    const block = this.find(path);
    if (block < 0 || block >= this.header.blockSize) return null;
    const entry = block * 4;
    let position = this.blockTable[entry];
    const compressedSize = this.blockTable[entry + 1];
    const fileSize = this.blockTable[entry + 2];
    const flags = this.blockTable[entry + 3];
    if (!(flags & FLAG_EXISTS) || (flags & FLAG_DELETE_MARKER)) return null;
    if (flags & FLAG_PATCH_FILE) throw new Error(`${path} es un parche delta de MPQ (no soportado)`);
    if (this.header.hiBlocks) position += this.header.hiBlocks[block] * 2 ** 32;
    position += this.header.base;
    if (fileSize === 0) return new Uint8Array(0);

    let key = 0;
    if (flags & FLAG_ENCRYPTED) {
      key = hashString(path.split(/[\\/]/).pop(), 3);
      if (flags & FLAG_FIX_KEY) key = (((key + (position - this.header.base)) >>> 0) ^ fileSize) >>> 0;
    }

    if (flags & FLAG_SINGLE_UNIT) {
      let raw = await this.source.read(position, compressedSize);
      if (flags & FLAG_ENCRYPTED) raw = decryptBytes(raw, key);
      if (flags & (FLAG_COMPRESS | FLAG_IMPLODE) && compressedSize < fileSize) return this.unpack(raw, fileSize, flags);
      return raw.subarray(0, fileSize);
    }

    const sectorSize = this.header.sectorSize;
    const sectors = Math.ceil(fileSize / sectorSize);
    const tableEntries = sectors + 1 + ((flags & FLAG_SECTOR_CRC) ? 1 : 0);
    let tableBytes = await this.source.read(position, tableEntries * 4);
    if (flags & FLAG_ENCRYPTED) tableBytes = decryptBytes(tableBytes, (key - 1) >>> 0);
    const offsets = new Uint32Array(tableBytes.buffer.slice(tableBytes.byteOffset, tableBytes.byteOffset + tableBytes.byteLength));
    const out = new Uint8Array(fileSize);
    // Un solo read por tramo contiguo de sectores (los ficheros suelen serlo).
    const first = offsets[0];
    const last = offsets[sectors];
    if (last < first || last > compressedSize + 8) throw new Error(`Tabla de sectores corrupta en ${path}`);
    const body = await this.source.read(position + first, last - first);
    for (let index = 0; index < sectors; index += 1) {
      const from = offsets[index] - first;
      const to = offsets[index + 1] - first;
      const expected = index === sectors - 1 ? fileSize - index * sectorSize : sectorSize;
      let raw = body.subarray(from, to);
      if (flags & FLAG_ENCRYPTED) raw = decryptBytes(raw, (key + index) >>> 0);
      const sector = (flags & (FLAG_COMPRESS | FLAG_IMPLODE)) && raw.length < expected
        ? await this.unpackSector(raw, expected, flags)
        : raw.subarray(0, expected);
      out.set(sector, index * sectorSize);
    }
    return out;
  }

  unpack(raw, size, flags) { return this.unpackSector(raw, size, flags); }

  async unpackSector(raw, expected, flags) {
    if (flags & FLAG_IMPLODE && !(flags & FLAG_COMPRESS)) return explodePkware(raw, expected);
    return decompressSector(raw, expected);
  }
}

// Fuente sobre un Blob/File (navegador) o cualquier objeto con slice().arrayBuffer().
export function blobSource(blob) {
  return {
    size: blob.size,
    async read(offset, length) {
      const end = Math.min(blob.size, offset + length);
      return new Uint8Array(await blob.slice(offset, end).arrayBuffer());
    },
  };
}

// Orden de prioridad de los MPQ del cliente 3.3.5a (mayor número = manda).
// Mismo criterio que tools/construir-parche-cliente-items.py (_mpqs_idioma).
export function archivePriority(relativePath, language) {
  const parts = relativePath.split('/');
  const name = parts[parts.length - 1].toLowerCase();
  const loc = (language || '').toLowerCase();
  const base = { 'common.mpq': 0, 'common-2.mpq': 1, 'expansion.mpq': 2, 'lichking.mpq': 3, 'patch.mpq': 4, 'patch-2.mpq': 5, 'patch-3.mpq': 6 };
  if (name in base) return base[name];
  if (name === `locale-${loc}.mpq`) return 10;
  if (name === `patch-${loc}.mpq`) return 20;
  if (name === `patch-${loc}-2.mpq`) return 21;
  if (name === `patch-${loc}-3.mpq`) return 22;
  if (name.startsWith('patch-') && name.endsWith('.mpq')) {
    const tag = name.slice(6, -4);
    if (tag.startsWith('zava')) return 200;
    if (tag.length === 1 && /[a-z0-9]/.test(tag)) return 30 + tag.charCodeAt(0);
    return 100;
  }
  return 50;
}
