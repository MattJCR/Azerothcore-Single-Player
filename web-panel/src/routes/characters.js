// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// routes/characters.js — búsqueda de personajes, "mis personajes" y armería
// (equipo, bolsas, banco y banco de hermandad). extraído de
// app.js con el mismo patrón de inyección que routes/serverConfig.js/
// botOperations.js.
import { worldTable } from '../db.js';
import { itemLocale } from '../i18n.js';
import { classifyInventory } from '../inventory.js';
import { formatItemRow } from '../items.js';

const ITEM_COLUMNS = `ii.guid AS itemGuid, ii.itemEntry AS entry, ii.count AS count,
  COALESCE(itl.Name, it.name) AS name, it.Quality AS quality, it.InventoryType AS inventoryType,
  it.ItemLevel AS ilvl, it.RequiredLevel AS requiredLevel, it.class AS itemClass, it.displayid AS displayId,
  it.armor AS armor, it.delay AS delay, it.dmg_min1 AS dmgMin, it.dmg_max1 AS dmgMax,
  it.stat_type1 AS statType1, it.stat_value1 AS statValue1,
  it.stat_type2 AS statType2, it.stat_value2 AS statValue2,
  it.stat_type3 AS statType3, it.stat_value3 AS statValue3,
  it.stat_type4 AS statType4, it.stat_value4 AS statValue4,
  it.stat_type5 AS statType5, it.stat_value5 AS statValue5`;
const ITEM_TEMPLATE_TABLE = worldTable('item_template');
// El nombre sale en el idioma del panel; en inglés no hay fila enUS y COALESCE cae en item_template.name.
const itemLocaleJoin = (request) => `LEFT JOIN ${worldTable('item_template_locale')} itl ON itl.ID = it.entry AND itl.locale = '${itemLocale(request)}'`;

