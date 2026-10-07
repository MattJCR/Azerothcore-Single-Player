// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// routes/players.js — jugadores conectados y sus posiciones (mapa GM). Extraído de app.js con el mismo patrón de inyección que
// routes/serverConfig.js/botOperations.js. botAccountIdSet se inyecta en vez
// de importarse porque también lo usa el dominio de métricas
// (fetchSystemStatus, población online), no sólo éste.
import { inferRole } from '../roles.js';

export function registerPlayersRoutes(app, { charactersDb, botAccountIdSet, requireAuth, requireGm, noStore }) {
  app.get('/api/players', requireAuth, async (_request, response, next) => {
    noStore(response);
    try {
      const [rows] = await charactersDb.query(
        `SELECT c.guid, c.name, c.race, c.class, c.gender, c.level, c.zone, c.map, c.account,
                GROUP_CONCAT(ct.spell ORDER BY ct.spell) AS talentSpells
         FROM characters c
         LEFT JOIN character_talent ct
           ON ct.guid = c.guid AND (ct.specMask & (1 << c.activeTalentGroup)) <> 0
         WHERE c.online = 1 AND c.deleteDate IS NULL
         GROUP BY c.guid, c.name, c.race, c.class, c.gender, c.level, c.zone, c.map, c.account
         ORDER BY c.level DESC, c.name ASC`,
      );
      const botAccountIds = rows.length ? await botAccountIdSet(rows.map((row) => Number(row.account))) : new Set();
      const players = rows.map((row) => {
        const spells = row.talentSpells ? String(row.talentSpells).split(',').map(Number) : [];
        return {
          guid: Number(row.guid), name: row.name, race: Number(row.race), class: Number(row.class),
          gender: Number(row.gender), level: Number(row.level), zone: Number(row.zone), map: Number(row.map),
          role: inferRole(Number(row.class), spells),
          type: botAccountIds.has(Number(row.account)) ? 'bot' : 'player',
        };
      });
      response.json({ players, updatedAt: new Date().toISOString() });
    } catch (error) {
      next(error);
    }
  });

  app.get('/api/map/players', requireAuth, requireGm, async (_request, response, next) => {
    noStore(response);
    try {
      const [rows] = await charactersDb.query(
        `SELECT guid, name, race, class, level, map, zone, position_x, position_y, position_z, orientation
         FROM characters WHERE online = 1 AND deleteDate IS NULL ORDER BY name`,
      );
      response.json({
        players: rows.map((row) => ({
          guid: Number(row.guid), name: row.name, race: Number(row.race), class: Number(row.class),
          level: Number(row.level), map: Number(row.map), zone: Number(row.zone),
          x: Number(row.position_x), y: Number(row.position_y), z: Number(row.position_z),
          orientation: Number(row.orientation),
        })),
        updatedAt: new Date().toISOString(),
      });
    } catch (error) {
      next(error);
    }
  });
}
