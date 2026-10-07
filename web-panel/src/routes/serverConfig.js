// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// routes/serverConfig.js — rutas HTTP de "Configuración del servidor"
//: extraídas de app.js, que concentraba
// autenticación, SQL, configuración, armería, métricas y acciones en un
// solo fichero. registerServerConfigRoutes() recibe sólo lo que necesita
// (nunca la `app` como excusa para tocar cualquier cosa): middlewares ya
// construidos, `panelDb` y las utilidades comunes de auditoría/no-cache.
// `createApp()` (app.js) sigue siendo el único punto de composición e
// inyección — este módulo no importa nada de configuración/BD por su
// cuenta, todo llega por parámetro.
//
// Tasas, GM/seguridad, chat, anuncios, ajustes de juego y módulos instalados
// (catálogo curado en serverConfigCatalog.js, derivado de
// REFERENCIA_CONFIG_PANEL.md). Sin recarga en caliente: cada cambio se
// encola aquí y un script privilegiado aparte (ac-panel-config-apply, fuera
// de este proceso) lo escribe en el fichero real y reinicia el worldserver
// por completo — el proceso del panel nunca toca disco ni invoca systemctl.
// Los parámetros de riesgo alto (GM.StartLevel, Warden.*, ...) se quedan de
// sólo lectura para todo el mundo: no hay nivel por encima de administrador
// al que reservárselos, así que la única opción segura es no exponerlos.
import { allCategories } from '../serverConfigCatalog.js';
import { resolveAndValidate, ServerConfigError } from '../serverConfigValidation.js';
import { readCurrentValues } from '../serverConfigFiles.js';

