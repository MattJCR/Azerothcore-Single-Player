// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// routes/help.js — Comandos y ayuda del servidor. extraído
// de app.js con el mismo patrón de inyección que routes/serverConfig.js/
// botOperations.js.
import { buildHelpCatalog, securityTier } from '../help.js';

export function registerHelpRoutes(app, { worldDb, currentGmLevel, requireAuth, noStore }) {
  app.get('/api/help', requireAuth, async (request, response, next) => {
    noStore(response);
    try {
      const tier = securityTier(await currentGmLevel(request.session.sub));
      const [[categoryRows], [commandRows], [articleRows]] = await Promise.all([
        worldDb.query('SELECT id, name, name_en AS nameEn, sort, min_security AS minSecurity, enabled FROM server_help_category'),
        worldDb.query('SELECT command_path AS path, category_id AS categoryId, title, title_en AS titleEn, description, description_en AS descriptionEn, syntax, examples, keywords, min_security AS minSecurity, enabled FROM server_help_command'),
        worldDb.query('SELECT id, category_id AS categoryId, title, title_en AS titleEn, body, body_en AS bodyEn, keywords, command_path AS commandPath, min_security AS minSecurity, sort, enabled, is_hot AS isHot FROM server_help_article'),
      ]);
      // En inglés se usan las columnas *_en; vacías (aún sin traducir), cae al español, igual que el módulo del juego.
      const pick = (spanish, english) => (request.lang === 'en' && english ? english : spanish);
      const categories = categoryRows.map((row) => ({
        id: Number(row.id), name: pick(row.name, row.nameEn), sort: Number(row.sort), minSecurity: Number(row.minSecurity), enabled: Boolean(row.enabled),
      }));
      const commands = commandRows.map((row) => ({
        path: row.path, categoryId: Number(row.categoryId), title: pick(row.title, row.titleEn), description: pick(row.description, row.descriptionEn) || '',
        syntax: row.syntax || '', examples: row.examples || '', keywords: row.keywords || '',
        minSecurity: row.minSecurity == null ? null : Number(row.minSecurity), enabled: Boolean(row.enabled),
      }));
      const articles = articleRows.map((row) => ({
        id: Number(row.id), categoryId: Number(row.categoryId), title: pick(row.title, row.titleEn), body: pick(row.body, row.bodyEn),
        keywords: row.keywords || '', commandPath: row.commandPath || '', minSecurity: Number(row.minSecurity), sort: Number(row.sort),
        enabled: Boolean(row.enabled), isHot: Boolean(row.isHot),
      }));
      response.json(buildHelpCatalog(tier, { categories, commands, articles }));
    } catch (error) {
      next(error);
    }
  });
}
