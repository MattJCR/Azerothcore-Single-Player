// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// routes/socialMap.js — mapa social del propio jugador (grupo, banda,
// hermandad, amigos). extraído de app.js con el mismo patrón
// de inyección que routes/serverConfig.js/botOperations.js. A diferencia de
// /api/map/players (routes/players.js: sólo GM, todo el reino), cualquier
// cuenta puede pedir esto, y sólo ve las posiciones de personajes con un
// vínculo social con uno de los suyos.
import { mergeRelations } from '../social.js';

export function registerSocialMapRoutes(app, { charactersDb, requireAuth, noStore }) {
  app.get('/api/social/map', requireAuth, async (request, response, next) => {
    noStore(response);
    try {
      const accountId = request.session.sub;
      const [charRows] = await charactersDb.query(
        `SELECT guid, name, race, class, level, online
         FROM characters WHERE account = ? AND deleteDate IS NULL
         ORDER BY online DESC, level DESC, name ASC`,
        [accountId],
      );
      const characters = charRows.map((row) => ({
        guid: Number(row.guid), name: row.name, race: Number(row.race),
        class: Number(row.class), level: Number(row.level), online: Boolean(row.online),
      }));

      const requested = Number.parseInt(request.query.character, 10);
      let focus = null;
      if (Number.isInteger(requested)) focus = characters.find((character) => character.guid === requested) || null;
      if (!focus) focus = characters.find((character) => character.online) || null;

      const emptyResponse = { characters, focus: null, raid: false, players: [], updatedAt: new Date().toISOString() };
      if (!focus) return response.json(emptyResponse);

      const [groupRows] = await charactersDb.query(
        `SELECT gmAll.memberGuid AS guid, g.groupType AS groupType
         FROM group_member gmMe
         JOIN group_member gmAll ON gmAll.guid = gmMe.guid
         LEFT JOIN \`groups\` g ON g.guid = gmMe.guid
         WHERE gmMe.memberGuid = ?`,
        [focus.guid],
      );
      const isRaid = groupRows.some((row) => (Number(row.groupType) & 2) !== 0);
      const groupMemberGuids = groupRows.map((row) => Number(row.guid));

      const [guildRows] = await charactersDb.query(
        `SELECT other.guid AS guid
         FROM guild_member me JOIN guild_member other ON other.guildid = me.guildid
         WHERE me.guid = ?`,
        [focus.guid],
      );
      const guildMemberGuids = guildRows.map((row) => Number(row.guid));

      // flags & 0x01 = SOCIAL_FLAG_FRIEND; deja fuera ignorados y silenciados.
      const [friendRows] = await charactersDb.query(
        'SELECT friend AS guid FROM character_social WHERE guid = ? AND (flags & 1) <> 0',
        [focus.guid],
      );
      const friendGuids = friendRows.map((row) => Number(row.guid));

      const relations = mergeRelations({
        focusGuid: focus.guid, groupMemberGuids, isRaid, guildMemberGuids, friendGuids,
      });
      const guids = [focus.guid, ...relations.keys()];

      const [positionRows] = await charactersDb.query(
        `SELECT guid, name, race, class, level, map, zone,
                position_x, position_y, position_z, orientation
         FROM characters
         WHERE guid IN (?) AND online = 1 AND deleteDate IS NULL`,
        [guids],
      );

      const players = positionRows.map((row) => {
        const guid = Number(row.guid);
        return {
          guid, name: row.name, race: Number(row.race), class: Number(row.class), level: Number(row.level),
          map: Number(row.map), zone: Number(row.zone),
          x: Number(row.position_x), y: Number(row.position_y), z: Number(row.position_z),
          orientation: Number(row.orientation),
          relation: guid === focus.guid ? 'self' : relations.get(guid),
        };
      });

      response.json({
        characters,
        focus: { guid: focus.guid, name: focus.name, online: focus.online },
        raid: isRaid,
        players,
        updatedAt: new Date().toISOString(),
      });
    } catch (error) {
      next(error);
    }
  });
}
