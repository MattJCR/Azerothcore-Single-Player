// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// El ciclo de `prepareResources` (lo que hay detrás de «Generar» y «Cancelar» del panel): con el
// servidor ya al día no se envía nada, un ciclo completo envía, inicia y espera, y cancelar en
// cualquier punto (antes de enviar, entre lotes, antes de iniciar o mientras genera) borra el
// trabajo en el servidor en vez de dejarlo a medias. El cliente de WoW se sustituye por un
// `read()` que devuelve DBC mínimos; no hace falta ningún MPQ.
import assert from 'node:assert/strict';
import test from 'node:test';
import { prepareResources } from '../public/client-resources.js';

// ItemDisplayInfo.dbc con un solo registro (campos 5 y 6 = iconos) y dos cadenas.
function itemDisplayInfo() {
  const fields = 8;
  const strings = Buffer.from('\0INV_Sword_01\0', 'latin1');
  const bytes = Buffer.alloc(20 + fields * 4 + strings.length);
  bytes.writeUInt32LE(0x43424457, 0); // WDBC
  bytes.writeInt32LE(1, 4);
  bytes.writeInt32LE(fields, 8);
  bytes.writeInt32LE(fields * 4, 12);
  bytes.writeInt32LE(strings.length, 16);
  bytes.writeInt32LE(1, 20); // id
  bytes.writeInt32LE(1, 20 + 5 * 4); // InventoryIcon[0] -> «INV_Sword_01»
  strings.copy(bytes, 20 + fields * 4);
  return new Uint8Array(bytes);
}

const client = {
  async read(path) {
    if (path.endsWith('ItemDisplayInfo.dbc')) return { data: itemDisplayInfo(), from: 'patch.MPQ' };
    if (path.endsWith('Item.dbc') || path.endsWith('Spell.dbc')) return { data: new Uint8Array([1, 2, 3, path.length]), from: 'locale.MPQ' };
    if (path.toLowerCase().includes('inv_sword_01')) return { data: new Uint8Array([9, 9, 9]), from: 'common.MPQ' };
    return null;
  },
};
class FakeZip {
  constructor() { this.files = 0; }
  file() { this.files += 1; }
  async generateAsync() { return new Uint8Array(this.files); }
}
const PLAN = { iconos: { necesario: true }, 'parche-esES': { necesario: true } };

// Servidor de mentira: guarda las llamadas y responde según el escenario.
function servidor({ alDia = false, onPut = () => {}, estados = ['listo'] } = {}) {
  const llamadas = [];
  const cola = [...estados];
  return {
    llamadas,
    http: {
      async json(metodo, ruta, cuerpo) {
        llamadas.push(`${metodo} ${ruta.replace(/[0-9a-f]{32}/, ':id')}`);
        if (metodo === 'POST' && ruta === '/api/recursos/trabajos') {
          return alDia ? { trabajo: null, plan: {} } : { trabajo: { id: 'a'.repeat(32), ficheros: [] }, plan: PLAN, recibido: cuerpo };
        }
        if (metodo === 'GET') return { trabajo: { estado: cola.length > 1 ? cola.shift() : cola[0], mensaje: 'x', progreso: {} } };
        return {};
      },
      async put(ruta) { llamadas.push(`PUT ${ruta.replace(/[0-9a-f]{32}/, ':id')}`); onPut(); },
    },
  };
}
const preparar = (s, extra = {}) => prepareResources({ client, http: s.http, JSZip: FakeZip, languages: ['esES'], ...extra });

test('con el servidor al día no se envía ni se inicia nada', async () => {
  const s = servidor({ alDia: true });
  const resultado = await preparar(s);
  assert.equal(resultado.estado, 'alDia');
  assert.deepEqual(s.llamadas, ['POST /api/recursos/trabajos']);
  assert.equal(resultado.resumen.iconos, 1);
});

test('un ciclo completo envía los lotes, inicia y termina en «listo»', async () => {
  const s = servidor();
  const resultado = await preparar(s);
  assert.equal(resultado.estado, 'listo');
  assert.deepEqual(s.llamadas.filter((c) => !c.startsWith('GET')), ['POST /api/recursos/trabajos', 'PUT /api/recursos/trabajos/:id/lote', 'POST /api/recursos/trabajos/:id/iniciar']);
});

test('cancelar antes del primer lote borra el trabajo y no envía nada', async () => {
  const s = servidor();
  const control = new AbortController();
  control.abort();
  await assert.rejects(preparar(s, { signal: control.signal }), /cancelada/);
  assert.deepEqual(s.llamadas, ['POST /api/recursos/trabajos', 'DELETE /api/recursos/trabajos/:id']);
});

test('cancelar tras enviar, antes de iniciar, borra el trabajo y no lo inicia', async () => {
  const control = new AbortController();
  const s = servidor({ onPut: () => control.abort() });
  await assert.rejects(preparar(s, { signal: control.signal }), /cancelada/);
  assert.ok(s.llamadas.includes('DELETE /api/recursos/trabajos/:id'), 'el trabajo queda borrado');
  assert.ok(!s.llamadas.some((c) => c.endsWith('/iniciar')), 'no se inicia');
});

test('cancelar mientras el servidor genera borra el trabajo', async () => {
  const control = new AbortController();
  const s = servidor({ estados: ['generando'] });
  setTimeout(() => control.abort(), 300);
  await assert.rejects(preparar(s, { signal: control.signal }), /cancelada/);
  assert.equal(s.llamadas.at(-1), 'DELETE /api/recursos/trabajos/:id');
});

test('un fallo del servidor al borrar no tapa la cancelación', async () => {
  const s = servidor();
  const original = s.http.json;
  s.http.json = async (metodo, ruta, cuerpo) => {
    if (metodo === 'DELETE') throw new Error('red caída');
    return original(metodo, ruta, cuerpo);
  };
  const control = new AbortController();
  control.abort();
  await assert.rejects(preparar(s, { signal: control.signal }), /cancelada/);
});
