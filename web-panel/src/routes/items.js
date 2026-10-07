// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// routes/items.js — buscador de objetos para el correo de moderación
// (reutiliza tablas que el panel ya puede leer). extraído de
// app.js con el mismo patrón de inyección que routes/serverConfig.js/
// botOperations.js.
import { iconForDisplayId } from '../item-icons.js';

export function registerItemsRoutes(app, { worldDb, requireAuth, requireGmLevel, noStore }) {
  app.get('/api/items/search', requireAuth, requireGmLevel(3, 'Se requiere una cuenta de administrador'), async (request, response, next) => {
    noStore(response);
    const query = typeof request.query.q === 'string' ? request.query.q.trim() : '';
    if (query.length < 2 || query.length > 50) return response.json({ items: [] });
    const escaped = query.replace(/[\\%_]/g, (char) => `\\${char}`);
    try {
      const [rows] = await worldDb.query(
        `SELECT it.entry AS entry, COALESCE(itl.Name, it.name) AS name, it.Quality AS quality,
                it.displayid AS displayId, it.InventoryType AS inventoryType, it.ItemLevel AS ilvl
         FROM item_template it
         LEFT JOIN item_template_locale itl ON itl.ID = it.entry AND itl.locale = 'esES'
         WHERE COALESCE(itl.Name, it.name) LIKE ?
         ORDER BY it.Quality DESC, it.ItemLevel DESC LIMIT 40`,
        [`%${escaped}%`],
      );
      response.json({
        items: rows.map((row) => ({
          entry: Number(row.entry), name: row.name, quality: Number(row.quality),
          icon: iconForDisplayId(Number(row.displayId) || 0), inventoryType: Number(row.inventoryType), ilvl: Number(row.ilvl),
        })),
      });
    } catch (error) {
      next(error);
    }
  });
}