export function registerCharactersRoutes(app, { authDb, charactersDb, currentGmLevel, requireAuth, noStore }) {
  app.get('/api/characters/search', requireAuth, async (request, response, next) => {
    noStore(response);
    const query = typeof request.query.q === 'string' ? request.query.q.trim() : '';
    if (query.length < 2 || query.length > 12) return response.json({ characters: [] });
    const escaped = query.replace(/[\\%_]/g, (char) => `\\${char}`);
    try {
      const isGm = (await currentGmLevel(request.session.sub)) > 0;
      const [rows] = await charactersDb.query(
        `SELECT c.guid, c.name, c.race, c.class, c.gender, c.level, c.online, c.account,
                (SELECT g.name FROM guild_member gm JOIN guild g ON g.guildid = gm.guildid WHERE gm.guid = c.guid LIMIT 1) AS guildName
         FROM characters c
         WHERE c.deleteDate IS NULL AND LOWER(c.name) LIKE LOWER(?)
         ORDER BY c.online DESC, c.level DESC, c.name ASC
         LIMIT 25`,
        [`${escaped}%`],
      );
      // El nombre de cuenta sólo se resuelve (y sólo se manda) para GMs: la
      // moderación lo necesita para saber si la cuenta está silenciada o
      // baneada, pero un jugador normal buscando en la armería no debe verlo.
      let usernameByAccount = new Map();
      if (isGm && rows.length) {
        const accountIds = [...new Set(rows.map((row) => Number(row.account)))];
        const [accountRows] = await authDb.query('SELECT id, username FROM account WHERE id IN (?)', [accountIds]);
        usernameByAccount = new Map(accountRows.map((row) => [Number(row.id), row.username]));
      }
      response.json({
        characters: rows.map((row) => ({
          guid: Number(row.guid), name: row.name, race: Number(row.race), class: Number(row.class),
          gender: Number(row.gender), level: Number(row.level), online: Boolean(row.online), guildName: row.guildName || null,
          ...(isGm ? { accountUsername: usernameByAccount.get(Number(row.account)) || null } : {}),
        })),
      });
    } catch (error) {
      next(error);
    }
  });

  app.get('/api/account/characters', requireAuth, async (request, response, next) => {
    noStore(response);
    try {
      const [rows] = await charactersDb.query(
        `SELECT guid, name, race, class, gender, level, online, zone, map
         FROM characters WHERE account = ? AND deleteDate IS NULL ORDER BY name`,
        [request.session.sub],
      );
      response.json({
        characters: rows.map((row) => ({
          guid: Number(row.guid), name: row.name, race: Number(row.race), class: Number(row.class),
          gender: Number(row.gender), level: Number(row.level), online: Boolean(row.online),
          zone: Number(row.zone), map: Number(row.map),
        })),
      });
    } catch (error) {
      next(error);
    }
  });

  app.get('/api/armory/:guid', requireAuth, async (request, response, next) => {
    noStore(response);
    const guid = Number.parseInt(request.params.guid, 10);
    if (!Number.isInteger(guid) || guid <= 0) return response.status(400).json({ error: 'Personaje no válido' });
    try {
      const [characterRows] = await charactersDb.query(
        `SELECT guid, account, name, race, class, gender, level, money, online, zone, map, totaltime
         FROM characters WHERE guid = ? AND deleteDate IS NULL LIMIT 1`,
        [guid],
      );
      const characterRow = characterRows[0];
      if (!characterRow) return response.status(404).json({ error: 'Personaje no encontrado' });
      const ownedByMe = Number(characterRow.account) === request.session.sub;

      const [guildRows] = await charactersDb.query(
        `SELECT gm.guildid AS guildId, gm.\`rank\` AS rankId, g.name AS guildName
         FROM guild_member gm JOIN guild g ON g.guildid = gm.guildid
         WHERE gm.guid = ? LIMIT 1`,
        [guid],
      );
      const guildRow = guildRows[0];

      const [equippedRows] = await charactersDb.query(
        `SELECT ci.slot AS slot, ${ITEM_COLUMNS}
         FROM character_inventory ci
         JOIN item_instance ii ON ii.guid = ci.item
         JOIN ${ITEM_TEMPLATE_TABLE} it ON it.entry = ii.itemEntry
         ${itemLocaleJoin(request)}
         WHERE ci.guid = ? AND ci.bag = 0 AND ci.slot <= 18
         ORDER BY ci.slot`,
        [guid],
      );

      const character = {
        guid: Number(characterRow.guid), name: characterRow.name, race: Number(characterRow.race),
        class: Number(characterRow.class), gender: Number(characterRow.gender), level: Number(characterRow.level),
        online: Boolean(characterRow.online), zone: Number(characterRow.zone), map: Number(characterRow.map),
        guildName: guildRow?.guildName || null, ownedByMe,
      };
      const result = {
        character,
        equipped: equippedRows.map((row) => ({ slot: Number(row.slot), item: formatItemRow(row) })),
        bags: null,
        bank: null,
        guildBank: null,
      };

      if (ownedByMe) {
        character.money = Number(characterRow.money);
        character.totaltime = Number(characterRow.totaltime);

        const [inventoryRows] = await charactersDb.query(
          `SELECT ci.bag AS bag, ci.slot AS slot, ${ITEM_COLUMNS}
           FROM character_inventory ci
           JOIN item_instance ii ON ii.guid = ci.item
           JOIN ${ITEM_TEMPLATE_TABLE} it ON it.entry = ii.itemEntry
           ${itemLocaleJoin(request)}
           WHERE ci.guid = ?`,
          [guid],
        );
        const formattedRows = inventoryRows.map((row) => ({ bag: Number(row.bag), slot: Number(row.slot), ...formatItemRow(row) }));
        const classified = classifyInventory(formattedRows);
        result.bags = classified.bags;
        result.bank = classified.bank;

        if (guildRow) {
          const [tabRows] = await charactersDb.query(
            'SELECT TabId AS tabId, TabName AS tabName FROM guild_bank_tab WHERE guildid = ? ORDER BY TabId',
            [guildRow.guildId],
          );
          let allowedTabIds;
          if (Number(guildRow.rankId) === 0) {
            allowedTabIds = tabRows.map((row) => Number(row.tabId));
          } else {
            const [rightRows] = await charactersDb.query(
              'SELECT TabId AS tabId, gbright AS gbright FROM guild_bank_right WHERE guildid = ? AND rid = ?',
              [guildRow.guildId, guildRow.rankId],
            );
            allowedTabIds = rightRows.filter((row) => Number(row.gbright) & 1).map((row) => Number(row.tabId));
          }

          const itemsByTab = new Map();
          if (allowedTabIds.length) {
            const [bankItemRows] = await charactersDb.query(
              `SELECT gbi.TabId AS tabId, ${ITEM_COLUMNS}
               FROM guild_bank_item gbi
               JOIN item_instance ii ON ii.guid = gbi.item_guid
               JOIN ${ITEM_TEMPLATE_TABLE} it ON it.entry = ii.itemEntry
               ${itemLocaleJoin(request)}
               WHERE gbi.guildid = ? AND gbi.TabId IN (?)
               ORDER BY gbi.TabId, gbi.SlotId`,
              [guildRow.guildId, allowedTabIds],
            );
            for (const row of bankItemRows) {
              const tabId = Number(row.tabId);
              if (!itemsByTab.has(tabId)) itemsByTab.set(tabId, []);
              itemsByTab.get(tabId).push(formatItemRow(row));
            }
          }

          result.guildBank = {
            guildName: guildRow.guildName,
            tabs: tabRows.map((row) => {
              const tabId = Number(row.tabId);
              const allowed = allowedTabIds.includes(tabId);
              return { tabId, tabName: row.tabName, allowed, items: allowed ? (itemsByTab.get(tabId) || []) : [] };
            }),
          };
        }
      }

      response.json(result);
    } catch (error) {
      next(error);
    }
  });
}
