// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// routes/content.js — rutas HTTP de addons/parches del cliente (app.js concentraba autenticación, SQL, configuración, armería,
// métricas y acciones en un solo fichero de ~1200 líneas; éste es uno de los
// grupos extraídos por dominio, con el mismo patrón de inyección que ya
// usaban routes/serverConfig.js y routes/botOperations.js). Sin estado
// propio: el catálogo y los ficheros ya los sirven addons.js/patches.js.
export function registerContentRoutes(app, { requireAuth, noStore, addonCatalog, findAddon, sendAddonZip, findPatch, patchCatalog }) {
  app.get('/api/addons', requireAuth, (_request, response) => {
    noStore(response);
    response.json({ ...addonCatalog(), ...patchCatalog() });
  });

  app.get('/api/addons/:id/download', requireAuth, (request, response, next) => {
    const addon = findAddon(request.params.id);
    if (!addon) return response.status(404).json({ error: 'Addon no encontrado' });
    sendAddonZip(addon, response, next);
  });

  app.get('/api/patches/:id/download', requireAuth, (request, response) => {
    const patch = findPatch(request.params.id);
    if (!patch) return response.status(404).json({ error: 'Parche no encontrado' });
    if (!patch.available) return response.status(409).json({ error: 'Este parche aún no está generado: un administrador debe preparar los recursos desde su cliente de WoW.' });
    response.download(patch.filePath, patch.file);
  });
}
