// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import test from 'node:test';
import assert from 'node:assert/strict';
import {
  CommandError, kickCommand, muteCommand, unmuteCommand, banCommand, unbanCommand,
  announceCommand, sendItemsCommand, sendMoneyCommand,
} from '../src/commands.js';

test('kick monta "kick <nombre> <motivo>" y admite motivo vacío', () => {
  assert.equal(kickCommand({ characterName: 'Testigo', reason: 'AFK en zona de jefe' }), 'kick Testigo AFK en zona de jefe');
  assert.equal(kickCommand({ characterName: 'Testigo', reason: '' }), 'kick Testigo');
});

test('mute exige una duración de la lista cerrada y nunca admite "perm"', () => {
  assert.equal(muteCommand({ characterName: 'Testigo', duration: '1h', reason: 'spam' }), 'mute Testigo 1h spam');
  assert.throws(() => muteCommand({ characterName: 'Testigo', duration: 'perm', reason: 'spam' }), CommandError);
  assert.throws(() => muteCommand({ characterName: 'Testigo', duration: '2h', reason: 'spam' }), CommandError);
});

test('unmute sólo lleva el nombre', () => {
  assert.equal(unmuteCommand({ characterName: 'Testigo' }), 'unmute Testigo');
});

test('ban acepta cuenta o personaje y traduce "perm" a duración 0', () => {
  assert.equal(banCommand({ type: 'character', name: 'Testigo', duration: '7d', reason: 'trampas' }), 'ban character Testigo 7d trampas');
  assert.equal(banCommand({ type: 'account', name: 'jugador_1', duration: 'perm', reason: 'trampas' }), 'ban account jugador_1 0 trampas');
  assert.throws(() => banCommand({ type: 'guild', name: 'Testigo', duration: '1d', reason: 'x' }), CommandError);
});

test('unban acepta el mismo tipo que ban', () => {
  assert.equal(unbanCommand({ type: 'account', name: 'jugador_1' }), 'unban account jugador_1');
  assert.equal(unbanCommand({ type: 'character', name: 'Testigo' }), 'unban character Testigo');
});

test('announce recorta espacios y rechaza texto vacío', () => {
  assert.equal(announceCommand({ text: '  El servidor reinicia en 5 minutos  ' }), 'announce El servidor reinicia en 5 minutos');
  assert.throws(() => announceCommand({ text: '' }), CommandError);
});

test('send items exige entry y cantidad numéricos y entrecomilla asunto y texto', () => {
  assert.equal(
    sendItemsCommand({ characterName: 'Testigo', subject: 'Regalo', text: 'Disfrútalo', itemEntry: 6948, count: 1 }),
    'send items Testigo "Regalo" "Disfrútalo" 6948:1',
  );
  assert.throws(() => sendItemsCommand({ characterName: 'Testigo', subject: 'x', text: 'y', itemEntry: 'DROP TABLE', count: 1 }), CommandError);
});

test('send money exige un entero positivo de cobre', () => {
  assert.equal(
    sendMoneyCommand({ characterName: 'Testigo', subject: 'Premio', text: 'Buen trabajo', copper: 10000 }),
    'send money Testigo "Premio" "Buen trabajo" 10000',
  );
  assert.throws(() => sendMoneyCommand({ characterName: 'Testigo', subject: 'x', text: 'y', copper: -5 }), CommandError);
  assert.throws(() => sendMoneyCommand({ characterName: 'Testigo', subject: 'x', text: 'y', copper: '100; DROP TABLE account' }), CommandError);
});

test('rechaza nombres de personaje y de cuenta fuera de la lista blanca de caracteres', () => {
  assert.throws(() => kickCommand({ characterName: 'a', reason: '' }), CommandError, 'demasiado corto');
  assert.throws(() => kickCommand({ characterName: 'Test123', reason: '' }), CommandError, 'con dígitos');
  assert.throws(() => kickCommand({ characterName: 'Test Testigo', reason: '' }), CommandError, 'con espacio');
  assert.throws(() => banCommand({ type: 'account', name: 'ab', duration: '1d', reason: 'x' }), CommandError, 'cuenta demasiado corta');
  assert.throws(() => banCommand({ type: 'account', name: 'nombre con espacio', duration: '1d', reason: 'x' }), CommandError);
});

test('ningún intento de inyección sobrevive al motivo, asunto o texto', () => {
  const injected = kickCommand({ characterName: 'Testigo', reason: 'motivo"; server shutdown 0 #' });
  assert.doesNotMatch(injected, /["]/);
  assert.equal(injected, 'kick Testigo motivo; server shutdown 0 #');

  const withNewline = announceCommand({ text: 'primera línea\nkick Testigo\rsegunda línea' });
  assert.doesNotMatch(withNewline, /[\r\n]/);
  assert.equal(withNewline, 'announce primera línea kick Testigo segunda línea');

  const withBackticks = muteCommand({ characterName: 'Testigo', duration: '1h', reason: '`rm -rf /`' });
  assert.doesNotMatch(withBackticks, /[`'"]/);
});

test('el motivo no puede superar el límite de banreason/mutereason (255)', () => {
  const long = 'x'.repeat(256);
  assert.throws(() => kickCommand({ characterName: 'Testigo', reason: long }), CommandError);
  assert.throws(() => banCommand({ type: 'character', name: 'Testigo', duration: '1d', reason: long }), CommandError);
});