export function registerServerConfigRoutes(app, { requireAuth, requireAdmin, requireCsrfHeader, noStore, audit, clientKey, panelDb }) {
  app.get('/api/server-config/categories', requireAuth, requireAdmin, async (_request, response, next) => {
    noStore(response);
    try {
      const [pendingRows] = await panelDb.query(
        "SELECT category_id AS categoryId, COUNT(*) AS count FROM panel_config_pending WHERE status = 'pending' GROUP BY category_id",
      );
      const pendingByCategory = new Map(pendingRows.map((row) => [row.categoryId, Number(row.count)]));
      response.json({
        categories: allCategories().map((category) => ({
          id: category.id, label: category.label, group: category.group, module: category.module,
          file: category.file, count: category.params.length, pendingCount: pendingByCategory.get(category.id) || 0,
        })),
      });
    } catch (error) {
      next(error);
    }
  });

  // la búsqueda de la pantalla sólo miraba dentro de la
  // categoría ya cargada, aunque el catálogo tiene muchas más. El índice es
  // puro catálogo en memoria (sin lectura de fichero ni consulta a BD, a
  // diferencia de /category/:id): puede pedirse una sola vez al entrar en la
  // vista sin abrir una categoría por cada resultado.
  app.get('/api/server-config/search-index', requireAuth, requireAdmin, (_request, response) => {
    noStore(response);
    response.json({
      params: allCategories().flatMap((category) => category.params.map((param) => ({
        key: param.key, description: param.description || '', categoryId: category.id, categoryLabel: category.label,
      }))),
    });
  });

  app.get('/api/server-config/category/:id', requireAuth, requireAdmin, async (request, response, next) => {
    noStore(response);
    const category = allCategories().find((entry) => entry.id === request.params.id);
    if (!category) return response.status(404).json({ error: 'Categoría no encontrada' });
    try {
      const currentValues = readCurrentValues(category.file, category.params.map((param) => param.key));
      const [pendingRows] = await panelDb.query(
        "SELECT key_name AS keyName, new_value AS newValue FROM panel_config_pending WHERE file_name = ? AND status = 'pending'",
        [category.file],
      );
      const pendingByKey = new Map(pendingRows.map((row) => [row.keyName, row.newValue]));
      response.json({
        category: { id: category.id, label: category.label, group: category.group, module: category.module, file: category.file },
        params: category.params.map((param) => ({
          key: param.key, type: param.type, min: param.min, max: param.max, risk: param.risk,
          description: param.description, default: param.default, options: param.options || null,
          current: currentValues.get(param.key) ?? param.default,
          pending: pendingByKey.get(param.key) ?? null,
        })),
      });
    } catch (error) {
      next(error);
    }
  });

  app.post('/api/server-config/stage', requireAuth, requireAdmin, requireCsrfHeader, async (request, response, next) => {
    noStore(response);
    let resolved;
    try {
      resolved = resolveAndValidate(request.body?.key, request.body?.file, request.body?.value);
    } catch (error) {
      if (error instanceof ServerConfigError) return response.status(400).json({ error: error.message });
      return next(error);
    }
    try {
      // panel_config_pending_seq da a esta escritura una revisión que crece
      // en cada stage(), incluida una reescritura de la misma clave ): apply-panel-config.sh captura un lote por revisión antes de
      // tocar los .conf y, al terminar, sólo retira esas revisiones exactas.
      // Una reescritura de la misma clave mientras ese lote se aplica se
      // queda con una revisión nueva y sobrevive a esa limpieza.
      const [seqResult] = await panelDb.execute('UPDATE panel_config_pending_seq SET n = LAST_INSERT_ID(n + 1)');
      const revision = seqResult.insertId;
      await panelDb.execute(
        `INSERT INTO panel_config_pending (key_name, file_name, category_id, new_value, revision, risk, status, created_by, created_at)
         VALUES (?, ?, ?, ?, ?, ?, 'pending', ?, NOW())
         ON DUPLICATE KEY UPDATE new_value = VALUES(new_value), revision = VALUES(revision), status = 'pending', created_by = VALUES(created_by), created_at = NOW()`,
        [resolved.param.key, resolved.param.file, resolved.param.categoryId, resolved.value, revision, resolved.param.risk, request.session.username],
      );
      await audit({
        actorAccount: request.session.sub, actorName: request.session.username, action: 'server-config-stage',
        target: `${resolved.param.file}:${resolved.param.key}`, detail: resolved.value, result: 'ok', ip: clientKey(request),
      });
      response.json({ ok: true, value: resolved.value });
    } catch (error) {
      next(error);
    }
  });

  app.post('/api/server-config/discard', requireAuth, requireAdmin, requireCsrfHeader, async (request, response, next) => {
    noStore(response);
    try {
      if (request.body?.key && request.body?.file) {
        await panelDb.execute(
          "UPDATE panel_config_pending SET status = 'discarded' WHERE key_name = ? AND file_name = ? AND status = 'pending'",
          [request.body.key, request.body.file],
        );
      } else {
        await panelDb.execute("UPDATE panel_config_pending SET status = 'discarded' WHERE status = 'pending'");
      }
      await audit({
        actorAccount: request.session.sub, actorName: request.session.username, action: 'server-config-discard',
        target: request.body?.key ? `${request.body.file}:${request.body.key}` : null, detail: null, result: 'ok', ip: clientKey(request),
      });
      response.status(204).end();
    } catch (error) {
      next(error);
    }
  });

  app.get('/api/server-config/pending', requireAuth, requireAdmin, async (_request, response, next) => {
    noStore(response);
    try {
      const [changeRows] = await panelDb.query(
        "SELECT key_name AS keyName, file_name AS fileName, category_id AS categoryId, new_value AS newValue, risk FROM panel_config_pending WHERE status = 'pending' ORDER BY created_at",
      );
      const [applyRows] = await panelDb.query(
        'SELECT status, requested_at AS requestedAt, applied_at AS appliedAt, error FROM panel_config_apply_request ORDER BY id DESC LIMIT 1',
      );
      response.json({
        changes: changeRows.map((row) => ({ key: row.keyName, file: row.fileName, categoryId: row.categoryId, value: row.newValue, risk: row.risk })),
        apply: applyRows[0] || null,
      });
    } catch (error) {
      next(error);
    }
  });

  // Nunca escribe en disco ni invoca systemctl desde aquí: sólo encola la
  // orden. Si ya hay una solicitud sin procesar, devuelve esa misma en vez de
  // duplicarla (ac-panel-config-apply.timer la recoge en segundos).
  app.post('/api/server-config/save', requireAuth, requireAdmin, requireCsrfHeader, async (request, response, next) => {
    noStore(response);
    try {
      const [pendingCountRows] = await panelDb.query("SELECT COUNT(*) AS count FROM panel_config_pending WHERE status = 'pending'");
      if (!Number(pendingCountRows[0]?.count)) return response.status(400).json({ error: 'No hay cambios pendientes' });

      // Idempotencia real: la columna generada `pending_marker` (ver
      // install.sh) sólo permite una fila 'pending' a la vez en toda la
      // tabla, así que el INSERT es la comprobación atómica — la comprobación
      // previa por SELECT dejaba una ventana entre leer e insertar en la que
      // dos peticiones simultáneas podían encolar dos solicitudes distintas.
      try {
        const [insertResult] = await panelDb.execute(
          "INSERT INTO panel_config_apply_request (requested_by, requested_at, status) VALUES (?, NOW(), 'pending')",
          [request.session.username],
        );
        await audit({
          actorAccount: request.session.sub, actorName: request.session.username, action: 'server-config-save',
          target: null, detail: `${pendingCountRows[0].count} cambios`, result: 'ok', ip: clientKey(request),
        });
        return response.status(202).json({ id: insertResult.insertId });
      } catch (error) {
        if (error?.code !== 'ER_DUP_ENTRY') throw error;
        const [existingRows] = await panelDb.query(
          "SELECT id FROM panel_config_apply_request WHERE status = 'pending' ORDER BY id DESC LIMIT 1",
        );
        if (existingRows[0]) return response.status(202).json({ id: existingRows[0].id });
        throw error;
      }
    } catch (error) {
      next(error);
    }
  });
}
