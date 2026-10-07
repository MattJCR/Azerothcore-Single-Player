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
        worldDb.query('SELECT id, name, sort, min_security AS minSecurity, enabled FROM server_help_category'),
        worldDb.query('SELECT command_path AS path, category_id AS categoryId, title, description, syntax, examples, keywords, min_security AS minSecurity, enabled FROM server_help_command'),
        worldDb.query('SELECT id, category_id AS categoryId, title, body, keywords, command_path AS commandPath, min_security AS minSecurity, sort, enabled, is_hot AS isHot FROM server_help_article'),
      ]);
      const categories = categoryRows.map((row) => ({
        id: Number(row.id), name: row.name, sort: Number(row.sort), minSecurity: Number(row.minSecurity), enabled: Boolean(row.enabled),
      }));
      const commands = commandRows.map((row) => ({
        path: row.path, categoryId: Number(row.categoryId), title: row.title, description: row.description || '',
        syntax: row.syntax || '', examples: row.examples || '', keywords: row.keywords || '',
        minSecurity: row.minSecurity == null ? null : Number(row.minSecurity), enabled: Boolean(row.enabled),
      }));
      const articles = articleRows.map((row) => ({
        id: Number(row.id), categoryId: Number(row.categoryId), title: row.title, body: row.body,
        keywords: row.keywords || '', commandPath: row.commandPath || '', minSecurity: Number(row.minSecurity), sort: Number(row.sort),
        enabled: Boolean(row.enabled), isHot: Boolean(row.isHot),
      }));
      response.json(buildHelpCatalog(tier, { categories, commands, articles }));
    } catch (error) {
      next(error);
    }
  });
}
