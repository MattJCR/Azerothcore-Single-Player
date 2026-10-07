// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// las cinco tarjetas de "Jugadores" usaban una cuadrícula de
// cuatro columnas, así que "Actualizado" quedaba sola en la segunda fila a
// 1440 px. La frescura se movió al panel de la tabla (mismo id #updated-time,
// sólo cambia dónde vive en el HTML) y la cuadrícula se dejó en cuatro
// tarjetas. Esta prueba no ejecuta layout real (jsdom no tiene motor de
// cajas): compara el NÚMERO de tarjetas contra el número de columnas que
// declara styles.css para .stats-grid, que es justo el desajuste que causó
// el bug — no hace falta una pantalla real para atraparlo la próxima vez.
//
// La legibilidad real a 390/720/1440px (M55) y el resto de pantallas con
// datos reales/permisos GM sigue pendiente de la comprobación en el panel
// desplegado — esto sólo cubre lo que es puro HTML/CSS.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { JSDOM } from 'jsdom';

const root = path.dirname(fileURLToPath(import.meta.url));
const indexHtml = fs.readFileSync(path.join(root, '..', 'public', 'index.html'), 'utf8');
const stylesCss = fs.readFileSync(path.join(root, '..', 'public', 'styles.css'), 'utf8');

function baseStatsGridColumnCount() {
  // La primera regla de .stats-grid (fuera de cualquier @media) es la que
  // manda en escritorio ancho (1440 px); las @media de más abajo la reducen
  // para pantallas estrechas, que es un caso distinto (ya cubierto por
  // .stats-grid { grid-template-columns: 1fr 1fr }, sin huérfanos posibles
  // con 4 tarjetas).
  const match = stylesCss.match(/\.stats-grid\s*\{[^}]*grid-template-columns:\s*repeat\((\d+),/);
  assert.ok(match, 'no se encontró la regla base de .stats-grid en styles.css');
  return Number(match[1]);
}

test('M55: la cuadrícula de "Jugadores" tiene tantas tarjetas como columnas declara .stats-grid (sin huérfanas)', () => {
  const dom = new JSDOM(indexHtml);
  const cards = dom.window.document.querySelectorAll('#players-view .stats-grid > .stat-card');
  assert.equal(cards.length, baseStatsGridColumnCount(), 'el número de tarjetas debe encajar en las columnas de escritorio, sin dejar una suelta en una fila nueva');
});

test('M55: la frescura de los datos vive en el panel de la tabla, no en una tarjeta suelta', () => {
  const dom = new JSDOM(indexHtml);
  const document = dom.window.document;
  const updated = document.querySelector('#updated-time');
  assert.ok(updated, '#updated-time debe seguir existiendo (players.js lo actualiza por id)');
  assert.ok(!updated.closest('.stat-card'), 'ya no debe ser una tarjeta más de stats-grid');
  assert.ok(updated.closest('.table-panel'), 'debe vivir en el encabezado del panel de la tabla');
});

test('M55: hay una pista de scroll horizontal para la tabla en móvil, oculta por defecto', () => {
  const dom = new JSDOM(indexHtml);
  const hint = dom.window.document.querySelector('#players-view .table-scroll-hint');
  assert.ok(hint, 'falta el aviso de que la tabla se puede desplazar a los lados');
  assert.match(stylesCss, /\.table-scroll-hint\s*\{[^}]*display:\s*none/, 'por defecto (escritorio) no debe mostrarse');
  assert.match(stylesCss, /@media \(max-width: 720px\)[^]*?\.table-scroll-hint\s*\{\s*display:\s*block/, 'a 720px o menos debe hacerse visible');
});
