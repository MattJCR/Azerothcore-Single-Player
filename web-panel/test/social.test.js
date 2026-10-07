// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import test from 'node:test';
import assert from 'node:assert/strict';
import { mergeRelations } from '../src/social.js';

test('el personaje elegido nunca aparece en el mapa aunque esté en sus propias listas', () => {
  const relations = mergeRelations({
    focusGuid: 7,
    groupMemberGuids: [7, 8],
    guildMemberGuids: [7, 9],
    friendGuids: [7],
  });
  assert.equal(relations.has(7), false);
  assert.equal(relations.get(8), 'party');
  assert.equal(relations.get(9), 'guild');
});

test('la banda gana al grupo según el flag isRaid', () => {
  const party = mergeRelations({ focusGuid: 1, groupMemberGuids: [2], isRaid: false });
  const raid = mergeRelations({ focusGuid: 1, groupMemberGuids: [2], isRaid: true });
  assert.equal(party.get(2), 'party');
  assert.equal(raid.get(2), 'raid');
});

test('prioridad: grupo/banda > amigo > hermandad para un mismo personaje', () => {
  const relations = mergeRelations({
    focusGuid: 1,
    groupMemberGuids: [2],
    friendGuids: [2, 3],
    guildMemberGuids: [2, 3, 4],
  });
  assert.equal(relations.get(2), 'party'); // en grupo y además amigo y hermandad
  assert.equal(relations.get(3), 'friend'); // amigo y hermandad
  assert.equal(relations.get(4), 'guild'); // sólo hermandad
});

test('ignora guids no válidos (0, negativos, no numéricos)', () => {
  const relations = mergeRelations({
    focusGuid: 1,
    guildMemberGuids: [0, -5, 'x', null, undefined, 6],
  });
  assert.deepEqual([...relations.keys()], [6]);
});

test('sin ninguna lista devuelve un mapa vacío', () => {
  assert.equal(mergeRelations({ focusGuid: 1 }).size, 0);
  assert.equal(mergeRelations().size, 0);
});
