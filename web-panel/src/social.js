// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Círculo social de un personaje para el mapa del jugador: grupo, banda,
// hermandad y amigos. Un mismo personaje puede pertenecer a varias categorías
// a la vez (tu compañero de banda que además es de tu hermandad y tu amigo);
// se le asigna la relación más "cercana" con esta prioridad:
//
//   self  → tú mismo (el personaje elegido)
//   raid  → tu banda
//   party → tu grupo
//   friend→ tu lista de amigos
//   guild → tu hermandad
//
// Grupo y banda son excluyentes (un grupo del core es una cosa o la otra), así
// que en la práctica sólo compiten self > (raid|party) > friend > guild.
export const RELATION_PRIORITY = ['self', 'raid', 'party', 'friend', 'guild'];

// Devuelve un Map<guid, relación> con la categoría ganadora de cada personaje.
// El propio `focusGuid` nunca aparece: se marca aparte como 'self'.
export function mergeRelations({
  focusGuid,
  groupMemberGuids = [],
  isRaid = false,
  guildMemberGuids = [],
  friendGuids = [],
} = {}) {
  const focus = Number(focusGuid);
  const relations = new Map();

  const assign = (rawGuid, relation) => {
    const guid = Number(rawGuid);
    if (!Number.isInteger(guid) || guid <= 0 || guid === focus) return;
    const current = relations.get(guid);
    if (!current || RELATION_PRIORITY.indexOf(relation) < RELATION_PRIORITY.indexOf(current)) {
      relations.set(guid, relation);
    }
  };

  const groupRelation = isRaid ? 'raid' : 'party';
  for (const guid of groupMemberGuids) assign(guid, groupRelation);
  for (const guid of friendGuids) assign(guid, 'friend');
  for (const guid of guildMemberGuids) assign(guid, 'guild');

  return relations;
}
