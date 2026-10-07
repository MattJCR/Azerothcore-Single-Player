// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Filtra el catálogo de mod-server-help (categorías, fichas de comando y
// artículos de acore_world) por el nivel de seguridad de la sesión: 0 jugador,
// 1 moderador, 2 GM, 3 administrador. Sólo se listan los comandos que tienen
// ficha propia (con descripción); el árbol completo de comandos del core sólo
// lo conoce el propio worldserver en memoria.
export function securityTier(gmlevel) {
  const level = Number(gmlevel) || 0;
  return Math.max(0, Math.min(3, level));
}

export function buildHelpCatalog(tier, { categories = [], commands = [], articles = [] } = {}) {
  const defaultCategoryBySecurity = new Map([[0, 2], [1, 14], [2, 13], [3, 15]]);
  const visibleCategories = categories
    .filter((category) => category.enabled && category.minSecurity <= tier)
    .sort((a, b) => a.sort - b.sort || a.name.localeCompare(b.name, 'es'));
  const visibleIds = new Set(visibleCategories.map((category) => category.id));

  const allCategoryById = new Map(categories.filter((category) => category.enabled).map((category) => [category.id, category]));
  const categoryById = new Map(visibleCategories.map((category) => [category.id, category]));
  const enabledCommands = commands.filter((command) => command.enabled);
  const visibleCommands = enabledCommands
    .filter((command) => {
      const category = allCategoryById.get(command.categoryId);
      if (!category) return false;
      // Las fichas antiguas o personalizadas pueden no tener todavía permiso
      // propio: en ese caso se conserva el comportamiento histórico de usar
      // el mínimo de la categoría. Las fichas de semilla sí declaran el nivel
      // exacto del comando.
      const minimum = command.minSecurity == null ? category.minSecurity : command.minSecurity;
      if (minimum > tier) return false;
      return categoryById.has(command.categoryId) || categoryById.has(defaultCategoryBySecurity.get(minimum));
    })
    .map((command) => {
      const originalCategory = allCategoryById.get(command.categoryId);
      const minSecurity = command.minSecurity == null ? originalCategory.minSecurity : command.minSecurity;
      return {
        ...command,
        // Igual que mod-server-help: si la categoría temática revelaría una
        // sección de nivel superior, cae en la categoría por defecto de su
        // permiso real (p. ej. `.gm on` aparece bajo Moderación para nivel 1).
        categoryId: categoryById.has(command.categoryId) ? command.categoryId : defaultCategoryBySecurity.get(minSecurity),
        minSecurity,
        isFamily: enabledCommands.some((candidate) => candidate.path.startsWith(`${command.path} `)),
      };
    })
    .sort((a, b) => a.path.localeCompare(b.path, 'es'));
  const visibleCommandPaths = new Set(visibleCommands.map((command) => command.path.toLocaleLowerCase('es')));

  const visibleArticles = articles
    .filter((article) => article.enabled
      && visibleIds.has(article.categoryId)
      && article.minSecurity <= tier
      && (!article.commandPath || visibleCommandPaths.has(article.commandPath.toLocaleLowerCase('es'))))
    .sort((a, b) => a.sort - b.sort || a.title.localeCompare(b.title, 'es'));

  return { categories: visibleCategories, commands: visibleCommands, articles: visibleArticles };
}
